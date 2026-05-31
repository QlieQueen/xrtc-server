#include "ice/ice_connection.h"

#include <sstream>
#include <rtc_base/logging.h>
#include <rtc_base/time_utils.h>
#include <rtc_base/helpers.h>
#include <rtc_base/string_encode.h>

#include "ice/stun_request.h"

namespace xrtc {

// ============================================================================
// RTT_RATIO — 指数平滑权重: old_rtt : new_rtt = 3 : 1
//
// rtt_new = rtt_old * 0.75 + rtt_measured * 0.25
// 避免单次测量波动导致 RTT 抖动过大。
// ============================================================================
const int RTT_RATIO = 3;
const int MIN_RTT = 100; // 100ms
const int MAX_RTT = 60000; // 60s

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
        signal_read_packet(this, buf, len, ts);
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

void IceConnection::print_pings_since_last_response(std::string& pings, size_t max) {
    std::stringstream ss;
    if (_pings_since_last_responses.size() > max) {
        for (int i = 0; i < max; i++) {
            ss << rtc::hex_encode(_pings_since_last_responses[i].id) << " ";
        }
        ss << "... " << (_pings_since_last_responses.size() - max) << " more";
    } else {
        for (auto ping : _pings_since_last_responses) {
            ss << rtc::hex_encode(ping.id) << " ";
        }
    }

    pings = ss.str();
}

// ============================================================================
// IceConnection::on_connection_request_response — 处理成功 STUN 响应
//
// 调用链: on_read_packet → MI 校验 → check_response → ConnectionRequest::on_request_response
//         → 此处。计算 RTT、打印 ping 列表、更新读写状态。
// ============================================================================
void IceConnection::on_connection_request_response(ConnectionRequest* request, StunMessage* msg) {
    int rtt = request->elapsed();
    std::string pings;
    print_pings_since_last_response(pings, 5);
    RTC_LOG(LS_INFO) << to_string() << ": Received "
        << stun_method_to_string(msg->type())
        << ", id=" << rtc::hex_encode(msg->transaction_id())
        << ", rtt=" << rtt
        << ", pings=" << pings;
    received_ping_response(rtt);
}

// ============================================================================
// IceConnection::set_state — 更新 Candidate pair 状态 (RFC 5245)
//
// 状态流转:
//   WAITING → IN_PROGRESS (发出 ping)
//   IN_PROGRESS → SUCCEEDED (收到回复)
//   任意状态 → FAILED (确认不通) → destroy()
// 仅当状态实际变化时才打日志。
// ============================================================================
void IceConnection::set_state(IceCandidatePairState state) {
    IceCandidatePairState old_state = _state;
    if (old_state != state) {
        _state = state;
        RTC_LOG(LS_INFO) << to_string() << ": set_state " << old_state
            << "->" << _state;
    }
}

// ============================================================================
// IceConnection::destroy — 销毁连接
//
// 发射 signal_connection_destroy 通知 Channel 清理引用，然后 delete this。
// 只能由 fail_and_destroy() 调用，不应直接调用。
// ============================================================================
void IceConnection::destroy() {
    RTC_LOG(LS_INFO) << to_string() << ": Connection destroyed";
    signal_connection_destroy(this);
    delete this;
}

void IceConnection::fail_and_destroy() {
    set_state(IceCandidatePairState::FAILED);
    destroy();
}

// ============================================================================
// IceConnection::_too_many_ping_failed — 连续 ping 失败次数是否达阈值
//
// 取第 max_pings 个未回复 ping，计算其期望响应时间:
//   expected_response_time = sent_time + rtt (rtt = 2 * _rtt 作为容忍窗口)
// 若 now > expected_response_time，说明该 ping 已超时 → 连续失败达阈值。
// ============================================================================
bool IceConnection::_too_many_ping_failed(size_t max_pings, int rtt, int64_t now) {
    if (_pings_since_last_responses.size() < max_pings) {
        return false;
    }

    int expected_response_time = _pings_since_last_responses[max_pings - 1].sent_time + rtt;
    return now > expected_response_time;
}

// ============================================================================
// IceConnection::_too_long_without_response — 最早未回复 ping 是否超时
//
// 检查 _pings_since_last_responses[0] (最旧未回复 ping) 的等待时间。
// now > sent_time + min_time → 超时。
// ============================================================================
bool IceConnection::_too_long_without_response(int min_time, int64_t now) {
    if (_pings_since_last_responses.empty()) {
        return false;
    }

    return now > _pings_since_last_responses[0].sent_time + min_time;
}

// ============================================================================
// IceConnection::update_state — 连接探活: 根据 ping 响应历史降级 write state
//
// 两阶段退化:
//   1. WRITABLE → WRITE_UNRELIABLE: >=5 个连续 ping 无回复 且 等待 >5s
//   2. WRITE_UNRELIABLE / WRITE_INIT → WRITE_TIMEOUT: 等待 >15s 无回复
//
// RTT 容忍窗口 = 2 * _rtt，钳位在 [MIN_RTT=100ms, MAX_RTT=60000ms]。
// 每次调用末尾 update_receiving 同步对端方向存活状态。
// ============================================================================
void IceConnection::update_state(int64_t now) {
    int rtt = 2 * _rtt;
    if (rtt < MIN_RTT) {
        rtt = MIN_RTT;
    } else if (rtt > MAX_RTT) {
        rtt = MAX_RTT;
    }

    if (_write_state == STATE_WRITABLE &&
            _too_many_ping_failed(CONNECTION_WRITE_CONNECT_FAILS, rtt, now) &&
            _too_long_without_response(CONNECTION_WRITE_CONNECT_TIMEOUT, now))
    {
        RTC_LOG(LS_INFO) << to_string() << ": UnWritable after "
            << CONNECTION_WRITE_CONNECT_FAILS << " ping fails and "
            << now - _pings_since_last_responses[0].sent_time
            << "ms without a response.";
        set_write_state(STATE_WRITE_UNRELIABLE);       
    }

    if ((_write_state == STATE_WRITE_UNRELIABLE || _write_state == STATE_WRITE_INIT) &&
            _too_long_without_response(CONNECTION_WRITE_TIMEOUT, now))
    {
        RTC_LOG(LS_INFO) << to_string() << ": Timeout after "
            << now - _pings_since_last_responses[0].sent_time
            << "ms without a response";
        set_write_state(STATE_WRITE_TIMEOUT);        
    }

    update_receiving(now);
}

// ============================================================================
// IceConnection::on_connection_request_error_response — 处理 STUN 错误响应
//
// 对端返回错误 (如 401 Unauthorized，500 Server Error 等)。
// 后续 commit 实现错误处理策略 (标记连接失败、重试等)。
// ============================================================================
void IceConnection::on_connection_request_error_response(ConnectionRequest* request, StunMessage* msg) {
    // TODO: 错误响应处理 (后续 commit)
    int rtt = request->elapsed();
    int error_code = msg->get_error_code_value();
    RTC_LOG(LS_INFO) << to_string() << ": Received "
        << stun_method_to_string(msg->type())
        << ", id=" << rtc::hex_encode(msg->transaction_id())
        << ", rtt=" << rtt
        << ", error code=" << error_code;
    if (error_code == STUN_ERROR_BAD_REQUEST ||
            error_code == STUN_ERROR_UNAUTHORIZED ||
            error_code == STUN_ERROR_SERVER_ERROR)
    {
        // todo: retry maybe recover
    } else {
        fail_and_destroy();
    }
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

// ============================================================================
// IceConnection::stable — 连接是否稳定 (RTT 样本充足且无丢包)
//
// 条件: RTT 样本数 > 4 (至少 5 次采样，确保平滑值可靠)
//       且当前没有 ping 等待超过 2*RTT (无丢包/严重延迟)
// 影响: stable=true → ping 间隔从 900ms 升到 2500ms
// ============================================================================
bool IceConnection::stable(int64_t now) const {
    return _rtt_samples > RTT_RATIO + 1 && !_miss_response(now);
}

// ============================================================================
// IceConnection::_miss_response — 是否有 ping 超时未回复
//
// 检查最早的未收到回复的 ping，等待时间 > 2 * rtt 视为丢失。
// 如果最近没有未回复的 ping (空队列)，返回 false (没丢包)。
// ============================================================================
bool IceConnection::_miss_response(int64_t now) const {
    if (_pings_since_last_responses.empty()) {
        return false;
    }

    int waiting = now - _pings_since_last_responses[0].sent_time;

    return waiting > 2 * _rtt;
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
    _last_ping_sent = now;
    ConnectionRequest* request = new ConnectionRequest(this);
    _pings_since_last_responses.push_back(SentPing(request->id(), now));
    RTC_LOG(LS_INFO) << to_string() << ": Sending STUN ping, id="
        << rtc::hex_encode(request->id());
    _request_manager.send(request);
    set_state(IceCandidatePairState::IN_PROGRESS);
    _num_pings_sent++;
}

// ============================================================================
// IceConnection::last_received — 最近一次收到任何数据的时间
//
// 取三类时间戳的最大值: ping request、ping response、数据包 (DTLS/RTP)
// ============================================================================
int64_t IceConnection::last_received() {
    return std::max(std::max(_last_ping_received, _last_ping_response_received),
        _last_data_received);
}

// ============================================================================
// IceConnection::receiving_timeout — 接收超时阈值
//
// 超过 2500ms 没收到任何数据 → receiving = false (对端方向失活)
// ============================================================================
int IceConnection::receiving_timeout() {
    return WEAK_CONNECTION_RECEIVE_TIMEOUT;
}

// ============================================================================
// IceConnection::priority — RFC 5245 candidate pair 优先级
//
// g = local candidate priority (controlling)
// d = remote candidate priority (controlled)
// priority = 2^32 * min(G, D) + 2 * max(G, D) + (G > D ? 1 : 0)
//
// 值越大连接越优, 在 _compare_connections 中作为第5级比较标准。
// ============================================================================
uint64_t IceConnection::priority() {
    uint32_t g = local_candidate().priority;
    uint32_t d = remote_candidate().priority;
    uint64_t priority = std::min(g, d);
    priority = priority << 32;
    return priority + 2 * std::max(g, d) + (g > d ? 1 : 0);
}

// ============================================================================
// IceConnection::update_receiving — 更新"对端→我"方向是否存活
//
// 两条判断路径:
//   Case 1: _last_ping_sent < _last_ping_response_received
//     → 我发了 ping，之后收到了对方的 ping response → 对端肯定活着
//   Case 2: 没收到 ping response
//     → 检查是否在 receiving_timeout() 窗口内收到过任何数据
//     → last_received() 取 _last_ping_received / _last_ping_response_received /
//       _last_data_received 三者的最大值
//
// 状态变化时发射 signal_state_change，触发 Channel 重新排序连接。
// ============================================================================
void IceConnection::update_receiving(int64_t now) {
    bool receiving = false;
    if (_last_ping_sent < _last_ping_response_received) {
        receiving = true;
    } else {
        receiving = last_received() > 0 &&
            (now < last_received() + receiving_timeout());
    }

    if (_receiving == receiving) {
        return;
    }

    RTC_LOG(LS_INFO) << to_string() << ": set receiving to " << receiving;
    _receiving = receiving;
    signal_state_change(this);
}

// ============================================================================
// IceConnection::set_write_state — 更新写状态
//
// "我们→对端"方向 (ping 回复率):
//   STATE_WRITABLE(0)     → ping 得到回复
//   STATE_WRITE_UNRELIABLE(1) → 少量失败
//   STATE_WRITE_INIT(2)   → 初始
//   STATE_WRITE_TIMEOUT(3)→ 超时
//
// 状态变化时发射 signal_state_change，触发 Channel 重新排序连接。
// ============================================================================
void IceConnection::set_write_state(WriteState state) {
    WriteState old_state = _write_state;
    _write_state = state;
    if (old_state != state) {
        RTC_LOG(LS_INFO) << to_string() << ": set write state from " << old_state
            << " to " << state;
        signal_state_change(this);
    }
}

// ============================================================================
// IceConnection::received_ping_response — 收到 ping 响应后的状态更新
//
// 1. 指数平滑更新 RTT (首次=测量值, 后续=old*0.75+new*0.25)
// 2. 记录 _last_ping_response_received 时间戳 (用于 RTT 和 receiving 判断)
// 3. 清空 _pings_since_last_responses (这些 ping 已收到回复)
// 4. update_receiving → 对端方向活着
// 5. set_write_state → 我们→对端方向设为 WRITABLE
// ============================================================================
void IceConnection::received_ping_response(int rtt) {
    // old_rtt : new_rtt = 3 : 1
    // 5 10 20 
    // rtt1 = 5
    // rtt2 = 5 * 0.75 + 10 * 0.25 = 3.75 + 2.5 = 6.25
    if (_rtt_samples > 0) {
        _rtt = rtc::GetNextMovingAverage(_rtt, rtt, RTT_RATIO);
    } else {
        _rtt = rtt;
    }

    ++_rtt_samples;

    _last_ping_response_received = rtc::TimeMillis();
    _pings_since_last_responses.clear();
    update_receiving(_last_ping_response_received);
    set_write_state(STATE_WRITABLE);
    set_state(IceCandidatePairState::SUCCEEDED);
}

int IceConnection::send_packet(const char* data, size_t len) {
    if (!_port) {
        return -1;
    }

    return _port->send_to(data, len, _remote_candidate.address);
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
