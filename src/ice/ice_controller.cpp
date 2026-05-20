#include "ice/ice_controller.h"

#include <rtc_base/time_utils.h>
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
// 新创建的 IceConnection 注册到 controller:
//   1. 加入 _connections 列表 (总览)
//   2. 加入 _unpinged_connections 集合 (待 ping 队列，参与 round-robin)
// ============================================================================
void IceController::add_connection(IceConnection* conn) {
    _connections.push_back(conn);
    _unpinged_connections.insert(conn);
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

// ============================================================================
// IceController::select_connection_to_ping — 选择本周期要 ping 的连接
//
// 两层限速设计:
//   Channel 层 (本方法): _last_ping_sent_ms → 控制整体发包节奏，一次只发一个 ping
//   Connection 层 (_find_next_pingable_connection): 单连接限速，保护已有连接
//
// ping_interval 决定逻辑 (降级触发 480ms→48ms):
//   1. channel weak (selected conn 断开) → 48ms 加速探测
//   2. 存在 ping 未满 3 次的连接 (新建 → 快速完成初探) → 48ms
//   3. 以上都不满足 → 480ms 正常节奏
//
// 为什么用新 interval 做门控 (而非上次定时器的 interval)?
//   新 interval 反映当前 channel 状态所需的 ping 间隔。
//   weak→strong: 新=480ms > 旧=48ms, 门控更严 → 立即减速
//   strong→weak: 新=48ms < 旧=480ms, 门控宽松 → 立即加速
//   保证 ping 节奏始终跟随 channel 状态变化
// ============================================================================
PingResult IceController::select_connection_to_ping(int64_t last_ping_sent_ms) {
    bool need_ping_more_at_weak = false;
    for (auto conn : _connections) {
        if (conn->num_pings_sent() < MIN_PINGS_AT_WEAK_PING_INTERVAL) {
            need_ping_more_at_weak = true;  // 存在未ping满3次的conn，则需要更快的周期ping
            break;
        }
    }

    // channel是weak状态 或者 存在未ping满3次的connection
    int ping_interval = (_weak() || need_ping_more_at_weak) ? WEAK_PING_INTERVAL
        : STRONG_PING_INTERVAL;

    int now = rtc::TimeMillis();
    const IceConnection* conn = nullptr;
    // Channel 级速率门: 距离上次 ping 过去了 >= 当前所需间隔才进入连接选择
    if (now >= _last_ping_sent_ms + ping_interval) {
        conn = _find_next_pingable_connection(now);
    }

    return PingResult(conn, ping_interval);
}

// ============================================================================
// IceController::_find_next_pingable_connection — 找下一个可 ping 的连接
//
// 两级选择:
//   1. selected connection 优先 (它用于发送数据，优先保持活跃)
//      条件: 存在 && writable && 已过 connection 级 ping 间隔
//   2. 如果 selected 不适合 → round-robin 公平选择 (unpinged / pinged 双集合)
//      - _unpinged_connections: 本轮尚未 ping 的连接
//      - _pinged_connections:   本轮已 ping 过的连接
//      - 当 unpinged 中无可 ping 连接时 → pinged 全部倒回 unpinged，开始新一轮
//      - 用 _more_pingable 选出最久未 ping 的连接 (最 overdue 优先)
//
// 注意: 被选中的连接不会立即从 unpinged 移除，
//       要等实际 ping 发出后才移到 pinged (后续 commit)。
// ============================================================================
const IceConnection* IceController::_find_next_pingable_connection(int64_t now) {
    if (_selected_connection && _selected_connection->writable() &&
            _is_connection_past_ping_interval(_selected_connection, now))
    {
        return _selected_connection;
    }

    bool has_pingable = false;
    for (auto conn : _unpinged_connections) {
        if (_is_pingable(conn)) {
            has_pingable = true;
            break;
        }
    }

    if (!has_pingable) {
        _unpinged_connections.insert(_pinged_connections.begin(),
            _pinged_connections.end());
        _pinged_connections.clear();
    }

    IceConnection* find_conn = nullptr;
    for (auto conn : _unpinged_connections) {
        if (_more_pingable(conn, find_conn)) {
            find_conn = conn;
        }
    }

    return find_conn;
}

// ============================================================================
// IceController::_more_pingable — 比较两个连接谁更应该被 ping
//
// 标准: last_ping_sent() 更小的优先 (等待时间更长 = 更 overdue)。
// 保证 round-robin 公平: 每个连接都能轮到自己被 ping。
// ============================================================================
bool IceController::_more_pingable(IceConnection* conn1, IceConnection* conn2) {
    if (!conn1) {
        return false;
    }

    if (!conn2) {
        return true;
    }

    if (conn1->last_ping_sent() < conn2->last_ping_sent()) {
        return true;
    }

    if (conn1->last_ping_sent() > conn2->last_ping_sent()) {
        return false;
    }

    return false;
}

// ============================================================================
// IceController::_is_connection_past_ping_interval — connection 级速率门
//
// 检查"当前时间是否已过该连接的上次 ping 时间 + 其允许间隔"。
// 两层限速的第二层: 即使 channel 级允许发 ping，单连接也必须过了自己的间隔。
// ============================================================================
bool IceController::_is_connection_past_ping_interval(const IceConnection* conn,
        int64_t now)
{
    int interval = _get_connection_ping_interval(conn, now);
    return now >= conn->last_ping_sent() + interval;
}

// ============================================================================
// IceController::_get_connection_ping_interval — 单连接的 ping 间隔 (三级)
//
//   1. 未满 3 次 ping              → WEAK_PING_INTERVAL (48ms)  快速初探
//   2. channel weak 或连接不稳定   → STABLING_CONNECTION_PING_INTERVAL (900ms)
//   3. channel strong 且连接稳定   → STABLE_CONNECTION_PING_INTERVAL (2500ms)
//
// 注意: 这只是允许 ping 的最小间隔，调度权在 channel 级速率门。
// ============================================================================
int IceController::_get_connection_ping_interval(const IceConnection* conn,
        int64_t now)
{
    if (conn->num_pings_sent() < MIN_PINGS_AT_WEAK_PING_INTERVAL) {
        return WEAK_PING_INTERVAL;
    }

    if (_weak() || !conn->stable(now)) {
        return STABLING_CONNECTION_PING_INTERVAL;
    }

    return STABLE_CONNECTION_PING_INTERVAL;
}

} // namespace xrtc
