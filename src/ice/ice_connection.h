#ifndef __ICE_CONNECTION_H_
#define __ICE_CONNECTION_H_

#include "base/event_loop.h"
#include "ice/udp_port.h"
#include "ice/candidate.h"
#include "ice/stun.h"
#include "ice/ice_credentials.h"
#include "ice/stun_request.h"
#include "ice/ice_connection_info.h"

#include <rtc_base/third_party/sigslot/sigslot.h>

namespace xrtc {

// ============================================================================
// IceConnection — ICE 连接 (对应于 RFC 5245 中的 "candidate pair")
//
// 每个 IceConnection 代表一个本地 candidate 与一个远程 candidate 的配对。
// 它跟踪该配对的连通性检查状态 (ping/pong)、nomination 等 ICE 状态机逻辑。
//
// 生命周期:
//   1. 由 UDPPort::create_connection() 创建 (收到 binding request 时)
//   2. 由 IceTransportChannel 持有和管理
//   3. 后续 commit 会添加状态机 (k_checking → k_connected 等)
//
// 关键成员:
//   _port: 归属的 UDPPort (本地端口)
//   _remote_candidate: 对端的候选地址 (prflx candidate)
// ============================================================================
class IceConnection : public sigslot::has_slots<> {
public:
    // 写状态 — 衡量"我们→对端是否畅通"（ping 回复率）
    enum WriteState {
        STATE_WRITABLE = 0,         // ping 能收到回复，连接健康
        STATE_WRITE_UNRELIABLE = 1, // 少量 ping 未收到回复，不稳定
        STATE_WRITE_INIT = 2,       // 初始状态，尚未收到 ping 回复
        STATE_WRITE_TIMEOUT = 3,    // 大量 ping 无回复，连接已死
    };

    // 记录一次发出的 ping: id 用于匹配响应，sent_time 用于 RTT 计算
    struct SentPing {
        SentPing(const std::string& id, int64_t ts) :
            id(id), sent_time(ts) {}

        std::string id;
        int64_t sent_time;
    };

    IceConnection(EventLoop* el, UDPPort* port, const Candidate& remote_candidate);
    ~IceConnection();

    const Candidate& remote_candidate() const { return _remote_candidate; }
    const Candidate& local_candidate() const;

    UDPPort* port() { return _port; }

    void handle_stun_binding_request(StunMessage* stun_msg);
    void send_stun_binding_response(StunMessage* stun_msg);
    void send_response_message(const StunMessage& response);

    void on_read_packet(const char* buf, size_t len, int64_t ts);
    void print_pings_since_last_response(std::string& pings, size_t max);
    // STUN 响应回调 — 由 ConnectionRequest 委托
    // 调用链: on_read_packet → MI 校验 → check_response → ConnectionRequest → 此处
    void on_connection_request_response(ConnectionRequest* request, StunMessage* msg);
    void on_connection_request_error_response(ConnectionRequest* request, StunMessage* msg);
    // ANSWER 到达后，补填已有连接的对端密码 (ufrag 匹配则填入 pwd)
    void maybe_set_remote_ice_params(const IceParameters& ice_params);

    void set_write_state(WriteState state); 
    WriteState write_state() { return _write_state; }
    // 读写状态查询 — 用于 Controller 的 ping 决策和 Channel 的状态聚合
    bool writable() { return _write_state == STATE_WRITABLE; }
    bool receiving() { return _receiving; }
    int64_t last_received();
    void update_receiving(int64_t now);
    int receiving_timeout();
    uint64_t priority();
    int rtt() const { return _rtt; }
    bool selected() { return _selected; }
    void set_selected(bool selected) { _selected = selected; }
    IceCandidatePairState state() const { return _state; }
    void set_state(IceCandidatePairState state);
    void destroy();
    void fail_and_destroy();
    void update_state(int64_t now);
    
    bool weak() { return !(writable() && receiving()); }   // 双向都通才不是 weak
    bool active() { return _write_state != STATE_WRITE_TIMEOUT; }  // 没超时就是活跃的
    bool stable(int64_t now) const;
    void ping(int64_t now);
    void received_ping_response(int rtt);

    int64_t last_ping_sent() const { return _last_ping_sent; }
    int num_pings_sent() const { return _num_pings_sent; }

    std::string to_string();

public:
    sigslot::signal1<IceConnection*> signal_state_change;
    sigslot::signal1<IceConnection*> signal_connection_destroy;    // 连接销毁时通知 Channel 清理
    sigslot::signal4<IceConnection*, const char*, size_t, int64_t> signal_read_packet;   // 非 STUN 数据包 (DTLS/RTP) → 向上传递

private:
    void _on_stun_send_packet(StunRequest* request, const char* buf, size_t size);
    bool _miss_response(int64_t now) const;   // 是否有 ping 等待超过 2*rtt 未回复
    bool _too_many_ping_failed(size_t max_ping, int rtt, int64_t now);
    bool _too_long_without_response(int min_time, int64_t now);

private:
    EventLoop* _el;
    UDPPort* _port;
    Candidate _remote_candidate;

    WriteState _write_state = STATE_WRITE_INIT;
    bool _receiving = false;
    bool _selected = false;     // 当前是否被 channel 选为 selected connection

    int64_t _last_ping_sent = 0;
    int64_t _last_ping_response_received = 0;
    int64_t _last_ping_received = 0;
    int64_t _last_data_received = 0;
    int _num_pings_sent = 0;
    std::vector<SentPing> _pings_since_last_responses;  // 已发但尚未收到响应的 ping 列表
    StunRequestManager _request_manager;                       // 管理 STUN 请求的发送与后续重传
    int _rtt = 3000;
    int _rtt_samples = 0; // 平滑算法采样数
    IceCandidatePairState _state = IceCandidatePairState::WAITING;  // RFC 5245 candidate pair 状态
};

} // namespace xrtc

#endif  // __ICE_CONNECTION_H_