# SFU 服务端面试总文档（知识库 + 口述稿）

> 所有数字已对照参考源码核对（`xrtcserver/src/`），修正已内嵌并标注出处行号，可放心使用。
> 阅读策略：平时扫 Part A（知识库），面试前背 Part B 的 B1 + B3 立场句。

---

# Part A 核心知识库

## A1 整体架构（1分钟）

WebRTC SFU，核心是信令 → ICE → DTLS → SRTP → RTP 转发五层链路。

线程每核模型：RtcServer 对 stream_name 做 CRC32 哈希，把同一房间的推流和拉流路由到同一个 RtcWorker 线程，单线程内处理，转发无锁。

```
TCP信令 → SignalingWorker → RtcServer（CRC32路由）→ RtcWorker → RtcStreamManager
                                                              ↓
                                                        PUSH流 / PULL流
```

## A2 证书体系（修正版）

**2 个证书，都在媒体面（DTLS）**：

| 证书 | 用途 | 认证方式 |
|------|------|---------|
| SFU DTLS 证书（自签） | DTLS 握手，证明 SFU 身份 | 指纹写在 offer 的 SDP 里 |
| 客户端 DTLS 证书（自签） | DTLS 握手，证明客户端身份 | 服务端用 ANSWER 里的指纹比对（`dtls_transport.cpp:315` `SetPeerCertificateDigest`） |

关键机制：**fingerprint 绑定**——双方自签证书的 SHA-256 指纹通过 SDP 传递，DTLS 握手时强制比对，匹配才建立连接，**不需要 CA**。这是 WebRTC 最核心的安全设计。

⚠️ 信令面：生产环境走 HTTPS/WSS 防 SDP 篡改；我的 demo 信令是自定义 TCP 协议（简化），面试要主动说明这一点，不要说"系统有 HTTPS 证书"。

## A3 媒体栈诞生——set_local_description（2分钟）

set_local_description 是媒体栈起点，做五件事：

1. 随机生成 ICE 凭据（ice-ufrag / ice-pwd）
2. 创建 ICE channel（逻辑通道）
3. 创建 DtlsTransport（绑定到 ICE channel）
4. 创建 DtlsSrtpTransport（订阅 DTLS 状态信号）
5. 触发 candidate 收集（socket() + bind() + O_NONBLOCK）

candidate 收集时每张网卡一个 UDPPort（一个 UDP socket）。BUNDLE + RTCP mux 开启时 audio/video 共享一个 ICE channel → **UDPPort 数 = 网卡数**。

一句话：set_local_description 不是在"填 SDP"，而是在"创建整个媒体传输栈"。

## A4 ICE 选路与连通性（3-4分钟）——重点

### 4.1 概念层级

- **UDPPort**：一个 UDP socket，绑定一个本地地址
- **IceConnection**：一个 UDP 四元组（客户端 IP:Port ↔ SFU IP:Port），一条候选路径
- **IceTransportChannel**：聚合一个 UDPPort 下的所有 IceConnection
- **IceController**：选出最优的 `_selected_connection`

### 4.2 冷启动时序

两个触发条件，谁后到谁启动定时器：

| 时序 | 说明 |
|------|------|
| STUN 先到，ANSWER 后到 | 先创建连接（密码空）→ ANSWER 补密码 → 启动定时器 |
| ANSWER 先到，STUN 后到 | 先存密码 → STUN 创建连接（密码已非空）→ 启动定时器 |

关键：`_is_pingable()` 要求 remote_candidate 同时具备 username（来自 STUN）和 password（来自 ANSWER）（`ice_controller.cpp:33`）。两个都到才可 ping。

### 4.3 状态分类

每个 IceConnection 跟踪三个独立状态：

| 状态 | 含义 | 更新方式 |
|------|------|---------|
| WriteState | 我→对端是否通畅 | ping 回复 → WRITABLE；超时降级 |
| _receiving | 对端→我是否存活 | 2.5s 内收到过数据（`WEAK_CONNECTION_RECEIVE_TIMEOUT=2500`） |
| IceCandidatePairState | RFC 5245 nomination | WAITING → IN_PROGRESS → SUCCEEDED |

关键：writable 和 receiving **独立判断**——UDP 链路可能单向通。

