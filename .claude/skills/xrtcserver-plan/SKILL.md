---
name: "xrtcserver-plan"
description: "通过沿着一条完整的 PUSH 请求链路逐层深入的方式,手写实现 xrtcserver 参考项目的 WebRTC 媒体中转服务"
---

# xrtcserver 实现路线 — 消息流程教学法

## 仓库路径

- **你自己的 xrtc-server**（待实现）：`/home/ydqun/workspace/webrtc/xrtc-server`
- **参考项目 xrtcserver**（已完整实现）：`/home/ydqun/workspace/webrtc/xrtcserver`

## 教学方式：消息流程驱动

本技能**不按文件列表组织**，而是**按一条 CMDNO_PUSH 消息的生命周期**逐段深入。

每节课跟踪**同一条消息**从 TCP 到达开始，经过每一层处理，直到最终完成。因为你始终知道"这个消息现在到了哪一步"，所以能始终理解每一行代码在整体中的位置。

```
CMDNO_PUSH 消息的完整生命周期（概览）:

TCP 数据到达
 → SignalingWorker 读取 xhead 头部
 → 校验 magic_num，读取 JSON body
 → 构造 RtcMsg，入队 RtcServer
   → RtcServer CRC32 路由
     → RtcWorker 出队，switch(cmdno)
       → RtcStreamManager::create_push_stream()
         → PushStream::create_offer()
           → PeerConnection::create_offer()
             → SessionDescription（codec）
             → TransportDescription（ICE ufrag/pwd）
             → IceAgent → IceTransportChannel → PortAllocator
               → UDPPort → socket + bind → Candidate（host）
             → DTLS fingerprint
             → to_string() → SDP 文本返回
             → 同时 UDP 端口已打开等待 ICE 流量
   ← 响应经原路径返回客户端
     客户端拿到 SDP offer + candidate
     客户端发 STUN Binding Request 到 candidate 的 IP:端口
   → UDPPort 收到 → StunMessage::read
     → IceConnection 状态机 → Binding Response 回复
     → ICE 状态推进: k_checking → k_connected
   → DTLS 握手开始
     → DtlsTransport → DTLS handshake over ICE
     → SrtpTransport 初始化（从 DTLS 导出 key）
     → SRTP 通道就绪
   → RTP/RTCP 数据流
     → DtlsSrtpTransport::unprotect_rtp
     → 解析 SSRC/seq → 转发
```

---

## Phase 指导原则

每个 Phase 分为三个部分：

1. **消息追踪**：CRITICAL — 描述当前 Phase 中消息走到了哪一步，上一步是什么，下一步是什么
2. **实现细节**：要写什么代码，每个代码片段在消息路径中扮演什么角色
3. **验证点**：如何确认这个 Phase 实现正确

**阅读背景笔记**：每个 Phase 开始前，先去 `note/` 阅读对应的背景知识 markdown，理解相关 WebRTC 概念再动手。

---

## Phase 0 — 当前状态诊断

### 消息当前走到了哪里

```
TCP 数据 → SignalingWorker → RtcServer → RtcWorker → RtcStreamManager
                                                            ↓
                                                      create_push_stream()
                                                            ↓
                                                     PushStream::create_offer()
                                                            ↓
                                                      SessionDescription(含codec + ICE ufrag/pwd)
                                                            ↓
                                                      to_string() → 含codec + ICE凭证，无candidate，无DTLS指纹
```

### 已实现的消息路径段

| 路径段 | 状态 | 涉及文件 |
|--------|------|---------|
| TCP 数据 → xhead 头部解析 → JSON body 读取 | ✅ | `signaling_worker.cpp`, `tcp_connection.cpp` |
| RtcMsg 构造 → 入队 RtcServer | ✅ | `signaling_worker.cpp` |
| RtcServer CRC32 路由 → RtcWorker | ✅ | `rtc_server.cpp` |
| RtcWorker → switch(cmdno) → create_push_stream | ✅ | `rtc_worker.cpp` |
| SessionDescription: codec 填入 (opus/H264) | ✅ | `session_description.cpp`, `codec_info.h` |
| IceCredentials::create_random_ice_credentials() | ✅ | `ice_credentials.cpp` |
| SessionDescription::to_string() SDP 序列化 | ✅ | `session_description.cpp` |
| BUNDLE group | ✅ | `session_description.cpp` |
| AddTransportInfo (写入 ice-ufrag/pwd，不含 fingerprint) | ✅ | `session_description.cpp`, `push_stream.cpp` |

### 未实现的消息路径段（即将进入的 Phase）

| 路径段 | 状态 | 需要的文件 |
|--------|------|-----------|
| PortAllocator 创建 → UDPPort → socket bind → Candidate 生成 | ❌ | `async_udp_socket`, `port_allocator`, `udp_port` |
| DTLS certificate fingerprint 填入 SDP | ❌ | 修改 `push_stream`/`rtc_stream_manager` 传证书 |
| PeerConnection + TransportController 整合 | ❌ | `peer_connection`, `transport_controller` |
| STUN 消息编解码 + Binding Request/Response | ❌ | `stun.h/.cpp` |
| IceConnection 状态机 + ping/pong | ❌ | `ice_connection`, `ice_controller` |
| IceTransportChannel + IceAgent 管理 | ❌ | `ice_transport_channel`, `ice_agent` |
| DTLS 握手 | ❌ | `dtls_transport`, `dtls_srtp_transport` |
| SRTP 加解密 | ❌ | `srtp_session`, `srtp_transport` |
| RTP/RTCP 解析与转发 | ❌ | `rtp_utils` |

### 当前消息经过的所有代码文件

```
main.cpp
  → signaling_server.cpp (TCP listen)
    → signaling_worker.cpp (xhead parse, JSON parse, RtcMsg send)
    → tcp_connection.cpp (TCP state machine: STATE_HEAD → STATE_BODY)
  → rtc_server.cpp (CRC32 route)
    → rtc_worker.cpp (switch cmdno → create_push_stream)
      → rtc_stream_manager.cpp (create PushStream, call create_offer)
        → push_stream.cpp (create_offer → build SessionDescription)
          → session_description.cpp (codec, transport_info, to_string)
          → ice_credentials.cpp (random ufrag/pwd)
          → codec_info.h (AudioCodecInfo, VideoCodecInfo)
          → stream_params.h (SsrcGroup, StreamParams)
          → ice_def.h (constants)
          → candidate.h (Candidate struct)
```

---

## Phase 1 — TCP 数据到达：xhead 头部解析与 JSON body 读取

**学习目标**：理解 signaling 服务的 TCP 连接如何被接受、怎样按 xhead 协议逐字节读取，以及短连接模型的设计意图。

> 如果你已经熟悉这部分代码可以跳过，直接进入 Phase 2 的背景阅读。

### 1.1 消息追踪

```
                    ← 消息从这里开始 →
                    
[signaling 服务] ──TCP connect──→ [xrtc-server]
                                       │
                                    signaling_server 接受连接
                                       │
                                    fd 通过 LockFreeQueue 传给 signaling_worker
                                       │
                                    signaling_worker 创建 TcpConnection
                                       │
                                    EventLoop IOWatcher 注册 READ 事件
                                       │
                                    收到 TCP 数据 → 进入 _read_query()
                                       │
                                    TcpConnection 状态机:
                                      STATE_HEAD: 读满 36 字节 xhead
                                      ↓ 校验 magic_num == 0xfb202202
                                      STATE_BODY: 读满 body_len 字节 JSON
                                      ↓
                                    _process_request() 解析 JSON
                                       ↓
                                    switch(cmdno) → cmdno=1(PUSH)
```

关键理解：
- **每个 TCP 连接只处理一个请求**（`bytes_processed` 设为 65536 阻止后续读）
- **36 字节 xhead 是二进制协议头**，magic_num 用于校验协议版本
- **JSON body 包含 cmdno/uid/stream_name/audio/video 等字段**

### 1.2 背景笔记

`note/step1-sdp-background.md` — SDP 结构、各行含义、SSRC、FeedBack、CodecParam

### 1.3 涉及代码

