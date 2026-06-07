# Phase 12 背景知识：SSRC、Track、Stream 三者关系

## 1. 真实 SDP 样本（来自 2026-06-04 22:16:55 日志）

这是客户端发来的 ANSWER SDP（`cmdno=3, type=push`），包含完整的 SSRC 信息：

```sdp
v=0
o=- 1726971885514509618 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE audio video
a=msid-semantic: WMS stream_id                  ← ① 声明 MediaStream ID = "stream_id"
m=audio 9 UDP/TLS/RTP/SAVPF 111                 ← ② audio m= section
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:fsPo
a=ice-pwd:Cg0krGhE4BYopuXUxpleUihn
a=ice-options:trickle
a=fingerprint:sha-256 91:4C:6C:02:C6:26:16:6B:...
a=setup:active
a=mid:audio
a=sendonly
a=msid:stream_id audio_label                    ← ③ audio track: stream_id / audio_label
a=rtcp-mux
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=fmtp:111 minptime=10;useinbandfec=1
a=ssrc:2074346944 cname:fMSlSokjQnGXlqQs       ← ④ audio SSRC 属性行
a=ssrc:2074346944 msid:stream_id audio_label
a=ssrc:2074346944 mslabel:stream_id
a=ssrc:2074346944 label:audio_label
m=video 9 UDP/TLS/RTP/SAVPF 96 97              ← ⑤ video m= section（两个 payload type）
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:fsPo
a=ice-pwd:Cg0krGhE4BYopuXUxpleUihn
a=ice-options:trickle
a=fingerprint:sha-256 91:4C:6C:02:C6:26:16:6B:...
a=setup:active
a=mid:video
a=sendonly
a=msid:stream_id video_label                    ← ⑥ video track: stream_id / video_label
a=rtcp-mux
a=rtpmap:96 H264/90000                          ← ⑦ 主编码：H264，PT=96
a=rtcp-fb:96 goog-remb
a=rtcp-fb:96 transport-cc
a=rtcp-fb:96 ccm fir
a=rtcp-fb:96 nack
a=rtcp-fb:96 nack pli
a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f
a=rtpmap:97 rtx/90000                           ← ⑧ RTX 重传编码，PT=97
a=fmtp:97 apt=96                                ← ⑨ apt=96 表示 RTX 关联主 payload type 96
a=ssrc-group:FID 2620414317 255283571           ← ⑩ FID 组：primary=2620414317, RTX=255283571
a=ssrc:2620414317 cname:fMSlSokjQnGXlqQs       ← ⑪ primary SSRC 属性
a=ssrc:2620414317 msid:stream_id video_label
a=ssrc:2620414317 mslabel:stream_id
a=ssrc:2620414317 label:video_label
a=ssrc:255283571 cname:fMSlSokjQnGXlqQs        ← ⑫ RTX SSRC 属性
a=ssrc:255283571 msid:stream_id video_label
a=ssrc:255283571 mslabel:stream_id
a=ssrc:255283571 label:video_label
```

### 1.1 SSRC 行逐行解读

从 SDP 中可以提取出 **3 个 SSRC**：

| SSRC | 所属 m= | 角色 | cname | track_id (label) | stream_id (mslabel) |
|------|---------|------|-------|------------------|---------------------|
| `2074346944` | audio | 唯一的 audio SSRC | `fMSlSokjQnGXlqQs` | `audio_label` | `stream_id` |
| `2620414317` | video | primary（主 H264 流）| `fMSlSokjQnGXlqQs` | `video_label` | `stream_id` |
| `255283571` | video | RTX（重传流）| `fMSlSokjQnGXlqQs` | `video_label` | `stream_id` |

**关键观察**：

1. **所有 SSRC 共享同一个 cname**：`fMSlSokjQnGXlqQs`，表示它们来自同一个端点。cname 是跨 SSRC 的端点级标识符，RTCP 用它关联来自同一源的不同媒体流。

2. **audio 只有 1 个 SSRC**：opus 音频不需要 RTX（音频丢包不重传，由 opus 内置 FEC 处理）

3. **video 有 2 个 SSRC**：H264 视频需要 RTX 重传，所以有 primary（H264 帧）和 RTX（重传包）两个 SSRC

4. **SSRC 行的结构规律**：每个 SSRC 有 4 行属性：
   ```
   a=ssrc:<ssrc> cname:<cname>         — 端点标识
   a=ssrc:<ssrc> msid:<stream_id> <track_id>  — 媒体流 + 轨道
   a=ssrc:<ssrc> mslabel:<stream_id>   — 媒体流标签（与 msid 第一段冗余）
   a=ssrc:<ssrc> label:<track_id>      — 轨道标签（与 msid 第二段冗余）
   ```

