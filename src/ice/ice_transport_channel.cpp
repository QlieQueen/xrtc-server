#include "ice/ice_transport_channel.h"

#include <sstream>
#include <rtc_base/logging.h>
#include <rtc_base/time_utils.h>

#include "ice/ice_connection.h"

namespace xrtc {

// 时钟容差: 从 last_ping_sent_ms 减去 5ms，避免因系统时钟粒度
// 导致错过 ping 周期 (比如定时器在 479ms 触发，本应 480ms 发 ping)
const int PING_INTERVAL_DIFF = 5;

// 定时器回调: libev 在 WEAK_PING_INTERVAL 到期时调用，
// 通过 data 指针找回 IceTransportChannel 实例，触发连通性检查
void ice_ping_cb(EventLoop* /*el*/, TimerWatcher* /*w*/, void* data) {
    IceTransportChannel* channel = (IceTransportChannel*)data;
    channel->_on_check_and_ping();
}

IceTransportChannel::IceTransportChannel(EventLoop* el, PortAllocator* alloctor,
        const std::string& transport_name,
        IceCandidateComponent component) :
    _el(el),
    _transport_name(transport_name),
    _alloctor(alloctor),
    _component(component),
    _ice_controller(std::make_unique<IceController>(this))
{
    RTC_LOG(LS_INFO) << "ice transport channel created, transport_name: " << _transport_name
        << ", component: " << component;
    // 创建重复定时器: 用于周期性连通性检查 (ping)
    _ping_watcher = _el->create_timer(ice_ping_cb, this, true);
}

IceTransportChannel::~IceTransportChannel() {

    // 停止并销毁 ping 定时器
    if (_ping_watcher) {
        _el->delete_timer(_ping_watcher);
        _ping_watcher = nullptr;
    }

    for (auto port : _ports) {
        delete port;
    }

    _ports.clear();

}

void IceTransportChannel::set_ice_params(const IceParameters& ice_params) {
    RTC_LOG(LS_INFO) << "set gathering ICE param"
        << ", transport_name: " << _transport_name
        << ", component: " << _component
        << ", ufrag: " << ice_params.ice_ufrag
        << ", pwd: " << ice_params.ice_pwd;
    _ice_params = ice_params;
}

void IceTransportChannel::set_remote_ice_params(const IceParameters& remote_ice_params) {
    RTC_LOG(LS_INFO) << "set remote ICE param"
        << ", transport_name: " << _transport_name
        << ", component: " << _component
        << ", ufrag: " << remote_ice_params.ice_ufrag
        << ", pwd: " << remote_ice_params.ice_pwd;
    _remote_ice_params = remote_ice_params;

    // ANSWER 到达后，为已有连接补填对端密码 (之前只有 ufrag)
    for (auto conn : _ice_controller->connections()) {
        conn->maybe_set_remote_ice_params(remote_ice_params);
    }
    // 凭据补全后重新检查状态: 可能首次满足 ping 条件
    _sort_connections_and_update_state();
}

void IceTransportChannel::gathering_candidate() {
    // 1.检查_ice_params的ufrag/pwd非空
    if (_ice_params.ice_ufrag.empty() || _ice_params.ice_pwd.empty()) {
        RTC_LOG(LS_WARNING) << "cannot gathering candidate, because ICE param is empty."
            << ", transport_name: " << _transport_name
            << ", component: " << _component
            << ", ufrag: " << _ice_params.ice_ufrag
            << ", pwd: " << _ice_params.ice_pwd;
        return;
    }

    auto network_list = _alloctor->get_networks();
    if (network_list.empty()) {
        RTC_LOG(LS_WARNING) << "cannot gathering candidate. because network list is empty."
            << ", transport_name: " << _transport_name
            << ", component: " << _component;
        return;
    }

    for (auto network : network_list) {
        UDPPort* port = new UDPPort(_el, _transport_name, _component, _ice_params);
        port->signal_unknown_address.connect(this, &IceTransportChannel::_on_unknown_address);
        _ports.push_back(port);

        Candidate c;
        int ret = port->create_ice_candidate(network, _alloctor->min_port(),
            _alloctor->max_port(), c);
        if (ret != 0) {
            continue;
        }

        _local_candidates.push_back(c);

    }
    signal_candidate_allocate_done(this, _local_candidates);
}

// ============================================================================
// IceTransportChannel::_on_unknown_address — 创建 peer reflexive candidate
//
// 当 UDPPort 收到一个未知对端地址的合法 STUN Binding Request 时触发。
// ICE RFC 5245 将此称为 "peer reflexive candidate" (prflx):
//   服务端从 STUN 消息中提取对端地址和 PRIORITY，构造一个 Candidate，
//   它代表"通过 STUN 发现的、对端可通的实际传输地址"。
//
// 与 local host candidate 的区别:
//   - host candidate: 服务端自己的 IP:Port (local)
//   - prflx candidate: 对端的 IP:Port (remote)，从 STUN 消息中发现的
//
// prflx candidate 和 host candidate 的配对形成 ICE candidate pair，
// 后续 ICE 连接状态机会通过发送/接收 STUN 请求完成连通性检查。
// ============================================================================
void IceTransportChannel::_on_unknown_address(UDPPort* port,
        const rtc::SocketAddress& addr,
        StunMessage* stun_msg,
        const std::string& remote_ufrag)
{
    // 1. 提取 PRIORITY 属性 (RFC 5245 §7.1.2.4): 对端计算的候选地址优先级
    const StunUint32Attribute* priority_attr;
    priority_attr = stun_msg->get_uint32_t(STUN_ATTR_PRIORITY);
    if (!priority_attr) {
        RTC_LOG(LS_WARNING) << to_string() << ": priority not found in the"
             << " binding request message, remote_addr: " << addr.ToString();
        port->send_binding_error_response(stun_msg, addr, STUN_ERROR_BAD_REQUEST,
            STUN_ERROR_REASON_BAD_REQUEST);
        return;
    }

    // 2. 用远程地址信息构造 prflx candidate
    uint32_t remote_priority = priority_attr->value();
    Candidate remote_candidate;
    remote_candidate.component = _component;
    remote_candidate.protocol = "udp";
    remote_candidate.priority = remote_priority;
    remote_candidate.address = addr;
    remote_candidate.username = remote_ufrag;  // 对端的 ICE ufrag
    remote_candidate.password = _remote_ice_params.ice_pwd;  // 对端的 ICE pwd
    remote_candidate.type = PRFLX_PORT_TYPE;  // "prflx"

    RTC_LOG(LS_INFO) << to_string() << "create peer reflexive candidate: "
        << remote_candidate.to_string();

    IceConnection* conn = port->create_connection(remote_candidate);
    if (!conn) {
        RTC_LOG(LS_WARNING) << to_string() << ": create connection from "
            << "peer reflexive candidate error, remote_addr: " << addr.ToString();
        port->send_binding_error_response(stun_msg, addr, STUN_ERROR_SERVER_ERROR,
            STUN_ERROR_REASON_SERVER_ERROR);
    }

    RTC_LOG(LS_INFO) << to_string() << ": create connection from "
        << "peer reflexive candidate success, remote_addr: " << addr.ToString();

    // 注册到 controller → 回复 binding request → 更新状态并可能启动 ping
    _add_connection(conn);

    conn->handle_stun_binding_request(stun_msg);

    _sort_connections_and_update_state();
}

// ============================================================================
// ============================================================================
// IceTransportChannel::_add_connection — 注册连接到 controller
//
// 新连接加入时:
//   1. 监听其状态变化信号 → 一旦读写状态变更, 触发重新排序和选路
//   2. 注册到 IceController → 参与 ping 决策和连接排序
// ============================================================================
void IceTransportChannel::_add_connection(IceConnection* conn) {
    conn->signal_state_change.connect(this,
        &IceTransportChannel::_on_connection_state_change);
    _ice_controller->add_connection(conn);
}

// ============================================================================
// IceTransportChannel::_on_connection_state_change — 连接状态变化槽函数
//
// 由 IceConnection::signal_state_change 触发 (set_write_state / update_receiving 中发射)。
// 每次连接读写状态变化时, 重新排序连接列表并根据排序结果决定是否切换 selected。
// ============================================================================
void IceTransportChannel::_on_connection_state_change(IceConnection* /*conn*/) {
    _sort_connections_and_update_state();
}

// ============================================================================
// IceTransportChannel::_sort_connections_and_update_state — 排序连接并更新状态
//
// 连接发生变化时（新增/状态变更）调用:
//   1. _maybe_switch_selected_connection — 按质量排序并可能切换最优连接
//   2. _maybe_start_pinging — 可能首次启动连通性检查
//   3. 后续 commit: _update_state — 更新 channel 的聚合状态
// ============================================================================
void IceTransportChannel::_sort_connections_and_update_state() {
    _maybe_switch_selected_connection(_ice_controller->sort_and_switch_connection());
    _maybe_start_pinging();
}

// ============================================================================
// IceTransportChannel::_maybe_switch_selected_connection — 可能切换最优连接
//
// 本 commit 为占位实现。后续 commit 将实现:
//   根据排序结果比较当前 selected 与候选连接, 决定是否切换。
// ============================================================================
void IceTransportChannel::_maybe_switch_selected_connection(IceConnection* conn) {

}

// ============================================================================
// IceTransportChannel::_maybe_start_pinging — 条件满足时启动连通性检查
//
// 连通性检查只会启动一次 (_start_pinging 防止重复启动)。
// 启动条件: 存在可 ping 的连接 (对端凭据已知 + channel weak)。
// 启动后会在后续 commit 中开启定时器，按策略周期发送 STUN binding request。
// ============================================================================
void IceTransportChannel::_maybe_start_pinging() {
    // 连通性检查只启动一次
    if (_start_pinging) {
        return;
    }

    if (_ice_controller->has_pingable_connection()) {
        RTC_LOG(LS_INFO) << to_string() << ": Have a pingable connection "
            << "for the first time, starting to ping";
        // 启动重复定时器，WEAK_PING_INTERVAL(48ms) * 1000 → 48000 usec
        // channel weak → 加速探测，每 48ms 触发一次 _on_check_and_ping
        _el->start_timer(_ping_watcher, _cur_ping_interval * 1000);
        _start_pinging = true;
    }
}


// ============================================================================
// IceTransportChannel::_on_check_and_ping — 周期性连通性检查入口
//
// 由定时器 ice_ping_cb 回调触发 (周期 = _cur_ping_interval)。
//
// 流程:
//   1. controller 选择本周期要 ping 的连接 + 返回新的 ping_interval
//   2. 如果选中了连接 → _ping_connection 发出 ping
//   3. 如果 interval 变化 (480ms↔48ms): 重启定时器
//      - 降级 (strong→weak): 480ms→48ms, 立即加速探测
//      - 升级 (weak→strong): 48ms→480ms, 节省带宽
//   4. 后续 commit: 检查超时、更新状态、切换连接
// ============================================================================
void IceTransportChannel::_on_check_and_ping() {
    auto result = _ice_controller->select_connection_to_ping(
        _last_ping_sent_ms - PING_INTERVAL_DIFF);

    RTC_LOG(LS_WARNING) << to_string() << ": ping result, conn: " << result.conn
        << ", ping interval: " << result.ping_interval;

    if (result.conn) {
        _ping_connection(const_cast<IceConnection*>(result.conn));
    }

    // ping_interval 变化时重启定时器: 降级加速 / 升级减速
    if (_cur_ping_interval != result.ping_interval) {
        _cur_ping_interval = result.ping_interval;
        _el->stop_timer(_ping_watcher);
        _el->start_timer(_ping_watcher, _cur_ping_interval * 1000);
    }
}

// ============================================================================
// IceTransportChannel::_ping_connection — 对指定连接发出 ping
//
// 更新 channel 全局 ping 时间，委托 IceConnection::ping() 创建并发送
// STUN Binding Request。
// ============================================================================
void IceTransportChannel::_ping_connection(IceConnection* conn) {
    _last_ping_sent_ms = rtc::TimeMillis();
    conn->ping(_last_ping_sent_ms);
}

std::string IceTransportChannel::to_string() {
    std::stringstream ss;
    ss << "Channel[" << this << ":" << _transport_name << ":" << _component
        << "]";
    return ss.str();
}

} // namespace xrtc

