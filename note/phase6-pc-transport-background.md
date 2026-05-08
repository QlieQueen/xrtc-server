# Phase 6 背景知识：PeerConnection + TransportController

## 为什么需要 PeerConnection

回顾当前（Phase 5）的实现：

```
PushStream::create_offer()
  → 自己创建 SessionDescription
  → 自己调用 PortAllocator 获取 candidates（同步）
  → 自己填写 codec、ICE、fingerprint 到 SDP
  → return SDP 字符串
```

这种做法的局限：

1. **职责混合**：PushStream 没有 SDP 管理能力，但直接操作 SessionDescription
2. **无法处理异步事件**：candidate 收集当前是同步的（创建即返回），但标准 ICE 候选收集可能是异步的
3. **没有状态机**：谁维护 local/remote SDP？谁处理 set_remote_sdp？谁管理 ICE/DTLS 生命周期？
4. **没有信号传递**：ICE 连接状态变化 → DTLS 握手开始 → SRTP 就绪，这条链路没有地方串联

**PeerConnection 就是来解决这些问题的**。它封装了三层职责：

| 职责 | 说明 |
|------|------|
| **SDP 管理** | 持有 local_desc 和 remote_desc，提供 create_offer / set_remote_sdp |
| **Transport 生命周期** | 通过 TransportController 管理 ICE → DTLS → SRTP 的完整生命周期 |
| **信号路由** | 将底层的传输信号（ICE 状态、candidate 收集完成、RTP 数据到达）转发给上层（RtcStream） |

---

## TransportController 的桥梁角色

TransportController 是 PeerConnection 下最重要的层。它的位置：

```
PeerConnection
  ↓
TransportController          ← Bridge：连接 ICE 和 DTLS 层
  ├── IceAgent               ← 管理 IceTransportChannel
  │     └── IceTransportChannel
  │           ├── UDPPort * N
  │           └── IceController
  │                 └── IceConnection * N
  ├── DtlsTransport * N      ← 每个 media 一个
  └── DtlsSrtpTransport * N  ← 每个 media 一个
```

**在 create_offer 时**，TransportController::set_local_description() 做了这些事：

```
for each content in SessionDescription:
  1. _ice_agent->create_channel(mid, RTP)
  2. 设置本地 ICE 参数（从 TransportDescription 取出 ufrag/pwd）
  3. 创建 DtlsTransport，绑到 IceTransportChannel
  4. 创建 DtlsSrtpTransport，绑到 DtlsTransport
  5. 连接所有信号通道

6. _ice_agent->gathering_candidate()   ← 关键一步！
   for each IceTransportChannel:
     → 创建 UDPPort
     → socket bind
     → 生成 Candidate
     → signal_candidate_allocate_done
       → IceAgent → TransportController
         → PeerConnection::_on_candidate_allocate_done
           → candidates 填入 _local_desc
```

---

## 从同步到异步：candidate 收集的变化

这是 Phase 6 最核心的变化。对照来看：

### Before（Phase 5 — 你的当前实现）

```
PushStream::create_offer():
  IceParameters ice_params = ...
  auto networks = _allocator->get_networks()
  for network in networks:
    port = UDPPort(...)
    port->create_ice_candidate(...)   ← 同步：创建 socket + bind + 生成 candidate
    candidates.push_back(c)
  
  content->add_candidates(candidates)  ← 立即填入
  return offer.to_string()             ← 返回时 candidate 已在 SDP 中
```

### After（Phase 6 — PeerConnection 方式）

```
PeerConnection::create_offer():
  _local_desc = SessionDescription (含 codec + transport_info，无 candidates)
  _transport_controller->set_local_description(_local_desc)
    → 为每个 mid 创建 IceTransportChannel
    → _ice_agent->gathering_candidate()
       → 异步！candidate 通过 signal 回调通知
  return _local_desc->to_string()
  → 此时返回的 SDP 可能还**没有** a=candidate: 行！
    （candidate 稍后才通过 _on_candidate_allocate_done 填入）
```

## 关键理解：sigslot 信号是同步调用，不是异步消息

这里有一个容易误读的地方：上面说 "candidate 通过 signal 回调通知"，这不代表它是**异步**的。

**sigslot 的 connect + emit 是同步函数调用，不是任务队列。** emit 时会立即遍历所有注册的 slot，在同一帧调用栈中依次调用它们。

所以整个执行顺序其实是这样：