### 4.4 5 级排序 + RTT 防抖

```
writable > write_state > receiving > priority > RTT
```

RTT 切换有 10ms 防抖（`ice_controller.cpp:217` `k_min_improvement`）：新连接 RTT 比当前 selected 小 10ms 以上才切。

write_state 四级（修正："5 次"实为"5 秒"）：

| 状态 | 含义 |
|------|------|
| WRITABLE(0) | 收到 ping 回复，正常收发 |
| UNRELIABLE(1) | **5 秒**无响应降级（`CONNECTION_WRITE_CONNECT_TIMEOUT=5000`），仍在尝试，可发数据 |
| INIT(2) | 尚未 ping 通 |
| TIMEOUT(3) | 15s 无回复（`CONNECTION_WRITE_TIMEOUT=15000`），已死亡，不可发数据 |

排序中 `ready_to_send()` 允许 UNRELIABLE 参与选路——不稳定但还能发，比 TIMEOUT 好。

### 4.5 切换的三道防线（为什么不等 15 秒）

| 防线 | 时间 | 条件 | 说明 |
|------|------|------|------|
| 第一道 | 2.5s | _receiving 失活 | 有其他 writable+receiving 的连接，立刻切 |
| 第二道 | 5s | write_state 降级 UNRELIABLE | 有其他可发数据的连接，立刻切 |
| 第三道 | 15s | write_state TIMEOUT | 所有连接都死 → k_failed，释放资源 |

本质：15s 不是"等待切换"，是"确认所有连接已死亡的终局宣告"。切换在 2.5s 或 5s 已发生。

### 4.6 Ping 限速机制

Channel 级两档：weak 48ms 高频探测（`WEAK_PING_INTERVAL`）／ strong 480ms 保活（`STRONG_PING_INTERVAL`）。

Connection 级三档（仅 strong 生效）：新连接（<3 次 ping）48ms ／ 不稳定 900ms ／ 稳定（≥5 次 RTT 采样无丢包）2500ms（`STABLE_CONNECTION_PING_INTERVAL`）。

关键：weak 时跳过 Connection 级间隔，所有连接高频 ping，目的是快速找到可用路径。

## A5 DTLS 与 SRTP（2分钟）

### 5.1 DTLS 启动的三个条件（谁最后到谁触发）

| 条件 | 来源 |
|------|------|
| DTLS 对象已创建 | ClientHello 到达 → `_setup_dtls()` |
| 远端 fingerprint 已收到 | ANSWER SDP 到达 |
| ICE writable | STUN ping/pong 成功 |

唯一延迟场景：ClientHello 先于 STUN 到达——UDPPort 还没有连接，包被丢弃（`udp_port.cpp:146` 无连接且非 STUN 即丢），客户端 1s 后重传。这是防 DoS 的设计取舍：缓存未知 UDP 包容易成为攻击向量。

### 5.2 SRTP 密钥导出

DTLS 握手完成后 `export_keying_material`（`dtls_transport.cpp:456`，调用点 `dtls_srtp_transport.cpp:229`）：

- client_write_key + client_write_salt → SFU **解密**用（收）
- server_write_key + server_write_salt → SFU **加密**用（发）

命名反直觉：SFU 是 DTLS 服务端（SSL_SERVER），server_write_key = "服务端写数据的密钥" = SFU 发数据的加密密钥。

## A6 RTP/RTCP 处理（1分钟）

### 6.1 两级解复用

| 层级 | 分拣点 | 判断 |
|------|--------|------|
| 第一级 | DtlsTransport | DTLS vs 非 DTLS（RTP/RTCP 都放行） |
| 第二级 | DtlsSrtpTransport | RTP vs RTCP（`packet[1] & 0x7F` 落在 64-95 → RTCP，rtp_utils.cpp:17） |

机制说明：RTCP 原生 PT 是 192-223，`& 0x7F` 后落进 64-95；这是启发式，面试被问到要能解释"为什么是 64-95"。

### 6.2 转发策略（补充：逐对端加解密——SFU 必考点）

