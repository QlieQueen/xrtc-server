# Phase 9 — ICE Ping 执行周期：规则与策略

## 一、基本原则

ICE ping（STUN Binding Request）的目的是**验证 candidate pair 的连通性**。ping 不是一次性操作，而是周期性执行。每次 ping 发出后，根据对端回复情况更新连接状态，再决定下一次 ping 谁、间隔多长。

**核心问题**：多个连接（candidate pair），每个连接的健康度不同，如何分配 ping 资源？

### 带宽约束

ping 会占用带宽。约束目标：STUN 流量不超过总带宽的 1%。

```
STUN 包大小 ≈ 60 bytes × 8 = 480 bits
目标带宽占用 < 1% of link

weak 模式:   480bit / 48ms  ≈ 10 kbps
strong 模式: 480bit / 480ms ≈ 1 kbps
stable 模式: 480bit / 2500ms ≈ 0.19 kbps
```

---

## 二、Ping 间隔的两个层级

Channel 周期和 Connection 周期是**两个不同层级**，各自有独立的间隔档位：

### Channel 周期（定时器分辨率）

决定 `_on_check_and_ping` 被唤起的频率。

| 间隔 | 值 | 适用场景 |
|------|-----|---------|
| `WEAK_PING_INTERVAL` | 48ms | selected connection 断开或不存在，紧急搜索新路径 |
| `STRONG_PING_INTERVAL` | 480ms | selected connection 健康，正常分辨率 |

### Connection 周期（单个连接的限速）

决定一个连接被 ping 后至少要等多久才能再次被 ping。

```
WEAK (48ms)
  │  新连接前 3 个 ping，快速探测
  │  num_pings_sent >= 3 后自动降级
  ↓
STABLING (900ms)
  │  channel weak 或 连接未 stable
  │  持续探测，直到连接变为 stable
  ↓
STABLE (2500ms)
  │  channel strong 且 连接 stable
  │  低频保活
```

| 间隔 | 值 | 适用场景 |
|------|-----|---------|
| `WEAK_PING_INTERVAL` | 48ms | 连接 ping 次数不足 3，快速判断是否可用 |
| `STABLING_CONNECTION_PING_INTERVAL` | 900ms | 连接还在探测中（未 stable），或 channel 仍 weak |
| `STABLE_CONNECTION_PING_INTERVAL` | 2500ms | 连接已稳定，低频保活 |

---

## 三、两层间隔：Channel 级别 vs Connection 级别

```
select_connection_to_ping()
  │
  ├── 第一步: 决定 ping_interval (Channel 级别)
  │     ├── weak || need_ping_more_at_weak → WEAK (48ms)
  │     └── 否则 → STRONG (480ms)
  │
  └── 第二步: 选连接 (Connection 级别)
        ├── now >= last_ping_sent + ping_interval ?
        │     ├── 是 → _find_next_pingable_connection()
        │     └── 否 → 不 ping (PingResult.conn = nullptr)
        │
        └── 选中连接后，该连接的 ping 间隔由 _get_connection_ping_interval() 决定:
              ├── num_pings < 3       → WEAK (48ms)
              ├── channel weak
              │   || !conn->stable() → STABLING (900ms)
              └── conn stable          → STABLE (2500ms)
```

**关键**：Channel 级别间隔决定"多久检查一次要不要 ping"，Connection 级别间隔决定"这个连接本身多久 ping 一次"。

### 3.1 实例：Timer 触发但连接不满足 ping 条件

以下是一个具体的时间线，展示 Channel 定时器 480ms 触发，但连接间隔 2500ms 未到时不发 ping：

```
初始条件: 1 个连接, stable, _last_ping_sent=0

T=480ms:  定时器触发
  → now >= _last_ping_sent + ping_interval ?  480 >= 0 + 480 ✓
  → 找连接: conn interval = 2500ms, last_ping=0, 480 < 0+2500 → 不满足 → conn=nullptr
  → 不发 ping

T=960ms:  定时器触发
  → 960 >= 0+480 ✓ → 找连接: 960 < 0+2500 → 不满足 → 不发 ping

T=1440ms: 同上 → 不发 ping

T=1920ms: 同上 → 不发 ping

T=2400ms: 定时器触发
  → 2400 >= 0+480 ✓ → 找连接: 2400 < 0+2500 → 不满足 → 不发 ping

T=2880ms: 定时器触发
  → 2880 >= 0+480 ✓ → 找连接: 2880 >= 0+2500 ✓ → conn 选中!
  → 发送 ping, _last_ping_sent = 2880
  → 连接 _nums_pings_sent = 1 (已 ≥3, 不影响)
```

**要点**：定时器触发 ≠ 一定发 ping。Channel 周期只负责"唤醒检查"，是否真发 ping 取决于被选中的连接是否超过了它自己的 interval。

---

## 四、Ping 连接选择策略

### 4.1 优先级

```
1. selected connection 优先
   └─ 如果 selected connection writable && 已过 ping 间隔 → 直接 ping 它

2. 公平轮转
   └─ 维护 _unpinged_connections 和 _pinged_connections 两个集合
      ping 过的放入 _pinged，_unpinged 为空时把 _pinged 全部倒回 _unpinged
      每轮从 _unpinged 中选最久未 ping 的连接
```

### 4.2 选择算法（`_find_next_pingable_connection`）

