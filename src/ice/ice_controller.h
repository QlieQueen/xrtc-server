#ifndef __ICE_CONTROLLER_H_
#define __ICE_CONTROLLER_H_

#include <set>
#include <vector>

#include "ice/ice_connection.h"

namespace xrtc {

class IceTransportChannel;

struct PingResult {
    PingResult(const IceConnection* conn, int ping_interval) :
        conn(conn), ping_interval(ping_interval) {}

    const IceConnection* conn = nullptr;
    int ping_interval = 0; // 下次channel ping的周期
};

// ============================================================================
// IceController — ICE 连接选择策略
//
// 职责:
//   1. 管理一组 IceConnection（一个 channel 的所有 candidate pair）
//   2. 判断是否有可 ping 的连接（连通性检查的前提条件）
//   3. 后续 commit: 从多个连接中选择最优的 selected connection
//
// weak channel 定义:
//   - 还没有 selected connection
//   - 或者 selected connection 不满足 writable && receiving
//
// 可 ping 条件:
//   1. 对端的 ice_ufrag / ice_pwd 已知
//   2. Channel 处于 weak 状态
// ============================================================================
class IceController {
public:
    IceController(IceTransportChannel* ice_channel) : _ice_channel(ice_channel) {}
    ~IceController() = default;

    void add_connection(IceConnection* conn);
    const std::vector<IceConnection*> connections() { return _connections; }
    bool has_pingable_connection();
    PingResult select_connection_to_ping(int64_t last_ping_sent_ms);
    IceConnection* sort_and_switch_connection();
    void set_selected_connection(IceConnection* conn) { _selected_connection = conn; }

private:
    // channel weak = 没有 selected connection 或 selected connection 不通畅
    bool _weak() {
        return _selected_connection == nullptr || _selected_connection->weak();
    }

    bool _is_pingable(IceConnection* conn);
    const IceConnection* _find_next_pingable_connection(int64_t now);
    bool _is_connection_past_ping_interval(const IceConnection* conn, int64_t now);
    int _get_connection_ping_interval(const IceConnection* conn,   int64_t now);
    bool _more_pingable(IceConnection* conn1, IceConnection* conn2);
    int _compare_connections(IceConnection* a, IceConnection* b);
    bool _ready_to_send(IceConnection* conn);

private:
    IceTransportChannel* _ice_channel;
    IceConnection* _selected_connection = nullptr;
    std::vector<IceConnection*> _connections;
    int64_t _last_ping_sent_ms = 0;                    // channel 全局速率门: 上次发出 ping 的时间
    std::set<IceConnection*> _pinged_connections;
    std::set<IceConnection*> _unpinged_connections;
};

} // namespace xrtc 


#endif // __ICE_CONTROLLER_H_