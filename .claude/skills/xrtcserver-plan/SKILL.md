---
name: "xrtcserver-plan"
description: "通过沿着一条完整的 PUSH 请求链路逐层深入的方式,手写实现 xrtcserver 参考项目的 WebRTC 媒体中转服务"
---

# xrtcserver 实现路线 — 逐层深入法

## 仓库路径

- **你自己的 xrtc-server**（待实现）：`/home/ydqun/workspace/webrtc/xrtc-server`
- **参考项目 xrtcserver**（已完整实现）：`/home/ydqun/workspace/webrtc/xrtcserver`

## 背景知识笔记

每个 step 开始前，先到 `note/` 目录阅读该 step 的背景知识 markdown 文件。背景知识会解释该 step 涉及的 WebRTC 概念（SDP、STUN、DTLS、ICE 等），方便理解每一行代码在干什么再动手写。

- `note/step1-sdp-background.md` — SDP 结构、各行含义、SSRC、FeedBack、CodecParam
- 后续 step 依次补充

## 实现方式：引导式自我实现

本技能采用**引导式自我实现**模式：
- Claude **不直接写代码**，而是告诉你每个文件需要什么内容、关键点是什么，以及为什么
- 你**自己动手写**，遇到问题再问
- 这种模式确保你真正理解每一行代码的含义，而不是复制粘贴

## 核心理念

不按模块分层去写（先写 11 个 ICE 文件才能看到效果），而是**沿着一条 PUSH 请求链路，逐层深入**。每完成一步，这条链路就更真实一步，每步都可编译、可运行、可验证。

---

## Step 0 — 现状（已完成）

目前的 xrtc-server 已经实现了：

```
Client TCP → SignalingServer → SignalingWorker → RtcServer → RtcWorker → RtcStreamManager
                                                                              ↓
                                                                        create_push_stream()
                                                                              ↓
                                                                       返回 SDP offer（空）
```

已经完成的部分：
- `src/base/`：EventLoop, Socket, LockFreeQueue, Conf, Log, xhead
- `src/server/`：SignalingServer/Worker, RtcServer/Worker, TcpConnection
- `src/stream/rtc_stream_manager.*`：基本骨架，但 `create_push_stream()` 返回的是 rtcbase 提供的空 offer

### CMakeLists.txt 现状

```cmake
target_link_libraries(xrtc-server
    libyaml-cpp.a librtcbase.a libssl.a libcrypto.a
    libabsl_strings.a libabsl_throw_delegate.a libabsl_bad_optional_access.a
    libev.a -lpthread -no-pie
)
```

---

## Step 1 — PUSH 返回真实的有 codec 的 SDP offer（不含 ICE/DTLS）

**目标**：客户端发 PUSH，拿到一个有 H264 音频/视频 codec 的 SDP 文本

### 新增文件

| # | 文件 | 内容 | 行数估计 | 关键点 |
|---|------|------|---------|--------|
| 1.1 | `src/pc/peer_connection_def.h` | `PeerConnectionState` 枚举 (`k_new`, `k_connecting`, `k_connected`, `k_disconnected`, `k_failed`, `k_closed`) | 15 | 纯头文件，无 cpp |
| 1.2 | `src/pc/codec_info.h + .cpp` | `CodecInfo`, `AudioCodecInfo`, `VideoCodecInfo`, `FeedBackParam`, `CodecParam` | 头80 + cpp20 | AudioCodecInfo 有 `channels` 字段；VideoCodecInfo 是空的 |
| 1.3 | `src/pc/stream_params.h + .cpp` | `SsrcGroup`, `StreamParams` | 头40 + cpp30 | `SsrcGroup(semantics, ssrcs)`, `StreamParams::has_ssrc()` |
| 1.4 | `src/pc/session_description.h + .cpp` | `SdpType`, `MediaType`, `RtpDirection`, `MediaContentDescription`, `AudioContentDescription`, `VideoContentDescription`, `ContentGroup`, `TransportDescription`, `ConnectionRole`, `SessionDescription` | 头150 + cpp200 | **最核心**：重点是 `to_string()` 序列化为标准 SDP 文本 |

