#include "ice/ice_connection.h"

#include <rtc_base/logging.h>

#include "ice/stun_request.h"

namespace xrtc {

IceConnection::IceConnection(EventLoop* el, UDPPort* port, const Candidate& remote_candidate) :
    _el(el), _port(port), _remote_candidate(remote_candidate)
{
    _request_manager.signal_send_packet.connect(this, &IceConnection::_on_stun_send_packet);
}

IceConnection::~IceConnection() { }


// ============================================================================
// IceConnection::local_candidate — 获取本地 candidate
//
// UDPPort 可能持有多个 local candidates (当前 host candidate)。
// 取第一个即可用于计算 prflx priority。
// ============================================================================
const Candidate& IceConnection::local_candidate() const {
    return _port->candidates()[0];
}

void IceConnection::handle_stun_binding_request(StunMessage* stun_msg) {
    // role的冲突问题，ice角色： controlling control（为什么xrtc-server不存在角色冲突问题）

    send_stun_binding_response(stun_msg);
}

void IceConnection::send_stun_binding_response(StunMessage* stun_msg) {
    const StunByteStringAttribute* username_attr = stun_msg->get_byte_string(
            STUN_ATTR_USERNAME);
    if (!username_attr) {
        RTC_LOG(LS_WARNING) << "send stun binding response error: no username";
        return;
    }

    StunMessage response;
    response.set_type(STUN_BINDING_RESPONSE);
    response.set_transaction_id(stun_msg->transaction_id());
    response.add_attribute(std::make_unique<StunXorAddressAttribute>(
        STUN_ATTR_XOR_MAPPED_ADDRESS,
        remote_candidate().address));
    response.add_message_integrity(_port->ice_pwd());
    response.add_fingerprint();

    send_response_message(response);
}

void IceConnection::send_response_message(const StunMessage& response) {
    const rtc::SocketAddress& addr = _remote_candidate.address;

    rtc::ByteBufferWriter buf;
    if (!response.write(&buf)) {
        return;
    }

    int ret = _port->send_to(buf.Data(), buf.Length(), addr);
    if (ret < 0) {
        RTC_LOG(LS_WARNING) << to_string() << ": send "
            << stun_method_to_string(response.type())
            << " error, to " << addr.ToString()
            << ", id=" << rtc::hex_encode(response.transaction_id());
        return;
    }

    RTC_LOG(LS_INFO) << to_string() << ": sent "
        << stun_method_to_string(response.type())
        << " to " << addr.ToString()
        << ", id=" << rtc::hex_encode(response.transaction_id());
}

void IceConnection::on_read_packet(const char* buf, size_t len, int64_t ts) {
    std::unique_ptr<StunMessage> stun_msg;
    std::string remote_ufrag;
    const Candidate& remote = _remote_candidate;
    if (!_port->get_stun_message(buf, len, remote.address, &stun_msg, &remote_ufrag)) {
        // 这个不是stun包，可能是其他的，比如dtls或者rtp包
    } else if (!stun_msg) {

    } else { // stun_msg
        switch (stun_msg->type()) {
            case STUN_BINDING_REQUEST: {
                if (remote_ufrag != remote.username) {
                    RTC_LOG(LS_WARNING) << to_string() << ": Received "
                        << stun_method_to_string(stun_msg->type())
                        << " with bad username=" << remote_ufrag
                        << " id=" << rtc::hex_encode(stun_msg->transaction_id());
                    _port->send_binding_error_response(stun_msg.get(),
                            remote.address, STUN_ERROR_UNAUTHORIZED, 
                            STUN_ERROR_REASON_UNAUTHORIZED);
                } else {
                    RTC_LOG(LS_INFO) << to_string() << ": Received "
                        << stun_method_to_string(stun_msg->type())
                        << " id=" << rtc::hex_encode(stun_msg->transaction_id());
                    handle_stun_binding_request(stun_msg.get());
                }
                break;
            }
            // STUN 响应处理 (Success / Error Response)
            // 1. 先用对端 ice_pwd 校验 MESSAGE-INTEGRITY，防止伪造响应
            // 2. 校验通过 → 通过 transaction_id 匹配原始请求并分发回调
            case STUN_BINDING_RESPONSE:
            case STUN_BINDING_ERROR_RESPONSE:
                stun_msg->validate_message_integrity(_remote_candidate.password);
                if (stun_msg->integrity_ok()) {
                    _request_manager.check_response(stun_msg.get());
                }
                break;
            default:
                break;
        }
    }
}

// ============================================================================
// IceConnection::on_connection_response — 处理成功 STUN 响应
//
// 调用链: on_read_packet → MI 校验 → check_response → ConnectionRequest::on_response
//         → 此处。后续 commit 将实现 RTT 计算及写状态更新。
// ============================================================================
void IceConnection::on_connection_response(ConnectionRequest* request, StunMessage* msg) {
    // TODO: RTT 计算 (后续 commit)
    RTC_LOG(LS_WARNING) << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~";
}

// ============================================================================
// IceConnection::on_connection_error_response — 处理 STUN 错误响应
//
// 对端返回错误 (如 401 Unauthorized，500 Server Error 等)。
// 后续 commit 实现错误处理策略 (标记连接失败、重试等)。
// ============================================================================
void IceConnection::on_connection_error_response(ConnectionRequest* request, StunMessage* msg) {
    // TODO: 错误响应处理 (后续 commit)
    RTC_LOG(LS_WARNING) << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~";
}

// ============================================================================
// IceConnection::maybe_set_remote_ice_params — 补填远程 ICE 密码
//
// 场景: STUN binding request 到达时，从 USERNAME 提取了客户端的 ufrag，
//       remote_candidate.username 已知，但 password 还是空的。
//       ANSWER SDP 到达后，set_remote_ice_params 被调用，
//       根据已匹配的 ufrag 把 password 补填进去。
//       此后该连接满足"远程凭据完整"条件，可以被 ping。
// ============================================================================
void IceConnection::maybe_set_remote_ice_params(const IceParameters& ice_params) {
    if (_remote_candidate.username == ice_params.ice_ufrag &&
            _remote_candidate.password.empty())
    {
        _remote_candidate.password = ice_params.ice_pwd;
    }
}

bool IceConnection::stable(int64_t now) const {
    // todo
    return false;
}

// ============================================================================
// IceConnection::ping — 发送 STUN Binding Request (连通性检查)
//
// 1. 创建 ConnectionRequest (构造 StunMessage 并生成随机 transaction_id)
// 2. 记录 SentPing (id + 时间)，用于后续响应匹配和 RTT 计算
// 3. 通过 StunRequestManager::send() → construct() → prepare() 填充属性并发送
// 4. _num_pings_sent++ 影响 ping 间隔计算 (前3次 WEAK 48ms)
//
// 内存管理: request 在收到对应 response 时 delete (后续 commit)
// ============================================================================
void IceConnection::ping(int64_t now) {
    ConnectionRequest* request = new ConnectionRequest(this);
    _pings_since_last_responses.push_back(SentPing(request->id(), now));
    _request_manager.send(request);
    _num_pings_sent++;
}

std::string IceConnection::to_string() {
    std::stringstream ss;
    ss << "Conn[" << this << ":" << _port->transport_name()
        << ":" << _port->component()
        << ":" << _port->local_addr().ToString()
        << ":" << _remote_candidate.address.ToString();
    return ss.str();
}

// ============================================================================
// IceConnection::_on_stun_send_packet — 响应 StunRequestManager 的信号
//
// StunRequest 序列化完成 → 通过 signal_send_packet 发射到此处 → UDP socket 发送。
// 信号/槽设计: StunRequest 不直接依赖 UDPPort，由 IceConnection 做中介。
// ============================================================================
void IceConnection::_on_stun_send_packet(StunRequest* request, const char* buf, size_t size) {
    int ret = _port->send_to(buf, size, _remote_candidate.address);
    if (ret < 0) {
        RTC_LOG(LS_WARNING) << to_string() << ": Failed to send STUN binding request, ret="
            << ret << ", id=" << rtc::hex_encode(request->id());
    }
}


} // namespace xrtc
