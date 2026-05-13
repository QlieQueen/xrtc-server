#include "ice/udp_port.h"

#include <netinet/in.h>
#include <sstream>
#include <memory>

#include <rtc_base/logging.h>
#include <rtc_base/crc32.h>
#include <rtc_base/byte_buffer.h>

#include "base/socket.h"
#include "ice/ice_connection.h"

namespace xrtc {

UDPPort::UDPPort(EventLoop* el, const std::string& transport_name,
        IceCandidateComponent component, const IceParameters& ice_params) :
    _el(el),
    _transport_name(transport_name),
    _component(component),
    _ice_params(ice_params)
{
}

UDPPort::~UDPPort() {
}

static std::string compute_foundation(const std::string& type,
    const std::string& protocol,
    const rtc::SocketAddress& base)
{
    std::stringstream ss;
    ss << type << base.HostAsURIString() << protocol;
    return std::to_string(rtc::ComputeCrc32(ss.str()));
}

int UDPPort::create_ice_candidate(Network* network, int min_port, int max_port,
    Candidate& c)
{
    _socket = create_udp_socket(network->ip().family());
    if (_socket < 0) {
        return -1;
    }

    if (sock_setnonblock(_socket) != 0) {
        close(_socket);
        _socket = -1;
        return -1;
    }

    struct sockaddr_in addr_in;
    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = network->ip().family();
    addr_in.sin_addr.s_addr = INADDR_ANY;

    if (sock_bind(_socket, (struct sockaddr*)&addr_in, sizeof(addr_in),
            min_port, max_port) != 0)
    {
        close(_socket);
        _socket = -1;
        return -1;
    }

    int port = 0;
    if (sock_get_address(_socket, nullptr, &port) != 0) {
        close(_socket);
        _socket = -1;
        return -1;
    }

    _local_addr.SetIP(network->ip());
    _local_addr.SetPort(port);

    _async_socket = std::make_unique<AsyncUdpSocket>(_el, _socket);
    _async_socket->signal_read_packet.connect(this,
            &UDPPort::_on_read_packet);
        
    RTC_LOG(LS_INFO) << "prepared socket address: " << _local_addr.ToString();

    c.component = _component;
    c.protocol = "udp";
    c.address = _local_addr;
    c.port = port;
    c.priority = c.get_priority(ICE_TYPE_PREFERENCE_HOST, 0, 0);
    c.username = _ice_params.ice_ufrag;
    c.password = _ice_params.ice_pwd;
    c.type = LOCAL_PORT_TYPE;
    c.foundation = compute_foundation(c.type, c.protocol, c.address);

    _candidates.push_back(c);

    return 0;
}

int UDPPort::send_to(const char* buf, size_t len, const rtc::SocketAddress& addr) {
    if (!_async_socket) {
        return -1;
    }

    return _async_socket->send_to(buf, len, addr);
}

// ============================================================================
// UDPPort::_on_read_packet — AsyncUdpSocket 的回调
//
// UDP 数据到达时的入口。完整的 STUN 处理流程:
//   1. get_stun_message() — validate_fingerprint → read() → USERNAME解析 → MI校验
//   2. 校验通过 → emit signal_unknown_address → 上层创建 prflx candidate
//   3. 校验失败 → send_binding_error_response (在 get_stun_message 内处理)
// ============================================================================
void UDPPort::_on_read_packet(AsyncUdpSocket* socket, char* buf, size_t size,
                const rtc::SocketAddress& addr, int64_t ts)
{
    std::unique_ptr<StunMessage> stun_msg;
    std::string remote_ufrag;
    bool res = get_stun_message(buf, size, &stun_msg, addr, &remote_ufrag);
    if (!res || !stun_msg) {
        return;
    }

    // 合法的 binding request → 通知上层创建 peer reflexive candidate
    if (STUN_BINDING_REQUEST == stun_msg->type()) {
        RTC_LOG(LS_INFO) << to_string() << ": Received "
            << stun_method_to_string(stun_msg->type())
            << " id=" << rtc::hex_encode(stun_msg->transaction_id())
            << " from " << addr.ToString();
        signal_unknown_address(this, addr, stun_msg.get(), remote_ufrag);
    }
}

bool UDPPort::get_stun_message(const char* data, size_t len,
        std::unique_ptr<StunMessage>* out_msg,
        const rtc::SocketAddress& addr,
        std::string* out_username)
{
    // ---- 第 1 步: 快速 FINGERPRINT 校验 (CRC32) ----
    if (!StunMessage::validate_fingerprint(data, len)) {
        return false;
    }

    out_username->clear();

    // ---- 第 2 步: 解析 STUN 消息 ----
    std::unique_ptr<StunMessage> stun_msg = std::make_unique<StunMessage>();
    rtc::ByteBufferReader buf(data, len);
    if (!stun_msg->read(&buf) || buf.Length() != 0) {
        return false;
    }

    // ---- 第 3 步: Binding Request 专项校验 ----
    if (STUN_BINDING_REQUEST == stun_msg->type()) {
        // 3a. 必须有 USERNAME 和 MESSAGE-INTEGRITY 属性
        if (!stun_msg->get_byte_string(STUN_ATTR_USERNAME) ||
            !stun_msg->get_byte_string(STUN_ATTR_MESSAGE_INTEGRITY))
        {
            RTC_LOG(LS_WARNING) << to_string() << ": received "
                    << stun_method_to_string(stun_msg->type())
                    << " without username/MI from "
                    << addr.ToString();
            send_binding_error_response(stun_msg.get(), addr, STUN_ERROR_BAD_REQUEST,
                STUN_ERROR_REASON_BAD_REQUEST);
            return true;
        }

        // 3b. 解析并验证 USERNAME (格式: local_ufrag:remote_ufrag)
        std::string local_ufrag;
        std::string remote_ufrag;
        if (!_parse_stun_username(stun_msg.get(), &local_ufrag, &remote_ufrag) ||
            local_ufrag != _ice_params.ice_ufrag)
        {
            RTC_LOG(LS_WARNING) << to_string() << ": received "
                << stun_method_to_string(stun_msg->type())
                << " with bad ufrag: " << local_ufrag
                << " from " << addr.ToString();
            send_binding_error_response(stun_msg.get(), addr, STUN_ERROR_UNATHORIZED,
                STUN_ERROR_REASON_UNATHORIZED);
            return true;
        }

        // 3c. 验证 MESSAGE-INTEGRITY (HMAC-SHA1 with ice_pwd)
        if (stun_msg->validate_message_integrity(_ice_params.ice_pwd) !=
                StunMessage::IntegrityStatus::k_integrity_ok)
        {
            RTC_LOG(LS_WARNING) << to_string() << ": received "
                << stun_method_to_string(stun_msg->type())
                << " with Bad M-I from "
                << addr.ToString();
            send_binding_error_response(stun_msg.get(), addr, STUN_ERROR_UNATHORIZED,
                STUN_ERROR_REASON_UNATHORIZED);
            return true;
        }

        // 校验全部通过 → 把对端的 ufrag 返回给调用方
        *out_username = remote_ufrag;
    }

    *out_msg = std::move(stun_msg);

    return true;
}


// ============================================================================
// UDPPort::_parse_stun_username — 解析 USERNAME 属性值
//
// USERNAME 格式 (RFC 5245 §7.1.2.3): local_ufrag:remote_ufrag
//   - fields[0] (local_ufrag): 本端的 ICE ufrag → 必须等于 _ice_params.ice_ufrag
//   - fields[1] (remote_ufrag): 对端的 ICE ufrag → 返回给调用方，后续创建 IceConnection 用
//
// 注意: "local/remote" 是从接收方视角命名，不是 STUN 协议中的命名。
//       对于 sender 来说拼写是 <peer_ufrag>:<sender_ufrag>。
// ============================================================================
bool UDPPort::_parse_stun_username(StunMessage* stun_msg, std::string* local_ufrag,
                std::string* remote_ufrag)
{
    local_ufrag->clear();
    remote_ufrag->clear();

    const StunByteStringAttribute* attr = stun_msg->get_byte_string(STUN_ATTR_USERNAME);
    if (!attr) {
        return false;
    }

    std::string username = attr->get_string();
    std::vector<std::string> fields;
    rtc::split(username, ':', &fields);
    if (fields.size() != 2) {
        return false;
    }

    *local_ufrag = fields[0];
    *remote_ufrag = fields[1];

    RTC_LOG(LS_WARNING) << "================local ufrag: " << *local_ufrag
                        << ", remote ufrag: " << *remote_ufrag;
    return true;
}

std::string UDPPort::to_string() {
    std::stringstream ss;
    ss << "Port[" << this << ":" << _transport_name << ":" << (int)_component
       << ":" << _ice_params.ice_ufrag << ":" << _ice_params.ice_pwd 
       << ":" << _local_addr.ToString() << "]";
    return ss.str();
}


// ============================================================================
// UDPPort::send_binding_error_response — 发送 STUN 错误响应
//
// 目前为空实现，后续 commit 会填充: 构造 STUN Error Response 并通过 UDP 发送
// ============================================================================
void UDPPort::send_binding_error_response(StunMessage* stun_msg,
        const rtc::SocketAddress& addr,
        int err_code,
        const std::string& reason)
{
    // TODO: 后续 commit 实现
}

// ============================================================================
// UDPPort::create_connection — 创建一个 ICeConnection 并注册到 _connections
//
// 当收到一个对端地址的合法 binding request 时，为这个 remote candidate
// 创建对应的 IceConnection 并存入 _connections map。
//
// 去重逻辑:
//   - 向 _connections 插入 (key=remote_address, value=conn)
//   - 若 key 已存在: 更新 value (替换为最新的 IceConnection)
//   - 若 key 不存在: 插入新条目
// ============================================================================
IceConnection* UDPPort::create_connection(const Candidate& remote_candidate) {
    IceConnection* conn = new IceConnection(_el, this, remote_candidate);
    auto ret = _connections.insert(
        std::make_pair(conn->remote_candidate().address, conn));
    // true = 插入成功（key不存在）；false = 插入失败（key已存在），更新 value 指
    if (ret.second == false && ret.first->second != conn) {
        RTC_LOG(LS_WARNING) << to_string() << "create ice connection on "
            << "an existing remote address, addr: "
            << conn->remote_candidate().address.ToString();
        ret.first->second = conn;
    }

    return conn;
}

} // namespace xrtc