```
stack frame 0: PeerConnection::create_offer()
  stack frame 1: _transport_controller->set_local_description(_local_desc.get())
    stack frame 2: _ice_agent->gathering_candidate()
      stack frame 3: IceTransportChannel::gathering_candidate()
        for network: UDPPort → create_ice_candidate()   ← socket+bind，同步
        for network: UDPPort → create_ice_candidate()   ← 同上
        signal_candidate_allocate_done(this, candidates)  ← sigslot emit
          ↓
          stack frame 4: IceAgent::on_candidate_allocate_done()
            signal_candidate_allocate_done(...)           ← sigslot emit
              ↓
              stack frame 5: TransportController::on_candidate_allocate_done()
                signal_candidate_allocate_done(...)        ← sigslot emit
                  ↓
                  stack frame 6: PeerConnection::_on_candidate_allocate_done()
                    content->add_candidates(candidates)     ← 已填入 _local_desc
                  ← 返回到 stack frame 5
                ← 返回到 stack frame 4
              ← 返回到 stack frame 3
            ← 返回到 gathering_candidate()
          ← 返回到 set_local_description()
        ← 返回到 create_offer()
  stack frame 1: return _local_desc->to_string()
  ← 此时 candidates 已经在 _local_desc 中了
```

**换句话说：`_on_candidate_allocate_done` 一定在 `to_string()` 之前执行完毕。** candidates 写入 `_local_desc` 是 `set_local_description()` 整个调用过程的一部分，不是后来才触发的。

因此其实不应该说"异步回调"——更准确的说法是：**candidate 收集是同步操作，sigslot 只是将代码解耦成发送者和接收者，不改变执行的同步性。** 真正的异步（UDP I/O、定时器触发的 ICE ping）要等到 EventLoop 跑起来之后才出现。

---

## set_remote_description：消息的向下旅程

## set_remote_description：消息的向下旅程

当客户端发回 ANSWER 时，`set_remote_sdp()` 处理它：

```cpp
PeerConnection::set_remote_sdp(sdp):
  1. 逐行解析 SDP
  2. 对每行分类：
     - a=ice-ufrag / a=ice-pwd → TransportDescription
     - a=fingerprint: → TransportDescription.identity_fingerprint
     - a=ssrc: → SsrcInfo（cname, msid, track_id）
     - a=ssrc-group: → SsrcGroup
     - m=audio/video → 确定当前媒体类型
  3. 组装 audio/video 的 StreamParams（track）
  4. TransportController::set_remote_description(_remote_desc):
     for each mid:
       - _ice_agent->set_remote_ice_params(ufrag, pwd)  → IceTransportChannel
       - dtls->set_remote_fingerprint(alg, digest)       → DtlsTransport
```

关键点：**ANSWER 处理完成后，ICE 连通性检查开始的条件已经具备**（双方都知道了对方的 ufrag/pwd，以及在哪里发 STUN）。

---

## 信号链全景

```
┌─────────────────────────────────────────────────────────────────┐
│                        PeerConnection                           │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                    TransportController                    │  │
│  │  ┌────────────────────────────────────────────────────┐   │  │
│  │  │                    IceAgent                       │   │  │
│  │  │  ┌──────────────────┐  ┌──────────────────┐      │   │  │
│  │  │  │ IceTransportCh.1 │  │ IceTransportCh.2 │      │   │  │
│  │  │  │   udp_port * N   │  │   udp_port * N   │      │   │  │
│  │  │  └────────┬─────────┘  └────────┬─────────┘      │   │  │
│  │  └───────────┼─────────────────────┼─────────────────┘   │  │
│  │              │                     │                      │  │
│  │     ┌────────▼─────────┐  ┌────────▼─────────┐           │  │
│  │     │ DtlsTransport(1) │  │ DtlsTransport(2) │           │  │
│  │     └────────┬─────────┘  └────────┬─────────┘           │  │
│  │              │                     │                      │  │
│  │     ┌────────▼─────────┐  ┌────────▼─────────┐           │  │
│  │     │DtlsSrtpTransport1│  │DtlsSrtpTransport2│           │  │
│  │     └──────────────────┘  └──────────────────┘           │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

信号连接（set_local_description 时建立）：

```
1. UDPPort → signal_read_packet
   → IceTransportChannel handles STUN Binding Request/Response

2. IceTransportChannel → signal_candidate_allocate_done
   → IceAgent::on_candidate_allocate_done
     → TransportController::on_candidate_allocate_done
       → PeerConnection::_on_candidate_allocate_done

3. IceTransportChannel → signal_ice_state_changed
   → IceAgent::_on_ice_state_changed → _update_state
     → signal_ice_state
       → TransportController::_on_ice_state → _update_state
         → signal_connection_state
           → PeerConnection::_on_connection_state
             → signal_connection_state
               → RtcStream::_on_connection_state
                 → RtcStreamListener::on_connection_state