- `src/server/signaling_server.cpp` — TCP listener, accept, fd dispatch
- `src/server/signaling_worker.cpp` — `_read_query()`, `_process_request()`, `_process_push()`
- `src/server/tcp_connection.cpp` — `conn_io_cb()`, state machine `STATE_HEAD/STATE_BODY`
- `src/base/xhead.h` — `xhead_t` struct, `XHEAD_SIZE=36`, `magic_num=0xfb202202`

### 1.4 消息在这一步的关键数据结构

```cpp
// xhead_t — 36 字节二进制协议头
struct xhead_t {
    uint16_t id;           // 消息 ID
    uint16_t version;      // 版本号
    uint32_t log_id;       // 日志 ID（网络字节序）
    char provider[16];     // 服务提供商
    uint32_t magic_num;    // 0xfb202202
    uint32_t reserved;     // 保留字段
    uint32_t body_len;     // JSON body 长度
};

// TcpConnection 状态机
enum ConnState { STATE_HEAD, STATE_BODY };

// RtcMsg — 从这一步创建，进入跨线程旅程
struct RtcMsg {
    int cmdno;             // CMDNO_PUSH=1
    uint64_t uid;
    std::string stream_name, stream_type;
    int audio, video;
    uint32_t log_id;
    void* worker;          // 回指 SignalingWorker
    void* conn;            // 回指 TcpConnection
    std::string sdp;       // 当前为空，Phase 3 被填入
};
```

### 1.5 理解：xhead 为什么是 36 字节？

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          id(2)           |        version(2)     |            ← 4字节
|                        log_id(4)                              | ← 4字节
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                        provider(16)                           |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      magic_num(4)               | reserved(4) | ← 8字节
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       body_len(4)                              | ← 4字节
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

总共：4 + 4 + 16 + 8 + 4 = **36 字节**

---

## Phase 2 — 跨线程路由：RtcMsg 入队、CRC32 分发、RtcWorker 处理

**学习目标**：理解多线程架构下消息如何在 EventLoop 线程间传递，CRC32 路由如何工作，以及为什么需要 pipe fd + write(int) 唤醒机制。

### 2.1 消息追踪

```
SignalingWorker::_process_push()
  → 构造 RtcMsg { cmdno=1, uid, stream_name, audio=1, video=1, worker, conn, fd }
  → g_rtc_server->send_rtc_msg(msg)
       ↓
  RtcServer::_q_msg.push(msg)        ← mutex 保护的多生产者队列
  write(pipe_send_fd, &msg, 1)       ← 1 字节通知
       ↓
  RtcServer recv_notify 回调触发
  → pop_msg()
  → CRC32(stream_name) % worker_num  ← 路由计算
  → rtc_worker->send_rtc_msg(msg)
       ↓
  RtcWorker LockFreeQueue 入队
  write(pipe_send_fd, &msg, 1)
       ↓
  RtcWorker recv_notify 回调
  → pop_msg()
  → switch(cmdno) → CMDNO_PUSH
    → _process_push()
      → _rtc_stream_mgr->create_push_stream(uid, stream_name, audio, video, log_id, certificate, offer)
```

### 2.2 关键理解

1. **为什么用 pipe + write(int)**：EventLoop 线程阻塞在 `ev_run()` 中，唯一安全的唤醒方式是让 `ev_io` watcher 检测到 fd 可读。pipe 的读端注册了 IOWatcher，写端写入数据后，libev 自动唤醒目标线程。

2. **为什么 RtcServer 用 std::mutex 而 RtcWorker 用 LockFreeQueue**：
   - RtcServer：可能被 N 个 SignalingWorker 同时写入 → 多生产者 → 需互斥锁
   - RtcWorker：只被 RtcServer（单生产者）写入 → 单生产者单消费者 → 无需锁

3. **CRC32 路由**：`hash(stream_name) % worker_num` 确保同一个流名的消息路由到同一个 worker

### 2.3 涉及代码

- `src/server/rtc_server.cpp` — `send_rtc_msg()`, `_process_rtc_msg()`, `_get_worker()`
- `src/server/rtc_worker.cpp` — `send_rtc_msg()`, `_process_rtc_msg()`, `_process_push()`
- `src/server/signaling_worker.cpp` — `_process_push()`, `send_rtc_msg()`
- `src/stream/rtc_stream_manager.cpp` — `create_push_stream()`

---

## Phase 3 — SDP offer 生成（codec 信息）

**学习目标**：理解 WebRTC SDP 的结构、codec 如何填入、MediaContentDescription 和 SessionDescription 的关系。

### 3.1 消息追踪

```
RtcStreamManager::create_push_stream()
  → new PushStream(el, uid, stream_name, audio, video, log_id)
  → stream->create_offer()                    ← 当前实现
       ↓
  SessionDescription offer(SdpType::k_offer)
  
  if (_audio):
    AudioContentDescription:
      add_codec(opus 111)                     ← AudioContentDescription 构造函数自带
      set_direction(RtpDirection::k_recv_only) ← 服务端是接收方
      set_rtcp_mux(true)
  
  if (_video):
    VideoContentDescription:
      add_codec(H264 96)                      ← VideoContentDescription 构造函数自带
      add_codec(rtx 97)                       ← 重传流，关联 H264
      set_direction(RtpDirection::k_recv_only)
      set_rtcp_mux(true)
  
  IceCredentials::create_random_ice_credentials()
  → offer.add_transport_info("audio", ice_params, nullptr)
  → offer.add_transport_info("video", ice_params, nullptr)
  
  ContentGroup("BUNDLE") → 将 audio, video 加入同一组
  → offer.add_group(bundle_group)
  
  → return offer.to_string()
       ↓
  返回标准 SDP 文本
```

### 3.2 生成的 SDP 文本

```
v=0
o=- 0 2 IN IP4 127.0.0.0
s=-
t=0 0
a=msid-semantic: WMS

m=audio 9 UDP/TLS/RTP/SAVPF 111       ← 音频媒体行，payload type 111
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:xxxx                        ← ICE 用户名片段
a=ice-pwd:yyyyyyyy                      ← ICE 密码
a=mid:audio
a=recvonly                              ← 服务端只收不发
a=rtcp-mux
a=rtpmap:111 opus/48000/2              ← opus 编码，48kHz，双声道
a=rtcp-fb:111 transport-cc             ← RTCP 反馈：传输拥塞控制
a=fmtp:111 minptime=10;useinbandfec=1  ← opus 参数：最小打包 10ms，带内 FEC

m=video 9 UDP/TLS/RTP/SAVPF 96 97     ← 视频媒体行，96=H264, 97=rtx
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:xxxx
a=ice-pwd:yyyyyyyy
a=mid:video
a=recvonly
a=rtcp-mux
a=rtpmap:96 H264/90000                 ← H264 编码，90kHz 时钟
a=rtcp-fb:96 goog-remb                 ← Google REMB 带宽估计
a=rtcp-fb:96 transport-cc
a=rtcp-fb:96 ccm fir                   ← 编解码控制：请求关键帧
a=rtcp-fb:96 nack                      ← NACK 丢包重传
a=rtcp-fb:96 nack pli                  ← NACK 图片丢失指示
a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f
a=rtpmap:97 rtx/90000                  ← rtx 重传流
a=fmtp:97 apt=96                       ← rtx 关联的原始 payload type
```

### 3.3 涉及文件

| 文件 | 角色 |
|------|------|
| `src/pc/codec_info.h` | CodecInfo 基类 + AudioCodecInfo(有channels) + VideoCodecInfo |
| `src/pc/session_description.h + .cpp` | SessionDescription, MediaContentDescription, AudioContentDescription(构造函数自带opus), VideoContentDescription(构造函数自带H264+rtx), ContentGroup, TransportDescription |
| `src/pc/stream_params.h + .cpp` | SsrcGroup, StreamParams（当前未使用，Phase 7 才用到）|
| `src/ice/ice_credentials.h + .cpp` | IceParameters, create_random_ice_credentials() |
| `src/ice/ice_def.h + .cpp` | ICE_UFRAG_LENGTH=4, ICE_PWD_LENGTH=24 |
| `src/stream/push_stream.h + .cpp` | PushStream::create_offer() — 组装 SDP |
| `src/stream/rtc_stream.h + .cpp` | RtcStream 基类 |

### 3.4 SDP 中的关键字段含义