### 改动文件

| 文件 | 变更内容 |
|------|---------|
| `src/stream/rtc_stream.h + .cpp` | 增加 `PeerConnection* _pc` 成员；增加 `start(rtc::RTCCertificate*)` 方法；增加 `create_offer()` 纯虚函数 |
| `src/stream/push_stream.h + .cpp` | 新增文件。继承 `RtcStream`，实现 `create_offer()`：构造 SessionDescription → 填入 codec 信息 → 调用 `to_string()` |
| `CMakeLists.txt` | 加入 `./src/pc/*.cpp` 到 `all_src` 和 `test_src` |

### 如何验证

编译运行，用 netcat 或 telnet 模拟客户端发送 PUSH 请求：

```
# 构造 PUSH 请求
HEADER=$(python3 -c "
import struct, json
body = json.dumps({'cmdno': 1, 'uid': 12345, 'stream_name': 'test', 'audio': 1, 'video': 1})
hdr = struct.pack('>HHIH16sIII', 0, 0, 1001, b'\x00'*16, 0xfb202202, 0, len(body))
print((hdr + body.encode()).hex())
")

echo "$HEADER" | xxd -r -p | nc 127.0.0.1 9000 | xxd
```

预期返回的 JSON 中 `offer` 字段是一个合法 SDP，包含：
```
v=0
m=audio 9 UDP/TLS/RTP/SAVPF 0 8 111
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:111 opus/48000/2
m=video 9 UDP/TLS/RTP/SAVPF 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=42e01f
```

### PushStream::create_offer() 伪代码

```
SessionDescription offer(SdpType::k_offer)

// 音频内容
AudioContentDescription audio_content
audio_content.add_codec(PCMU)
audio_content.add_codec(PCMA)
audio_content.add_codec(opus)
audio_content.set_direction(k_send_only)  // push 是发送方
offer.add_content(audio_content)

// 视频内容
VideoContentDescription video_content
video_content.add_codec(H264)
video_content.set_direction(k_send_only)
offer.add_content(video_content)

return offer.to_string()
```

注意：`to_string()` 需要遍历 `_contents`，每个 content 输出一个 `m=` 行 + 对应的 `a=` 行。SSRC 先硬编码一个值（如 `0xdeadbeef`），后面 ICE 阶段再改。

---

## Step 2 — 给 SDP 加上 ICE 信息（host candidate）

**目标**：SDP offer 中出现 ICE ufrag/pwd 和 host candidate

### 新增文件

| # | 文件 | 关键内容 |
|---|------|---------|
| 2.1 | `src/ice/ice_def.h` | `IceCandidateComponent { RTP=1, RTCP=2 }`；`IcePriorityValue`；`ICE_UFRAG_LENGTH`等常量 | 纯头 |
| 2.2 | `src/ice/ice_credentials.h + .cpp` | `IceParamters{ ice_ufrag, ice_pwd }`；`IceCredentials::create_random_ice_credentials()` | 用随机字符串生成 ufrag/pwd |
| 2.3 | `src/ice/candidate.h + .cpp` | `Candidate` 结构体；`get_priority()`；`to_string()` | foundation = 网卡名 |
| 2.4 | `src/base/network.h + .cpp` | `Network{name, ip}`；`NetWorkManager` 用 `getifaddrs()` 枚举本机 IP | POSIX API |
| 2.5 | `src/ice/port_allocator.h + .cpp` | `PortAllocator`：持有 `NetWorkManager`，提供网卡列表和端口范围 | 简单的 allocator |
| 2.6 | `src/base/async_udp_socket.h + .cpp` | 基于 EventLoop IOWatcher 的异步 UDP socket；`send_to()` / `signal_read_packet` | 用 IOWatcher 监听 UDP fd |
| 2.7 | `src/ice/udp_port.h + .cpp` | `UDPPort`：绑定 UDP 端口，创建 Candidate，STUN 消息收发（收发函数先留空） | UDP 绑定 + candidate 生成 |

### 改动文件