4. DtlsSrtpTransport → signal_rtp_packet_received
   → TransportController::_on_rtp_packet_received
     → PeerConnection::_on_rtp_packet_received
       → RtcStream::_on_rtp_packet_received
         → RtcStreamListener::on_rtp_packet_received
```

---

## 新类速览

### PeerConnection（新增 src/pc/peer_connection.h + .cpp）

```cpp
class PeerConnection : public sigslot::has_slots<> {
public:
    PeerConnection(EventLoop* el, PortAllocator* allocator);
    int init(rtc::RTCCertificate* certificate);
    void destroy();
    std::string create_offer(const RTCOfferAnswerOptions& options);
    int set_remote_sdp(const std::string& sdp);
    int send_rtp(const char* data, size_t len);
    int send_rtcp(const char* data, size_t len);

    SessionDescription* remote_desc();
    SessionDescription* local_desc();

    sigslot::signal2<PeerConnection*, PeerConnectionState> signal_connection_state;
    sigslot::signal3<PeerConnection*, rtc::CopyOnWriteBuffer*, int64_t>
        signal_rtp_packet_received;
    sigslot::signal3<PeerConnection*, rtc::CopyOnWriteBuffer*, int64_t>
        signal_rtcp_packet_received;

private:
    EventLoop* _el;
    std::unique_ptr<SessionDescription> _local_desc;
    std::unique_ptr<SessionDescription> _remote_desc;
    rtc::RTCCertificate* _certificate;
    std::unique_ptr<TransportController> _transport_controller;
    TimerWatcher* _destroy_timer;
    std::vector<StreamParams> _audio_source;
    std::vector<StreamParams> _video_source;
};
```

### RTCOfferAnswerOptions（新增 src/pc/peer_connection_def.h）

```cpp
struct RTCOfferAnswerOptions {
    bool send_audio = true;
    bool send_video = true;
    bool recv_audio = true;
    bool recv_video = true;
    bool use_rtp_mux = true;   // BUNDLE
    bool use_rtcp_mux = true;  // RTP/RTCP 复用
    bool dtls_on = true;
};
```

PushStream 的用法：
```cpp
options.send_audio = false;    // Push 服务端不发
options.send_video = false;
options.recv_audio = _audio;   // 根据请求决定
options.recv_video = _video;
```

### TransportController（新增 src/pc/transport_controller.h + .cpp）

```cpp
class TransportController : public sigslot::has_slots<> {
public:
    int set_local_description(SessionDescription* desc);
    int set_remote_description(SessionDescription* desc);
    void set_local_certificate(rtc::RTCCertificate* cert);
    int send_rtp(const std::string& transport_name, const char* data, size_t len);

    sigslot::signal4<...> signal_candidate_allocate_done;
    sigslot::signal2<TransportController*, PeerConnectionState> signal_connection_state;
    sigslot::signal3<...> signal_rtp_packet_received;
    sigslot::signal3<...> signal_rtcp_packet_received;

private:
    EventLoop* _el;
    IceAgent* _ice_agent;
    std::map<std::string, DtlsTransport*> _dtls_transport_by_name;
    std::map<std::string, DtlsSrtpTransport*> _dtls_srtp_transport_by_name;
    rtc::RTCCertificate* _local_certificate;
    PeerConnectionState _pc_state;
};
```

---

## 现有文件的改动

### RtcStream（src/stream/rtc_stream.h + .cpp）

| 改动 | 说明 |
|------|------|
| 增加 `PeerConnection* _pc` 成员 | 在构造函数中 `new PeerConnection(el, allocator)` |
| 增加 `start()` 中的 `_pc->init(certificate)` | 传入证书 |
| 增加 `_on_connection_state` / `_on_rtp_packet_received` | PeerConnection 信号回调 |
| 增加 `set_remote_sdp()` → `_pc->set_remote_sdp(sdp)` | 转发 ANSWER 到 PeerConnection |
| 增加 `send_rtp()` / `send_rtcp()` → `_pc->send_rtp()` | 数据发送 |
| 增加 `register_listener()` | 注册 RtcStreamListener（RtcStreamManager） |
| 增加 ICE 超时定时器 | 30 秒内未 connected 则触发 on_stream_exception |
| `_certificate` 成员移除 | 证书通过 `init()` 传给 PeerConnection |

### PushStream（src/stream/push_stream.h + .cpp）

`create_offer()` 大幅简化：

```cpp
std::string PushStream::create_offer() {
    RTCOfferAnswerOptions options;
    options.send_audio = false;
    options.send_video = false;
    options.recv_audio = _audio;
    options.recv_video = _video;
    return _pc->create_offer(options);
}
```

不再直接操作 SessionDescription、PortAllocator、UDPPort 等。所有细节下移到 PeerConnection + TransportController。

### RtcStreamManager（src/stream/rtc_stream_manager.h + .cpp）

| 改动 | 说明 |
|------|------|
| 增加 `register_listener` 调用 | `stream->register_listener(this)` |
| 增加 `RtcStreamListener` 接口实现 | `on_connection_state`, `on_rtp_packet_received` 等 |
| 增加 `set_answer()` | 处理 ANSWER 消息 |

---

## 状态机：PeerConnectionState

```
                +-------+
                |  NEW  |   ← PeerConnection 刚创建，create_offer 未调用
                +-------+
                    |
                    ↓ (create_offer / set_local_description)
              +-----------+
              | CONNECTING|   ← ICE checking + DTLS connecting
              +-----------+
               /          \
              v            v
        +-----------+  +--------+
        | CONNECTED |  | FAILED |   ← ICE 或 DTLS 失败
        +-----------+  +--------+
              |
              v (ICE 断开后重连超时)
        +--------------+
        | DISCONNECTED |
        +--------------+