### 1.2 ssrc-group 行解读

```
a=ssrc-group:FID 2620414317 255283571
```

- **FID** = Flow Identification，RFC 5888 定义。表示这两个 SSRC 属于同一个 Track，前者是 primary，后者是 RTX
- **SSRC 顺序有含义**：第一个是 primary（2620414317），第二个是 RTX（255283571）
- audio m= section **没有** ssrc-group，因为只有一个 SSRC，不需要分组

### 1.3 如何从 SDP 行聚合出 Track

解析过程按 **track_id（label）** 分组：

```
Step 1: 读取所有 a=ssrc: 行 → 得到 SSRC 属性表
Step 2: 按 label= 的值分组
        label=audio_label → Track{id="audio_label", stream_id="stream_id"}
        label=video_label → Track{id="video_label", stream_id="stream_id"}
Step 3: 填充每个 Track 的 ssrcs
        Track "audio_label": ssrcs = [2074346944]
        Track "video_label": ssrcs = [2620414317, 255283571]
Step 4: 读取 a=ssrc-group: 行 → 填充对应 Track 的 ssrc_groups
        Track "video_label": ssrc_groups = [{semantics="FID", ssrcs=[2620414317, 255283571]}]
```

最终得到 2 个 Track：

```
audio_track: StreamParams {
    id = "audio_label"
    stream_id = "stream_id"
    cname = "fMSlSokjQnGXlqQs"
    ssrcs = [2074346944]
    ssrc_groups = []                        // 单 SSRC，无分组
}

video_track: StreamParams {
    id = "video_label"
    stream_id = "stream_id"
    cname = "fMSlSokjQnGXlqQs"
    ssrcs = [2620414317, 255283571]
    ssrc_groups = [SsrcGroup{              // FID 组
        semantics = "FID",
        ssrcs = [2620414317, 255283571]
    }]
}
```

### 1.4 对比 PushStream 生成的 offer（无 SSRC）

再看看同一次请求中，服务端 PushStream 生成的 offer SDP：

```sdp
v=0
o=- 0 2 IN IP4 127.0.0.0
s=-
t=0 0
a=group:BUNDLE audio video
a=msid-semantic: WMS
m=audio 9 UDP/TLS/RTP/SAVPF 111
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=candidate:1070451813 1 udp 2113937151 172.27.95.199 10025 typ host
a=ice-ufrag:YZDE
a=ice-pwd:diZ85dTbUF3A1f3KLcakj5Bb
a=fingerprint:sha-256 38:43:2A:...
a=setup:actpass
a=mid:audio
a=recvonly                                     ← 注意：recvonly vs client 的 sendonly
a=rtcp-mux
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=fmtp:111 minptime=10;useinbandfec=1
m=video 9 UDP/TLS/RTP/SAVPF 96 97
...
a=mid:video
a=recvonly
...
a=rtpmap:96 H264/90000
a=rtpmap:97 rtx/90000
a=fmtp:97 apt=96
```

**关键差异**：PushStream 的 offer **没有任何 SSRC 行**。

原因：PushStream 是 `recvonly`——它接收媒体但不发送。所以它不需要声明 SSRC。SSRC 由**发送端**（client）在 answer SDP 中声明。这符合 WebRTC 语义：谁发送媒体，谁声明 SSRC。

**Phase 12 的关键变化**：当 PullStream 作为发送端（sendonly）生成 offer 时，**它需要在自己的 offer SDP 中包含从 PushStream 获取的 SSRC 信息**。

### 1.5 两个层面的 "stream" 辨识

```
a=msid-semantic: WMS stream_id    ← "stream_id" 是 WebRTC MediaStream 的 ID
                                    存在 StreamParams::stream_id 中

业务层 stream_name = "xrtc_985"   ← 这是 RtcStream 的 _stream_name
                                    由客户端在 JSON body 中指定
```

**为什么这是两个不同概念？**

```
Client JSON: {"stream_name":"xrtc_985", ...}
  → RtcStream::_stream_name = "xrtc_985"    // 应用层：用来 find_push_stream()
  → SDP msid 里的 stream_id = "stream_id"   // WebRTC 层：客户端自定义的 MediaStream 标签

一个 _stream_name = "xrtc_985" 的 PushStream，
其 remote SDP 里可能 msid 的 stream_id 是 "stream_id" 或其他任意值。
两者没有对应关系。
```

