# Phase 9 核心概念：连通性检查

## 架构：一个 Channel 管理多个 Connection

```
IceTransportChannel (RTP)
├── IceController          ← 选最优连接
├── IceConnection (地址对 A: 192.168.1.1:45678 ↔ 10.0.0.2:12345)
├── IceConnection (地址对 B: 192.168.1.1:45678 ↔ 10.0.0.3:12346)
└── IceConnection (地址对 C: ...)
```

一个本地 UDP 端口可能对应多个对端地址。每个对端地址就是一个 IceConnection。但真正用来发数据的只有一个最优的（selected connection）。Controller 负责"选哪一个"。

---

## 一、IceConnection 的读和写

IceConnection 代表**一个 candidate pair**（本地地址 + 对端地址）。读和写是独立判断的。

### 1. 写状态（WriteState）— 4 个状态

**衡量标准：对端有没有回复我们的 ping**

```
STATE_WRITE_INIT (初始)
  连接刚创建，还没发过 ping，也没收到过任何 ping 回复。

  ↓ 发送 ping，收到回复

STATE_WRITABLE (可写)
  最近的 ping 都能收到回复。我们认为这个连接是"健康的"，可以安全地发送数据。

  ↓ 开始丢回复（连续 5 次 ping 失败，且超过 2 倍 RTT）

STATE_WRITE_UNRELIABLE (不可靠)
  ping 偶尔失败，网络抖动或丢包。连接还能用但不稳定。

  ↓ 超过 15 秒没收到任何 ping 回复

STATE_WRITE_TIMEOUT (超时/失败)
  连接彻底不可用，认为已死。
```

**本质**：写的健康度取决于"我们发的探测包，对端回了没有"。

### 2. 读状态（Receiving）— 只有 true/false

**衡量标准：最近有没有收到对端发来的东西**

只要有数据在近 2.5 秒内到达，就是 receiving = true。三种数据都算：

- 对端发的 ping 请求
- 对端回给我们的 ping 回复
- 对端发的实际数据包（DTLS / RTP）

超过 2.5 秒没收到任何东西 → receiving = false。读状态不是"收到过就永久 true"，它有时间窗口。

**本质**：读的健康度取决于"对端有没有在跟我们说话"。

### 3. 读 vs 写的区别

| 维度 | 含义 | 判断依据 | 状态层级 |
|------|------|----------|----------|
| WriteState | 我们 → 对端是否畅通 | ping 回复率 | 4 级渐变（INIT / WRITABLE / UNRELIABLE / TIMEOUT） |
| Receiving | 对端 → 我们是否畅通 | 数据到达时间 | 2 级开关（最近 2.5s 有/无） |

---

## 二、Connection 的 strong / weak

```
strong: writable = true  AND  receiving = true   ← 双向都通
weak:   writable = false OR   receiving = false   ← 至少一个方向有问题
```

weak 不是一种独立状态，是 writable + receiving 组合出来的**派生判断**。

---

## 三、Channel 的 weak

```
_selected_connection == nullptr       // 还没有选出最优连接
      ||
_selected_connection->weak()          // 选出来的连接不满足 writable && receiving
```

即：要么没有 selected connection，要么有但它不通畅。

---

## 四、Connection failed 的定义

在这个文档语境下，"failed" = `_write_state == STATE_WRITE_TIMEOUT`，也就是 `!active()`。

回头看四个写状态与 active 的关系：

```
INIT       → active=true  (还活着)
WRITABLE   → active=true  (很健康)
UNRELIABLE → active=true  (不稳定但还活着)
TIMEOUT    → active=false (死了，即 failed)
```

---

## 五、连接何时可 Ping

三个条件：

1. **必须知道对端的 ice_ufrag 和 ice_pwd**（否则没法构造 STUN 请求）
2. **Channel 为 weak 状态** — 需要积极探测来找更好的连接
3. **Connection 不是 failed 状态**（即 active=true，不是 TIMEOUT）