| SDP 行 | 含义 | 为什么用这个值 |
|--------|------|---------------|
| `m=audio 9 UDP/TLS/RTP/SAVPF` | 媒体行：端口9，协议是 DTLS 加密的 SRTP | 端口9是占位（真正的端口在 candidate 中），`SAVPF` 表示支持反馈 |
| `a=recvonly` | 服务端只接收 | PUSH 场景下客户端是发送方 |
| `a=rtcp-mux` | RTP 和 RTCP 共用同一端口 | 节省端口资源 |
| `a=fmtp:96 packetization-mode=1` | H264 打包模式 1（非交错模式）| 标准 WebRTC H264 参数 |
| `a=fmtp:96 profile-level-id=42e01f` | H264 约束基线 profile | 兼容性最好的 H264 子集 |

### 3.5 理解当前阶段缺失的两块信息

当前的 SDP 中设置了 ICE ufrag/pwd（`a=ice-ufrag`, `a=ice-pwd`），但缺少：
1. **`a=candidate:`** — 因为还没有 UDPPort，不知道实际 IP 和端口
2. **`a=fingerprint:`** — 因为 `add_transport_info` 传入的 certificate 是 `nullptr`

这就是接下来 Phase 4 和 Phase 5 要填充的内容。

### 3.6 验证

```bash
# 编译并启动服务端
cd build && cmake .. && make -j$(nproc) && ./xrtc-server &

# 模拟发送 PUSH 请求
python3 -c "
import struct, json, socket
body = json.dumps({'cmdno': 1, 'uid': 12345, 'stream_name': 'test', 'audio': 1, 'video': 1})
hdr = struct.pack('<HHI16sIII', 0, 0, 1001, b'\x00'*16, 0xfb202202, 0, len(body))
s = socket.socket()
s.connect(('127.0.0.1', 9000))
s.sendall(hdr + body.encode())
data = s.recv(4096)
print(data.decode(errors='replace'))
s.close()
"
```

预期：JSON 响应中包含 `"err_no":0` 和 `"offer":"v=0\r\n..."` 字段

---

## Phase 4 — UDPPort 创建 + Candidate 生成（消息路径的分叉点）

**学习目标**：理解为什么 SDP 创建流程突然"分叉"——创建 SDP 的过程中需要同时分配 UDP 端口、生成 candidate 并写回 SDP。

### 4.1 消息追踪 — 这是最关键的转折点

消息处理到这里，发生了一个重要变化：

```
之前（Phase 3）：
  create_offer() → 直接在栈上构造 SessionDescription → 返回 SDP 字符串
  整个过程是同步的、纯数据结构的操作

现在（Phase 4）：
  create_offer() → 构造 SessionDescription
    → 需要知道"我在哪个 IP:端口上收数据"
    → 需要创建 UDP socket + bind()
    → UDP socket 创建后，EventLoop 要注册 IOWatcher 监听
    → socket bind 后，通过 getsockname() 拿到实际端口
    → 从 IP + port 创建 Candidate 对象
    → Candidate 填入 MediaContentDescription
  → to_string() → SDP 中现在有 a=candidate: 行了
```

**消息处理在这里从纯内存操作变成了涉及 I/O 的操作。**

### 4.2 新增的类和它们的职责（沿消息路径展开）

```
PortAllocator（端口分配器）
  └─ 持有 NetworkManager
  └─ 持有端口范围配置
  └─ get_udp_ports() → 为每个 Network 创建 UDPPort
       │
       ├─ NetworkManager（网络接口管理器）
       │   └─ 构造函数调用 getifaddrs() 枚举本机所有非回环 IPv4 网卡
       │   └─ 每个网卡存为 Network{name, ip}
       │
       └─ for each Network:
              │
              UDPPort（UDP 端口）
                ├─ 创建 UDP socket: socket(AF_INET, SOCK_DGRAM, 0)
                ├─ set non-blocking: fcntl(fd, F_SETFL, O_NONBLOCK)
                ├─ bind(): bind(fd, addr) 到 Network 的 IP + 端口(0=自动分配)
                ├─ getsockname() 取出实际分配的端口
                │
                ├─ AsyncUdpSocket（异步 UDP I/O）
                │   └─ EventLoop.create_io_event() 注册 IOWatcher
                │   └─ signal_read_packet 信号 → 数据到达时触发
                │   └─ send_to() 发送 UDP 数据
                │
                ├─ Candidate 生成（从 UDPPort 信息创建）
                │   └─ foundation = 网卡名（如 "eth0"）
                │   └─ component_id = 1 (RTP, rtcp-mux)
                │   └─ protocol = "UDP"
                │   └─ priority = RFC 5245 公式计算：
                │   │     priority = 2^24 * type_preference + 2^8 * local_preference + (256 - component_id)
                │   │              = 16777216 * 126 + 256 * 65535 + 255
                │   │              = 2130706431
                │   └─ address = Network 的 IP 字符串
                │   └─ port = getsockname() 返回的实际端口
                │   └─ type = "host"
                │
                └→ Candidate 填入 MediaContentDescription
                  → to_string() → a=candidate: 行写入 SDP
```

### 4.3 文件创建的顺序（消息路径决定）

当 `create_offer()` 运行时，调用的层级链是：

```
PushStream::create_offer()
  → 需要 PortAllocator → 传入 push_stream 或存在 rtc_stream_manager
  → allocator->get_udp_ports() → 需要 UDPPort
  → new UDPPort(network) → 需要 AsyncUdpSocket
  → new AsyncUdpSocket(fd, el) → 需要 EventLoop
  → UDPPort::create_candidate() → 需要 Candidate
```

所以**文件创建顺序**是倒过来的：

| 顺序 | 文件 | 为什么先写它 |
|------|------|-------------|
| 4.1 | `src/base/async_udp_socket.h + .cpp` | 最底层：封装 socket + IOWatcher，UDPPort 依赖它 |
| 4.2 | `src/ice/port_allocator.h + .cpp` | 次底层：持有 NetworkManager，提供端口分配接口 |
| 4.3 | `src/ice/udp_port.h + .cpp` | 最高层：使用上面两者，对外暴露 Candidate 生成接口 |

但**教学顺序**是反过来的（从消息路径的入口开始讲）：

1. **消息流入口**：`PushStream::create_offer()` 需要 PortAllocator
2. **PortAllocator**：接受 push_stream 创建 UDPPort 的请求
3. **UDPPort**：内部需要 AsyncUdpSocket
4. **AsyncUdpSocket**：最底层的事件驱动 UDP I/O

### 4.4 4.1 — AsyncUdpSocket：UDP 事件驱动的基石

消息路径上的角色：UDP 数据包到达时，EventLoop 通过 IOWatcher 触发回调，调起 AsyncUdpSocket 的信号。

```cpp
// async_udp_socket.h — 前瞻
class AsyncUdpSocket {
public:
    // 构造函数：接收已创建的 UDP fd 和 EventLoop
    AsyncUdpSocket(EventLoop* el, int fd);
    
    // 发送 UDP 数据（内部调用 sendto）
    int send_to(const char* data, size_t len, 
                const struct sockaddr* addr, socklen_t addrlen);
    
    // 读取 UDP 数据（IOWatcher 回调中调用）
    int read_packet();

    // 信号：当收到 UDP 数据包时触发
    sigslot::signal2<AsyncUdpSocket*, char*, size_t> signal_read_packet;

    int fd() { return _fd; }

private:
    EventLoop* _el;
    int _fd;                          // UDP socket fd
    IOWatcher* _watcher = nullptr;    // libev I/O watcher
    char* _read_buffer = nullptr;     // 接收缓冲区（64KB 或 1500 字节 MTU）
};
```

**为什么 AsyncUdpSocket 不负责创建 socket？**
- 职责分离：AsyncUdpSocket 只关心"有了 fd 之后怎么异步收发"
- Socket 创建（socket() + bind()）由上层 UDPPort 完成，因为 UDPPort 需要多个创建步骤（选网卡、选端口等）

**IOWatcher 注册时机**：
- 在 UDP 创建、bind、设为非阻塞后，由 UDPPort 创建 AsyncUdpSocket 并注册 IOWatcher
- `el->create_io_event(udp_port_io_cb, udp_port)`
- `el->start_io_event(watcher, fd, EventLoop::READ)`

### 4.5 4.2 — PortAllocator：UDP 端口资源的管家