---

## 2. 概念层次总览

从底层到顶层：

```
SSRC (uint32_t)
  └─ Track (StreamParams struct)  ── 一组 SSRC 的容器
       └─ MediaContentDescription  ── 一个 m= section 的 tracks
            └─ PeerConnection      ── 一个 PC 的音频+视频 tracks
                 └─ RtcStream      ── 业务层 stream（PushStream / PullStream）
```

## 3. SSRC — 最底层的 RTP 流标识

**定义**：SSRC（Synchronization Source）是一个 32 位无符号整数，RFC 3550 定义，唯一标识 RTP 会话中的一个媒体源。它的值由客户端随机生成（如 `2074346944`）。

### 3.1 信令层 — SDP 中的 SSRC

每个 SSRC 通过 `a=ssrc:<ssrc-id> <attribute>:<value>` 行（RFC 5576）携带属性，SSRC 之间的关系通过 `a=ssrc-group:<semantics> <ssrc-id> ...` 行（RFC 5888）描述。

属性表：

| 属性 | 示例 | 含义 |
|------|------|------|
| `cname` | `cname:fMSlSokjQnGXlqQs` | 规范端点标识符，同一端点的所有 SSRC 共享同一个 cname |
| `msid` | `msid:stream_id audio_label` | `<MediaStream ID> <Track ID>`，将 SSRC 归属到特定 Track |
| `mslabel` | `mslabel:stream_id` | MediaStream 的 label，与 `msid` 的第一段相同（历史冗余） |
| `label` | `label:audio_label` | Track 的 label，与 `msid` 的第二段相同（历史冗余） |

### 3.2 数据面 — RTP 包头中的 SSRC

```
RTP Header: [V|P|X|CC|M|PT] [Sequence Number] [Timestamp] [SSRC (bytes 8-11)]
```

`parse_rtp_ssrc()` 从 raw RTP packet 的 byte 8 读取 uint32。

**SSRC 在信令和数据面保持一致**。客户端在 answer SDP 中声明用 SSRC=2074346944 发送音频，它实际的 RTP 包头里就写 2074346944。服务端转发的 RTP 包保持这个 SSRC 不变。

## 4. Track（StreamParams）— SSRC 的容器

### 4.1 数据结构

```cpp
// stream_params.h
struct SsrcGroup {
    std::string semantics;         // e.g. "FID"
    std::vector<uint32_t> ssrcs;   // the SSRCs in this group
};

struct StreamParams {
    std::string id;                    // track ID (= SDP label / msid 第二段)
    std::vector<uint32_t> ssrcs;       // all SSRCs belonging to this track
    std::vector<SsrcGroup> ssrc_groups; // SSRC groups (e.g. FID for RTX)
    std::string cname;                 // canonical endpoint identifier
    std::string stream_id;             // which MediaStream this track belongs to
};
```

**虽然叫 StreamParams，它就是 Track。** 参考代码直接写：
```cpp
std::vector<StreamParams> audio_tracks; // 对应 webrtc::AudioTrack
std::vector<StreamParams> video_tracks; // 对应 webrtc::VideoTrack
```

### 4.2 为什么一个 Track 有多个 SSRC？

从真实 SDP 可见 video track 有 2 个 SSRC——主 H264 流用 `2620414317`，RTX 重传流用 `255283571`。通过 `SsrcGroup{semantics="FID", ssrcs=[2620414317, 255283571]}` 关联。

audio track 只有 1 个 SSRC，因为 opus 音频用内置 FEC 处理丢包，不需要 RTX。

### 4.3 Track id 和 stream_id 的含义

```
a=msid:stream_id audio_label
        ↑           ↑
    stream_id       id (track_id)
```

- `id`：Track 自身的标签。同一个 Track 的所有 SSRC 共享同一个 label。SDP 解析时用它来分组
- `stream_id`：Track 所属的 WebRTC MediaStream 的标识。同一端点的所有 Track 通常共享同一个 stream_id

## 5. Stream — 业务层会话

### 5.1 不是 WebRTC MediaStream

RtcStream 是服务端的业务抽象，代表一个完整的 peer connection 会话：

```
RtcStream (基类，拥有 PeerConnection)
  ├─ PushStream  (recv-only：接收推流端的媒体)
  └─ PullStream  (send-only：发送媒体给拉流端)
```

### 5.2 两个 "stream" 概念辨析