- **RTP**：单向转发，推流端 → 拉流端
- **RTCP**：双向转发。拉流端的 PLI（关键帧请求）经 RTCP 回到推流端，触发 I 帧生成
- **逐对端加解密**：每个连接 SRTP 密钥独立，SFU 必须**解密后按接收者重加密**——推流端包不能直接转给拉流端。但只在 SRTP 层解密，codec 层不解码，所以 CPU 便宜

## A7 PULL 流与 SSRC 透传（1.5分钟）

| 维度 | PushStream | PullStream |
|------|-----------|-----------|
| SDP direction | recvonly | sendonly |
| offer 中 SSRC | 无 | push 端的原始 SSRC |
| 数据方向 | 从客户端接收 | 发送给客户端 |

为什么 PULL 必须透传 SSRC：SFU 不解码不转码不修改 RTP 包头，SSRC 在包头，SRTP 加密只覆盖 payload，原封不动到达拉流端。PullStream 的 offer 不声明这个 SSRC，拉流端收到未知 SSRC 的包不知道编码类型，只能丢弃。

流程：PushStream 的 remote SDP 提取 SSRC → 注入 PullStream 的 offer → 拉流端按 SDP 声明的 SSRC 解码。

## A8 STOP 与资源清理（1分钟）

三条清理路径，都收敛到 `_remove_push_stream` / `_remove_pull_stream`：

| 路径 | 触发 | 响应 |
|------|------|------|
| 主动停止 | 客户端发 STOP | 返回 JSON |
| ICE 失败 | 所有连接 TIMEOUT（`k_ice_timeout=30000`，rtc_stream.cpp:14） | 无响应 |
| 30s 超时 | PC 状态一直没到 k_connected | 无响应 |

**10ms 延迟析构**（`peer_connection.cpp:114`）：ICE 的 `_on_check_and_ping()` 在 timer 回调栈内执行，栈内同步 delete 会销毁 `_ice_controller`，函数返回后还要访问它 → 空指针崩溃。所以 `destroy()` 创建 10ms 定时器，等当前 event loop 迭代完成再析构。

## A9 协议层解复用（STUN/DTLS/SRTP 分拣）

为什么需要：**一个 UDP 端口同时承载三种协议**——STUN（ICE 连通性）、DTLS（握手）、SRTP（媒体），收发都必须按协议特征分流。

### 复用（通道层面，省端口/省候选路径）

- **RTCP mux**（RFC 5761）：RTP/RTCP 共用同一 UDP 端口
- **BUNDLE**（RFC 8843）：audio/video 共用一个传输通道 → UDPPort 数 = 网卡数

### 解复用（接收路径，三级分拣）

| 层级 | 分拣 | 依据 |
|------|------|------|
| UDPPort | STUN vs 其他 | magic cookie `0x2112A442` + FINGERPRINT 属性校验（stun.cpp:52） |
| DtlsTransport | DTLS vs 非 DTLS | 首字节 20-63（DTLS record content type，dtls_transport.cpp:15） |
| DtlsSrtpTransport | RTP vs RTCP | `packet[1] & 0x7F` ∈ [64,95] → RTCP（rtp_utils.cpp:17） |

首字节速查：STUN 0x00/0x01、DTLS 20-63、RTP/RTCP ≥ 0x80（V=2）。首字节只能粗分，STUN 必须再验 magic cookie + fingerprint 防误判（SRTP 加密下首字节不可信的特征只有版本位）。

## A10 I/O 多路复用（libev/epoll 事件模型）

### 10.1 事件循环与后端

- EventLoop 封装 libev：`ev_loop_new(EVFLAG_AUTO)`（event_loop.cpp:16），Linux 上自动选 **epoll**，可回退 **select**
- **每线程一个 ev_loop**——thread-per-core 模型的基础；跨线程通知的 pipe fd 也是常驻读监听（signaling_worker.cpp:100）
- IOWatcher 封装 `ev_io`，start/stop 对应 `ev_io_start` / `ev_io_stop`（EV_READ / EV_WRITE mask）

### 10.2 LT 语义：读常驻 vs 写按需（核心）

libev 的 io watcher 是 **level-triggered（水平触发）**：