消息路径上的角色：当 PushStream::create_offer() 需要知道"我在哪个端口上"时，它问 PortAllocator。

```cpp
// port_allocator.h — 核心接口
class PortAllocator {
public:
    PortAllocator();
    ~PortAllocator();
    
    // 获取所有 UDPPort（每个网卡对应一个）
    // 这是 create_offer() 调用的入口方法
    std::vector<UDPPort*> get_udp_ports(EventLoop* el);
    
    NetworkManager* network_manager() { return _network_manager; }

private:
    NetworkManager* _network_manager;  // 持有网卡枚举器
    std::vector<UDPPort*> _udp_ports;  // 已创建的 UDP 端口
};
```

**为什么 create_offer() 不自己直接创建 UDPPort？**
- PortAllocator 是 ICE 层的资源管理器，一个 RtcWorker 可能同时处理多个流
- PortAllocator 可以缓存 UDPPort，避免为每个流重复创建 socket
- 未来需要支持端口范围配置、多网卡选择等策略

### 4.6 4.3 — UDPPort：从网卡到 Candidate 的桥梁

消息路径上的角色：创建 socket + bind + 提取信息 → 生成 Candidate。

```cpp
// udp_port.h — 核心接口
class UDPPort {
public:
    // 构造函数：指定 EventLoop、网卡和端口范围
    UDPPort(EventLoop* el, Network* network, int min_port = 0, int max_port = 0);
    ~UDPPort();
    
    // 创建候选地址（从已绑定的 socket 信息提取）
    Candidate create_candidate();
    
    // 获取基础信息
    int fd() { return _async_udp_socket ? _async_udp_socket->fd() : -1; }
    uint16_t port() { return _port; }
    Network* network() { return _network; }

    // STUN 消息处理（当前先实现信号声明，函数体留空）
    // Phase 6 再填充实现

private:
    EventLoop* _el;
    Network* _network;             // 网卡信息（名称 + IP）
    int _fd = -1;                  // UDP socket fd
    uint16_t _port = 0;            // 绑定的端口
    AsyncUdpSocket* _async_udp_socket = nullptr;
    
    bool _create_udp_socket();     // 创建 socket + bind
};
```

**UDP socket 创建流程（_create_udp_socket）**：

```
1. socket(AF_INET, SOCK_DGRAM, 0)       → fd
2. fcntl(fd, F_SETFL, O_NONBLOCK)       → 非阻塞
3. struct sockaddr_in addr               → 准备地址
   addr.sin_family = AF_INET
   addr.sin_addr = network->ip (二进制)
   addr.sin_port = htons(0)              → 0 = 自动分配
4. bind(fd, (sockaddr*)&addr, len)       → 绑定
5. getsockname(fd, &addr, &len)          → 取出实际端口
6. _port = ntohs(addr.sin_port)           → 保存端口
7. _async_udp_socket = AsyncUdpSocket(el, fd) → 封装异步
8. 注册 IOWatcher READ
```

**Candidate 生成（create_candidate）**：

```
Candidate c;
c.foundation = network->name             // "eth0"
c.component_id = 1                        // IceCandidateComponent::RTP
c.protocol = "UDP"
c.priority = get_priority("host", 0)     // 2130706431
c.address = network->ip                  // "192.168.1.100"
c.port = _port                           // getsockname 返回的端口
c.type = "host"
return c;
```

**优先级计算（get_priority）**：

```cpp
uint32_t get_priority(const std::string& type, int adapter_index) {
    // type_preference: host=126, prflx=110, srflx=100, relay=0
    uint32_t type_pref = get_type_preference(type);
    // local_preference: 第一个网卡=65535，依次减 1
    uint32_t local_pref = 65535 - adapter_index;
    // RFC 5245 公式
    return (2 << 23) * type_pref + (2 << 7) * local_pref + (256 - 1);
    // = 16777216 * 126 + 256 * 65535 + 255
    // = 2130706431
}
```

### 4.7 改动现有文件

| 文件 | 变更 |
|------|------|
| `src/stream/rtc_stream_manager.h + .cpp` | 增加 `std::unique_ptr<PortAllocator> _allocator` 成员，构造函数初始化 |
| `src/stream/push_stream.h + .cpp` | `create_offer()` 通过 PortAllocator 获取 UDPPort → 生成 Candidates → 填入 SessionDescription |
| `CMakeLists.txt` | 确认 `./src/ice/*.cpp` 在 GLOB 范围内 |

### 4.8 PushStream::create_offer() 中新增的逻辑

```
PushStream::create_offer():
  SessionDescription offer(SdpType::k_offer)
  IceParameters ice_params = IceCredentials::create_random_ice_credentials()
  
  // ★ 新增：从 PortAllocator 获取 UDPPort，生成 candidates
  vector<UDPPort*> ports = _allocator->get_udp_ports(_el);
  vector<Candidate> candidates;
  for (auto port : ports) {
      candidates.push_back(port->create_candidate());
  }
  
  if (_audio):
    auto audio = new AudioContentDescription()
    audio->set_direction(k_recv_only)
    audio->set_rtcp_mux(true)
    audio->add_candidates(candidates)           // ★ 新增
    offer.add_content(audio)
    offer.add_transport_info("audio", ice_params, nullptr)
  
  if (_video):
    // 同上，加上 candidates
  
  // BUNDLE group...
  
  return offer.to_string()
  // ★ SDP 中现在有 a=candidate: 行了
```

### 4.9 验证

SDP offer 中多出：
```
a=ice-ufrag:bmOu
a=ice-pwd:gN7glSPNwmh1J0uo+Olrdgcd
a=candidate:eth0 1 UDP 2130706431 192.168.1.100 54321 typ host  ← ★ 新增
```

可以直接用 `ss -uln` 查看 UDP 端口是否已监听。

---

## Phase 5 — DTLS fingerprint 填入 SDP

**学习目标**：理解 DTLS 证书在信令阶段的角色——它不是在握手时才生成，而是在 SDP 交换阶段就通过 fingerprint 告诉对方"我期待收到这个证书"。

### 5.1 消息追踪

```
RtcServer::init()
  → 创建 DTLS certificate（SSL_CTX 初始化）
  → certificate 存入 RtcServer，传递给每个 RtcWorker
       ↓
RtcWorker::_process_push()
  → msg 中有 certificate 指针
  → _rtc_stream_mgr->create_push_stream(..., certificate, offer)
       ↓
RtcStreamManager::create_push_stream()
  → 需要把 certificate 传给 PushStream
  → PushStream::create_offer(certificate) 或通过 set_certificate()
       ↓
  SessionDescription::add_transport_info("audio", ice_params, certificate)
    → 不再传 nullptr
    → 调用 certificate->ssl_fingerprint() 获取指纹
    → 创建 SSLFingerprint 对象
    → TransportDescription.identity_fingerprint = fingerprint
    → TransportDescription.connection_role = ACTPASS（offer 方）
       ↓
  to_string():
    a=fingerprint:sha-256 AA:BB:CC:DD:...
    a=setup:actpass
```

### 5.2 代码改动

| 文件 | 变更 |
|------|------|
| `src/server/rtc_server.h + .cpp` | 已有 DTLS 证书创建逻辑，确认证书传递是否正确 |
| `src/stream/rtc_stream.h` | 增加 `start(rtc::RTCCertificate* certificate)` 方法 |
| `src/stream/rtc_stream_manager.h + .cpp` | `create_push_stream()` 将证书传给 PushStream/PeerConnection |
| `src/stream/push_stream.h + .cpp` | 接收证书，传给 SessionDescription::add_transport_info() |

注意：`session_description.cpp` 中 `add_transport_info()` 已经有 fingerprint 处理逻辑：

```cpp
if (certificate) {
    tdesc->identity_fingerprint = rtc::SSLFingerprint::CreateFromCertificate(*certificate);
    if (!tdesc->identity_fingerprint) {
        RTC_LOG(LS_WARNING) << "get fingerprint failed";
        return false;
    }
}
tdesc->connection_role = (_sdp_type == SdpType::k_offer) ? ACTPASS : ACTIVE;
```

`to_string()` 中也已经有：
```cpp
if (transport_info->identity_fingerprint) {
    ss << "a=fingerprint:" << transport_info->identity_fingerprint->algorithm
       << " " << transport_info->identity_fingerprint->GetRfc4572Fingerprint()
       << "\r\n";
    ss << "a=setup:" << connection_role_to_string(transport_info->connection_role) << "\r\n";
}
```

