# Phase 7 背景知识：解析 Answer SDP + 设置远端 ICE

## ANSWER 在 WebRTC 连接建立中的位置

完整的 WebRTC 连接建立分四步：

```
1. Offer  (服务端 → 客户端): "我能收 Opus/H264，我的 ICE ufrag=svr1 pwd=xxx，我在 192.168.1.100:54321 等你"
2. Answer (客户端 → 服务端): "我用 Opus/H264，我的 ICE ufrag=cli1 pwd=yyy，我在 10.0.0.2:5000 等你"
3. ICE 连通性检查 (双方互发 STUN Binding Request)
4. DTLS 握手 (在选定的 candidate pair 上建立加密通道)
```

**Phase 7 做的是第 2 步**：接收并解析客户端发来的 Answer SDP，提取其中的 ICE 参数和媒体信息，为第 3 步 ICE 连通性检查铺路。

---

## Answer SDP 和 Offer SDP 的区别

### 客户端发来的 Answer SDP 示例

```
v=0
o=- 0 2 IN IP4 127.0.0.0
s=-
t=0 0
a=group:BUNDLE audio video

m=audio 9 UDP/TLS/RTP/SAVPF 111          ← 客户端选用了 opus (111)
c=IN IP4 0.0.0.0
a=ice-ufrag:clientUfrag                  ← ★ 客户端的 ICE ufrag
a=ice-pwd:clientIcePwd                   ← ★ 客户端的 ICE pwd
a=fingerprint:sha-256 AA:BB:CC:DD:...    ← ★ 客户端的 DTLS 证书指纹
a=setup:active                            ← ★ 客户端作为 DTLS client（主动方）
a=mid:audio
a=sendonly                                ← ★ 客户端是发送方（推流场景）
a=rtcp-mux
a=rtpmap:111 opus/48000/2
a=fmtp:111 minptime=10;useinbandfec=1
a=ssrc:12345 cname:clientAudioCname      ← ★ 客户端的音频 SSRC
a=ssrc:12345 msid:stream1 audio_track    ← ★ SSRC 的 Stream/Track 映射
a=ssrc:12345 mslabel:stream1
a=ssrc:12345 label:audio_track
a=candidate:... 1 udp 2130706431 10.0.0.2 5000 typ host  ← ★ 客户端候选地址

m=video 9 UDP/TLS/RTP/SAVPF 96           ← 客户端选用了 H264 (96)
c=IN IP4 0.0.0.0
a=ice-ufrag:clientUfrag
a=ice-pwd:clientIcePwd
a=fingerprint:sha-256 AA:BB:CC:DD:...
a=setup:active
a=mid:video
a=sendonly
a=rtcp-mux
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=42e01f
a=ssrc:67890 cname:clientVideoCname
a=ssrc:67890 msid:stream1 video_track
a=ssrc:67890 mslabel:stream1
a=ssrc:67890 label:video_track
a=ssrc-group:FID 67890 67891              ← ★ SSRC 分组：主流 + 重传流
a=rtpmap:97 rtx/90000                     ← ★ rtx 重传 codec
a=fmtp:97 apt=96
```

### Offer vs Answer 的关键差异

| 属性 | Offer | Answer |
|------|-------|--------|
| `a=setup` | `actpass`（服务端可做 client 或 server） | `active`（客户端做 DTLS client）或 `passive` |
| `a=direction` | `recvonly`（推流场景服务端收） | `sendonly`（推流场景客户端发） |
| `a=ice-ufrag/pwd` | 服务端生成的随机值 | 客户端生成的随机值 |
| `a=fingerprint` | 服务端的 DTLS 证书指纹 | 客户端的 DTLS 证书指纹 |
| `a=candidate` | 服务端的 UDP 地址 | 客户端的 UDP 地址 |
| `a=ssrc` | offer 中服务端推流时有，但我们 PUSH 场景服务端不发所以没有 | answer 中客户端作为发送方一定要有 |
| codec 列表 | offer 列出所有支持的 codec | answer 从中选择子集 |

---

## 解析 Answer SDP 要提取什么

### 第一类：ICE 传输参数（TransportDescription 级别）