```
1. selected connection writable && past ping interval? → 优先
2. 当前轮 (_unpinged_connections) 还有可 ping 的吗?
   ├── 没有 → 重置: _unpinged = _unpinged + _pinged, _pinged 清空
   └── 有 → 选 last_ping_sent 最早的 (最久没被 ping 的)
```

### 4.3 已 ping 标记

每次实际发出 ping 后，`mark_connection_pinged(conn)` 把连接从 `_unpinged` 移到 `_pinged`。以此保证每轮中每个连接都能被 ping 一次。

---

## 五、计时器调整

`_on_check_and_ping()` 每次执行：

```
result = controller->select_connection_to_ping(_last_ping_sent_ms)

if _cur_ping_interval != result.ping_interval:
    stop_timer()
    start_timer(new_interval)
    _cur_ping_interval = new_interval

if result.conn != nullptr:
    ping(result.conn)      ← commit 7/8 实现
    _last_ping_sent_ms = now
```

定时器间隔会动态切换：
- Channel 从 strong 变 weak → 48ms → 加速探测
- Channel 从 weak 变 strong → 480ms → 降低开销

---

## 六、定时器降级：480ms → 48ms 的触发条件

一旦定时器切到 480ms，只有以下两种情况会重新降级到 48ms：

### 触发条件 1：新连接加入

```
新 connection 创建 (prflx candidate 到达)
  → _add_connection → controller->add_connection
  → 新连接 num_pings_sent = 0 (< MIN_PINGS_AT_WEAK_PING_INTERVAL = 3)
  → _on_check_and_ping 中 select_connection_to_ping:
    → need_ping_more_at_weak = true
    → ping_interval = WEAK_PING_INTERVAL (48ms)
    → 定时器切换回 48ms
```

**目的**：给新连接 3 次快速探测（各间隔 48ms），快速判断它是否可用。发满 3 个 ping 后自动停用 WEAK 间隔。

### 触发条件 2：Channel 变 weak

```
selected connection 状态变化:
  → writable 变 false (对端不再回复 ping)
  或 receiving 变 false (超过 2.5s 没收到对端数据)
  → conn->weak() = true
  → _weak() = true (因为 selected connection 是 weak)
  → _on_check_and_ping 中 select_connection_to_ping:
    → _weak() = true
    → ping_interval = WEAK_PING_INTERVAL (48ms)
    → 定时器切换回 48ms
```

**目的**：当前选中的连接断开了，需要紧急搜索替代路径。48ms 快速轮询所有候选连接。

### 降级决策代码路径

```
_on_check_and_ping() 每次执行:

  select_connection_to_ping():
    │
    ├── 检查 need_ping_more_at_weak:
    │     遍历所有连接，有 num_pings_sent < 3 ?
    │       ├── 是 → ping_interval = 48ms  ← 触发条件 1
    │       └── 否 → 继续
    │
    ├── 检查 _weak():
    │     _selected_connection == nullptr || _selected_connection->weak() ?
    │       ├── 是 → ping_interval = 48ms  ← 触发条件 2
    │       └── 否 → ping_interval = 480ms
    │
    └── 如果间隔变化 → stop_timer + start_timer(新间隔)
```

### 总结

```
480ms → 48ms 降级场景:

  场景 1: 新连接         num_pings < 3         → 48ms 快速探测 3 次 → 自动回到 480ms
  场景 2: Channel weak   selected 断开或不健康  → 48ms 紧急搜索    → channel 恢复 strong 后回到 480ms
```

---

## 七、状态转换触发点

```
事件                               → 调用链
─────────────────────────────────────────────────────────
新 connection 创建                  → _add_connection
                                    → _sort_connections_and_update_state
                                    → _maybe_start_pinging (首次启动定时器)

ANSWER 到达 (remote ICE params)    → set_remote_ice_params
                                    → 补填密码 → _sort_connections_and_update_state

定时器到期                         → _on_check_and_ping
                                    → select_connection_to_ping
                                    → 可能调整定时器间隔
```

---

## 八、完整 Ping 周期流程图

```
                        ┌──────────────────────---┐
                        │  _maybe_start_pinging   │
                        │  首次:start_timer48(ms)  │
                        └──────────┬───────────---┘
                                   │
                                   ▼
                        ┌──────────────────────┐
                  ┌─────│  定时器到期 (48ms)     │
                  │     └──────────┬───────────┘
                  │                │
                  │     ┌──────────▼───────────┐
                  │     │ select_connection_to │
                  │     │       _ping()        │
                  │     └──────────┬───────────┘
                  │                │
                  │     ┌──────────▼──────────-─┐
                  │     │ 间隔变了? stop+start    │
                  │     │ conn != null? → ping  │
                  │     │ _last_ping_sent = now │
                  │     └──────────┬───────────-┘
                  │                │
                  └────────────────┘
                    (循环，间隔可能是 48/480ms)
```

---

## 九、常量速查

| 常量 | 值 | 含义 |
|------|-----|------|
| `WEAK_PING_INTERVAL` | 48ms | channel weak 时 channel 级别间隔 |
| `STRONG_PING_INTERVAL` | 480ms | channel strong 时 channel 级别间隔 |
| `STABLING_CONNECTION_PING_INTERVAL` | 900ms | 连接还在建立中 |
| `STABLE_CONNECTION_PING_INTERVAL` | 2500ms | 连接已稳定 |
| `MIN_PINGS_AT_WEAK_PING_INTERVAL` | 3 | 每个连接以 48ms 间隔最少 ping 次数 |
| `STUN_PACKET_SIZE` | 480bit | 单个 STUN 包大小 |
