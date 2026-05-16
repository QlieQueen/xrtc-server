# Phase 9 背景：ICE 连接状态机 + Controller

## 1. 本 Phase 解决什么问题

Phase 8 结束后，我们已经能接收 STUN binding request、校验、返回 binding response。但这是**被动响应**——对端发来请求，我们回复。

ICE 协议的核心是**连通性检查**（Connectivity Check）：双方互相发 STUN binding request 探测地址对的可达性。本 Phase 实现**主动发起**连通性检查，并管理连接的状态。

## 2. 新增/修改的类

| 类 | 职责 | 参考文件 |
|----|------|----------|
| `IceConnection` | ICE candidate pair 状态机 — ping/pong、读写状态、RTT | `ice_connection.h/.cpp` |
| `IceController` | 连接选择器 — 决定 ping 哪个连接、选 selected connection | `ice_controller.h/.cpp` |
| `IceTransportChannel` | 传输通道 — 管理一组连接、ping 定时器、通道状态聚合 | `ice_transport_channel.h/.cpp` |
| `IceAgent` | Agent 状态聚合 — 聚合所有 channel 的状态 | `ice_agent.h/.cpp` |
| `StunRequest` / `StunRequestManager` | STUN 请求的发送和响应匹配 | `stun_request.h/.cpp` |
| `IceCandidatePairState` | Candidate pair 状态枚举 | `ice_connection_info.h` |

## 3. ICE 层次结构

```
IceAgent                          ← 聚合所有 channel 的状态
  ├── IceTransportChannel (RTP)   ← 管理一组连接 + ping 定时器
  │     ├── IceController         ← 决策引擎：选谁 ping、选谁做 selected
  │     ├── IceConnection         ← candidate pair 状态机
  │     ├── IceConnection
  │     └── ...
  └── IceTransportChannel (RTCP)
        ├── IceController
        ├── IceConnection
        └── ...
```

## 4. IceConnection 状态机

### 4.1 WriteState（写状态）

```
STATE_WRITE_INIT(2)              ← 初始状态
  └─ ping 得到响应 → STATE_WRITABLE(0)  ← 可写
       └─ 连续 5 次 ping 无响应 → STATE_WRITE_UNRELIABLE(1)  ← 不可靠
            └─ 15s 无响应 → STATE_WRITE_TIMEOUT(3)  ← 超时
```

**状态转换触发**（`update_state()`）：
- WRITABLE → UNRELIABLE：`_too_many_ping_fails(5次, 2*rtt)` **且** `_too_long_without_response(5s)`
- UNRELIABLE/INIT → TIMEOUT：`_too_long_without_response(15s)`

### 4.2 Receiving（读状态）

```cpp
// 判断逻辑
if (_last_ping_sent < _last_ping_response_received) {
    receiving = true;  // 收到了比最近发出的 ping 更新的响应
} else {
    receiving = last_received() > 0 
        && (now < last_received() + receiving_timeout());  // 2500ms 内有数据到达
}
```

### 4.3 IceCandidatePairState

```
WAITING  → IN_PROGRESS  → SUCCEEDED
                         → FAILED
```

- **WAITING**：初始状态，连通性检查未开始
- **IN_PROGRESS**：已发送 ping，等待响应（`ping()` 时设置）
- **SUCCEEDED**：收到 ping response（`received_ping_response()` 时设置）
- **FAILED**：收到致命错误响应（401/420/500 之外的错误 → `fail_and_destroy()`）

### 4.4 RTT 计算

```
首次: rtt = 测量值
后续: rtt = old_rtt * 0.75 + new_rtt * 0.25   (指数平滑, RTT_RATIO=3)
```

## 5. Ping 机制

### 5.1 Ping 间隔

| 间隔 | 值 | 适用场景 |
|------|-----|----------|
| WEAK_PING_INTERVAL | 48ms | channel 为 weak 状态 OR 连接 ping 次数 < 3 |
| STABLING_CONNECTION_PING_INTERVAL | 900ms | channel weak OR 连接不稳定 |
| STABLE_CONNECTION_PING_INTERVAL | 2500ms | 连接稳定 |

### 5.2 连接何时可 ping（`_is_pingable`）

```
1. 对端的 ice_ufrag、ice_pwd 必须已知
2. Channel 为 weak 状态 → 可 ping
3. 已超过该连接的 ping 间隔 → 可 ping
```

### 5.3 Ping 选择策略（`select_connection_to_ping`）

