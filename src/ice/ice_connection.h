#ifndef __ICE_CONNECTION_H_
#define __ICE_CONNECTION_H_

#include "base/event_loop.h"
#include "ice/udp_port.h"
#include "ice/candidate.h"
#include "ice/stun.h"

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
class IceConnection {
public:
    // 写状态 — 衡量"我们→对端是否畅通"（ping 回复率）
    enum WriteState {
        STATE_WRITABLE = 0,         // ping 能收到回复，连接健康
        STATE_WRITE_UNRELIABLE = 1, // 少量 ping 未收到回复，不稳定
        STATE_WRITE_INIT = 2,       // 初始状态，尚未收到 ping 回复
        STATE_WRITE_TIMEOUT = 3,    // 大量 ping 无回复，连接已死
    };

    IceConnection(EventLoop* el, UDPPort* port, const Candidate& remote_candidate);
    ~IceConnection();

    const Candidate& remote_candidate() const { return _remote_candidate; }

    void handle_stun_binding_request(StunMessage* stun_msg);
    void send_stun_binding_response(StunMessage* stun_msg);
    void send_response_message(const StunMessage& response);

    void on_read_packet(const char* buf, size_t len, int64_t ts);

    // 读写状态查询 — 用于 Controller 的 ping 决策和 Channel 的状态聚合
    bool writable() { return _write_state == STATE_WRITABLE; }
    bool receiving() { return _receiving; }
    bool weak() { return !(writable() && receiving()); }   // 双向都通才不是 weak
    bool active() { return _write_state != STATE_WRITE_TIMEOUT; }  // 没超时就是活跃的
    
    std::string to_string();
private:
    EventLoop* _el;
    UDPPort* _port;
    Candidate _remote_candidate;

    WriteState _write_state = STATE_WRITE_INIT;
    bool _receiving = false;
};

} // namespace xrtc

#endif  // __ICE_CONNECTION_H_