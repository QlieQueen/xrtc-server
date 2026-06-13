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
    _component(component),
    _alloctor(alloctor),
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

    std::vector<IceConnection*> connections = _ice_controller->connections();
    for (auto connection : connections) {
        connection->destroy();
    }

    for (auto port : _ports) {
        delete port;
    }
    _ports.clear();

    _ice_controller.reset(nullptr);

    RTC_LOG(LS_INFO) << to_string() << ": IceTransportChannel destroy";
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
    conn->signal_connection_destroy.connect(this,
        &IceTransportChannel::_on_connection_destroyed);
    conn->signal_read_packet.connect(this,
        &IceTransportChannel::_on_read_packet);

    _had_connection = true; // 标记曾经有连接, _compute_ice_transport_state 用

    _ice_controller->add_connection(conn);
}

// ============================================================================
// IceTransportChannel::_on_read_packet — 转发非 STUN 数据包到 DtlsTransport
//
// 数据流: IceConnection::on_read_packet → signal_read_packet → 此处 → signal_read_packet
//         → DtlsTransport::_on_read_packet
// ============================================================================
void IceTransportChannel::_on_read_packet(IceConnection* /*conn*/,
        const char* buf, size_t len, int64_t ts)
{
    signal_read_packet(this, buf, len, ts);
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
// IceTransportChannel::_on_connection_destroyed — 连接销毁回调
//
// 由 IceConnection::signal_connection_destroy 触发。
// 从 controller 清理引用；若被销毁的是 selected connection 则清空并重新选路。
// ============================================================================
void IceTransportChannel::_on_connection_destroyed(IceConnection* conn) {
    _ice_controller->on_connection_destroyed(conn);
    RTC_LOG(LS_INFO) << to_string() << ": Remove connection: " << conn
        << " with " << _ice_controller->connections().size() << " remaining";
    if (_selected_connection == conn) {
        RTC_LOG(LS_INFO) << to_string() 
            << ": Selected connection destroyed, should select a new connection";
        _switch_selected_connection(nullptr);
        _sort_connections_and_update_state();
    } else {
        _update_state();
    }
}

// ============================================================================
// IceTransportChannel::_set_receiving — 更新"对端→我们"方向的 channel 聚合状态
//
// 仅当状态实际变化时才发射 signal_receiving_state_change，
// 通知上层 (IceAgent) channel 的 receiving 状态变更。
// ============================================================================
void IceTransportChannel::_set_receiving(bool receiving) {
    if (receiving == _receiving) {
        return;
    }

    _receiving = receiving;
    RTC_LOG(LS_INFO) << to_string() << ": Change receiving to " << _receiving;
    signal_receiving_state_change(this);
}

// ============================================================================
// IceTransportChannel::_set_writable — 更新"我们→对端"方向的 channel 聚合状态
//
// 仅当 selected connection 存在且 writable() 时 _writable 才为 true。
// 状态变化时发射 signal_writable_state_change。
// ============================================================================
void IceTransportChannel::_set_writable(bool writable) {
    if (writable == _writable) {
        return;
    }

    if (writable) {
        _has_been_connection = true; // 曾经连通过, 用于检测 k_disconnected
    }

    _writable = writable;
    RTC_LOG(LS_INFO) << to_string() << ": Change writable to " << _writable;
    signal_writable_state_change(this);
}

// ============================================================================
// IceTransportChannel::_update_state — 聚合所有连接的读写状态到 channel 级
//
// writable: selected_connection 存在且 writable() → 我们→对端可发送数据
// receiving: 任意连接 receiving() → 对端→我们路径存活
// ============================================================================
void IceTransportChannel::_update_state() {
    bool writable = _selected_connection && _selected_connection->writable();
    _set_writable(writable);

    bool receiving = false;
    for (auto conn : _ice_controller->connections()) {
        if (conn->receiving()) {
            receiving = true;
            break;
        }
    }

    _set_receiving(receiving);

    // 新增: 计算 ICE Transport 状态并通知 IceAgent
    IceTransportState state = _compute_ice_transport_state();
    if (state != _state) {
        _state = state;
        signal_ice_state_change(this);
    }
}

// ========================================================================
// _compute_ice_transport_state — 根据连接状态计算 ICE Transport 状态
//
// 两阶段标记:
//   _had_connection:      曾创建过连接 → 如果现在一个 active 都没有 → k_failed
//   _has_been_connection: 曾经 writable → 如果现在不是 writable → k_disconnected
//
// 判断优先级: failed > disconnected > new > checking > connected
// ========================================================================
IceTransportState IceTransportChannel::_compute_ice_transport_state() {
    bool has_connection = false;
    for (auto conn : _ice_controller->connections()) {
        if (conn->active()) {
            has_connection = true;
            break;
        }
    }

    if (_had_connection && !has_connection) {
        return IceTransportState::k_failed;
    }

    if (_has_been_connection && !writable()) {
        return IceTransportState::k_disconnected;
    }

    if (!_had_connection && !has_connection) {
        return IceTransportState::k_new;
    }

    if (has_connection && !writable()) {
        return IceTransportState::k_checking;
    }

    return IceTransportState::k_connected;
}

// ============================================================================
// IceTransportChannel::_sort_connections_and_update_state — 排序连接并更新状态
//
// 连接发生变化时（新增/状态变更）调用:
//   1. _maybe_switch_selected_connection — 按质量排序并可能切换最优连接
//   2. _update_state — 聚合连接状态, 更新 channel 级 writable / receiving
//   3. _maybe_start_pinging — 可能首次启动连通性检查
// ============================================================================
void IceTransportChannel::_sort_connections_and_update_state() {
    _maybe_switch_selected_connection(_ice_controller->sort_and_switch_connection());
    _update_state();
    _maybe_start_pinging();
}

// ============================================================================
// IceTransportChannel::_maybe_switch_selected_connection — 非空包装
//
// 仅当 sort_and_switch_connection 返回非空连接时才执行切换。
// 实际切换逻辑在 _switch_selected_connection。
// ============================================================================
void IceTransportChannel::_maybe_switch_selected_connection(IceConnection* conn) {
    if (!conn) {
        return;
    }

    _switch_selected_connection(conn);
}

// ============================================================================
// IceTransportChannel::_switch_selected_connection — 执行 selected 切换
//
// 更新旧/新连接的 _selected 标记，更新 channel 和 controller 的引用。
// conn 为 nullptr 时表示清空 selected (如当前 selected 被销毁)。
// ============================================================================
void IceTransportChannel::_switch_selected_connection(IceConnection* conn) {
    IceConnection* old_selected_connection = _selected_connection;
    _selected_connection = conn;
    if (old_selected_connection) {
        old_selected_connection->set_selected(false);
        RTC_LOG(LS_INFO) << to_string() << ": previous connection: "
            << old_selected_connection->to_string();
    }

    if (!_selected_connection) {
        RTC_LOG(LS_INFO) << to_string() << ": No connection selected";
        return;
    }

    RTC_LOG(LS_INFO) << to_string() << ": New selected connection: "
        << conn->to_string();

    _selected_connection->set_selected(true);
    _ice_controller->set_selected_connection(_selected_connection);
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
//   1. _update_connection_states → 可能触发 conn timeout → k_failed → 资源清理
//      **注意**: 后续对 _ice_controller 的访问依赖 PeerConnection::destroy()
//      的延迟析构机制。如果在 conn timeout 回调链中同步 delete this, 此处会
//      访问已析构的 _ice_controller 导致 coredump。
//   2. controller 选择本周期要 ping 的连接 + 返回新的 ping_interval
//   3. 如果选中了连接 → _ping_connection 发出 ping
//   4. 如果 interval 变化 (480ms↔48ms): 重启定时器
//      - 降级 (strong→weak): 480ms→48ms, 立即加速探测
//      - 升级 (weak→strong): 48ms→480ms, 节省带宽
//   5. 后续 commit: 检查超时、更新状态、切换连接
// ============================================================================
void IceTransportChannel::_on_check_and_ping() {
    _update_connection_states();
    auto result = _ice_controller->select_connection_to_ping(
        _last_ping_sent_ms - PING_INTERVAL_DIFF);

    if (result.conn) {
        IceConnection* conn = const_cast<IceConnection*>(result.conn);
        _ping_connection(conn);
        _ice_controller->mark_connection_pinged(conn);
    }

    // ping_interval 变化时重启定时器: 降级加速 / 升级减速
    if (_cur_ping_interval != result.ping_interval) {
        _cur_ping_interval = result.ping_interval;
        _el->stop_timer(_ping_watcher);
        _el->start_timer(_ping_watcher, _cur_ping_interval * 1000);
    }
}

// ============================================================================
// IceTransportChannel::_update_connection_states — 每周期轮询所有连接的探活状态
//
// 在 _on_check_and_ping() 开头调用，遍历全部连接调用 update_state()。
// 降级逻辑在 IceConnection::update_state() 中: WRITABLE → UNRELIABLE → TIMEOUT。
// 复制 connections 列表遍历，避免 update_state 间接触发列表变更。
// ============================================================================
void IceTransportChannel::_update_connection_states() {
    std::vector<IceConnection*> connections = _ice_controller->connections();
    int64_t now = rtc::TimeMillis();
    for (auto conn : connections) {
        conn->update_state(now);
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

int IceTransportChannel::send_packet(const char* data, size_t len) {
    if (!_ice_controller->ready_to_send(_selected_connection)) {
        RTC_LOG(LS_WARNING) << to_string() << ": Selected connection not ready to send";
        return -1;
    }

    int sent = _selected_connection->send_packet(data, len);
    if (sent <= 0) {
        RTC_LOG(LS_WARNING) << to_string() << ": Selected connection send failed";
    }


    return sent;
}

std::string IceTransportChannel::to_string() {
    std::stringstream ss;
    ss << "Channel[" << this << ":" << _transport_name << ":" << _component
        << "]";
    return ss.str();
}

} // namespace xrtc