所以关键改动是：**把证书从 `nullptr` 改为真实的 `certificate` 指针**。

### 5.3 验证

SDP offer 中多出：
```
a=fingerprint:sha-256 72:6D:FC:06:53:D0:64:AE:54:D2:3F:9F:6F:AC:C2:7E:7C:7C:CF:6F:CB:01:16:E2:9A:DC:0E:77:3C:0E:B6:AB
a=setup:actpass
```

---

## Phase 6 — PeerConnection + TransportController 整合

**学习目标**：理解为什么需要 PeerConnection 层——它将 SDP 生成、ICE、DTLS、SRTP 统一管理。

### 6.1 消息追踪

```
改为通过 PeerConnection 处理：

PushStream::create_offer()
  → _pc->create_offer(options)          ← 之前是直接构造 SessionDescription
       ↓
  PeerConnection::create_offer()
    → new SessionDescription(SdpType::k_offer)
    → IceCredentials::create_random_ice_credentials()
    → AudioContentDescription / VideoContentDescription（codec + direction）
    → add_transport_info(mid, ice_params, certificate)  ← 含 fingerprint
    → ContentGroup BUNDLE
    → _transport_controller->set_local_description(desc)
         ↓
       TransportController
         → IceAgent 开始 gather candidate
         → IceAgent 为每个 transport name 创建 IceTransportChannel
         → IceTransportChannel → PortAllocator::get_udp_ports()
         → UDPPort 创建 → socket bind → Candidate 生成
         → signal_candidate_allocate_done 回调
              ↓
           PeerConnection::_on_candidate_allocate_done()
             → candidate 填入 local_desc 对应的 content
             → 这时 SDP 中的 a=candidate: 行才有数据
    → return _local_desc->to_string()
```

### 6.2 关键变化

**改动前**（Phase 3-5 的实现）：
```
PushStream::create_offer()
  → 直接操作 SessionDescription 对象
  → 手动从 PortAllocator 获取 UDPPort 生成 candidates
  → 手动设置冰信息、fingerprint
```

**改动后**（Phase 6 的实现）：
```
PushStream::create_offer()
  → _pc->create_offer(options)
    → PeerConnection 内部管理了 SDP + ICE + DTLS 的完整一致性
  → candidate 通过信号异步回调添加（不再同步）
```

### 6.3 新增文件

| 文件 | 角色 |
|------|------|
| `src/pc/transport_controller.h + .cpp` | 桥梁层：持有 IceAgent + DtlsTransport/DtlsSrtpTransport 的 map，串联 sigslot 信号 |
| `src/pc/peer_connection.h + .cpp` | 顶层：持有 TransportController + local/remote SessionDescription，对外暴露 create_offer/set_remote_sdp |

### 6.4 TransportController 的 sigslot 信号链

```
IceAgent::signal_candidate_allocate_done
  → TransportController::on_candidate_allocate_done
    → signal_candidate_allocate_done（暴露给 PeerConnection）

IceAgent::signal_ice_state
  → TransportController::_on_ice_state
    → _update_state()
      → signal_connection_state（暴露给 PeerConnection）

DtlsTransport::signal_rtp_packet_received    ← 后续 Phase
  → TransportController::_on_rtp_packet_received
    → signal_rtp_packet_received（暴露给 PeerConnection）
```

### 6.5 RTCOfferAnswerOptions

```cpp
struct RTCOfferAnswerOptions {
    bool send_audio = true;     // 是否发送音频
    bool send_video = true;     // 是否发送视频
    bool recv_audio = true;     // 是否接收音频
    bool recv_video = true;     // 是否接收视频
    bool use_rtp_mux = true;    // BUNDLE
    bool use_rtcp_mux = true;   // RTP/RTCP 复用
    bool dtls_on = true;        // 启用 DTLS
};
```

PushStream 的选项：
```cpp
options.send_audio = false;    // Push 流，服务端不发送
options.send_video = false;
options.recv_audio = _audio;   // 根据客户端请求决定
options.recv_video = _video;
```

### 6.6 涉及改动

| 文件 | 变更 |
|------|------|
| `src/stream/rtc_stream.h + .cpp` | 增加 `PeerConnection* _pc` 成员；`start(certificate)` 初始化 PeerConnection |
| `src/stream/rtc_stream_manager.h + .cpp` | 构造时初始化 PortAllocator，传给 PushStream 的 PeerConnection |
| `src/stream/push_stream.h + .cpp` | `create_offer()` 改为调用 `_pc->create_offer(options)`，不再直接构造 SessionDescription |

---

## Phase 7 — set_remote_sdp：解析 Answer 中的 ICE 和 DTLS 信息

### 7.1 消息追踪

```
客户端收到 SDP offer 后，发 ANSWER 请求回来：
→ CMDNO_ANSWER (3)
→ body 中携带 answer SDP（客户端生成的 answer，包含客户端的 ICE candidate 和 fingerprint）

处理流程：
SignalingWorker::_process_answer()
  → 构造 RtcMsg { cmdno=3, sdp=answer_sdp }
  → g_rtc_server->send_rtc_msg(msg)

RtcServer → RtcWorker
  → RtcWorker::_process_answer()
    → _rtc_stream_mgr->set_answer(msg->stream_name, msg->sdp)
      → find PushStream
      → stream->set_remote_sdp(msg->sdp)
        → _pc->set_remote_sdp(sdp)
```

### 7.2 PeerConnection::set_remote_sdp() 解析过程

```
for each SDP line:
  a=ice-ufrag:xxx  → TransportDescription.ice_ufrag
  a=ice-pwd:yyy    → TransportDescription.ice_pwd
  a=fingerprint:... → TransportDescription.identity_fingerprint
  a=ssrc:...        → 解析 SSRC 信息 (cname, msid, mslabel, label)
  a=ssrc-group:...  → 解析 SSRC 组 (FID 等)
  m=audio/video     → 确定当前解析的媒体类型
```

```cpp
int PeerConnection::set_remote_sdp(const std::string& sdp) {
    // 1. 按行分割 SDP
    // 2. 解析 ICE transport 信息
    //    a=ice-ufrag / a=ice-pwd / a=fingerprint
    //    填充到 AudioContentDescription / VideoContentDescription
    // 3. 解析 SSRC 信息
    //    a=ssrc:<ssrc> cname:xxx
    //    a=ssrc:<ssrc> msid:stream_id track_id
    //    填充到 StreamParams
    // 4. 解析 SSRC group
    //    a=ssrc-group:FID <ssrc1> <ssrc2>
    //    填充到 StreamParams.ssrc_groups
    // 5. _transport_controller->set_remote_description(_remote_desc.get())
    //    将远程 ICE 参数传给 IceAgent
}
```

### 7.3 TransportController::set_remote_description()

```cpp
int TransportController::set_remote_description(SessionDescription* desc) {
    // 1. 为每个 transport 获取 Remote ICE params
    // 2. 通过 IceAgent 设置远程 ICE 参数
    //    ice_agent->set_remote_ice_params(transport_name, ufrag, pwd)
    // 3. 创建 DtlsTransport（使用远程 fingerprint）
    //    dtls_transport->set_remote_fingerprint(fingerprint)
    // 4. 启动 ICE 连通性检查
    //    ice_agent->start_ice()
    // 5. 连接 ICE → DTLS 信号链
    //    ice_agent->signal_ice_state → dtls_transport 开始 DTLS 握手
}
```

### 7.4 新增文件（此阶段）

| 文件 | 角色 |
|------|------|
| `src/ice/ice_transport_channel.h + .cpp` | 每个媒体流的 ICE 通道：管理 UDPPort、connections、state machine |
| `src/ice/ice_agent.h + .cpp` | 管理所有 IceTransportChannel，处理 ICE 状态 |
| `src/ice/ice_connection_info.h` | IceCandidatePairState 枚举 |

### 7.5 ICE 候选选择的触发

`set_remote_sdp` 完成后，客户端 answer 中的 candidate 被解析，服务端知道了"客户端在哪个 IP:端口上"。此时开始 ICE 连通性检测。

---

## Phase 8 — STUN 消息编解码：Binding Request/Response

