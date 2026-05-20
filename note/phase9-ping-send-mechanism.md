# ICE Ping 发送机制与设计思路

## 一、完整调用链

```
libev 定时器 (周期 = _cur_ping_interval)
  │
  └─ ice_ping_cb()
       │
       └─ IceTransportChannel::_on_check_and_ping()
            │
            ├─ [1] IceController::select_connection_to_ping(_last_ping_sent_ms)
            │     │
            │     ├─ 计算 ping_interval (channel 级分辨率)
            │     ├─ Channel 速率门: now >= _last_ping_sent_ms + ping_interval ?
            │     └─ _find_next_pingable_connection()
            │           │
            │           ├─ ① selected connection 优先 (writable + 已过间隔)
            │           └─ ② round-robin: unpinged → _more_pingable 选最 overdue
            │
            ├─ [2] 如果选中连接 → _ping_connection(conn)
            │     │
            │     ├─ _last_ping_sent_ms = now  (更新 channel 全局速率门)
            │     └─ conn->ping(now)
            │           │
            │           ├─ new ConnectionRequest(this)
            │           └─ _pings_since_last_responses.push_back(SentPing(id, now))
            │
            └─ [3] 如果 interval 变化 → 重启定时器
```

## 二、各层职责

| 层 | 类 | 职责 |
|----|-----|------|
| Channel | `IceTransportChannel` | 定时器管理、channel 全局速率门、驱动 ping 周期 |
| Controller | `IceController` | 选择"该 ping 谁"、两层限速、round-robin 公平调度 |
| Connection | `IceConnection` | 创建请求、记录 ping 历史(等响应)、状态机 |
| Request | `StunRequest/ConnectionRequest` | 构造 STUN 消息、填充属性 |

### 为什么分这么多层？

**Channel 和 Controller 分离**：Channel 管定时器周期（分辨率），Controller 管连接选择策略。两者可以独立变化——比如换一种选择算法（加权/优先级），只改 Controller。

**Connection 和 Request 分离**：一次 ping 对应一个 Request 对象。Connection 记录"发过哪些 ping"，Request 负责"这次 ping 的消息长什么样"。后续收到响应时，通过 Request 的 id 匹配，计算 RTT。

**StunRequest 基类抽象**：ICE 有多种 STUN 消息类型（Binding Request / Binding Indication），共同流程是 construct() → prepare()，各自填充不同属性。模板方法模式让子类只需关心"填什么字段"。

## 三、两层限速设计

```
Channel 层                        Connection 层
─────────────────────────────────────────────────
_last_ping_sent_ms                conn->last_ping_sent()
     │                                    │
     │← ping_interval →│                  │← conn_interval →│
     │  WEAK:   48ms   │                  │  WEAK:     48ms  │
     │  STRONG: 480ms  │                  │  STABLING: 900ms │
     │                 │                  │  STABLE:  2500ms │
     │                 │                  │                  │
     ▼                 ▼                  ▼                  ▼
 [能发 ping 吗?]                  [这个 conn 能发 ping 吗?]
```

- **Channel 门**：控制"整体发包节奏"，一次最多发一个 ping
- **Connection 门**：控制"单个连接不被过度 ping"

一个 ping 要发出，必须**两层都通过**。

## 四、Round-Robin 公平选择

```
unpinged (本轮未 ping)            pinged (本轮已 ping)
┌───┬───┬───┐                     ┌───┬───┐
│ A │ B │ C │                     │   │   │
└───┴───┴───┘                     └───┴───┘

选择 C (最 overdue) → C 移到 pinged

┌───┬───┐                         ┌───┐
│ A │ B │                         │ C │
└───┴───┘                         └───┘

... 继续选择和移动 ...

┌───┐                             ┌───┬───┐
│   │                             │ A │ B │ (C 也在, 省略)
└───┘                             └───┴───┘

unpinged 无可 ping 连接 → pinged 全倒回 unpinged → 新一轮开始

┌───┬───┬───┐                     ┌───┐
│ A │ B │ C │                     │   │
└───┴───┴───┘                     └───┘
```

选择标准：`_more_pingable()` — `last_ping_sent()` 更小（更久没 ping）的优先。保证不会饿死任何一个连接。

## 五、StunRequest 模板方法模式

```
StunRequest (基类)
  ├─ construct()           ← 对外接口，固定的发送流程
  │   └─ prepare(_msg)     ← 虚函数，子类重写，填具体属性
  │
  └─ _msg (StunMessage*)   ← 持有消息对象，子类构造函数中 new 传入

ConnectionRequest (子类)
  ├─ 构造函数: StunRequest(new StunMessage())
  └─ prepare(): 填充 Binding Request 属性 (USERNAME, PRIORITY, MI, etc.)
```

后续如果加 Binding Indication：

```
BindingIndicationRequest (子类)
  └─ prepare(): 填充 Indication 属性 (不需要 USERNAME, 不需要 MI 验证)
```

基类不需要知道消息内容，只管发送框架。符合开闭原则。

## 六、关键数据结构

### SentPing — 记录已发 ping

```cpp
struct SentPing {
    std::string id;       // transaction_id，匹配响应
    int64_t sent_time;    // 发送时间，计算 RTT
};
```

存放在 `_pings_since_last_responses` 中。收到响应时根据 id 找到对应的 SentPing，算出 RTT，然后移除。

### PingResult — Controller 返回结果

```cpp
struct PingResult {
    const IceConnection* conn;  // 选中的连接 (nullptr = 本轮跳过)
    int ping_interval;           // 返回给 Channel 的分辨率
};
```

`select_connection_to_ping` 的结果：告诉 Channel "该 ping 谁" 和 "下个周期用多快"。

## 七、定时器周期切换

| 触发条件 | 方向 | 原因 |
|---------|------|------|
| channel 变 weak | 480→48ms | 当前连接断开，加速找新连接 |
| 新连接 ping < 3 次 | 480→48ms | 新连接快速初探 |
| channel 恢复 strong + 所有连接 ping ≥ 3 次 | 48→480ms | 稳定了，节省带宽 |