| 文件 | 变更 |
|------|------|
| `src/stream/rtc_stream_manager.h + .cpp` | 增加 `std::unique_ptr<PortAllocator> _allocator` 成员，构造时初始化 |
| `src/stream/push_stream.h + .cpp` | `create_offer()` 增加：创建 `UDPPort` → 生成 `Candidate` → 生成 `IceParamters` → 填入 `SessionDescription` |
| `src/pc/session_description.h + .cpp` | `TransportDescription` 增加 `ice_ufrag`/`ice_pwd`；`to_string()` 输出 `a=ice-ufrag:` / `a=ice-pwd:` / `a=candidate:` |
| `CMakeLists.txt` | 加入 `./src/ice/*.cpp` |

### 验证

SDP offer 中多出：
```
a=ice-ufrag:xxxx
a=ice-pwd:yyyy
a=candidate:1 1 UDP 2130706431 192.168.x.x 54321 typ host
```

---

## Step 3 — 给 SDP 加上 DTLS fingerprint

**目标**：SDP offer 中带上 DTLS 证书指纹

### 改动文件

| 文件 | 变更 |
|------|------|
| `src/pc/session_description.h + .cpp` | `TransportDescription` 增加 `identity_fingerprint`；`to_string()` 输出 `a=fingerprint:sha-256 XX:XX:...` 和 `a=setup:actpass` |
| `src/stream/push_stream.h + .cpp` | `create_offer(certificate)` 接收证书参数；用 `certificate->ssl_fingerprint()` 获取指纹 |
| `src/stream/rtc_stream.h + .cpp` | `start(certificate)` 传递证书 |

### 验证

SDP offer 中多出：
```
a=fingerprint:sha-256 AA:BB:CC:...
a=setup:actpass
```

---

## Step 4 — STUN + ICE 连通性检查

**目标**：服务端 UDP 端口监听，能收发 STUN binding request/response，ICE 连接状态从 `k_checking` 变为 `k_connected`

### 新增文件（量最大，建议拆子步骤）

#### 4a — STUN 消息编解码（改动 1 个文件）

| 文件 | 关键内容 |
|------|---------|
| `src/ice/stun.h + .cpp` | `StunMessage`：read/write 编解码；`StunAttribute` 基类 + `StunUInt32Attribute`, `StunByteStringAttribute`, `StunAddressAttribute`, `StunXorAddressAttribute`, `StunErrorCodeAttribute`；`STUN_BINDING_REQUEST(0x0001)`, `STUN_BINDING_RESPONSE(0x0101)` 类型常量 |

核心逻辑：Binding Request 包含 USERNAME + PRIORITY + MESSAGE_INTEGRITY + FINGERPRINT 属性，服务端收到后用 ice_pwd 验证完整性，返回 XOR-MAPPED-ADDRESS。

#### 4b — StunRequest 管理器（改动 1 个文件）

| 文件 | 关键内容 |
|------|---------|
| `src/ice/stun_request.h + .cpp` | `StunRequestManager`：用 `std::map<transaction_id, StunRequest*>` 管理待确认的请求；`send()` / `check_response()` / `remove()`；`signal_send_packet` 回调 |

#### 4c — IceConnection（改动 1 个文件）

| # | 文件 | 关键内容 |
|---|------|---------|
| 4c1 | `src/ice/ice_connection_info.h` | `IceCandidatePairState { WAITING, IN_PROGRESS, SUCCEEDED, FAILED }` |
| 4c2 | `src/ice/ice_connection.h + .cpp` | `IceConnection` 状态机：`ping()` / `handle_stun_binding_request()` / `send_stun_binding_response()` / `set_write_state()` / `update_receiving()` / `rtt()` / `priority()`；`WriteState { STATE_WRITABLE, STATE_WRITE_UNRELIABLE, STATE_WRITE_INIT, STATE_WRITE_TIMEOUT }` |

核心逻辑：每个连接对端是一个 candidate pair。每 48ms（弱）或 480ms（强）发送 ping（Binding Request），统计 response。连续超时则置为 WRITE_TIMEOUT。