```

`TransportController::_update_state()` 通过聚合所有 DtlsTransport 和 IceTransportChannel 的状态计算当前 PeerConnectionState。

---

## 消息路径对比

### Before（Phase 5）

```
PushStream::create_offer()
  → PortAllocator::get_networks()
  → for each network:
      UDPPort → socket bind → Candidate
  → content->add_candidates(candidates)
  → SessionDescription::to_string()
  → SDP（含 a=candidate:）
```

### After（Phase 6）

```
PushStream::create_offer()
  → _pc->create_offer(options)                     ← 委托给 PC
    → _local_desc = new SessionDescription()        
    → add audio/video content + transport_info       ← 此时 SDP 含 codec + ICE ufrag/pwd + fingerprint
    → _transport_controller->set_local_description()
      → _ice_agent->create_channel("audio", RTP)    ← 为每个 mid 创建 channel
      → _ice_agent->gathering_candidate()
        → UDPPort * N → socket bind → Candidate     ← 同步完成
        → signal_candidate_allocate_done
          → PeerConnection 填入 _local_desc
    → return _local_desc->to_string()               ← SDP 完整
```

---

## 依赖关系

### 新增文件对现有文件的影响

| 新文件 | 依赖已有文件 | 其他地方依赖它 |
|--------|-------------|---------------|
| PeerConnection | SessionDescription, TransportController, IceCredentials, Candidate, PortAllocator | RtcStream |
| TransportController | IceAgent, SessionDescription, DtlsTransport, DtlsSrtpTransport | PeerConnection |

### 代码生成的结构

```
PeerConnection 先写：
  → 依赖 SessionDescription（已有）、TransportController（待写）
  → TransportController 先于 PeerConnection 写

IceAgent 是一个独立的模块：
  → 依赖 IceTransportChannel（待写）、PortAllocator（已有）
  → 这一步只写 IceAgent 的**骨架**（create_channel, gathering_candidate）
  → 不实现 STUN（Phase 8）和 ICE 状态机（Phase 9）
```

### Phase 6 实际代码编写顺序

1. **IceAgent**（新文件）— 骨架：create_channel、get_channel、gathering_candidate（复用现有的 PortAllocator + UDPPort 创建逻辑）
2. **TransportController**（新文件）— set_local_description、信号桥接
3. **PeerConnection**（新文件）— create_offer、set_remote_sdp（含 SDP 解析）
4. **修改 RtcStream** — 增加 PeerConnection 成员、信号回调
5. **修改 PushStream** — create_offer 简化为 `_pc->create_offer(options)`
6. **修改 RtcStreamManager** — 增加 register_listener、set_answer

---

## 总结

Phase 6 的引入改变了架构：

```
Before:  PushStream → [直接操作 SDP + PortAllocator] → SDP
After:   PushStream → PeerConnection → TransportController → IceAgent → PortAllocator
                                              ↓
                                       DtlsTransport → DtlsSrtpTransport (后续 Phase)
```

关键好处：
1. **职责分离**：PushStream 只关心"推流"，不关心传输细节
2. **异步准备**：candidate 回调机制为后续 Phase（ICE 状态机、DTLS）铺路
3. **状态管理**：统一的 PeerConnectionState 状态机
4. **扩展性**：增加 PULL 流时，PullStream 复用同一套 PeerConnection 架构

下一步就是动手实现：从 IceAgent 的骨架开始。
