# Phase 9 总结：ICE 连接状态机 + Controller

## 完成情况

21/21 commits，15 个用户 commit（`1.5.54` ~ `1.5.69`），耗时约 7 天。

## 架构全景

```
IceTransportChannel                          ← 定时器驱动 + 状态聚合
  │
  ├── IceController                          ← 决策引擎
  │     ├── sort_and_switch_connection()     ← 4+1 级排序, 选最优连接
  │     ├── select_connection_to_ping()      ← round-robin 公平选择
  │     └── _is_pingable()                   ← 凭据 + 间隔双重门控
  │
  └── IceConnection[]                        ← candidate pair 状态机
        ├── ping() / received_ping_response()
        ├── update_state()                   ← 探活退化: WRITABLE → UNRELIABLE → TIMEOUT
        ├── update_receiving()               ← 对端方向存活检测
        └── StunRequestManager               ← 请求/响应 transaction_id 匹配
```

## 三层职责划分

| 层 | 职责 | 关键方法 |
|----|------|---------|
| **IceConnection** | 单连接状态机：ping/pong、RTT、读写状态、探活退化 | `ping()`, `received_ping_response()`, `update_state()`, `update_receiving()` |
| **IceController** | 决策引擎：选谁 ping、选谁是 best connection | `select_connection_to_ping()`, `sort_and_switch_connection()` |
| **IceTransportChannel** | 编排层：定时器驱动循环、selected 管理、状态聚合发射信号 | `_on_check_and_ping()`, `_update_state()`, `_switch_selected_connection()` |

## 数据流：一次完整的连通性检查周期

```
定时器触发 (48ms / 480ms)
  └─ _on_check_and_ping()
       ├─ _update_connection_states()         ← 轮询所有连接, 降级死连接
       ├─ select_connection_to_ping()
       │    ├─ 计算 ping_interval             ← channel 级速率门
       │    └─ _find_next_pingable_connection()
       │         ├─ selected 优先             ← writable + 过间隔
       │         └─ 否则 round-robin          ← unpinged → pinged 双集合
       └─ _ping_connection(conn)
            └─ conn->ping(now)
                 ├─ new ConnectionRequest      ← 构造 STUN Binding Request
                 ├─ _request_manager.send()    ← 序列化 + 记录 transaction_id
                 └─ _on_stun_send_packet()     ← signal → UDPPort::send_to()

                    ... 对端回复 ...

UDP 包到达 → IceConnection::on_read_packet()
  └─ STUN_BINDING_RESPONSE
       ├─ validate_message_integrity(remote_pwd)
       └─ _request_manager.check_response()
            ├─ 按 transaction_id 匹配 ConnectionRequest
            ├─ on_connection_request_response()
            │    └─ received_ping_response(rtt)
            │         ├─ 指数平滑更新 RTT      ← old*0.75 + new*0.25
            │         ├─ 清空 _pings_since_last_responses
            │         ├─ update_receiving()     ← 对端方向存活
            │         ├─ set_write_state(WRITABLE)
            │         └─ set_state(SUCCEEDED)
            └─ delete request                  ← 内存释放
```

## 核心难点

### 1. 两层限速

Channel 级 48ms/480ms 控制全局发包节奏，Connection 级 48ms/900ms/2500ms 保护单连接不被过度 ping。两者独立判断，选中的连接必须同时通过两层门控。

### 2. 48ms 死循环（commit 19）

现象：ping interval 永远 48ms，日志无限循环。

根因链：
```
_is_pingable 缺少 int64_t now 参数
  → strong channel 返回 false
    → round-robin continue 过滤掉所有连接
      → 没有连接被 ping 满 3 次
        → need_ping_more_at_weak 永远 true
          → ping_interval 永远 WEAK_PING_INTERVAL (48ms)
```

修复：`_is_pingable` 增加第三个分支 `_is_connection_past_ping_interval(conn, now)`，让 strong channel 下也能正常选出可 ping 连接。同时修正 `_get_connection_ping_interval` 中 `stable` 条件反置。

### 3. selected 切换判断链（commit 14-16）

三层把关：
1. top connection 不能发数据 → 不切换
2. 尚无 selected → 直接选 top（冷启动）
3. top RTT 比当前 selected 小 ≥ 10ms → 切换（防抖阈值 `k_min_improvement`）

### 4. channel 的 writable 与 receiving 不对称

| | writable | receiving |
|------|----------|-----------|
| 方向 | 我们→对端 | 对端→我们 |
| 依赖 | 只看 selected connection | 任意连接 |
| 原因 | 发送路径由 nominated pair 决定 | 接收路径由对端控制 |

销毁非 selected 连接时仍需更新 receiving，因为它可能是唯一在接收的连接。

### 5. 检测 ≠ 响应（commit 20）

`update_state` 只降级标记（WRITABLE → UNRELIABLE → TIMEOUT），不做清理。定时器不会停，连接不会被销毁。真正的退出在 Phase 11：用 `active()` 计算 `IceTransportState::k_failed` → PC 销毁 Channel → 析构函数停定时器。

## Bug 记录

| Bug | 表现 | 根因 |
|-----|------|------|
| RTT 钳位反置 | 超时检测永远不触发 | `rtt < MAX_RTT` 应为 `rtt > MAX_RTT` |
| set_state 先 log 后赋值 | 日志显示 `old -> old` | 应先赋值 `_state = state` 再 log |
| stable 条件反置 | ping 间隔异常 | `conn->stable(now)` 应为 `!conn->stable(now)` |
| `_is_pingable` 签名未更新 | 48ms 死循环 | 缺 `int64_t now`，strong channel 永远返回 false |
| vector erase 后 ++iter | UB | `break` 代替继续迭代 |
| `set_selected` 参数/变量名不一致 | 编译通过但逻辑错误 | `_selected = selected` 写成 `_selected = select` |

## 关键设计决策

1. **信号/槽解耦**：StunRequest 不直接依赖 UDPPort，通过 `signal_send_packet` → IceConnection → UDPPort 发送
2. **round-robin 公平性**：双集合（unpinged / pinged）保证每个连接都能轮到，`_more_pingable` 选最 overdue
3. **防抖阈值**：`k_min_improvement = 10ms`，避免 RTT 微小抖动导致频繁切换 selected
4. **指数平滑 RTT**：权重 old:new = 3:1，避免单次测量抖动

## 下一步

Phase 10: DTLS 握手 + SRTP 加密传输 (`b01ce7f` → `092c650`)