#### 4d — IceController（改动 1 个文件）

| 文件 | 关键内容 |
|------|---------|
| `src/ice/ice_controller.h + .cpp` | `IceController`：管理所有 connections，`sort_and_switch_connection()` 按优先级选路，`select_connection_to_ping()` 决定下一个要 ping 的 connection |

选路策略：优先选 nominated（`USE_CANDIDATE` 属性）且 writable + receiving 的 connection。按 candidate type 优先级排序（host > prflx > srflx > relay）。

#### 4e — IceTransportChannel + IceAgent（各 1 个文件）

| 文件 | 关键内容 |
|------|---------|
| `src/ice/ice_transport_channel.h + .cpp` | `IceTransportChannel`：持有 `UDPPort` / connections / `IceController`；`gathering_candidate()` / `send_packet()` / `set_ice_params()` / `set_remote_ice_params()`；ICE 状态机 `k_new→k_checking→k_connected→k_completed`；定时 ping timer |
| `src/ice/ice_agent.h + .cpp` | `IceAgent`：管理多个 `IceTransportChannel`（audio/video），`create_channel()` / `get_channel()` / `gathering_candidate()`；`signal_candidate_allocate_done` / `signal_ice_state` |

### 改动文件

| 文件 | 变更 |
|------|------|
| `src/stream/rtc_stream.h + .cpp` | `_on_connection_state()` 回调监听 ICE 状态变化 |
| `src/stream/push_stream.h + .cpp` | `create_offer()` 中通过 `PortAllocator` 创建 `UDPPort` → 生成 candidates；ICE credential 写入 SDP |

### 验证

服务端运行后，UDP 端口监听 STUN。客户端发 Binding Request，服务端回复 Binding Response。ICE 状态从 `k_new` → `k_checking` → `k_connected`。

---

## Step 5 — DTLS 握手 + SRTP 加密通道

**目标**：DTLS 握手完成，SRTP 通道建立，数据包可 protect/unprotect

### 新增文件

| # | 文件 | 关键内容 | 前置依赖 |
|---|------|---------|---------|
| 5.1 | `src/pc/srtp_session.h + .cpp` | `SrtpSession` 封装 libsrtp2：`set_send()` / `set_recv()` / `protect_rtp()` / `unprotect_rtp()` / `protect_rtcp()` / `unprotect_rtcp()` | libsrtp2 (CMake 中加 `libsrtp2.a`) |
| 5.2 | `src/pc/srtp_transport.h + .cpp` | `SrtpTransport`：管理 `_send_session` + `_recv_session`，`set_rtp_params()` / `reset_params()` / `is_srtp_active()` | srtp_session |
| 5.3 | `src/pc/dtls_transport.h + .cpp` | `DtlsTransport`：用 `rtc::SSLStreamAdapter` 做 DTLS 握手；`set_local_certificate()` / `set_remote_fingerprint()` / `send_packet()`；`StreamInterfaceChannel` 桥接 ICE 层和 DTLS 层 | ice_transport_channel |
| 5.4 | `src/pc/dtls_srtp_transport.h + .cpp` | `DtlsSrtpTransport` 继承 `SrtpTransport`：DTLS 握手完成 → `_extract_params()` 导出 key → `_setup_dtls_srtp()` 初始化 SRTP 加解密 | dtls_transport + srtp_transport |

### 链路打通

Step 5 完成后，数据链路变成：
```
PushStream → PeerConnection → TransportController
    → DtlsSrtpTransport → DtlsTransport → IceTransportChannel
    → IceConnection → UDPPort → UDP socket
```

每个向下层转发的发送，和从下层向上层转发的接收，都通过 sigslot 信号链接。

---

## Step 6 — TransportController + PeerConnection

**目标**：统一的 create_offer / set_remote_sdp 接口，管理 ICE + DTLS + SRTP 的完整生命周期

### 新增文件

