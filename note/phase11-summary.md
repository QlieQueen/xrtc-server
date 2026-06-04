# Phase 11 总结：PC/ICE/Agent 三态聚合 + 失败资源清理

## 完成情况

5/5 commits，对应 `1.5.81` ~ `1.5.85`，新增约 150 行代码。

## 本 Phase 目标

将 ICE 层、DTLS 层、PeerConnection 层的状态信号逐级向上聚合，最终通知到 RtcStream。当 PC 进入 `k_failed` 时触发资源清理。

## 架构全景

```
信号聚合链路（自底向上）：

IceConnection::update_state()
  └─ signal_state_change
       └─ IceTransportChannel::_on_connection_state_change()
            └─ _sort_connections_and_update_state()
                 └─ _update_state()
                      └─ _compute_ice_transport_state()
                           └─ signal_ice_state_change(IceTransportState)
                                └─ IceAgent::_on_ice_state_change()
                                     └─ _update_state() — 聚合所有 channel
                                          └─ signal_ice_state(IceTransportState)
                                               └─ TransportController::_on_ice_state()
                                                    └─ signal_connection_state(PeerConnectionState)
                                                         └─ PeerConnection::
                                                         _on_connection_state()
                                                              └─ signal_connection_state()
                                                                   └─ RtcStream::
                                                                   _on_connection_state()
                                                                        └─ RtcStreamManager::
                                                                        on_connection_state()
                                                                             └─ delete push_stream
```

```
DTLS 信号链路（并行的另一条）：

DtlsTransport::_on_dtls_event()
  └─ signal_dtls_state / signal_writable_state / signal_receiving_state
       └─ TransportController::_on_dtls_state()
            └─ signal_connection_state(PeerConnectionState)
```

## 三层状态聚合

### 第一层：IceTransportChannel → IceTransportState

`_compute_ice_transport_state()` 根据连接状态计算 channel 级状态：

| 条件 | 结果 |
|------|------|
| `_had_connection && !has_connection` (曾有过连接但全部死了) | `k_failed` |
| `_has_been_connection && !writable` (曾连通但现在断开) | `k_disconnected` |
| `!_had_connection && !has_connection` (从未有过连接) | `k_new` |
| `has_connection && !writable` (有连接但还不能发) | `k_checking` |
| `writable` (selected 连接可写) | `k_connected` |

**两个标记变量的区别**：
- `_had_connection` — 曾经创建过连接（add_connection 时设为 true），用于判断 k_failed
- `_has_been_connection` — 曾经 writable 过（_set_writable(true) 时设为 true），用于判断 k_disconnected

### 第二层：IceAgent → 聚合多个 Channel

```
IceAgent::_update_state():
  ice_state_count[all_channels]  → 统计每种状态的 channel 数量
  优先级: failed > disconnected > new > checking > completed > connected
```

多个 channel（audio/video）取最差状态。例如 audio=k_connected 但 video=k_failed → agent=k_failed。

### 第三层：PeerConnection → 聚合 ICE + DTLS

```
PeerConnection 状态 = ICE 状态 || DTLS 状态 (取最差):
  优先级: failed > disconnected > new > connecting > connected
```

两路状态都通过 TransportController 汇聚后再发 `signal_connection_state`。

## 核心难点

### 1. re-entrant destruction coredump（Phase 11 最大坑）

**现象**：ICE ping timer 回调 `_on_check_and_ping()` 中触发资源清理，返回后访问已析构的 `_ice_controller`，崩溃在 `select_connection_to_ping()`。

**根因链路**：
```
_on_check_and_ping()                              ← timer 回调
  └─ _update_connection_states()
       └─ conn->update_state(now)                 ← 检测到 15s 超时
            └─ set_write_state(TIMEOUT)
                 └─ signal_state_change
                      └─ ... 层层上报 ...
                           └─ RtcStreamManager::on_connection_state(k_failed)
                                └─ delete push_stream
                                     └─ ~IceTransportChannel()    ← this 析构
                                          └─ ~IceController()
  └─ _ice_controller->select_connection_to_ping()  ← CRASH: this=0x0
```

**为什么会走完整个析构链**：`_update_connection_states()` 复制了 connection 列表遍历（`std::vector<IceConnection*> connections = _ice_controller->connections()`），所以 connection 层面的迭代是安全的。真正的危险在 connection 的状态变化通过信号链一路向上，最终销毁了整个 IceTransportChannel 本身。

**解决方案 — 延迟析构**：

```cpp
// PeerConnection::destroy() — 10ms 一次性 timer 延迟 delete
void PeerConnection::destroy() {
    _destroy_timer = _el->create_timer(destroy_timer_cb, this, false);
    _el->start_timer(_destroy_timer, 10000);
}

// timer 回调中在干净的调用栈中执行 delete
void destroy_timer_cb(EventLoop*, TimerWatcher*, void* data) {
    PeerConnection* pc = (PeerConnection*)data;
    delete pc;
}
```

这样 `_on_check_and_ping()` 完整执行完毕后返回到 libev，event loop 下一个迭代才触发 timer → `delete pc`。

### 2. `~PeerConnection()` 为什么是 private

**设计意图**：强制任何人都不准直接 `delete pc`，必须走 `destroy()` → timer → `delete pc`。