```
a=ice-ufrag:xxx    → TransportDescription.ice_ufrag     ← 远端 ICE ufrag
a=ice-pwd:yyy      → TransportDescription.ice_pwd       ← 远端 ICE pwd
a=fingerprint:...  → TransportDescription.identity_fingerprint ← 远端 DTLS 指纹
a=setup:active     → TransportDescription.connection_role       ← 远端 DTLS 角色
```

这些参数通过 `TransportController::set_remote_description()` 下发给 `IceAgent` → `IceTransportChannel`。

### 第二类：媒体参数（MediaContentDescription 级别）

```
m=audio 9 UDP/TLS/RTP/SAVPF 111      → 确定解析上下文是 audio
a=mid:audio                            → 确定 media ID
a=sendonly                             → direction = k_send_only
a=rtcp-mux                             → rtcp_mux = true
```

### 第三类：SSRC 参数（StreamParams 级别）

```
a=ssrc:12345 cname:xxx        → StreamParams.cname
a=ssrc:12345 msid:s1 t1       → StreamParams.stream_id = "s1", StreamParams.id = "t1"
a=ssrc:12345 mslabel:s1       → (已废弃，可忽略)
a=ssrc:12345 label:t1         → (已废弃，可忽略)
a=ssrc-group:FID 67890 67891  → SsrcGroup("FID", {67890, 67891})
```

多个 `a=ssrc:` 行如果共享同一个 msid，它们属于同一个 StreamParams（多个 SSRC 比如 simulcast 或 FID 分组）。

## 解析策略：状态机式逐行解析

SDP 没有嵌套结构，是扁平的 key=value 行列表。解析思路：

```
初始化: current_mid = "", current_content = nullptr

for each line:
  if line starts with "m=":
    → 确定当前媒体类型（audio/video）
    → 创建对应的 AudioContentDescription 或 VideoContentDescription
    → 提取 payload types 列表
    → 设置 current_mid（后面 a=mid: 行会更新）
  
  if line starts with "a=mid:":
    → 更新 current_mid
  
  if line starts with "a=ice-ufrag:" / "a=ice-pwd:":
    → 存入当前 media 对应的 TransportDescription
  
  if line starts with "a=fingerprint:":
    → 解析 algorithm + digest → 构造 SSLFingerprint
  
  if line starts with "a=setup:":
    → active → ACTIVE, passive → PASSIVE, actpass → ACTPASS
  
  if line starts with "a=ssrc:":
    → 解析 ssrc, attribute, value
    → 按 ssrc 聚合到对应的 StreamParams
  
  if line starts with "a=ssrc-group:":
    → 解析 semantics, ssrc_list → SsrcGroup
  
  if line starts with "a=sendonly" / "a=recvonly" / etc:
    → 设置 direction
  
  if line starts with "a=rtcp-mux":
    → 设置 rtcp_mux = true
  
  if line starts with "a=candidate:":
    → Phase 7 可以先忽略（客户端的 candidate 存在 answer 里）
    → 或者解析但暂不处理（ICE 连接时可能需要）
```

## Connection Role 协商

这是 DTLS 握手前的重要一步。RFC 4145 定义了 SETUP 属性：

```
Offer:  a=setup:actpass    ← "我可以做 client 也可以做 server"
Answer: a=setup:active     ← "我做 client（主动发起 DTLS ClientHello）"
```

或者反过来：
```
Offer:  a=setup:actpass
Answer: a=setup:passive    ← "我做 server（等待 DTLS ClientHello）"
```

标准做法：**offer 方写 actpass，answer 方决定 active 或 passive**。客户端通常会选 `active`（主动发起 DTLS 握手），服务端选 `passive`（等待）。

Phase 7 解析 answer 时，需要把 `a=setup:active` 记录到 `TransportDescription.connection_role`。

---

## SSRC 和 StreamParams 解析的细节

### 一个 SSRC 的多行属性

同一个 SSRC 的属性分布在多行中：

```
a=ssrc:12345 cname:clientAudio                    ← cname
a=ssrc:12345 msid:stream1 audio_track              ← stream_id + track_id
a=ssrc:12345 mslabel:stream1                       ← (废弃，= stream_id)
a=ssrc:12345 label:audio_track                     ← (废弃，= track_id)
```

解析时需要按 SSRC 值聚合这些属性。数据结构：