| 文件 | 关键内容 |
|------|---------|
| `src/pc/transport_controller.h + .cpp` | `TransportController`：持有 `IceAgent` + `DtlsTransport/DtlsSrtpTransport` map；`set_local_description()` / `set_remote_description()` / `set_local_certificate()` / `send_rtp()` / `send_rtcp()`；ICE candidate → DTLS → SRTP 的 sigslot 串联 |
| `src/pc/peer_connection.h + .cpp` | `PeerConnection`：持有 `TransportController` + `SessionDescription`（local/remote）；`create_offer(options)` / `set_remote_sdp(sdp)` / `send_rtp()` / `send_rtcp()`；`signal_connection_state` / `signal_rtp_packet_received` / `signal_rtcp_packet_received` |

### 改动的文件

| 文件 | 变更 |
|------|------|
| `src/stream/rtc_stream.h + .cpp` | 用 PeerConnection 代替之前的简单 offer 生成逻辑 |

---

## Step 7 — 完整 Stream 层

**目标**：推拉流全流程跑通，RTP/RTCP 数据收发

### 新增/改动的文件

| # | 文件 | 关键内容 |
|---|------|---------|
| 7.1 | `src/stream/pull_stream.h + .cpp` | 继承 `RtcStream`，方向改为 `k_recv_only`；`create_offer()` 生成 pull SDP |
| 7.2 | `src/stream/rtc_stream.h + .cpp` | 完善 `set_remote_sdp()` → `PeerConnection::set_remote_sdp()`；RTP/RTCP 数据通过 `RtcStreamListener` 回调上报 |
| 7.3 | `src/stream/rtc_stream_manager.h + .cpp` | 完善：`stop_push()` / `stop_pull()` / `set_answer()` / `find_push_stream()` / `remove_push_stream()`；`RtcStreamListener` 回调实现：数据转发（pull 阶段再实现） |
| 7.4 | `src/module/rtp_rtcp/rtp_utils.h + .cpp` | `infer_rtp_packet_type()` / `parse_rtp_ssrc()` / `parse_rtp_sequence_number()` / `get_rtcp_type()` |

### CMakeLists.txt 最终

```cmake
file(GLOB all_src
    "./src/*.cpp"
    "./src/base/*.cpp"
    "./src/server/*.cpp"
    "./src/stream/*.cpp"
    "./src/pc/*.cpp"
    "./src/ice/*.cpp"
    "./src/module/rtp_rtcp/*.cpp"
)

target_link_libraries(xrtc-server
    libyaml-cpp.a librtcbase.a libssl.a libcrypto.a
    libabsl_strings.a libabsl_throw_delegate.a libabsl_bad_optional_access.a
    libev.a libsrtp2.a -lpthread -no-pie
)
```

---

## 架构参考（每个对话都会用到）

### 全局变量（`src/global.cpp`）

```cpp
xrtc::GeneralConf* g_conf = nullptr;
xrtc::XrtcLog* g_log = nullptr;
xrtc::SignalingServer* g_signaling_server = nullptr;
xrtc::RtcServer* g_rtc_server = nullptr;
```

### RtcMsg（跨线程消息，`src/xrtc_server_def.h`）

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
    void* certificate;  // DTLS certificate from RtcServer::init
};
```

### 消息处理流程

```
SignalingWorker::_process_request()
  → switch(cmdno)
    → _process_push/pull → 构造 RtcMsg → g_rtc_server->send_rtc_msg(msg)
    → _process_stop_push/pull/answer → 同上但不设 conn/fd（不需下行响应）

RtcServer::_process_rtc_msg()
  → pop_msg() → _get_worker(stream_name) → CRC32(stream_name) % worker_num
  → worker->send_rtc_msg(msg)

RtcWorker::_process_rtc_msg()
  → pop_msg() → switch(cmdno)
    → _process_push/pull:
        _rtc_stream_mgr->create_push/pull_stream(...)
        msg->sdp = offer  // SDP offer 被填入 msg
        ((SignalingWorker*)msg->worker)->send_rtc_msg(msg)  // 下行回给 SignalingWorker
    → _process_stop_push/pull:
        _rtc_stream_mgr->stop_push/pull(...)
        无需下行响应
    → _process_answer:
        _rtc_stream_mgr->set_answer(...)
        无需下行响应