```
1. 确定 ping 间隔：weak 或 ping 次数 < 3 → WEAK(48ms)，否则 → STRONG(480ms)
2. 确定 ping 对象（_find_next_pingable_connection）：
   a. selected_connection 可写且超间隔 → ping selected
   b. 否则从 _unpinged_connections 中选 wait 最久的
   c. _unpinged_connections 空了 → 把 _pinged_connections 倒回 _unpinged
```

### 5.4 ping 流程

```
IceTransportChannel::_on_check_and_ping()        ← 定时器回调
  └─ _update_connection_states()                ← 更新所有连接状态
  └─ _ice_controller->select_connection_to_ping()  ← 选一个连接
  └─ _ping_connection(conn)                     ← 发送 ping
       └─ conn->ping(now)
            └─ new ConnectionRequest             ← 构造 STUN binding request
            └─ _request_manager.send(request)    ← 发送，记录到 map
```

### 5.5 Ping 响应处理

```
UDP 包到达 → IceConnection::on_read_packet()
  └─ 是 STUN_BINDING_RESPONSE:
       └─ validate_message_integrity(remote_pwd)
       └─ _request_manager.check_response(stun_msg)
            └─ 按 transaction_id 匹配到 ConnectionRequest
            └─ on_request_response(msg) → received_ping_response(rtt)
```

## 6. ConnectionRequest — STUN Ping 请求

`ConnectionRequest` 继承 `StunRequest`，重写 `prepare()`：

```cpp
void ConnectionRequest::prepare(StunMessage* msg) {
    msg->set_type(STUN_BINDING_REQUEST);
    msg->add_attribute(USERNAME);         // remote_ufrag:local_ufrag
    msg->add_attribute(ICE_CONTROLLING);  // 角色标记
    msg->add_attribute(USE_CANDIDATE);    // 提名标记
    msg->add_attribute(PRIORITY);         // prflx priority
    msg->add_message_integrity(remote_pwd);
    msg->add_fingerprint();
}
```

## 7. StunRequest / StunRequestManager

```
StunRequestManager {
    map<transaction_id, StunRequest*> _requests;  ← 待匹配的请求
    
    send(request):
        1. request->set_manager(this)
        2. request->construct() → prepare(_msg)   ← 子类填充属性
        3. _requests[id] = request                ← 记录到 map
        4. request->send() → write(&buf) → signal_send_packet
    
    check_response(msg):
        1. 按 transaction_id 查找 request
        2. 匹配 response type (SUCCESS / ERROR)
        3. 调用 on_request_response / on_request_error_response
        4. delete request（从 map 中移除）
}
```

## 8. IceController — 连接选择器

```
IceController {
    vector<IceConnection*> _connections;           ← 所有连接
    set<IceConnection*>    _unpinged_connections;  ← 待 ping 集合
    set<IceConnection*>    _pinged_connections;    ← 已 ping 集合
    IceConnection*         _selected_connection;   ← 最佳连接
    
    // 核心算法
    sort_and_switch_connection():
        1. 按 writable > receiving > priority > rtt 排序所有连接
        2. 取第一名 top_connection
        3. 若 ready_to_send(top) 且不是当前 selected → 返回 top
        4. 否则 top rtt 比 selected rtt 低至少 10ms → 返回 top
}
```

### 连接排序规则（`_compare_connections`）

优先级从高到低：
1. writable() 更优
2. write_state 值更小更优（WRITABLE=0 < UNRELIABLE=1 < INIT=2 < TIMEOUT=3）
3. receiving() 更优
4. priority() 更大更优
5. RTT 更小更优（secondary sort in `sort_and_switch_connection`）

## 9. IceTransportChannel — 通道状态聚合

### 9.1 状态机

```
k_new → k_checking → k_connected → k_completed
                   → k_failed
       k_connected → k_disconnected
```

**状态计算**（`_compute_ice_transport_state`）：
```
有过连接 + 所有非活跃     → k_failed
曾经连通 + 当前不可写      → k_disconnected
未有过连接 + 无连接        → k_new
有活跃连接 + 不可写        → k_checking
有活跃连接 + 可写          → k_connected
```

### 9.2 事件流

```
收到 binding request → _on_unknown_address
  → port->create_connection(prflx_candidate)
  → _add_connection(conn)     ← 注册信号、加入 controller
  → conn->handle_stun_binding_request(msg)  ← 回复 binding response
  → _sort_connections_and_update_state()
       ├─ _maybe_switch_selected_connection   ← 可能切换 selected
       ├─ _update_state                        ← 更新 writable/receiving/state
       └─ _maybe_state_pinging                 ← 首次启动 ping 定时器
```