```cpp
// 解析时的中间结构
std::map<uint32_t, StreamParams> pending_ssrcs;

// 读到 a=ssrc:12345 cname:xxx → pending_ssrcs[12345].cname = "xxx"
// 读到 a=ssrc:12345 msid:s1 t1 → pending_ssrcs[12345].stream_id = "s1"
//                                pending_ssrcs[12345].id = "t1"
```

### SSRC Group

```
a=ssrc-group:FID 67890 67891
```

含义：SSRC 67890 和 67891 属于同一个 FID 组（主编码流 + 重传流）。`semantics = "FID"`, `ssrcs = [67890, 67891]`。

SsrcGroup 关联到哪个 StreamParams？找到包含这些 SSRC 的 StreamParams，把 group 加进去。

---

## 消息追踪：Phase 7 在完整链路中的位置

```
客户端收到 offer → 生成 answer → 发 CMDNO_ANSWER(3)

SignalingWorker::_process_answer()
  → JSON 中取 "sdp" 字段（客户端生成的 answer SDP）
  → 构造 RtcMsg { cmdno=3, stream_name, sdp=answer_sdp }
  → g_rtc_server->send_rtc_msg(msg)

RtcServer → CRC32(stream_name) → RtcWorker
  → RtcWorker::_process_answer()                     ← Phase 7 要填空
    → RtcStreamManager::set_answer(stream_name, sdp)  ← Phase 7 新增
      → find PushStream by stream_name
      → stream->set_remote_sdp(sdp)                   ← Phase 7 新增
        → _pc->set_remote_sdp(sdp)                     ← Phase 7 要实现
          ├─ 逐行解析 SDP → 构造 _remote_desc
          │   ├─ 每遇到 m= 行 → 创建 MediaContentDescription
          │   ├─ 每遇到 a=ice-ufrag/pwd → 填充 TransportDescription
          │   ├─ 每遇到 a=fingerprint → SSLFingerprint
          │   ├─ 每遇到 a=ssrc: → StreamParams
          │   └─ 每遇到 a=ssrc-group: → SsrcGroup
          └─ _transport_controller->set_remote_description(_remote_desc)
              └─ for each content:
                   ├─ 取 TransportDescription → 拿出远端 ice_ufrag/pwd
                   ├─ _ice_agent->set_remote_ice_params(mid, params)
                   └─ (DTLS 设置留到 Phase 10)
```

解析完成后，**两端 ICE 参数就全部就绪**：

| 参数 | 存在哪里 | 何时设置 |
|------|--------|---------|
| 本端 ufrag/pwd | IceTransportChannel._ice_params | Phase 6 — set_local_description() |
| 远端 ufrag/pwd | IceTransportChannel._remote_ice_params | Phase 7 — set_remote_description() |
| 本端 candidate | MediaContentDescription._candidates | Phase 6 — gathering_candidate() |
| 远端 candidate | 客户端 answer SDP 中的 `a=candidate:` | Phase 7 可选解析 |
| 本端 fingerprint | TransportDescription.identity_fingerprint | Phase 6 — add_transport_info() |
| 远端 fingerprint | TransportDescription.identity_fingerprint | Phase 7 — 从 answer 解析 |

**Phase 8 就可以开始发 STUN Binding Request 了**。

---

## 哪些东西 Phase 7 不处理

| 不处理的内容 | 原因 |
|-------------|------|
| `a=candidate:` 行 | 远端的 candidate 存储在 answer SDP 中，Phase 8/9 ICE 状态机需要。Phase 7 可以先解析存储，也可以解析但暂不处理 |
| DTLS 握手 | 还没到那一步，Phase 10 才做 |
| ICE 连通性检查 | Phase 8-9 的内容 |
| RTCP feedback 解析 | 当前不需要 |
| codec 协商验证 | 客户端选的 codec 一定在 offer 列表中，不需要验证 |

---

## 总结

Phase 7 的本质工作：**把客户端的 answer SDP 文本解析成 `_remote_desc` SessionDescription 对象，然后把 ICE 参数下发给 IceAgent/IceTransportChannel。**

完成后，两端的信息就完整了——服务端知道自己的 IP:端口，也知道客户端的 IP:端口（从 candidate 行）；知道自己的 ufrag/pwd，也知道客户端的 ufrag/pwd。下一步就可以开始 STUN Binding Request 做连通性检测。
