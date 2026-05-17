#include "ice/ice_controller.h"

#include <rtc_base/logging.h>

namespace xrtc {

// ============================================================================
// IceController::has_pingable_connection — 检查是否存在可 ping 的连接
//
// 遍历所有连接，只要有一个满足 _is_pingable 条件就返回 true。
// 用于 _maybe_start_pinging: 首次发现可 ping 连接时启动连通性检查。
// ============================================================================
bool IceController::has_pingable_connection() {
    for (auto conn : _connections) {
        if (_is_pingable(conn)) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// IceController::add_connection — 注册连接
//
// 由 IceTransportChannel::_add_connection 调用。
// 新创建的 IceConnection 注册到 controller，后续参与 ping 决策和排序。
// ============================================================================
void IceController::add_connection(IceConnection* conn) {
    _connections.push_back(conn);
}

// ============================================================================
// IceController::_is_pingable — 判断连接是否可以发送 ping
//
// 条件:
//   1. 对端的 ice_ufrag 和 ice_pwd 必须已知 (否则无法构造 STUN 请求)
//      STUN ping 可能比 SDP answer 更快到达，此时 remote_ice_params 尚未设置
//   2. Channel 为 weak 状态 → 需要积极探测来找更好的连接
// ============================================================================
bool IceController::_is_pingable(IceConnection* conn) {
    const Candidate& remote = conn->remote_candidate();
    if (remote.username.empty() || remote.password.empty()) {
        RTC_LOG(LS_WARNING) << "remote ICE ufrag and pwd is empty, cannot ping.";
        return false;  // STUN ping 请求可能比 answer 更快到达服务端
    }

    if (_weak()) {
        return true;   // channel weak → 需要积极连通性检查
    }

    return false;      // channel strong → 暂时不需要 ping
}

} // namespace xrtc
