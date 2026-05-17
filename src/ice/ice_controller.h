#ifndef __ICE_CONTROLLER_H_
#define __ICE_CONTROLLER_H_

#include <vector>

#include "ice/ice_connection.h"

namespace xrtc {

class IceTransportChannel;

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
    bool has_pingable_connection();

private:
    // channel weak = 没有 selected connection 或 selected connection 不通畅
    bool _weak() {
        return _selected_connection == nullptr || _selected_connection->weak();
    }

    bool _is_pingable(IceConnection* conn);

private:
    IceTransportChannel* _ice_channel;
    IceConnection* _selected_connection = nullptr;
    std::vector<IceConnection*> _connections;
};

} // namespace xrtc 


#endif // __ICE_CONTROLLER_H_