### 8.1 消息追踪

```
客户端在 answer SDP 中告诉服务端"我在 10.0.0.2:5000 上收 UDP"。
服务端也已经在 SDP offer 中告诉客户端"我在 192.168.1.100:54321 上收 UDP"。

接下来双方开始发 STUN Binding Request 做连通性检测：

                              STUN Binding Request
  [客户端:10.0.0.2:5000] ──────────────────────────────→ [服务端:192.168.1.100:54321]
                              UDP 包到达 UDPPort
                                    ↓
                             AsyncUdpSocket::signal_read_packet
                                    ↓
                             信号触发 → UDPPort::on_read_packet()
                                    ↓
                             StunMessage::read(data, len)
                                    ↓
                             验证 MESSAGE-INTEGRITY（用 ice_pwd）
                                    ↓
                             构造 Binding Response：
                               - XOR-MAPPED-ADDRESS = 客户端实际地址
                               - MESSAGE-INTEGRITY = HMAC-SHA1(ice_pwd, 消息)
                               - FINGERPRINT = CRC32 校验
                                    ↓
                             通过 UDPPort 发回
                             UDP sendto 到客户端地址
                                    ↓
                             客户端的 STUN 事务完成
```

### 8.2 STUN 消息格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|0 0|     STUN Message Type     |         Message Length        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Magic Cookie                          |
|                         0x2112A442                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                     Transaction ID (96 bits)                  |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          Attributes                           |
|                      (variable length)                        |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Type: 0x0001 = Binding Request, 0x0101 = Binding Response Success
Magic Cookie: 固定 0x2112A442
Transaction ID: 12 字节随机数（Request 和 Response 必须一致）

### 8.3 STUN Attributes

| Attribute Type | Value | 方向 | 用途 |
|---------------|-------|------|------|
| MAPPED-ADDRESS | 0x0001 | Response | 客户端映射地址（非 XOR 版本，旧 RFC）|
| XOR-MAPPED-ADDRESS | 0x0020 | Response | 客户端映射地址（XOR 混淆版本）|
| USERNAME | 0x0006 | Request | ICE ufrag 组合（local_ufrag:remote_ufrag）|
| MESSAGE-INTEGRITY | 0x0008 | Both | HMAC-SHA1(ice_pwd, 完整消息) |
| PRIORITY | 0x0024 | Request | 发送方的 candidate 优先级 |
| USE-CANDIDATE | 0x0025 | Request | 标记此连接为 nominated（最终选路）|
| FINGERPRINT | 0x8028 | Both | CRC32(完整消息) ^ 0x5354554E |

### 8.4 新增文件

| 文件 | 角色 |
|------|------|
| `src/ice/stun.h + .cpp` | StunMessage 编解码器；StunAttribute 基类；StunUInt32Attribute, StunByteStringAttribute, StunAddressAttribute, StunXorAddressAttribute, StunErrorCodeAttribute |

### 8.5 StunMessage 核心接口

```cpp
class StunMessage {
public:
    StunMessage();
    
    // 从 UDP 数据解析 STUN 消息
    bool read(const char* data, size_t len);
    
    // 验证 MESSAGE-INTEGRITY
    bool validate_message_integrity(const std::string& pwd);
    
    // 添加属性
    void add_attribute(std::unique_ptr<StunAttribute> attr);
    
    // 获取属性
    StunAttribute* get_attribute(int type);
    
    // 序列化（带 MESSAGE-INTEGRITY 和 FINGERPRINT）
    std::string write(const std::string& pwd);
    
    // 获取 type
    int type() { return _type; }
    
    // 设置/获取 transaction_id
    void set_transaction_id(const std::string& id);
    std::string transaction_id();

public:
    static const int STUN_BINDING_REQUEST = 0x0001;
    static const int STUN_BINDING_RESPONSE = 0x0101;
};
```

### 8.6 UDPPort 的 on_read_packet 实现

```cpp
void UDPPort::on_read_packet(AsyncUdpSocket* /*socket*/, 
        char* buf, size_t len) 
{
    // 1. 判断是否是 STUN 消息
    //    STUN 消息的前两位是 0x00，且 Magic Cookie 是 0x2112A442
    if (len < 20 || (buf[0] & 0xC0) != 0) {
        // 非 STUN 消息（可能是 DTLS 或 RTP），上层处理
        return;
    }
    
    // 2. 解析 STUN 消息
    StunMessage msg;
    if (!msg.read(buf, len)) {
        return;
    }
    
    // 3. 根据消息类型处理
    switch (msg.type()) {
        case StunMessage::STUN_BINDING_REQUEST:
            _handle_binding_request(msg);
            break;
        case StunMessage::STUN_BINDING_RESPONSE:
            _handle_binding_response(msg);
            break;
    }
}
```

### 8.7 Binding Request 处理流程

```
_handle_binding_request(StunMessage& request):
  1. 取出 USERNAME 属性
  2. 检查 ufrag 是否匹配本地 ice_ufrag
  3. 用关联的 ice_pwd 验证 MESSAGE-INTEGRITY
  4. 构造 Binding Response:
     - 复制 request 的 transaction_id
     - 添加 XOR-MAPPED-ADDRESS（客户端 IP:端口 XOR Magic Cookie）
     - 计算并添加 MESSAGE-INTEGRITY（用 ice_pwd）
     - 计算并添加 FINGERPRINT
  5. 通过 AsyncUdpSocket::send_to() 发回
```

---

## Phase 9 — ICE 连接状态机

### 9.1 消息追踪

```
当 STUN Binding Request/Response 交换成功后：

ICE 连接状态开始推进：

IceConnection 创建
  → state = WAITING（等待被选中做连通性检查）
      ↓
  IceController 选择此连接
  → 发 STUN Binding Request（ping）
  → state = IN_PROGRESS
      ↓
  收到 Binding Response（pong）
  → state = SUCCEEDED
  → write_state = WRITABLE
  → receiving = true
  → 更新 RTT
      ↓
  IceController 排序所有 connection
  → 优先选 nominated + writable + receiving 的 connection
  → 切换到最佳 connection
      ↓
  IceTransportChannel 状态更新
  → k_checking → k_connected → k_completed
      ↓
  IceAgent::signal_ice_state 触发
  → TransportController → PeerConnection → signal_connection_state
```

### 9.2 新增文件

| 文件 | 角色 |
|------|------|
| `src/ice/ice_connection.h + .cpp` | 每条 candidate pair 的连接状态：ping/pong、writable、receiving、RTT |
| `src/ice/ice_controller.h + .cpp` | 连接选择器：管理所有 connections，按优先级排序，选择下一个要 ping 的 |
| `src/ice/stun_request.h + .cpp` | StunRequest 管理器：跟踪未回复的 Binding Request，超时重试 |

### 9.3 IceConnection 状态机

```
              +----------+
              |  INIT    |  ← 创建但未开始连通性检查
              +----------+
                    |
                    ↓ (被 IceController 选中 ping)
              +------------+
              | IN_PROGRESS|  ← 已发 Binding Request，等待 Response
              +------------+
                   /    \
                  v      v
           +----------+ +-------+
           | SUCCEEDED| | FAILED|  ← 超时未收到回复
           +----------+ +-------+
                |
          set_write_state(STATE_WRITABLE)
          update_receiving(true)
```

### 9.4 选路策略（IceController）

```cpp
// sort_and_switch_connection() 排序规则：
// 1. 已 nominated（USE-CANDIDATE）且 WRITABLE + receiving > 其他
// 2. 按 candidate type 优先级：host > prflx > srflx > relay
// 3. 同类型按优先级值（priority）降序
// 4. 同优先级按 RTT 升序

// select_connection_to_ping() 选择规则：
// 1. 优先 ping 未尝试过的 connection（WAITING 状态）
// 2. 其次 ping 最近失败过的（周期性重试）
// 3. 已 SUCCEEDED 的 connection 每 N 秒发一次 keepalive ping
```

### 9.5 StunRequestManager

```cpp
class StunRequestManager {
public:
    StunRequestManager(EventLoop* el, UDPPort* port);
    
    // 发送 STUN Binding Request
    void send(StunRequest* request);
    
    // 检查收到的 Binding Response 是否匹配某个待确认的 Request
    StunRequest* check_response(StunMessage& msg);
    
    // 移除已处理的 Request
    void remove(StunRequest* request);
    
    // 信号：当需要发送 STUN 数据包时触发
    sigslot::signal3<UDPPort*, const char*, size_t> signal_send_packet;
};
```