- **读可以常驻**：没有数据时 fd 不可读 → LT 不触发 → 常驻零开销；而数据随时可能到，必须长期监听（async_udp_socket.cpp:15 注释）
- **写不能常驻**：fd **几乎总是可写**（内核发送缓冲区没满）→ LT 下写回调会**每个事件循环迭代都触发** → 常驻写监听 = busy loop，CPU 空转
- 所以写采用"**乐观发送 + 按需监听，发完即卸**"：

```
UDP（async_udp_socket.cpp:129 _add_udp_packet）——严格的乐观发送：
  1. 先 flush 积压队列（队列有残留说明 buffer 满，直接入队）
  2. 队列空 → 直接 sock_send_to 直发，省去排队开销
  3. 直发失败（sent==0，缓冲区满）→ 入队 + start_io_event(WRITE)（:159）
  4. WRITE 回调 send_data() flush 队列 → 队列空 → stop_io_event(WRITE)（:114）
     ——"防止写事件一直触发"

TCP（signaling_worker.cpp）——有数据即挂，发完即卸：
  1. 有响应要发 → _add_reply() 入队 + start_io_event(WRITE)（:262）
  2. WRITE 回调 _write_query() 循环写；nwritten==0（buffer 满）→ 保持监听等下次
  3. reply_list 清空 → stop_io_event(WRITE)（:392）
```

### 10.3 读路径：LT 监听 + ET 风格 drain

监听是 LT，但 `recv_data()` 用 **while 循环读到 EAGAIN**（async_udp_socket.cpp:58）——一次 READ 事件把内核缓冲区读空，模拟 ET 语义，减少回调触发次数。注意 `_buf` 复用，上层回调必须同步复制数据，不能异步引用。

### 10.4 面试速答

- "epoll 用 LT 还是 ET？" → libev 默认 **LT**；读路径 while drain 模拟 ET
- "为什么写监听不常驻？" → LT 下写恒触发 → busy loop → 乐观发送，EAGAIN 才挂监听，发完即卸
- "为什么用 libev 而不是裸 epoll？" → 自动选后端、io/timer/pipe 统一抽象、每线程一 loop 契合 thread-per-core

## A11 自定义协议信令解析

为什么自定义而不是 HTTP/WS：**36 字节定长头 + JSON body**，解析是纯内存偏移运算，比 HTTP 文本解析快；二进制头可扩展字段。

### xhead_t（36 字节，xhead.h）

| 字段 | 类型 | 偏移 |
|------|------|------|
| id | uint16 | 0-1 |
| version | uint16 | 2-3 |
| log_id | uint32 | 4-7 |
| provider | char[16] | 8-23 |
| magic_num | uint32 = 0xfb202202 | 24-27 |
| reserved | uint32 | 28-31 |
| body_len | uint32 | 32-35 |

### 解析状态机（signaling_worker.cpp:396）

```
while (sdslen(querybuf) >= bytes_expected + bytes_processed):
    STATE_HEAD:  等满 36 字节 → 校验 magic_num（不符即丢）→ 转 STATE_BODY
    STATE_BODY:  等满 body_len → 切 header/body 两个 Slice → nlohmann JSON 解析
                 → 按 CMDNO 分发（PUSH/PULL/ANSWER/STOP_PUSH/STOP_PULL）
    处理完 → bytes_processed = 65536 封口（短连接模型：一连接一请求）
```

要点：`bytes_expected` 随状态切换（36 → body_len）；`querybuf` 是 sds 缓冲，粘包/半包都靠"长度够不够"判断；magic_num 是二进制协议的第一道校验。

## A12 协议设计的统一视角：TLV（面试加分项）

任何协议都只回答三个问题——**TLV 三问**：

| 问题 | 作用 | 本项目实例 |
|------|------|-----------|
| **T（Type）**：怎么识别是什么 | 协议识别 / 消息类型 | magic_num=0xfb202202、STUN message type、CMDNO |
| **L（Length）**：怎么切边界 | 防粘包/半包 | body_len、STUN header 的 length、attribute 的 16-bit length |
| **V（Value）**：怎么解内容 | 内容语义 | 定长结构体 / JSON key-value / attribute 循环 |

TLV 的两大流派：

- **定长流派**：位置即类型——字段偏移是编译期常量，零解析开销。xhead_t、RTP 头都属于这类
- **变长流派**：显式携带 type+length——可扩展，未知属性按 L 跳过也不影响解析。STUN attribute、WebSocket frame、HTTP 头都是这个家族