任何能在 ICE/DTLS/STUN 任意一层 timer 回调中触发的析构，都必须用这个模式。把 `~PeerConnection()` 设为 private 是编译期的强制执行：

- `std::unique_ptr<PeerConnection>` → 编译失败（unique_ptr 需要访问析构函数）
- `PeerConnection _pc` (值类型) → 编译失败（值语义需要析构函数）
- `PeerConnection* _pc` 裸指针 → 可以通过 `_pc->destroy()` 走延迟路径
- 只有 `friend destroy_timer_cb` 能调 `delete pc`

### 3. 状态聚合的"取最差"策略

三个层级都用相同的优先级取最差策略：多个子状态中按 `failed > disconnected > new > checking > connected` 选优先级最高的。这保证任何一个子系统出问题都会被整体感知到。

**注意区分**：
- IceAgent 用计数：`ice_state_count[k_disconnected]` 统计有几个 channel 是断开的
- PeerConnection 用双路 OR：ICE 状态 || DTLS 状态

### 4. 信号链路延伸到 RtcStream

Phase 11 之前，状态信号只在 ICE/DTLS/PeerConnection 层内流转。Phase 11 把这条链路打通到 RtcStream：

- `RtcStreamListener` 接口 — 抽象 listener，解耦 stream 和 manager
- `RtcStream::register_listener()` — manager 注册自己
- `RtcStreamManager::on_connection_state()` — k_failed 时 `_remove_push_stream()`

## 疑问与答疑

### Q1: 为什么崩溃时 `this=0x0`，内存不是应该变成野指针吗？

A: `std::unique_ptr` 析构时调用 `reset(nullptr)`，将内部指针置为 0。所以 `delete push_stream` → `~IceTransportChannel()` → `_ice_controller.reset(nullptr)` 后，`_ice_controller` 的底层裸指针变成了 0x0，访问时正好触发 `this=0x0` 的 nullptr dereference，比野指针更容易定位。

### Q2: `_update_connection_states` 复制了 connections 列表，为什么还会出问题？

A: 复制列表保护的是"connection 层面的遍历安全"（遍历过程中 connection 可以被 erase），但保护不了"this 层面的析构安全"。connection 的状态变化通过信号链可以一路销毁整个 IceTransportChannel，这超出了复制列表的控制范围。

### Q3: 为什么不在 `_on_check_and_ping` 里直接检查 `this` 是否有效？

A: C++ 中没有通用的方法在成员函数中检测 `this` 是否已被析构。常见方案：
- `shared_ptr<self>` + `weak_ptr` 检查 — 需要整个对象生命周期管理改成 shared_ptr，改动太大
- `bool _destroyed` flag — 需要每个可能触发析构的调用点都检查，容易遗漏
- 延迟析构 — 从根源上消除问题，一劳永逸

### Q4: 10ms 延迟够吗？会不会 timer 在别的 thread 执行？

A: `_el->create_timer` 创建在 RtcWorker 的 event loop 上，和 ICE ping timer 在同一个线程。延迟后的 delete 会在当前 event loop 迭代完成后执行，10ms 足够（通常 <1ms 就能返回到 libev）。

## Bug 记录

| Bug | 表现 | 根因 |
|-----|------|------|
| `destroy_time_cb` typo | 编译报错 undefined symbol | 函数名 `destroy_timer_cb` 错写成 `destroy_time_cb` |
| `PeerConnection _pc;` 值类型 | 编译报错 private destructor | 值类型成员析构需要访问 `~PeerConnection()`，但它是 private |
| `unique_ptr<PeerConnection>` | 同上编译报错 | unique_ptr 默认 deleter 也需访问 `~PeerConnection()` |
| 直接 delete pc 无延迟 | coredump | re-entrant destruction — timer 回调中析构 this |

## 关键设计决策

1. **`~PeerConnection()` 设为 private + friend destroy_timer_cb** — 编译期强制走延迟析构路径
2. **10ms timer 延迟** — 足够 event loop 完成当前迭代，人眼无感知
3. **`RtcStreamListener` 接口** — stream 不直接依赖 manager，通过 listener 解耦
4. **`RtcStreamType::k_push/k_pull`** — stream 类型枚举，用于 `on_connection_state` 中按类型分支清理
5. **`_had_connection` vs `_has_been_connection`** — 两个标记区分"曾有连接"和"曾连通"，用于 k_failed 和 k_disconnected 的判断

## 与 Phase 10 的关系

| Phase 10 | Phase 11 |
|----------|----------|
| DtlsTransport 内部状态机 | DtlsTransport 状态信号向外暴露 |
| signal_dtls_state/signal_writable_state 发射 | 这些信号被 TransportController 订阅 |
| 单层状态（DTLS | ICE） | 多层聚合（Channel→Agent→PC） |
| DTLS 握手完成 | — | 握手完成后 ICE 可能因网络断开变为 failed |

## 与 Phase 9 的关系

| Phase 9 | Phase 11 |
|---------|---------|
| ICE 探活检测连接超时 | 超时 → k_failed 触发资源清理 |
| update_state 只降级标记 | 降级通过信号链最终销毁对象 |
| "检测 ≠ 响应" | Phase 11 就是"响应"：检测到 failed → 清理 |

## 下一步

Phase 12: PULL 流 + STOP 命令 + SSRC (`d73cd98` → `29eb026`, 10 commits)