### 9.6 IceTransportChannel 状态机

```
      +-----+
      | NEW |    ← IceTransportChannel 刚创建
      +-----+
          |
          ↓ (set_remote_ice_params 调用后)
      +----------+
      | CHECKING |   ← 开始发 STUN Binding Request
      +----------+
         /      \
        v        v
  +-----------+  +------------+
  | CONNECTED |  |  FAILED    |  ← 至少一个 candidate pair 成功
  +-----------+  +------------+
        |
        v
  +-----------+
  | COMPLETED |   ← 选路完成，所有检查结束
  +-----------+
```

---

## Phase 10 — DTLS 握手

### 10.1 消息追踪

```
当 ICE 状态达到 k_connected 时：

IceTransportChannel::signal_ice_state(k_connected)
  → TransportController::_on_ice_state()
    → 创建 DtlsTransport（如果还没有）
        ↓
  DtlsTransport 开始 DTLS 握手：
  
  1. StreamInterfaceChannel 桥接：
     - DtlsTransport 通过 StreamInterfaceChannel 向 ICE 层读写数据
     - ICE 层通过 UDPPort 收发 DTLS 数据包（DTLS 包和 STUN 包通过 UDP 端口类型区分）
  
  2. DTLS 握手流程（通过 rtc::SSLStreamAdapter）：
     - ClientHello  ← 服务端作为 DTLS 服务端，等待客户端 Hello
     - ← ServerHello + Certificate + ServerKeyExchange
     - ← CertificateRequest（可选）
     - ← ServerHelloDone
     - ClientCertificate + ClientKeyExchange → 
     - CertificateVerify → 
     - ChangeCipherSpec + Finished → 
     - ← ChangeCipherSpec + Finished
     - DTLS 连接建立
        ↓
  3. _setup_dtls_srtp():
     - 使用 SSLStreamAdapter::GetSrtpParams() 导出 SRTP key
     - key 分 send_key 和 recv_key
        ↓
  4. SrtpTransport::set_rtp_params(send_key, recv_key):
     - 创建 SrtpSession(send) 和 SrtpSession(recv)
     - 调用 srtp_create() 初始化 libsrtp2
     - SRTP 通道就绪
```

### 10.2 DTLS 数据包和 STUN 数据包的区分

UDP 端口收到数据后，如何区分是 STUN 还是 DTLS？

```cpp
// UDPPort::on_read_packet() 中判断：
if (len >= 2 && (buf[0] & 0xC0) == 0) {
    // 前两位是 00 → STUN 消息
    // STUN 消息的前两位必须是 00（RFC 5389）
    handle_stun(...);
} else {
    // 非 STUN → DTLS（或未来的 RTP/SRTP）
    // DTLS ContentType 的前两位不是 00
    // 比如 ContentType=22(handshake) = 0x16 → 前两位 00
    // 实际上 DTLS 的 content type 范围是 20-255，高两位至少 01
    // 所以 (buf[0] & 0xC0) != 0 可以区分
    handle_dtls(...);
}
```

实际上更准确的区分方式：
- STUN：`buf[0] & 0xC0 == 0` 且 `magic_cookie == 0x2112A442`
- DTLS：`buf[0] & 0xC0 != 0`（ContentType ∈ [20, 255]）  
- RTP/SRTP：`buf[0] & 0xC0 == 0x80`（RTP 版本号 = 2）

### 10.3 DTLS/SRTP 链路打通后的数据路径

```
           发送方向（服务端 → 客户端）

PushStream → PeerConnection
  → TransportController::send_rtp("audio", data, len)
    → DtlsSrtpTransport::send_rtp(data, len)
      → SrtpSession::protect_rtp(data, len, out, out_len)
        → srtp_protect() 加密
      → DtlsTransport::send_packet(data, len)
        → IceTransportChannel::send_packet(data, len)
          → IceConnection::send_packet(data, len)
            → UDPPort::send_to(data, len, remote_addr)
              → AsyncUdpSocket::send_to()  ← 真的 UDP sendto
                  ↓
              网络 → 客户端

           接收方向（客户端 → 服务端）

网络 → AsyncUdpSocket::signal_read_packet
  → UDPPort::on_read_packet()
    → 判断非 STUN → DtlsTransport::signal_read_packet
      → DtlsSrtpTransport 收到
        → SrtpSession::unprotect_rtp(data, len, out, out_len)
          → srtp_unprotect() 解密
        → signal_rtp_packet_received
          → PeerConnection → RtcStreamListener → 应用层
```

### 10.4 新增文件

| 文件 | 角色 |
|------|------|
| `src/pc/srtp_session.h + .cpp` | 封装 libsrtp2：protect/unprotect RTP/RTCP |
| `src/pc/srtp_transport.h + .cpp` | 管理 send/recv SRTP session |
| `src/pc/dtls_transport.h + .cpp` | DTLS 握手：SSLStreamAdapter 封装 |
| `src/pc/dtls_srtp_transport.h + .cpp` | 连接 DTLS 和 SRTP：握手完成时自动设置 SRTP key |

### 10.5 libsrtp2 集成

CMakeLists.txt 添加：
```cmake
target_link_libraries(xrtc-server
    ...
    libsrtp2.a        # ★ 新增
    ...
)
```

`srtp_session.cpp` 初始化：
```cpp
SrtpSession::SrtpSession() {
    srtp_init();  // 全局初始化一次
}

bool SrtpSession::set_send(int cipher_type, const uint8_t* key, int key_len) {
    srtp_policy_t policy = {0};
    policy.ssrc.type = ssrc_any_outbound;
    policy.key = key;
    policy.rtp.cipher_type = cipher_type;
    // ... 配置 cipher, auth 等参数
    srtp_create(&_session, &policy);
}

bool SrtpSession::protect_rtp(const uint8_t* in, int in_len, 
        uint8_t* out, int* out_len) {
    return srtp_protect(_session, out, &in_len) == srtp_err_status_ok;
}
```

---

## Phase 11 — RTP/RTCP 数据转发

### 11.1 消息追踪

```
SRTP 通道就绪后，客户端开始发送音视频数据：

[客户端] ──UDP→ SRTP 加密的 RTP 包
  → AsyncUdpSocket 收到
    → UDPPort on_read_packet
      → 判断非 STUN → DtlsTransport
        → DtlsSrtpTransport
          → SrtpSession::unprotect_rtp() ← 解密
            → signal_rtp_packet_received
              → TransportController
                → PeerConnection
                  → signal_rtp_packet_received
                    → RtcStream::_on_rtp_packet_received
                      → RtcStreamListener::on_rtp_packet_received

服务端→客户端 同理：
  PushStream → PeerConnection::send_rtp()
    → TransportController::send_rtp()
      → DtlsSrtpTransport::send_rtp()
        → SrtpSession::protect_rtp() ← 加密
          → DtlsTransport → IceTransportChannel → UDP socket
```

### 11.2 RTP 头部解析（rtp_utils）

```cpp
// rtp_utils.h
namespace rtp_utils {

// 推断 RTP 包类型（RTP/RTCP/STUN/DTLS）
enum PacketType { k_rtp, k_rtcp, k_stun, k_dtls, k_unknown };

PacketType infer_rtp_packet_type(const char* buf, size_t len);

// 从 RTP 头部中提取 SSRC（第 8-11 字节）
uint32_t parse_rtp_ssrc(const char* buf);

// 从 RTP 头部中提取 sequence number（第 2-3 字节，网络序）
uint16_t parse_rtp_sequence_number(const char* buf);

// 获取 RTCP 包类型（第 1 字节）
int get_rtcp_type(const char* buf);

} // namespace rtp_utils
```

### 11.3 PacketType 判别逻辑