**STUN 是教科书式 TLV，而且是两级嵌套**（RFC 5389）：

- **报文级**：头部 20 字节自带 T（Message Type 2B）+ L（Message Length 2B，属性区字节数，同 body_len 语义）+ 附加身份字段（Magic Cookie 0x2112A442 4B 防伪识别、Transaction ID 12B 请求-响应配对），V = 属性区。**和 xhead_t 同构，唯一差别是 T 的位置**（STUN 的 T 在头，xhead 的 cmdno 在 JSON body）
- **属性级**：每个 attribute = 16-bit type + 16-bit length + value，按 L 逐条跳过，不需要知道每个属性的值（stun.cpp 的 `read()` 就是这个循环）
- 两级都是 TLV，层层递归

**流式解析的统一状态机**：等满定长头 → 读出 L → 等满 L → 处理。signaling_worker 的 STATE_HEAD/STATE_BODY 与 STUN 解析、WebSocket 收帧是同一个模式，只是定长头和 L 的位置不同。

**口述稿（30 秒）**：

> "协议设计本质是 TLV 思想，就回答三个问题：Type——怎么识别这是什么；Length——怎么切边界；Value——内容怎么解析。我信令的 36 字节定长头是 TLV 的定长变体——字段偏移在代码里是常量，magic_num 负责识别，body_len 负责切边界，body 用 JSON 自描述。STUN 是教科书式的变长 TLV：message type + length + 一串 type-length-value 属性，解析时靠 L 逐条跳过，不需要知道每个属性的值。WebSocket frame、HTTP 头其实都是这个家族的变体。所以我不只会用别人设计好的协议——需要自定义协议时，我知道设计要点在哪里。"

---

# Part B 口述稿

## B1 90 秒电梯版（约 470 字，时间紧砍【】段落）

> 我实现的是一套 WebRTC 媒体转发服务——SFU，信令、ICE、DTLS、SRTP、RTP 转发五层链路完整闭环。
>
> 架构是线程每核模型：客户端通过 TCP 信令发 PUSH/PULL 请求，RtcServer 对 stream_name 做 CRC32 哈希，把同一个房间的推流和拉流路由到同一个 RtcWorker 线程，单线程内处理，转发全程无锁。
>
> 【媒体栈从 set_local_description 创建：生成 ICE 凭据、建 ICE channel 和 DTLS 传输、触发 candidate 收集，每张网卡一个 UDP socket。】
>
> 最核心的难点是 ICE 选路。每个对端地址是一条 IceConnection，按 writable、write_state、receiving、priority、RTT 五级排序，切换带 10ms 防抖。关键设计：selected 连接挂了不等 15 秒——2.5 秒读方向失活、或 5 秒写方向降级，就立刻切换；15 秒只是所有连接死掉之后的终局宣告，触发资源释放。
>
> 几点我做得比较扎实的设计：
> 一是**逐对端加解密**——每个连接的 SRTP 密钥独立，SFU 必须解密后按接收者重新加密；但只在 SRTP 层解密，codec 层不解码，所以 CPU 便宜。
> 二是 **PULL 流透传 PUSH 端原始 SSRC**——SFU 不改包头，SDP 不声明它，拉流端拿到包也没法解码。
> 三是**析构延迟 10ms**——ICE 回调栈内同步 delete 会访问已释放的 controller，直接崩溃。
> 四是【DTLS ClientHello 先于 STUN 到达就丢弃、靠客户端 1 秒重传——防 DoS 的取舍。】
>
> 整套系统从信令到转发、从连接到清理，是完整闭环的。

## B2 完整版口述稿（约 3 分钟，被追问展开时用）

"你能讲一下你的 SFU 是怎么设计的吗？"