SignalingWorker::_process_rtc_msg()
  → pop_msg() → switch(cmdno)
    → CMDNO_PUSH/PULL → _response_server_offer(msg)
      构造 JSON 响应（含 err_no + offer sdp）→ _add_reply() → TCP socket 写回客户端
```

### EV 回调设计模式

```cpp
// 每个 Server/Worker 都有一个 friend C 风格函数作为 libev 回调入口
friend void signaling_worker_recv_notify(EventLoop*, IOWatcher*, int fd, int events, void* data);

// 回调函数实现中通过 data 指针转回 C++ 对象
void signaling_worker_recv_notify(EventLoop*, IOWatcher*, int fd, int, void* data) {
    int msg;
    read(fd, &msg, sizeof(int));
    ((SignalingWorker*)data)->_process_notify(msg);
}

// I/O 事件回调使用位掩码检查
void conn_io_cb(EventLoop*, IOWatcher*, int fd, int events, void* data) {
    auto* worker = (SignalingWorker*)data;
    if (events & EventLoop::READ)  worker->_read_query(fd);
    if (events & EventLoop::WRITE) worker->_write_query(fd);
}
```

### 队列使用对照

| 组件 | 队列类型 | 用途 |
|------|---------|------|
| `SignalingWorker` | `LockFreeQueue<int>` | 从 SignalingServer 接收的 conn fd（SPSC）|
| `SignalingWorker` | `std::queue + std::mutex` | 从 RtcServer 接收的下行 RtcMsg（多生产者）|
| `RtcServer` | `std::queue + std::mutex` | 从 SignalingWorker 接收的上行 RtcMsg（多生产者）|
| `RtcWorker` | `LockFreeQueue<shared_ptr<RtcMsg>>` | 从 RtcServer 接收的 RtcMsg（SPSC）|

### 测试约定

```bash
# 从项目根目录运行
./build/xrtc-server-test --gtest_filter="*"

# 单测命名：RtcServerTest.StartStop / SignalingWorkerTest.*
# 测试文件在 test/server/ 和 test/base/ 下
```

测试中通过 `#define private public` 在 include 前访问私有成员。

### 编译命令

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
# 或
./auto_build.sh
```

---

## 常见陷阱汇总

1. **`_process_query_buffer` 死循环**：while 条件使用了错误的比较，注意是 `>= c->bytes_processed + c->bytes_expected` 不是 `>= c->bytes_expected`
2. **WRITE 事件未停止**：写完 reply_list 后必须 `stop_io_event(w, fd, WRITE)`，否则 CPU 100%
3. **EOF 不处理**：`sock_read_data` 返回 0 表示对方关闭连接，必须 `_close_conn()`
4. **析构顺序**：析构函数中必须先 `notify(QUIT)` + `join()` 线程，再释放 EventLoop 等成员
5. **Log 野指针**：`rtc::LogMessage::RemoveLogToStream()` 必须在全局析构前调用
6. **link 顺序**：`librtcbase.a` 必须在 `libssl.a`/`libcrypto.a` 之前
7. **push_msg/pop_msg 非线程安全**：SignalingWorker 的 `_q_msg` 用 `std::mutex` 保护，RtcWorker 的用 `LockFreeQueue`（SPSC 无需锁）
8. **SDP 中 SSRC 冲突**：当前阶段先硬编码（如 `0xdeadbeef`），后期通过 `StreamParams` 生成唯一 SSRC
9. **`rtc::RTCCertificate` 引用计数**：使用 `rtc::scoped_refptr` 或原始指针传递，注意生命周期管理

---

## 每步完成后运行测试

每次完成一个 step 后，先编译通过，然后运行测试套件：

```bash
cd build && cmake .. && make -j$(nproc) && ./xrtc-server-test
```

如果编译失败，检查：
1. `CMakeLists.txt` 是否添加了新文件的 glob pattern
2. 新文件中的 `#include` 路径是否正确（相对路径基于 `./src` 和 `../rtcbase/src`）
3. link 依赖是否完整（特别是新增 `libsrtp2.a` 后）