```
Client JSON body:  {"stream_name":"xrtc_985", ...}
                     → RtcStream::_stream_name = "xrtc_985"

SDP msid line:     a=msid:stream_id audio_label
                     → StreamParams::stream_id = "stream_id"
                     → StreamParams::id        = "audio_label"
```

| 概念 | 变量 | 来源 | 用途 |
|------|------|------|------|
| 业务流名 | `RtcStream::_stream_name` | JSON `stream_name` 字段 | 关联 Push/Pull，如 `find_push_stream("xrtc_985")` |
| MediaStream ID | `StreamParams::stream_id` | SDP `msid` 第一段 | WebRTC 层概念，值由客户端决定 |
| Track ID | `StreamParams::id` | SDP `label` / msid 第二段 | 分组 SSRC 的 key |
| SSRC | `uint32_t` | SDP `a=ssrc:` 行 | RTP 流标识 |

### 5.3 PushStream（推流端，recv-only）

```
create_offer() → send_audio=false, send_video=false, recv_audio=true, recv_video=true
```

- 接收推流端的媒体。其 offer SDP 中 `a=recvonly`，**不包含 SSRC 行**
- `get_audio_source()` / `get_video_source()` — 从 remote SDP 提取 tracks，为 PullStream 提供媒体源

### 5.4 PullStream（拉流端，send-only）

```
create_offer() → send_audio=true, send_video=true, recv_audio=false, recv_video=false
```

- 向拉流端发送媒体。其 offer SDP 中 `a=sendonly`，**必须包含 SSRC 行**
- `add_audio_source()` / `add_video_source()` — 注入来自 PushStream 的 tracks，offer 时写给客户端

## 6. 推拉流转发——完整数据流

### 6.1 SDP 信令流

```
Publisher (sendonly)
  │  answer SDP: a=ssrc:2074346944 label:audio_label ...
  ▼
PushStream::set_remote_sdp(answer)
  │  parse_ssrc_info()      → 解析每条 a=ssrc: → SsrcInfo
  │  parse_ssrc_group_info()→ 解析 a=ssrc-group: → SsrcGroup
  │  create_track_from_ssrc_info() → 按 label 分组 → StreamParams
  │
  │  audio_track: {id="audio_label", ssrcs=[2074346944], cname="fMSlSokj..."}
  │  video_track: {id="video_label", ssrcs=[2620414317, 255283571],
  │                ssrc_groups=[{FID, [2620414317, 255283571]}], cname="fMSlSokj..."}
  ▼
RtcStreamManager::create_pull_stream("xrtc_985")
  │  push_stream->get_audio_source()  → [audio_track]
  │  push_stream->get_video_source()  → [video_track]
  │  pull_stream->add_audio_source([audio_track])
  │  pull_stream->add_video_source([video_track])
  ▼
PullStream::create_offer()
  │  PeerConnection::create_offer()
  │    audio_content->add_stream(audio_track)
  │    video_content->add_stream(video_track)
  │  SessionDescription::to_string()
  │    build_ssrc() → 写入 a=ssrc: 和 a=ssrc-group: 行
  ▼
Consumer (recvonly)
  offer SDP: a=ssrc:2074346944 label:audio_label ...  (来自 PullStream)
```

### 6.2 媒体数据面（Phase 13 内容，此处只给路径）

```
Publisher RTP (SSRC=2074346944)
  → UDPPort → IceConnection → DtlsTransport(decrypt)
    → on_rtp_packet_received() → RtcStreamManager
      → PullStream::send_rtp() → DtlsTransport(encrypt)
        → UDP → Consumer (SSRC 保持 2074346944)
```

---

## 7. 核心概念速查

| 概念 | 对应类型 | 层级 | 真实示例 |
|------|---------|------|---------|
| SSRC | `uint32_t` | RTP | `2074346944` |
| SSRC 属性 | SDP `a=ssrc:` 行 | 信令 | `a=ssrc:2074346944 cname:fMSlSokjQnGXlqQs` |
| SSRC 组 | `SsrcGroup` | 信令 | `FID [2620414317, 255283571]` |
| Track | `StreamParams` | 信令 | `{id="audio_label", stream_id="stream_id", ssrcs=[2074346944]}` |
| MediaStream | `StreamParams::stream_id` | WebRTC | `"stream_id"` |
| 业务流 | `RtcStream::_stream_name` | 应用 | `"xrtc_985"` |
| PushStream | `PushStream` | 应用 | recv-only，接收推流 |
| PullStream | `PullStream` | 应用 | send-only，发送给拉流端 |
