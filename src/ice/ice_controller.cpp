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
//
// 为什么每检查一个 conn 都要判断 _weak()？
//   _weak() 检查的是 selected connection (当前用于发送数据的连接)，
//   不是正在遍历的这个 conn。它是 channel 级别的"紧急信号"：
//   - Channel 健康时: selected connection 双向通畅，不急于 ping，
//     可以等 ping 间隔到了再发 (后续 commit 补充 _is_connection_past_ping_interval)
//   - Channel weak 时: selected connection 断开或不存在，
//     需要加速探测所有候选连接以尽快找到替代路径，
//     此时跳过间隔限制，所有有凭据的连接都立即视为可 ping
// ============================================================================
bool IceController::_is_pingable(IceConnection* conn) {
    const Candidate& remote = conn->remote_candidate();
    if (remote.username.empty() || remote.password.empty()) {
        RTC_LOG(LS_WARNING) << "remote ICE ufrag and pwd is empty, cannot ping.";
        return false;  // STUN ping 请求可能比 answer 更快到达服务端
    }

    // channel weak = 紧急模式: 当前选中连接已断，需要积极探测找新路径
    if (_weak()) {
        return true;   // 跳过 ping 间隔限制，所有候选连接均可 ping
    }

    return false;      // channel strong → 按 ping 间隔限制 (后续 commit 补充)
}

} // namespace xrtc