> 我实现的 SFU 是一个基于 libev 事件驱动的 WebRTC 媒体转发服务，核心是五层链路：信令 → ICE → DTLS → SRTP → RTP 转发。
>
> 首先，信令层通过 TCP 接收客户端的 PUSH/PULL 请求，RtcServer 根据 stream_name 做 CRC32 哈希，把同一个房间的推流和拉流路由到同一个 RtcWorker 线程，这样同一房间的数据在单一线程内处理，转发过程无锁。
>
> 媒体栈的创建从 set_local_description 开始。它做了四件事：生成 ICE 凭据、创建 ICE channel、创建 DtlsTransport 和 DtlsSrtpTransport、触发 candidate 收集。gathering_candidate 会为每张网卡创建一个 UDP socket，绑定到本地端口。
>
> ICE 选路是核心。每个客户端地址对应一个 IceConnection，通过 48ms 或 480ms 的定时器做连通性检查。选路按 writable > write_state > receiving > priority > RTT 排序。切换有 10ms 防抖，防止 RTT 小波动导致频繁切换。selected 连接死了不会等 15 秒——2.5s 读方向失活或 5s 写方向降级就会触发切换。15s 是终局宣告，确认所有连接都死了才释放资源。
>
> DTLS 握手需要三个条件：DTLS 对象创建、远端 fingerprint 收到、ICE writable，谁最后到谁触发。唯一有延迟的场景是 DTLS ClientHello 先于 STUN 到达——此时连接还没创建，包被丢弃，客户端 1s 后重传，这不是 bug 是防 DoS 的设计取舍。握手完成后导出 SRTP 密钥，用于 RTP/RTCP 加解密——注意是**逐对端加解密**：每个连接密钥独立，必须解密后按接收者重加密，但只在 SRTP 层解密、codec 层不解码。RTP 单向转发，RTCP 双向转发，拉流端发 PLI 请求通过 RTCP 回到推流端触发 I 帧。
>
> PULL 流要透传 PUSH 端的原始 SSRC，因为 SFU 不解码不转码，RTP 包头的 SSRC 原封不动到达拉流端。如果 SDP 不声明这个 SSRC，拉流端收到包后不知道编码类型，只能丢弃。
>
> 资源清理有三条路径：客户端主动 STOP、ICE 失败、30s 超时。析构用 10ms 延迟，因为 ICE timer 回调栈内不能同步 delete，会访问已释放的 _ice_controller 导致崩溃。
>
> 总的来说，这个 SFU 在单网卡云服务器上能做到 1 个 UDPPort 支撑所有连接，通过 5 级排序和 RTT 防抖把 ICE 选路做稳，再用 10ms 延迟析构把资源释放的并发问题兜住。整个系统从信令到转发、从连接到清理是完整闭环的。

## B3 三个追问应答

### 追问一：房间人多了怎么扩容？

> 这是我架构里明确的瓶颈：CRC32 把同一个房间钉在单线程上，几百人没问题，上千人就不行了。解法有三条路：
> 一是 **per-track 分发**——worker 粒度从"房间"拆到"track"，不同轨道放不同线程，代价是跨线程的 SSRC 映射和带宽控制变复杂。
> 二是 **SFU 级联**——大房间拆到多台机器，SFU 之间树状转发，这是 mediasoup、LiveKit 的路线，拿一跳延迟换水平扩展。
> 三是**哈希分片**——沿用我现在的 CRC32 思路，把"房间→worker"变成"房间→分片→worker"。
> 真要做，我倾向先做级联，因为它不动单机内的线程模型，风险最小。

### 追问二：没做 simulcast / 带宽估计 / 丢包重传，怎么看？

> 我实现的是最小闭环，这几块确实没做，但接入点我都清楚，都是"加功能"，不是"改架构"：
> **simulcast**——推流端发多档清晰度，SFU 按拉流端带宽选档转发。接入点在转发逻辑：解析 RTP 扩展头里的 rid 字段（SDP 的 a=rid），把"按房间转发"改成"按 track + 档位"转发。
> **带宽估计（GCC/REMB）**——我的 RTCP 双向转发已经打通，反馈包通路是现成的，缺的只是码率控制算法。
> **NACK 重传**——给推流端加一个 1-2 秒的 RTP 重传缓存，收到 NACK 就重发，缓存窗口和内存上限是设计要点。
> FEC 一般是浏览器发送端做的，SFU 主要透传。
> 所以核心链路是闭环的，这些是往链路上挂功能。

### 追问三：为什么 thread-per-core，而不是每连接一个线程？

