# xrtc-server 技术深潜：重难点详解

> 面向面试的技术深读。按 6 大模块逐个讲清开发中的**真实难点、踩过的 bug、设计权衡**——所有 `commit` 号均可在本仓库 `git log` 查证。
> 配套：系统总览与全链路走读见 [`README.md`](README.md) 与 [`note/xrtc-server-architecture-summary.md`](note/xrtc-server-architecture-summary.md)。

## 模块导航

| # | 模块 | 关键难点 |
|---|------|---------|
| 1 | [信令协议 + 并发架构](#m1) | 半包累积 · CRC32 路由 · 不对称回程 · stream_name/uid 正交 · 两层错误分流 · IO 掩码 bug |
| 2 | [SDP + PeerConnection](#m2) | SDP 作为控制/数据层接口 · BUNDLE 聚合 · SSRC 透传 |
| 3 | [ICE + STUN + 连通性检查](#m3) | STUN 位交织 + MI 不对称 · **48ms 死循环** · 两层限速 + 5 级排序防抖 |
| 4 | [DTLS 握手](#m4) | OpenSSL↔ICE 阻抗匹配 · ClientHello 竞态 |
| 5 | [SRTP + RTP/RTCP 加解密](#m5) | 密钥角色映射 · 方向隔离 · 单端口解复用 |
| 6 | [RTP 转发 + 状态聚合 + 异常](#m6) | **回调栈内析构 UAF** · 双向 RTCP |

---

<a id="m1"></a>
### 模块 1：信令协议 + 并发架构
> `src/server/`、`src/base/` · Phase 1-2

**解决的问题**：客户端经 TCP 信令通道发 PUSH/PULL/ANSWER 命令（36 字节 `xhead` 二进制头 + JSON body），经线程流水线 SignalingServer → SignalingWorker → RtcServer → RtcWorker 路由到目标线程执行 WebRTC 流操作，再把 SDP offer / 错误沿原路写回。

**难点 1 — TCP 半包累积读取：一个 sds 缓冲怎么拼出完整请求**（`_read_query` / `_process_query_buffer`）
TCP 是字节流，一次 `read` 不保证读到完整请求。`TcpConnection` 用一个 `sds`（Redis 动态字符串）`querybuf` 做累积缓冲，`_read_query` 每次把新数据**追加**到已有数据末尾：
```cpp
int read_len = c->bytes_expected;                     // 本阶段还差多少字节（头=36 或 body_len）
int qb_len   = sdslen(c->querybuf);                   // 已攒长度 = 这次追加写入的偏移
c->querybuf  = sdsMakeRoomFor(c->querybuf, read_len); // 只扩 free 空间、不动已有数据（可能 realloc，故回写）
sock_read_data(fd, c->querybuf + qb_len, read_len);   // 绕过 sds API，read() 直接落进内部 buffer
sdsIncrLen(c->querybuf, nread);                        // 绕过 API 写了裸内存，手动把 sds 的 len += nread
```
`qb_len` 是**写入偏移**（新数据接旧数据后面、不覆盖），绕过 sds API 直写省一次拷贝、代价是必须 `sdsIncrLen` 手动同步 `len`。配套的 `_process_query_buffer` 用 `while (sdslen >= bytes_expected + bytes_processed)` + STATE_HEAD/BODY 两态：先攒够 36 字节头（解析出 `body_len`、切 BODY），再攒够 body。`bytes_processed` **一值两用**——BODY 阶段 =36 作偏移基准（`head=querybuf`、`body=querybuf+36` 始终在原位，缓冲不前移无 memmove），处理完置 65536 让循环永假、封死连接（短连接：一连接一请求）。**隐含依赖 LT 水平触发**：`read_len` 被 `bytes_expected` 卡死、一次只 read 一次，内核缓冲剩余数据靠 libev 再次报告可读来取；换 ET 不循环 read 就丢包。

**难点 2 — 两级分工：Server 是连接分发器、Worker 才碰消息**
SignalingServer（1 线程）只 `accept` 新连接，`_dispatch_new_conn` 用 `_next_worker_index++`（环回）round-robin 把**连接 fd**（不是消息）塞进某 worker 的 `LockFreeQueue<int>` + pipe 唤醒；SignalingWorker（N 线程）`_new_conn` 接管 fd、注册读事件 + 超时 timer，此后这条连接的读/解析/写回全归它。经典 **acceptor + worker pool**：Server 干连接级负载均衡、不碰任何消息内容，Worker 干消息级 I/O。

**难点 3 — CRC32(stream_name) 路由保证媒体面零跨线程**
`RtcServer::_get_worker` 用 `ComputeCrc32(stream_name) % worker_num` 把同名流的 PUSH/PULL/ANSWER 全发往同一个 RtcWorker（1.5.6 / `48c6788`）。这样 push→pull 的转发（解密→重加密→发送）在同一线程内同步调用，无需跨线程队列或锁，媒体面延迟可控。若改用轮询分发，PUSH/PULL 落到不同 Worker，转发就得跨线程、多一次拷贝和同步。

**难点 4 — 按生产者模型分类选队列**
四条跨线程队列类型不统一，而是按生产者数量精确匹配：SignalingServer→SignalingWorker 单生产者用无锁 `LockFreeQueue<int>`（SPSC）；SignalingWorker→RtcServer 是 N 个 worker 多生产者、只能用 `std::queue + mutex`；回程 RtcServer→RtcWorker、RtcWorker→SignalingWorker 又回到 SPSC 无锁队列（1.5.4 / `668ef8b`）。所有队列都用 pipe write(1 int) 唤醒目标线程的 libev loop，pipe 只作唤醒信号、不传业务数据。

**难点 5 — 去程要路由、回程自带返回地址（不对称）**
去程 SignalingWorker →（`g_rtc_server->send_rtc_msg`）RtcServer →（CRC32 路由）RtcWorker，**三跳需路由**；回程 RtcWorker →（`msg->worker->send_rtc_msg`）SignalingWorker，**一跳直达、绕过 RtcServer**。能直达是因为构造 RtcMsg 时把返回地址塞进了"信封"：`msg->worker=this`（哪个 worker）、`msg->conn=c`（哪条连接）、`msg->fd`。去程按 stream_name 找 worker，回程凭信封上的寄件人原路直投，省掉反向路由、也不必再算一次 CRC32。每个 cmdno（含 STOP/ANSWER）都回一个结果。

**难点 6 — stream_name vs uid：资源的"地址"与"所有者"正交设计**

两个字段从 JSON 到路由到存储到转发，职责完全不相交。理解这一正交性是理解整个系统资源模型的关键。

**stream_name 的四重角色**：

| 层面 | 角色 | 代码位置 |
|------|------|---------|
| 路由 | CRC32 哈希选 RtcWorker | `rtc_server.cpp:_get_worker` — `ComputeCrc32(stream_name) % worker_num` |
| 存储 | `_push_streams` / `_pull_streams` 的 map key | `rtc_stream_manager.cpp` — `std::unordered_map<std::string, PushStream*>` |
| 关联 | push↔pull 转发查找 | `on_rtp_packet_received` → `_find_pull_stream(stream->get_stream_name())` |
| 去重 | 同名流只允许一个 PushStream + 一个 PullStream | create 时旧条目被覆盖或先删后建 |

**uid 的唯一角色：鉴权守卫**。只在修改/删除路径上出场，且遵循统一的"**先定位、后鉴权**"模式：

```cpp
// set_answer / _remove_push_stream / _remove_pull_stream 的统一模板：
auto it = _push_streams.find(stream_name);   // 1. 用 stream_name 定位资源
if (uid != stream->get_uid()) {              // 2. 用 uid 验证所有权
    return -1;                               // 3. 不匹配 → 拒绝
}
// 4. 通过 → 执行操作
```

uid 不参与路由、不参与查找、不参与转发。为什么？三个场景说明：

- **路由**：PUSH 和 PULL 可能来自不同 uid（推流 uid=100，拉流 uid=200）。如果 uid 参与路由，两个请求落到不同 Worker，转发就必须跨线程。
- **查找**：pull 端只知道 stream_name（"我要拉哪个流"），不知道推流端的 uid。用 uid 做 key 的话 pull 端根本无法找到 push 流。
- **转发**：媒体面每包都查 uid 是额外开销，且媒体包本身不带 uid 字段——UDP 上的 RTP 包只有 SSRC。

**面试一句话**：`stream_name` 是资源的**地址**（定位、路由、关联），`uid` 是资源的**所有者**（鉴权）。一个保证正确性，一个保证安全性。两者正交——地址不需要知道所有者是谁，所有者也不影响地址的解析。

**边界 case**：同一个 uid 可以拥有多个 stream（不同 stream_name），这是合法的——一个用户可以同时推多路流。但一个 stream_name 同时只能有一个 PushStream + 一个 PullStream。另外，同一个 stream_name 的 push 和 pull 可以是不同 uid（如 uid=100 推、uid=200 拉）——这正是 SFU 一对多转发的模型基础。

**难点 7 — 回程的"信封拆验"：不只是投递，还要防连接被替换**

难点 5 讲了回程一跳直达的基本思路。这里深挖的是回程抵达 SignalingWorker 后，**不是直接信任 `msg->conn` 就用**，而是做三步校验：

```cpp
// signaling_worker.cpp _response_server_offer 的回程校验逻辑：
if (!msg->conn) return;                                    // ① 空指针
if (msg->fd < 0 || msg->fd >= MAX_TCP_CONN) return;        // ② fd 越界
if (_tcp_conns[msg->fd] != (TcpConnection*)msg->conn) return; // ③ 指针一致性
```

**为什么需要第 ③ 步？** 回程是异步的——RtcMsg 在 RtcWorker 队列里排队、ICE gathering、DTLS 握手可能需要几百毫秒。这段时间内原 TCP 连接可能已因超时被关闭、fd 被 `accept` 复用给新连接、`_tcp_conns[fd]` 指向了新的 `TcpConnection*`。此时 `msg->conn` 是悬空指针（指向已 free 的旧连接），直接使用会 crash。用 fd 做 index 查当前连接表再比对指针，三个字段（worker + conn + fd）构成了一条"防篡改回执"。

**协议错误 vs 业务错误的两层分流**：

这是回程设计中一个容易被忽略的细节。SignalingWorker 在两类场景下的行为不同：

| 错误类型 | 触发条件 | 行为 | 原因 |
|---------|---------|------|------|
| 协议错误 | magic_num 不对、JSON 解析失败、缺少必填字段 | **直接关连接，不发响应** | 客户端实现有 bug 或恶意请求，不值得回包 |
| 业务错误 | 流创建失败、uid 不匹配、stream 不存在 | **正常 JSON 响应，`errno: -1`** | 正常流程的一部分，客户端需要知道结果 |

协议错误发生在 `_process_request` 和 `_process_push/pull/answer` 的字段提取阶段——还没到 RtcWorker，SignalingWorker 自己就能判定，直接 `return -1` 导致 `_close_conn`。业务错误发生在 RtcWorker 处理阶段——走完整回程通道，带 `err_no` 字段：

```json
// 业务错误响应
{ "errno": -1, "err_msg": "process error", "offer": "" }

// 成功响应
{ "errno": 0, "err_msg": "success", "offer": "<SDP string>" }
```

这个分层的本质是：**协议错误是"你的请求我没法理解"，不需要回复；业务错误是"你的请求我理解了但做不到"，必须告诉你结果。**

**响应头的复用技巧**：响应包的 36 字节 xhead 直接从请求头 `memcpy` 复用（id、version、log_id、provider 原样拷贝），只改 `body_len`。log_id 原样返回是关键的日志串联手段——请求和响应用同一个 log_id，排查问题时 grep 一个 id 就能看到全链路。

**踩过的 bug**：
- **IO 事件掩码三连击，SDP offer 永远发不回去**（`d4575ae`）：(a) 回调用 `io->events` 而非 libev 的 `revents`，永远收到完整 READ|WRITE 掩码；(b) `conn_io_cb` 用逻辑与 `events && EventLoop::READ` 而非位与 `events & ...`；(c) 新 accept 的 fd 没设 `O_NONBLOCK`。结果 WRITE 事件触发时误调 `_read_query` 阻塞在空连接、响应发不出，直到客户端 10s 超时。典型的事件驱动位运算陷阱。
- **`sock_read_data` 不区分 EAGAIN**：`read()` 返回 -1 一律当致命错误 `_close_conn`；配合上面修复给 fd 设了 `O_NONBLOCK` 后，非阻塞下理论上可能 EAGAIN→-1→误关（"readable 后立即单次 read"命中概率极低，属隐患而非现网 bug）。

**设计权衡**：① 短连接一请求一 TCP、用 `bytes_processed=65536` 封死，消除半包/连接复用复杂度，代价是控制面高 QPS 会成瓶颈；② CRC32 取模路由保证同名流确定性落点，但不支持一致性哈希——增减 Worker 会重置全部映射，是"Worker 数为部署常量、运行时不伸缩"的有意取舍。

---

<a id="m2"></a>
### 模块 2：SDP + PeerConnection
> `src/pc/peer_connection.cpp`、`session_description.cpp` · Phase 3/7/12

**解决的问题**：手写 SDP 生成/解析引擎，在无 libwebrtc 依赖下完成 offer/answer 协商 + SSRC 透传，支撑 SFU 的信令面。

**难点 1 — SDP 是控制层与数据层的接口（承上启下）**
客户端应用信令（PUSH/PULL/ANSWER）在 RtcWorker 收敛，产物就是 SDP；但 SDP 不只是控制文本，它是**数据层的参数载体 + 触发器**。`create_offer → set_local_description` 当场建 ICE channel/DTLS 并启动 candidate gathering；`set_remote_sdp（ANSWER）→ set_remote_description` 注入 remote `ice-ufrag/pwd`（喂 ICE）、`fingerprint`（喂 DTLS）、`ssrc`（喂 RTP 转发）、`candidate`（喂 ICE）。所以控制层与数据层的边界就是这两个 `set_*_description`——之后 ICE/DTLS/SRTP 全被 SDP 字段驱动。关键：两者**时序交织**而非串行——offer 一生成服务端 ICE 就在 gathering，answer 还没回、客户端的 STUN Binding / DTLS ClientHello 可能已在 UDP 上到达（正是 DTLS 模块要缓存 ClientHello 的根因）。

**难点 2 — BUNDLE 下的单传输通道聚合**
create_offer 把 audio/video 两个 m= section 加入同一 BUNDLE group（1.5.11/1.5.26），TransportController 只对 group 首个 mid 建一套 IceTransportChannel + DtlsTransport，其余 mid 复用。这要求解析 answer（1.5.28-1.5.30）时也按 BUNDLE 绑定同一 TransportDescription——ICE ufrag/pwd、DTLS fingerprint 全 PC 只有一份，但 answer 每个 m= 都重复写了这些行，解析时必须去重合并，不能每个 m= 建独立通道。

**难点 3 — SSRC 透传：Push 的接收身份 → Pull 的发送身份**
PushStream 的 offer 是 recvonly、不含 SSRC；客户端 answer 到达后从中提取 SSRC/cname/track/stream_id 聚合成 StreamParams。PullStream 创建时（1.5.89-1.5.91）必须把这些从 push 端拿到的 SSRC 原样写进自己的 sendonly offer。隐含约束：SFU 不转码、只做 SRTP 解密重加密，SSRC 是 RTP 包头核心标识，一旦改写，拉流端就无法把收到的 RTP 与 SDP 声明的流对应、解码直接失败。

**踩过的 bug**（手写 SDP 拼接的格式红线，都缺格式化单元测试兜底）：
- SDP 首行 `o=-` 误写成 `0=-`（数字 0 而非字母 o）——客户端 SDP 解析器拒绝。
- `a=candidate` 行漏掉 `typ` 关键字（1.5.32 / `e790041`），不符 RFC 5245，客户端丢弃全部候选地址、ICE 无法建连。
- SSRC 分组写成 `a=ssrc_group:FID` 而非 `a=ssrc-group:FID`（1.5.91 修复），漏连字符，拉流端识别不了分组语义。

**设计权衡**：手写逐行 `starts_with` + 冒号分割解析（约 150 行），放弃通用 SDP 解析能力，但 answer 格式由自己的 offer 决定、可控，不需要通用解析器——在健壮性与工程复杂度间取实用平衡。

---

<a id="m3"></a>
### 模块 3：ICE + STUN + 连通性检查
> `src/ice/` · Phase 4/8/9

**解决的问题**：信令交换完 SDP（各自 ICE ufrag/pwd 和候选地址）后，通过 STUN Binding Request/Response 双向探测候选地址对的实际可达性，从多条备选路径中选出一条最优 RTP/RTCP 传输通道，并持续监控其健康状态。

**难点 1 — STUN 消息编解码：二进制协议的正确性**（1.5.36~1.5.47）

20 字节定长头 + TLV 属性，看似简单但处处是陷阱。核心难点两个：Message Type 的位布局（历史债导致的交织排列）、MESSAGE-INTEGRITY 的验证与构造不对称。

**1a. Message Type 位布局：为什么 class 拆成两个 bit 分散放？**

Message Type 是 16 位字段，承载 12 位 method + 2 位 class。直觉会想"高 2 位 class + 低 14 位 method"，但实际是交织的：

```
bits: |15 14 13 12 11 10 9 8|7 6 5 4|3 2 1 0|
      |M11 M10 M9 M8 M7 C1 M6|M5 M4 C0|M3 M2 M1 M0|
```

class 两位分别落在 bit 4（C0）和 bit 8（C1），对应掩码：

```cpp
k_stun_class_mask  = 0x0110  // bits 4 + 8
k_stun_method_mask = 0x3EEF  // 除 bit 4 和 bit 8 外的所有位
```

三个关键值：
```
Binding Request:  0x0001  (class=00 → C1=0,C0=0, method=0b000000000001)
Success Response: 0x0101  (class=10 → C1=1,C0=0, method 同)
Error Response:   0x0111  (class=11 → C1=1,C0=1, method 同)
```

根因是 RFC 3489 时代没有 class 概念，bit 4 和 bit 8 分别有独立含义。RFC 5389 为了向后兼容，没有重新定义连续的高位，而是保留这两个 bit 位置拼成 class。

**踩过的 bug — Response type 写成 0x1001**：凭记忆把 bit 12（0x1000）当成 "success class"，写了 `0x1001`。Wireshark 显示 "Unknown Request" 才发现——0x1001 的 class 两位（bit4=0, bit8=0）其实是 Request。正确的 Success Response 是 `0x0101`（bit8=1，bit4=0）。（1.5.48 / `d308728`）
**关键教训**：二进制协议不要凭记忆写常量，必须从掩码反推或直接引用 RFC 表格。面试时可以主动提这个 case 来展示你对协议兼容性设计的敏感度。

**1b. MESSAGE-INTEGRITY 验证与构造的不对称**

MI 是 HMAC-SHA1，用 ice password 做 key，对消息内容做认证。但 HMAC 的"消息范围"是从头到 MI 属性末尾——而 MI 后面还有一个 FINGERPRINT（CRC32）属性。

**构造时**（`_add_message_integrity_of_type`）：
```
流程：属性列表 [USERNAME, PRIORITY, ..., MI(占位), FINGERPRINT(还没加)]
                                        ↑
                                   MI 是当前最后一个属性
                                   → Message Length 天然指向 MI 末尾
                                   → 序列化 → 直接算 HMAC → 填回占位 → 再加 FINGERPRINT
```
MI 是倒数第二个被添加的属性（仅早于 FINGERPRINT），所以在加 MI 的那一刻 Message Length 天然指向 MI 末尾。HMAC 范围正确，不需要任何调整。

**验证时**（`_validate_message_integrity_of_type`）：
```
收到的包：[USERNAME, PRIORITY, ..., MI(20B), FINGERPRINT(8B)]
                                       ↑              ↑
                                  HMAC 应该算到这里   但不应该包含这个
                                  → 必须把 Message Length 临时修正为"指向 MI 末尾"
                                  → 算完 HMAC 再还原
                                  → 如果直接用原包算，FINGERPRINT 被纳入 → HMAC 不匹配
```

```cpp
// 验证时的关键代码逻辑：
uint16_t mi_end = mi_pos + k_stun_message_integrity_size; // MI 属性末尾偏移
uint16_t orig_len = (data[2] << 8) | data[3];              // 保存原始 length
data[2] = (mi_end - k_stun_header_size) >> 8;              // 修正 length = 不含 FINGERPRINT
data[3] = (mi_end - k_stun_header_size) & 0xFF;
// ... 计算 HMAC-SHA1 ...
data[2] = (orig_len >> 8) & 0xFF;                          // 还原 length
data[3] = orig_len & 0xFF;
```

本质原因：**构造时 MI 是末属性，验证时 MI 后还有 FINGERPRINT**。两次操作中 MI 在属性列表里的位置不同，导致 Message Length 需要不同的处理。这就是"构造和验证不能复用同一段逻辑"的根本原因。

**面试时怎么讲**：先描述 MI 和 FINGERPRINT 的位置关系，再说"验证时必须把 Message Length 临时改小，因为 FINGERPRINT 不能纳入 HMAC 范围"，最后点出"构造时没这个问题是因为 MI 当时还是最后一个属性"。三个层次递进，展示你对协议细节的完整掌控。

**难点 2 — 48ms 死循环：五跳推理链定位 ping 周期 bug**（1.5.67 / `d3e665a`）
现象：ping 间隔始终卡在 WEAK 档 48ms，无任何连接被选中 ping，日志无限循环。推理链：`_is_pingable` 缺 `int64_t now` 参数 → strong channel 下它始终返回 false → round-robin 遍历时所有连接被 `continue` 跳过 → 无连接发满 3 次 ping → `need_ping_more_at_weak` 恒为 true → `select_connection_to_ping` 每次返回 48ms → 定时器永不升档。修复：`_is_pingable` 增加 `_is_connection_past_ping_interval(conn, now)` 分支，并修正 `_get_connection_ping_interval` 里 stable 条件反置。根因不在 crash 也不在显式逻辑错，而是一个缺失参数引发的条件链共振——全项目调试含金量最高的 case。

**难点 3 — 两层限速 + 连接排序与切换防抖**（1.5.53~1.5.63）
Channel 级速率门（48ms weak / 480ms strong）控制全局发包节奏，Connection 级间隔门保护单连接不被过度 ping，两层都通过才真正发包。连接 5 级排序：writable > write_state > receiving > priority > RTT。切换 selected connection 设 `k_min_improvement = 10ms` RTT 防抖，防两连接 RTT 接近时反复切换（ping-pong 振荡）。注意方向不对称：writable（我方→对端）只看 selected connection，receiving（对端→我方）看任意连接，销毁非 selected 连接后仍需重算 receiving。

**踩过的 bug**：
- **STUN Binding Response type 值错误**（1.5.48 / `d308728`）：凭记忆写 0x1001（class=00 即 Request），Wireshark 显示 "Unknown Request" 才定位，正确应为 0x0101（class=10 即 Success Response）。
- **48ms 死循环**（1.5.67 / `d3e665a`）：见难点 2，`_is_pingable` 缺 `now` 参数的五跳条件链共振。
- **IceController 僵尸成员**（1.5.113 / `4270272`）：Controller 多声明了一个永不更新（恒为 0）的 `_last_ping_sent_ms`，Channel 级速率门形同虚设——本应由 `IceTransportChannel` 传入的参数变成了 Controller 内永不变化的假成员。

**易混点**：MESSAGE-INTEGRITY 的验证与构造对 Message Length 处理不同（验证时 MI 后还有 FINGERPRINT 需手动截断，构造时 MI 是末属性长度天然正确）；RTT 指数平滑 `_rtt = (_rtt*3 + measurement)/4`，首次测量直接赋值不参与平滑以避免 outlier。

---

<a id="m4"></a>
### 模块 4：DTLS 握手
> `src/pc/dtls_transport.cpp` · Phase 10（commit 1.5.70–1.5.80）

**解决的问题**：在已连通的 ICE 通道上用 OpenSSL 完成 DTLS 握手、为 SRTP 派生密钥。服务端为 DTLS server role，证书自签名、靠 SDP fingerprint 认证。

**难点 1 — OpenSSL（同步流式）↔ ICE（异步包式）的阻抗匹配**
OpenSSL 的 BIO 是同步 read/write 流模型，ICE 收发是异步 UDP 包。自研 `StreamInterfaceChannel` 适配器桥接两者：上行 ICE 收包 → `on_received_packet()` 写入 `BufferQueue` → `SignalEvent(SE_READ)` 唤醒 OpenSSL 经 BIO 回调链消费；下行 `SSL_write` → BIO → `Write()` → ICE `send_packet()`。`BufferQueue` 容量限死 2（`k_max_pending_packets`），防 OpenSSL 消费不及时缓冲无限涨。（1.5.72/76/77）

**难点 2 — Trickle ICE 下的三方竞态**
DTLS 启动需三个条件（本地证书、远端指纹、ICE writable），但客户端**不等 ICE 连通就发 ClientHello**。两条时序路径：正常路径 answer 先到、指纹就绪一把配好；乱序路径 ClientHello 先到 → 缓存 `_catched_client_hello` + `_setup_dtls`（指纹空、跳过 `SetPeerCertificateDigest`）→ answer 到再补调、`_dtls` 不重建。若丢弃早到的 ClientHello，会触发 DTLS 秒级重传、握手延迟飙升。（1.5.71/74/75）

**难点 3 — ICE writable 边沿事件丢失的补偿**
DTLS 先备好而 ICE 未 writable 时 `_maybe_start_dtls` 跳过，此后无人再触发 → DTLS 永不启动。订阅 ICE `signal_writable_state_change`，writable 时补触发（仅 k_new）。"边沿触发丢失 → 用状态变化补偿"是异步编程的典型坑。（1.5.76）

**踩过的 bug**：`_is_fingerprint_change` 误用 `_remote_fingerprint_value` 而非算法字段 `_remote_fingerprint_alg` 判空，指纹变更检测逻辑错误；新增 `_on_receiving_state` 时漏加 `ice_transport_channel::receiving()` 访问器（fix 1.5.80 后）。

**易混点**：两个都叫 `SignalEvent` 但方向相反——我们发 `SE_READ` 是"叫 OpenSSL 来读 BufferQueue"，OpenSSL 发 `SE_OPEN/READ/CLOSE` 是"告诉我们握手完成/明文就绪/关闭"。谁点火、谁接收是区分关键。

---

<a id="m5"></a>
### 模块 5：SRTP 密钥体系 + RTP/RTCP 加解密
> `src/pc/dtls_srtp_transport.cpp` · Phase 13

**解决的问题**：DTLS 握手完成后从主密钥导出 SRTP 密钥材料并填入 libsrtp，实现 RTP/RTCP 原地加解密；同时把 RTP/RTCP 从同一 UDP 端口解复用。

**难点 1 — 密钥导出与 client/server 角色映射**
DTLS 握手完成后用 `SSL_export_keying_material`（RFC 5705）以标签 `EXTRACTOR-dtls_srtp` 导出原始密钥材料，格式为 `[client_write_key | server_write_key | client_write_salt | server_write_salt]` 连续缓冲。服务端作为 DTLS server：send_key = server_write_key + server_write_salt（加密发出），recv_key = client_write_key + client_write_salt（解密收到）。密钥长度由协商的 crypto suite 查表得到。（1.5.93）

**难点 2 — SrtpSession 方向隔离与原地加解密**
`SrtpSession` 封装 libsrtp 的 `srtp_ctx_t`，用 `srtp_policy_t.ssrc.type` 强制方向隔离：`ssrc_any_outbound` 只允许 `srtp_protect`（加密），`ssrc_any_inbound` 只允许 `srtp_unprotect`（解密），一个 session 不能混用。加密前需按 `auth_tag_len` 扩容 buffer 再 `protect_rtp`，解密后 `out_len` 减掉 auth tag 由调用方 `SetSize` 截短；SRTCP 比 SRTP 多 4 字节 SRTCP index。libsrtp 用全局引用计数管理，首个 session 构造时 `srtp_init()`、末个析构时 `srtp_shutdown()`。（1.5.97/1.5.102/1.5.106/1.5.110）

**难点 3 — RTP/RTCP 单端口解复用（RFC 5761）**
`infer_rtp_packet_type` 校验 version bits（byte0>>6==2）后，取 byte1 低 7 位（`& 0x7F`）按区间判定，落在 [64,96) 判为 RTCP、其余为 RTP。而 `get_rtcp_type` 读**完整 8 bit**（SR=200/RR=201/SDES=202/BYE=203/APP=204），不做掩码。DTLS 握手前的包由 DtlsTransport 直接 pass-through，握手后 `DtlsSrtpTransport` 订阅 `signal_read_packet` 接管所有收包。（1.5.100/1.5.101）

**踩过的 bug**：
- `get_rtcp_type` 误把解复用用的 `& 0x7F` 掩码搬来取真实 RTCP 类型，SR(200) 被抹成 72——RTCP PT 是完整 8 bit 不应掩码（1.5.103 纠正）。
- `_setup_dtls_srtp` 里两个 if 未串联，`_extract_params` 失败后仍执行 `set_rtp_params` 传入未初始化 key buffer，应短路 return（1.5.99 修复）。

**易混点 / 设计权衡**：`DtlsSrtpTransport` 不继承而是持有 `DtlsTransport` 指针并订阅其信号（组合优于继承，保持可独立测试）；`_maybe_setup_dtls_srtp` 被 `_on_dtls_state`（握手完成信号）和 `set_dtls_transport`（兜底）双触发，内部用 `is_srtp_active() || !is_dtls_writable()` 做幂等——"信号驱动 + 手动兜底"解决时序竞争。

---

<a id="m6"></a>
### 模块 6：RTP 转发 + 状态聚合 + 异常处理
> `src/stream/rtc_stream_manager.cpp`、`src/pc/` · Phase 11/14

**解决的问题**：把 ICE 层、DTLS 层、PC 层三态信号逐级聚合为统一 PeerConnectionState，`k_failed` 时触发资源清理；同时打通转发路径——push 收到的 SRTP 包解密后用 pull 的密钥重加密发出，RTCP 在 push/pull 间双向中继。

**难点 1 — 回调栈内自我析构导致 use-after-free**（1.5.85）
ICE 探活定时器回调 `_on_check_and_ping()` 在同一栈帧内检测到连接超时 → `update_state(TIMEOUT)` → 信号链层层上报到 `on_connection_state(k_failed)` → `delete push_stream` → 析构 `IceTransportChannel` → `_ice_controller.reset()`。但回调返回后调用方仍要 `_ice_controller->select_connection_to_ping()` 选下一个 ping 候选，此时裸指针已被置 0，触发 nullptr dereference。修复：`PeerConnection::destroy()` 用 10ms 一次性 timer 延迟 `delete pc`，确保 ICE timer 所在的事件循环迭代完整退出后才析构；同时把 `~PeerConnection()` 设为 private，从编译期拦截所有直接 `delete`。

**难点 2 — RTCP 双向转发：接口缺失 + 加密函数误用**（1.5.108/1.5.111）
联调发现两个叠加问题：一是 `send_rtcp` 错误调用了 `protect_rtp` 而非 `protect_rtcp`，导致 SRTCP（比 SRTP 多 4 字节 index）格式不匹配、客户端校验失败；二是 RTCP 只实现了 push→pull 单向，漏掉 pull→push，导致拉流端发的 PLI/FIR（请求 I 帧）到不了推流端、拉流画面一直黑。修复后在 `on_rtcp_packet_received` 按 `k_push`/`k_pull` 分支双向中继。

**踩过的 bug**：
- 延迟析构 timer 回调名 typo：`destroy_timer_cb` 误写 `destroy_time_cb`，链接期 undefined symbol（1.5.85）。
- 异常路径未收敛：ICE 30s 超时回调 `_ice_timeout_timer_cb` 里直接 `delete stream`，与 `k_failed` 信号链的 cleanup 形成两套逻辑，改为统一入口 `on_stream_exception()`（1.5.111）。

**设计权衡**：延迟析构（10ms timer + private 析构 + friend timer callback）vs 全面改 shared_ptr/weak_ptr——前者以 10ms 延迟为代价从根源消除栈内自我析构且编译期强制，后者侵入式要求所有持有 `this` 的上下文改造。这是事件驱动 C++ 生命周期管理的经典范式。
