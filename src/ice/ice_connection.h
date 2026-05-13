#ifndef __ICE_CONNECTION_H_
#define __ICE_CONNECTION_H_

#include "ice/udp_port.h"
#include "ice/candidate.h"
#include "base/event_loop.h"

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
    IceConnection(EventLoop* el, UDPPort* port, const Candidate& remote_candidate);
    ~IceConnection();

    const Candidate& remote_candidate() const { return _remote_candidate; }

private:
    EventLoop* _el;
    UDPPort* _port;
    Candidate _remote_candidate;
};

} // namespace xrtc

#endif  // __ICE_CONNECTION_H_