第三条的含义：Channel weak 时，所有还活着（active）的连接（INIT / WRITABLE / UNRELIABLE）都应该被探测，看谁能变成可用的。只有已经超时死掉的（TIMEOUT / failed）才跳过 — ping 一个死连接没有意义。

---

## 六、IceTransportChannel 的状态 — 6 个状态

IceTransportChannel 管理**一组 connection**（一个 component 的所有 candidate pair）。它的状态是对所有 connection **聚合**出来的结论。

```
k_new (新建)
  还没有任何 connection，或者虽然有过但都死了，且从未连通成功过。

  ↓ 有 connection 创建，开始探测

k_checking (探测中)
  有活跃的 connection，但还没有任何一个达到 writable。
  "正在拼命找能通的路"

  ↓ 至少有一个 connection 变成 writable

k_connected (已连通)
  有活跃的 connection 并且它是可写的。
  "找到了一条能走的路"

  ↓ 选出了 selected connection 并且稳定

k_completed (完成)
  连通性检查结束，selected connection 确定，可以正常传输数据。

  ↓ 曾经连通，但当前 writable 丢失

k_disconnected (断连)
  本来已经通了，现在又不可写了（网络抖动 / 对端挂了）。
  "路还在，暂时不通"

  ↓ 所有 connection 都 inactive

k_failed (失败)
  所有 connection 都死了，没有任何可用的候选地址。
  "没路了"
```

---

## 七、Connection 状态 vs Channel 状态的核心区别

| | IceConnection | IceTransportChannel |
|---|---|---|
| 粒度 | **一个**地址对 | **一组**地址对（一个 component） |
| 关心什么 | 这个地址对通不通 | 整个 component 能不能传数据 |
| 状态决定者 | 自己的 ping/pong 表现 | 所有 connection 的聚合 |
| 举例 | "192.168.1.1↔10.0.0.2 可写" | "RTP 通道已连通（因为有 1 个 writable connection）" |

**Channel 不关心具体哪个 connection 通，只关心"有没有一个能用的"。** 就像你有 3 条路去公司，只要有一条能走，你就说"能通"。

---

## 八、连通性检查的完整逻辑链

```
IceTransportChannel (RTP)
  │
  │  问题: "RTP 通道通了吗？"
  │
  ├── IceConnection (地址对 A)  写=WRITABLE  读=true  → strong
  ├── IceConnection (地址对 B)  写=INIT      读=false → weak
  └── IceConnection (地址对 C)  写=TIMEOUT   读=false → inactive

  ↓ 聚合:
  - 有 active connection (A, B)
  - 有 writable connection (A)
  → Channel 状态 = k_connected
```

---

## 九、Ping 在 ICE 中的作用

ping 是**生成判断依据的手段**。发 ping → 等回复 → 更新 WriteState。没有 ping，就不知道写状态是什么，也就无法判断连接好坏、channel 状态、该选哪个 connection。

ping 是 ICE 协议的"心跳"：它既是探测工具（这个地址对能不能通），也是状态判断的原材料（写状态怎么变、选哪个连接）。

---

## 十、概念速查表

| 概念 | 来源 | 含义 |
|------|------|------|
| writable | WriteState == WRITABLE | 我们→对端 ping 通畅 |
| receiving | _receiving | 近 2.5s 有收到对端数据 |
| strong (conn) | writable && receiving | 双向都通 |
| weak (conn) | !(writable && receiving) | 至少一个方向不通 |
| active | WriteState != TIMEOUT | 连接还活着 |
| failed | WriteState == TIMEOUT | 连接已死 |
| strong (channel) | selected!=null && !selected->weak() | 有选出来的且健康的连接 |
| weak (channel) | selected==null \|\| selected->weak() | 没有健康的最优连接 |
| pingable | 知凭据 + channel weak + conn active | 可以发送 STUN binding request |
| k_checking | 有 active conn 但无 writable | 探测中 |
| k_connected | 有 active conn 且 writable | 已连通 |
| k_failed | 所有 conn 都 inactive | 彻底失败 |
