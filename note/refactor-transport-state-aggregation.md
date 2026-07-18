# 重构：TransportController 状态聚合下沉到 DtlsTransport

## 现状问题

`TransportController::_update_state()` 同时读取 ICE 和 DTLS 两套状态统计，做了本应由 DtlsTransport 内部完成的合并工作：

```cpp
// transport_controller.cpp:128-162 — 当前代码
int total_connected = ice_state_counts[IceTransportState::k_connected] + 
    dtls_state_counts[DtlsTransportState::k_connected];
int total_failed = ice_state_counts[IceTransportState::k_failed] + 
    dtls_state_counts[DtlsTransportState::k_failed];
int total_closed = ice_state_counts[IceTransportState::k_closed] +   // ← ICE k_closed 恒 0
    dtls_state_counts[DtlsTransportState::k_closed];
int total_ice_complete = ice_state_counts[IceTransportState::k_completed]; // ← 恒 0
// ...
```

问题：
1. ICE 的 `k_completed` / `k_closed` 在枚举中定义了但 channel 层永不产出，统计变量恒为 0
2. TransportController 绕过了 DtlsTransport 直接读 `ice_channel()->state()`，破坏了封装
3. DtlsTransport 已经在监听 ICE 的 writable/receiving 信号，但 TransportController 没有利用这些信息

## 目标

TransportController 只关注 DtlsTransport 的状态。ICE 的状态变化由 DtlsTransport 消化，合入自己的状态后再上报。

```
现在:  TransportController → ice_channel->state()  (绕过 DtlsTransport)
                          → dtls->dtls_state()

理想:  TransportController → dtls->merged_state()
         DtlsTransport 内部合并:
           ICE writable/receiving + DTLS handshake state → merged state
```

## 方案

### 1. DtlsTransport 新增 `merged_state()` 方法

```cpp
// dtls_transport.h 新增
enum class DtlsMergedState {
    k_new,
    k_connecting,
    k_connected,
    k_disconnected,
    k_failed,
    k_closed,
};

DtlsMergedState merged_state() const;
```

```cpp
// dtls_transport.cpp 实现
DtlsMergedState DtlsTransport::merged_state() const {
    // DTLS 终态直接透传
    if (_dtls_state == DtlsTransportState::k_failed)   return DtlsMergedState::k_failed;
    if (_dtls_state == DtlsTransportState::k_closed)   return DtlsMergedState::k_closed;
    if (_dtls_state == DtlsTransportState::k_new)      return DtlsMergedState::k_new;
    if (_dtls_state == DtlsTransportState::k_connecting) return DtlsMergedState::k_connecting;

    // k_connected 时检查 ICE 健康度
    if (_dtls_state == DtlsTransportState::k_connected) {
        if (!writable() && !receiving())   return DtlsMergedState::k_failed;       // ICE 死了
        if (!writable())                   return DtlsMergedState::k_disconnected;  // ICE 断流
        return DtlsMergedState::k_connected;  // 一切正常
    }

    return DtlsMergedState::k_new;
}
```

### 2. 简化 IceTransportState 枚举

删除永不产出的 `k_completed` 和 `k_closed`：

```cpp
// ice_transport_channel.h
enum class IceTransportState {
    k_new,
    k_checking,
    k_connected,
    k_failed,
    k_disconnected,
    // k_completed,  // 删除 — _compute_ice_transport_state 永不产出
    // k_closed,     // 删除 — 同上
};
```

### 3. 简化 IceAgent::_update_state()

删除 `total_ice_completed` / `total_ice_closed` 统计和永不命中的分支，Agent 变成干净的透传层：

```cpp
void IceAgent::_update_state() {
    // 统计各 channel 状态
    // failed > disconnected > new > checking → else connected
    // 无 k_completed / k_closed 分支
}
```

### 4. 简化 TransportController::_update_state()

```cpp
void TransportController::_update_state() {
    PeerConnectionState pc_state = PeerConnectionState::k_new;

    std::map<DtlsMergedState, int> state_counts;
    for (auto& [name, dtls] : _dtls_transport_by_name) {
        state_counts[dtls->merged_state()]++;
    }

    int total = _dtls_transport_by_name.size();
    int total_new         = state_counts[DtlsMergedState::k_new];
    int total_connecting  = state_counts[DtlsMergedState::k_connecting];
    int total_connected   = state_counts[DtlsMergedState::k_connected];
    int total_disconnected = state_counts[DtlsMergedState::k_disconnected];
    int total_failed      = state_counts[DtlsMergedState::k_failed];
    int total_closed      = state_counts[DtlsMergedState::k_closed];

    if (total_failed > 0) {
        pc_state = PeerConnectionState::k_failed;
    } else if (total_disconnected > 0) {
        pc_state = PeerConnectionState::k_disconnected;
    } else if (total_new + total_closed == total) {
        pc_state = PeerConnectionState::k_new;
    } else if (total_connecting + total_new > 0) {
        pc_state = PeerConnectionState::k_connecting;
    } else if (total_connected + total_closed == total) {
        pc_state = PeerConnectionState::k_connected;
    }

    if (_pc_state != pc_state) {
        _pc_state = pc_state;
        signal_connection_state(this, pc_state);
    }
}
```

**核心变化**：不再从 `ice_channel()->state()` 读 ICE 状态，只从 `dtls->merged_state()` 读合并后的状态。

### 5. 删除 TransportController 中的信号连接

`TransportController` 不再需要直接订阅 `IceAgent::signal_ice_state`：

```cpp
// transport_controller.cpp 中删除:
_ice_agent->signal_ice_state.connect(this, &TransportController::_on_ice_state);
```

ICE 状态变化 → DtlsTransport::_on_writable_state / _on_receiving_state → merged_state 变化 → 由 DtlsTransport 的信号（已有 `signal_writable_state`、`signal_receiving_state`、`signal_dtls_state`）触发 TransportController::_update_state()。

## 前置条件

完成 DTLS transport 深挖，确认 DtlsTransport 的 writable/receiving 信号覆盖了 ICE 的所有状态变化路径。

## 涉及文件

| 文件 | 修改 |
|------|------|
| `src/ice/ice_transport_channel.h` | 删除 `k_completed`、`k_closed` |
| `src/ice/ice_agent.cpp` | 简化 `_update_state()`，删除死代码 |
| `src/pc/dtls_transport.h` | 新增 `DtlsMergedState` 枚举 + `merged_state()` |
| `src/pc/dtls_transport.cpp` | 实现 `merged_state()` |
| `src/pc/transport_controller.h` | 删除 `_on_ice_state` 回调 |
| `src/pc/transport_controller.cpp` | 简化 `_update_state()`，删除 ICE 信号订阅 |