> 每连接一线程的问题：连接数到千级万级时，线程切换成本高、cache 局部性差，跨线程共享状态必须加锁，锁竞争变成瓶颈。thread-per-core 把"按连接分线程"换成"按房间分线程"：哈希路由后，同一房间的数据都在一条线程上，天然无锁，转发就是内存拷贝。这是 nginx、SRS 这类事件驱动服务的典型思路。
> 代价也明确：一个房间被钉在一颗核上，房间越大越吃亏——这就是前面说的扩容瓶颈。这是取舍，不是缺陷。

## B4 冷问题的一句话接法

| 问题 | 接法 |
|------|------|
| RTP/RTCP 怎么分？ | `& 0x7F` 启发式：RTCP 原生 PT 是 192-223，与 0x7F 后落进 64-95（rtp_utils.cpp:17） |
| SRTP 密钥哪来的？ | DTLS 握手后 `export_keying_material`，拆出 client/server write key + salt |
| writable 和 receiving 为什么不合并？ | UDP 可能单向通——对端能发过来、你发不过去 |
| 为什么 PULL 的 offer 是 sendonly？ | SDP direction 声明方向，拉流端只收不发 |
| 为什么不用 HTTP 做信令？ | 36 字节定长头 + JSON body，纯内存偏移解析，比 HTTP 文本解析快，可扩展 |
| epoll 用 LT 还是 ET？ | libev 默认 LT；读路径 while 循环读到 EAGAIN 模拟 ET（async_udp_socket.cpp:58） |
| 为什么写监听不常驻？ | LT 下 fd 几乎恒可写，常驻会 busy loop；乐观发送，EAGAIN 才挂监听，发完即卸 |
| 自定义协议怎么设计？ | TLV 三问：Type 识别、Length 切边界、Value 解析；定长 vs 变长流派（A12） |

## B5 练习重点

- B1 决定第一印象，练到不卡壳；B2 是完整版，被追问再展开。
- B3 记住每段**首句立场句**即可，细节被追问再展开：
  - 扩容 → "这是我架构里明确的瓶颈"
  - 缺失特性 → "都是加功能，不是改架构"
  - 线程模型 → "这是取舍，不是缺陷"

## B6 引导面试官的三招（带节奏）

### 招式一：开场给地图（主动定框架）

电梯版讲完，直接给选项：

> "这是整体框架。每一层我都有实现细节——ICE 选路的切换策略、DTLS 握手时序、SRTP 密钥导出、资源清理，你想先听哪块？"

面试官 90% 会从你给的清单里挑，或者反问"你觉得哪块最有挑战"——那就等于把方向盘交给你了。

### 招式二：回答结尾埋钩子（反直觉点）

每段回答结尾抛一个"非显而易见"的结论，好奇心会引着面试官往你那问：

- ICE 选路讲完 → "有个反直觉的设计：selected 挂了不是等 15 秒才切，2.5 秒就切了。"
- DTLS 讲完 → "唯一会慢的场景是 ClientHello 先于 STUN 到——我选择直接丢包，靠客户端 1 秒重传，这是个防 DoS 的取舍。"
- 转发讲完 → "SFU 必须逐对端加解密，但只在 SRTP 层解密、codec 层不解码。"
- 析构讲完 → "直接 delete 会崩溃，所以延迟 10ms 析构。"

这些反直觉点就是钩子——面试官九成会追问"为什么"，一追问就进你的弹药库了。

### 招式三：被问偏了，轻"翻译"回弹药库

面试官问 A，找弹药库里最近的 B 接住，但要诚实：

- "怎么做 NAT 穿透？" → "我的实现里是 STUN 打洞 + ICE 连通性检测，展开讲讲？"
- "怎么保证稳定性？" → "资源清理三条路径 + 10ms 延迟析构，这里有个真实崩溃案例……"
- "协议怎么设计的？" → 只讲 TLV 三问两句 + "细节我可以展开"

### 主动避开的雷区

TLV 深水区、ET/LT 细节、simulcast 具体实现——知道但别主动展开。被问到了用一句"这块我了解原理但没实现，核心链路我可以讲得更细"轻轻绕开，诚实且不丢分。

带节奏不是坏事——面试官喜欢有主见、能自己组织知识体系的候选人。