## 10. Selected Connection（选中连接）

`selected_connection` 是选出来用于**发送数据**的最佳连接。

**切换时机**：
- 新连接创建（`_on_unknown_address`）
- 连接状态变更（`_on_connection_state_change`）
- 连接销毁（`_on_connection_destroyed`）

**切换条件**（`sort_and_switch_connection`）：
- 新连接排序第一 **且** ready_to_send
- 或者 新连接 RTT 比当前 selected 低至少 10ms

**数据发送路径**：
```
上层 send_packet(data)
  → IceTransportChannel::send_packet()
    → ready_to_send(selected_connection)? 
    → selected_connection->send_packet(data, len)
      → _port->send_to(data, len, remote_addr)
```

## 11. Phase 9 参考 commits（21 个）

按顺序：

| # | Commit | 内容 |
|---|--------|------|
| 1 | `a0bdda8` | UDP 包高性能发送（你的 `_send_data_from_list` 提前做了） |
| 2 | `af25839` | ICE 连接保活 — 定时检查连接是否超时 |
| 3 | `da9e320` | 发送 STUN 错误响应消息 |
| 4 | `89b5985` | 服务侧连通性检查 — `_on_check_and_ping()` |
| 5 | `97afa2c` | ICE 传输通道 ping 周期 — timestamp 追踪 |
| 6 | `d95f7ea` | ICE 连接 ping 优先级选择 |
| 7 | `c365708` | 选择一个连接执行 ping 请求 |
| 8 | `410644e` | 构造 STUN 绑定请求（ConnectionRequest::prepare） |
| 9 | `6225d4f` | ICE 普通提名和积极提名 |
| 10 | `183ca73` | 发送 STUN ping 请求 |
| 11 | `c90a868` | 处理 STUN 响应（RTT 计算） |
| 12 | `3710be1` | 输出 RTT 和 ping id |
| 13 | `d26dce4` | 更新 Ice 连接读写状态 |
| 14 | `f017815` | 选中连接切换策略 |
| 15 | `cb60f45` | 切换策略考虑连接优先级 |
| 16 | `722f876` | 开始切换 selected 连接 |
| 17 | `a60d5ee` | STUN 请求错误响应处理 |
| 18 | `b808117` | 设置 Candidate pair 状态 |
| 19 | `63b5f45` | 处理 ICE ping 周期问题 |
| 20 | `9bc4471` | ICE 连接探活机制 |
| 21 | `9bb997d` | 更新 ICE 传输通道状态 |

## 12. 学习重点

### 必理解的核心概念

1. **WriteState 状态迁移** — 4 个状态的转换条件（ping fail 次数、超时时长）
2. **Ping 间隔分档** — 为什么 weak 状态用 48ms、stable 用 2500ms
3. **Connection 优先级公式** — `2^32*min(g,d) + 2*max(g,d) + (g>d?1:0)`，来自 RFC 5245
4. **Controller 的 pinged/unpinged 集合** — 如何实现 round-robin ping
5. **Selected connection 的选择和切换** — 排序算法 + RTT 改善阈值
6. **StunRequestManager 的设计** — transaction_id map 匹配请求/响应
7. **IceTransportChannel 状态计算** — had_connection / has_been_connection / active 的语义

### 需要注意的细节

1. **`_on_check_and_ping` 中先 `_update_connection_states` 再 ping** — 因为 `update_state` 可能删除连接
2. **StunRequest 的 transaction_id 是随机生成的** — 与本地的 `StunMessage` 不同
3. **Ping 用的是 remote_candidate.password** — 而不是 `_port->ice_pwd()`
4. **error response 中 401/420/500 不会销毁连接** — 可能 recover
5. **`last_received()` 取 max of ping_received / ping_response_received / data_received** — 三种数据都能证明可达

### 与你现有代码的关系

| 你已有的 | Phase 9 要改的 |
|----------|---------------|
| `IceConnection` 基础构造 + `send_stun_binding_response` | + WriteState + Receiving + `ping()` + `update_state()` |
| `signal_unknown_address` → 上层创建 prflx | 上移 → `IceTransportChannel::_on_unknown_address` |
| `UDPPort::_on_read_packet` 处理新地址 | 已有连接走 `IceConnection::on_read_packet` |
| `AsyncUdpSocket` 异步发送 | 基本完成，commit 1 可跳过 |
| `PortAllocator` 基础 | + `set_port_range()` |