```cpp
PacketType infer_rtp_packet_type(const char* buf, size_t len) {
    if (len < 2) return k_unknown;
    
    // STUN: 前两位是 0x00，且 Magic Cookie = 0x2112A442
    if ((buf[0] & 0xC0) == 0 && len >= 4 &&
        buf[4] == 0x21 && buf[5] == 0x12 && 
        buf[6] == 0xA4 && buf[7] == 0x42) {
        return k_stun;
    }
    
    // DTLS: ContentType 在 [20, 255] 范围（20=ChangeCipherSpec, 21=Alert, 22=Handshake, 23=App）
    if (buf[0] >= 20 && buf[0] <= 255) {
        return k_dtls;
    }
    
    // RTP: 版本号 = 2 → 前两位是 10 (0x80)
    if ((buf[0] & 0xC0) == 0x80) {
        // RTCP 的 payload type 在 [200, 211] 范围内
        if (buf[1] >= 200 && buf[1] <= 211) {
            return k_rtcp;
        }
        return k_rtp;
    }
    
    return k_unknown;
}
```

### 11.4 新增文件

| 文件 | 角色 |
|------|------|
| `src/module/rtp_rtcp/rtp_utils.h + .cpp` | RTP/RTCP 包类型判别、SSRC 和 seq 解析 |
| `src/stream/pull_stream.h + .cpp` | 拉流类：继承 RtcStream，方向 k_send_only |

---

## Phase 12 — 完整推拉流

### 12.1 消息追踪

```
PUSH 流的完整生命周期：

  create_push_stream() → new PushStream() → start(certificate)
    → PeerConnection::init(certificate)
    → PeerConnection::create_offer(options)
      → SDP offer（含 codec + ICE + fingerprint + candidates）
      → ICE 开始 gather candidate（异步，通过信号回调填入 SDP）
    → SDP offer 返回 → 写入 RtcMsg.sdp → 返回 signaling
      ↓
  客户端发 ANSWER → set_remote_sdp(answer_sdp)
    → 解析远程 ICE 参数、fingerprint
    → ICE 连通性检查开始
    → DTLS 握手
    → SRTP 通道建立
    → RTP 数据收发
      ↓
  客户端发 STOP_PUSH → stop_push()
    → PushStream 关闭
    → ICE 释放
    → UDP 端口关闭
```

PULL 流同理，只是方向相反：
```
  create_pull_stream() → new PullStream()
    → PeerConnection::create_offer(options)
      → options.send_audio = true    ← 服务端要发送
      → options.send_video = true    ← 服务端要发送
      → options.recv_audio = false
      → options.recv_video = false
    → SDP offer 中方向为 a=sendonly    ← 服务端是发送方
```

### 12.2 新增/修改文件

| 文件 | 角色 |
|------|------|
| `src/stream/pull_stream.h + .cpp` | 拉流实现，方向 k_send_only |
| `src/stream/rtc_stream_manager.h + .cpp` | 完善：stop_push/stop_pull、find_push_stream/remove_push_stream |
| `src/stream/rtc_stream.h + .cpp` | 增加 set_remote_sdp / start / destroy 方法 |

---

## 架构参考（每个 Phase 都会用到）

### 全局变量

```cpp
xrtc::GeneralConf* g_conf = nullptr;
xrtc::XrtcLog* g_log = nullptr;
xrtc::SignalingServer* g_signaling_server = nullptr;
xrtc::RtcServer* g_rtc_server = nullptr;
```

### RtcMsg（跨线程消息）

```cpp
struct RtcMsg {
    int cmdno;          // CMDNO_PUSH(1), PULL(2), ANSWER(3), STOPPUSH(4), STOPPULL(5)
    uint64_t uid;
    std::string stream_name, stream_type;
    int audio, video;
    uint32_t log_id;
    void* worker;       // SignalingWorker* 回指，用于下行响应
    void* conn;         // TcpConnection* 回指
    int fd;
    std::string sdp;
    int err_no;
    void* certificate;  // DTLS certificate
};
```

### 类加载路径（完整运行时的调用栈）

```
SignalingWorker::_process_request()
  → switch(cmdno)
    → _process_push → 构造 RtcMsg → g_rtc_server->send_rtc_msg(msg)

RtcServer::_process_rtc_msg()
  → pop_msg() → CRC32(stream_name) % worker_num → rtc_worker->send_rtc_msg(msg)

RtcWorker::_process_rtc_msg()
  → pop_msg() → switch(cmdno)
    → CMDNO_PUSH:
        _rtc_stream_mgr->create_push_stream(...)
          → PushStream(el, allocator, uid, stream_name, audio, video, log_id)
          → PushStream::start(certificate)
            → _pc = new PeerConnection(el, allocator)
            → _pc->init(certificate)
          → offer = PushStream::create_offer()
            → _pc->create_offer(options)
              → SessionDescription 构造
              → TransportController::set_local_description()
                → IceAgent gather candidates (异步)
              → to_string() 返回
        msg->sdp = offer
        ((SignalingWorker*)msg->worker)->send_rtc_msg(msg)
    
    → CMDNO_ANSWER:
        _rtc_stream_mgr->set_answer(stream_name, sdp)
          → stream->set_remote_sdp(sdp)
            → _pc->set_remote_sdp(sdp)
              → TransportController::set_remote_description()
                → IceAgent set remote params + start ice
    
    → CMDNO_STOP_PUSH:
        _rtc_stream_mgr->stop_push(stream_name)
          → _pc->destroy()

SignalingWorker::_process_rtc_msg()
  → pop_msg() → CMDNO_PUSH/PULL → _response_server_offer(msg)
    → 构造 JSON → _add_reply() → TCP socket 写回
```

### I/O 回调设计模式

```cpp
// 每个 I/O 对象都有一个 friend C 函数作为 libev 回调入口
friend void conn_io_cb(EventLoop*, IOWatcher*, int fd, int events, void* data);

// 回调中通过 data 指针转回 C++ 对象
void conn_io_cb(EventLoop*, IOWatcher*, int fd, int events, void* data) {
    auto* worker = (SignalingWorker*)data;
    if (events & EventLoop::READ)  worker->_read_query(fd);
    if (events & EventLoop::WRITE) worker->_write_query(fd);
}
```

### 队列策略

| 组件 | 队列类型 | 用途 |
|------|---------|------|
| SignalingWorker → fds | `LockFreeQueue<int>` (SPSC) | 从 SignalingServer 接收 conn fd |
| SignalingWorker → RtcMsg | `std::queue + mutex` | 从 RtcServer 接收下行消息（多生产者）|
| RtcServer → RtcMsg | `std::queue + mutex` | 从 SignalingWorker 接收上行消息（多生产者）|
| RtcWorker → RtcMsg | `LockFreeQueue<shared_ptr<RtcMsg>>` (SPSC) | 从 RtcServer 接收消息 |

### 编译

```bash
cd build && cmake .. && make -j$(nproc)
./xrtc-server-test --gtest_filter="*"
```

### 测试

```bash
# 单测命名：RtcServerTest.StartStop / SignalingWorkerTest.*
# 测试文件在 test/server/ 和 test/base/ 下
./build/xrtc-server-test --gtest_filter="RtcServerTest.*"
./build/xrtc-server-test --gtest_filter="*Worker*"
```

---

## 常见陷阱

1. **`_process_query_buffer` 死循环**：while 条件用 `>= c->bytes_processed + c->bytes_expected`，不是 `>= c->bytes_expected`
2. **WRITE 事件未停止**：写完 reply_list 后必须 `stop_io_event(w, fd, WRITE)`，否则 CPU 100%
3. **EOF 不处理**：`sock_read_data` 返回 0 → 对方关闭连接 → 必须 `_close_conn()`
4. **析构顺序**：必须先 `notify(QUIT)` + `join()` 线程，再释放 EventLoop 等成员
5. **Log 野指针**：`rtc::LogMessage::RemoveLogToStream()` 必须在全局析构前调用
6. **link 顺序**：`librtcbase.a` 必须在 `libssl.a`/`libcrypto.a` 之前
7. **push_msg/pop_msg 线程安全**：SignalingWorker 用 `std::mutex`，RtcWorker 用 `LockFreeQueue`
8. **STUN/DTLS 包区分**：`(buf[0] & 0xC0) == 0` 为 STUN；`buf[0] & 0xC0 == 0x80` 为 RTP
9. **`rtc::RTCCertificate` 引用计数**：使用 `rtc::scoped_refptr` 或原始指针，注意生命周期
10. **SRTP key 导出**：DTLS 握手完成后立即通过 `SSLStreamAdapter::GetSrtpParams()` 导出，过时失效
