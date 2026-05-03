# Step 1 背景知识：WebRTC 中的 SDP

## SDP 在 WebRTC 里是干嘛的

两个端（浏览器↔服务端）要建立音视频通话，**必须先通过信令通道交换 SDP**。

SDP 的本质是一个**媒体能力协商清单**：
> "我能发 H264，我收 Opus 音频，我用 SSRC=0x1234 发视频包，我的端口是 9…"

双方交换 SDP 后，如果能找到两端都支持的 codec，媒体通道就建立起来。

整个信令交互流程：**offer → answer → ICE + DTLS → 媒体数据**

Step 1 要做的就是 **第一步：生成 offer SDP**。

---

## SDP 的逐行解释

以推流场景下服务端生成的 offer SDP 为例：

```
v=0                                          ← SDP 版本号，固定
o=- 0 0 IN IP4 0.0.0.0                      ← 会话发起者（origin），推流服务不需要真实 IP
s=-                                          ← 会话名称，固定
t=0 0                                        ← 会话活动时间，0 表示永久
a=group:BUNDLE audio video                   ← 音频和视频走同一个 RTP 通道（BUNDLE 优化）

m=audio 9 UDP/TLS/RTP/SAVPF 0 8 111         ← 音频媒体行
  ↑       ↑      ↑              ↑
 媒体类型 端口   传输协议        payload type 列表（0=PCMU, 8=PCMA, 111=opus）

a=mid:audio                                  ← 媒体标识，和 a=group 里的名字对应
a=rtpmap:0 PCMU/8000                         ← payload type 0 对应 PCMU，采样率 8000
a=rtpmap:8 PCMA/8000
a=rtpmap:111 opus/48000/2                    ← 双声道
a=fmtp:111 minptime=10;useinbandfec=1        ← Opus 的参数
a=ssrc:12345678 cname:streamaudio            ← 音频流的 SSRC 标识
a=ssrc:12345678 msid:audio audio
a=sendonly                                   ← 方向：推流只发送不收

m=video 9 UDP/TLS/RTP/SAVPF 96              ← 视频媒体行
a=mid:video
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=42e01f
a=ssrc:87654321 cname:streamvideo
a=ssrc:87654321 msid:video video
a=sendonly
```

---

## 每个 SDP 行的含义

| SDP 行 | 含义 |
|--------|------|
| `v=` | SDP 版本号，固定为 0 |
| `o=` | 会话发起者信息（username, session_id, version, network_type, address_type, address） |
| `s=` | 会话名称 |
| `t=` | 会话活动时间（起止时间戳，0 0 表示永久） |
| `a=group:BUNDLE …` | 多个媒体流复用同一个传输通道 |
| `m=` | **媒体描述行**：<media> <port> <transport> <fmt_list> |
| `a=mid:` | 媒体标识符，与 BUNDLE group 对应 |
| `a=rtpmap:` | RTP 载荷映射：`<payload_type> <encoding_name>/<clock_rate>[/<channels>]` |
| `a=fmtp:` | 格式参数：`<payload_type> <param_name>=<value>;…` |
| `a=ssrc:` | SSRC 属性：`<ssrc> <attribute>[:<value>]` |
| `a=sendonly` | 方向属性：sendonly / recvonly / sendrecv / inactive |

### `m=` 行详解

```
m=audio 9 UDP/TLS/RTP/SAVPF 0 8 111
  ↑     ↑       ↑              ↑
 媒体  端口   传输协议         RTP payload type 列表（空格分隔）
```

传输协议固定为 `UDP/TLS/RTP/SAVPF`，这是 WebRTC 的标准：
- **AVP** = Audio/Video Profile（基本 RTP）
- **SAVP** = Secure AVP（加 SRTP 加密）
- **AVPF** = AVP with Feedback（加 RTCP 反馈）
- **SAVPF** = SAVPF = SRTP + RTCP Feedback，是 WebRTC 使用的完整组合

端口写 9 是惯例（WebRTC 实际端口由 ICE UDP 端口决定，`m=` 行的端口仅作占位）。

---

## FeedBackParam 对应 SDP 中的哪部分

它在 SDP 中对应 `a=rtcp-fb:` 行：

```
a=rtcp-fb:96 nack                           ← 支持丢包重传
a=rtcp-fb:96 nack pli                       ← 支持关键帧请求
a=rtcp-fb:96 goog-remb                      ← 支持接收端码率估计
```

`FeedBackParam` 构造方式：
- `FeedBackParam("nack")` → `a=rtcp-fb:96 nack`
- `FeedBackParam("nack", "pli")` → `a=rtcp-fb:96 nack pli`

> 当前 Step 1 暂不填充 feedback_param，后续需要时才加。

---

## CodecParam 对应 SDP 中的哪部分

`CodecParam` 是 `map<string, string>`，对应 `a=fmtp:` 行中的 key=value 对：

```
a=fmtp:96 packetization-mode=1;profile-level-id=42e01f
CodecParam["packetization-mode"] = "1";
CodecParam["profile-level-id"]   = "42e01f";
```

---

## 完整的 m= 媒体段结构

一个 `m=` 行及其下属的所有 `a=` 行，合起来称为一个 **媒体段（media section）**。下面是完整的结构：

```
m=audio 9 UDP/TLS/RTP/SAVPF 0 8 111        ← 媒体行：类型 端口 协议 payload_type_list
c=IN IP4 0.0.0.0                             ← 连接数据（WebRTC 中通常填 0.0.0.0）
a=mid:audio                                   ← 媒体标识符
a=ice-ufrag:xxx                               ← ICE 用户名（Step 2 加）
a=ice-pwd:yyy                                 ← ICE 密码（Step 2 加）
a=fingerprint:sha-256 AA:BB:...               ← DTLS 证书指纹（Step 3 加）
a=setup:actpass                                ← DTLS 角色（Step 3 加）
a=rtcp-mux                                    ← RTCP 复用 RTP 通道
a=sendonly                                     ← 方向属性

-- 每个 codec 的属性 --
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:111 opus/48000/2
a=fmtp:111 minptime=10;useinbandfec=1
a=rtcp-fb:111 nack                             ← codec 级别的 RTCP 反馈（后续再加）

-- 每个 SSRC 的属性 --
a=ssrc:12345678 cname:streamaudio
a=ssrc:12345678 msid:audio audio
```

### 属性按作用范围分类

| 范围 | 属性 | 说明 |
|------|------|------|
| **媒体段级** | `a=mid:`, `a=sendonly`, `a=ice-ufrag`, `a=ice-pwd`, `a=fingerprint`, `a=setup`, `a=rtcp-mux` | 影响整个 m= 行 |
| **codec 级** | `a=rtpmap:`, `a=fmtp:`, `a=rtcp-fb:` | 跟在 m= 行后面，属于某个 payload type |
| **SSRC 级** | `a=ssrc:`, `a=ssrc-group:` | 描述某个 RTP 流的属性 |

### m= 行的完整语法

```
m=<media> <port> <proto> <fmt> ...
  ↑        ↑       ↑        ↑
 音频/视频 端口  传输协议  payload type 编号列表
```

- `<media>`：`audio` 或 `video`（也可能是 `application` 用于数据通道）
- `<port>`：WebRTC 中写 `9`（ICE 层决定实际端口）
- `<proto>`：固定 `UDP/TLS/RTP/SAVPF`
- `<fmt>`：空格分隔的 RTP payload type 编号

---

## 音频 m= 行的特点

```
m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8 126
                       payload types: 111=opus, 0=PCMU, 8=PCMA, 126=telephone-event

a=rtpmap:111 opus/48000/2         ← ★ 音频特有：/channels 后缀（2=立体声）
a=rtpmap:0 PCMU/8000              ←    PCMU 固定 pt=0，不需要 /channels（默认1）
a=rtpmap:8 PCMA/8000              ←    PCMA 固定 pt=8
a=rtpmap:126 telephone-event/8000 ←    电话按键音

a=fmtp:111 minptime=10;useinbandfec=1  ← opus 编码参数：
                                         minptime=10 最短打包10ms
                                         useinbandfec=1 带内前向纠错
a=fmtp:126 0-16                     ← telephone-event 支持的事件编号

a=ssrc:12345678 cname:streamaudio
a=ssrc:12345678 msid:audio audio
a=sendonly
```

### 音频的特殊点总结

| 特点 | 说明 |
|------|------|
| **固定 payload type** | PCMU=0, PCMA=8 是固定的，opus=111 等是动态分配的 |
| **/channels 后缀** | `a=rtpmap:111 opus/48000/2` 最后一个 `/2` 表示双声道 |
| **fmtp 参数不同** | opus 关心 `minptime` 和 `useinbandfec`，不需要 profile-level-id |
| **通常不需要 rtcp-fb** | 音频对丢包有一定容忍度，不需要 nack/pli 等反馈 |
| **采样率** | PCMU/PCMA 是 8000，opus 是 48000 |

---

## 视频 m= 行的特点

```
m=video 9 UDP/TLS/RTP/SAVPF 96 97 98        ← 全是动态 payload type
                                             96=H264, 97=VP8, 98=VP9

a=rtpmap:96 H264/90000                       ← ★ 视频特有：没有 /channels
a=rtpmap:97 VP8/90000                        ←    视频的 clockrate 固定 90000
a=rtpmap:98 VP9/90000

a=fmtp:96 packetization-mode=1;profile-level-id=42e01f  ← H264 特有参数：
                                                          packetization-mode=1: 非交织模式
                                                          profile-level-id: 档次/级别

a=rtcp-fb:96 nack                             ← 视频需要丢包重传
a=rtcp-fb:96 nack pli                         ← 关键帧请求（收到后发 IDR 帧）
a=rtcp-fb:96 goog-remb                        ← 码率估计反馈
a=rtcp-fb:96 ccm fir                          ← 全内请求（也是关键帧请求）

a=ssrc:87654321 cname:streamvideo
a=ssrc:87654321 msid:video video
a=sendonly
```

### 视频的特殊点总结

| 特点 | 说明 |
|------|------|
| **全是动态 payload type** | 视频没有固定 pt，96 及以上都是动态可协商的 |
| **无 /channels** | `a=rtpmap:96 H264/90000` 没有 /channels |
| **clockrate 固定 90000** | 所有视频 codec 的时钟频率都是 90000Hz |
| **H264 的 fmtp** | `packetization-mode`（1=单NAL单元模式）、`profile-level-id`（如 42e01f = Baseline 4.1） |
| **需要 RTCP 反馈** | nack（丢包重传）, pli（关键帧请求）, remb（码率估计） |
| **可有多层 simulcast** | 多个 SSRC 通过 `a=ssrc-group:SIM` 分组 |

---

## 代码中的对应关系

当 `to_string()` 处理一个 `MediaContentDescription` 时：

```cpp
// 伪代码：session_description.cpp 中的 to_string()

// m= 行：遍历 _codecs 收集 pt 列表
ss << "m=" << (type == MEDIA_TYPE_AUDIO ? "audio" : "video")
   << " 9 UDP/TLS/RTP/SAVPF"
   << collect_pt_list(_codecs)                    // 遍历 codecs 写所有 id
   << "\r\n";

// a=rtpmap：每个 codec 一行
for (auto& codec : _codecs) {
    ss << "a=rtpmap:" << codec->id << " "
       << codec->name << "/" << codec->clockrate;
    if (type == MEDIA_TYPE_AUDIO && channels > 1)  // ★ 音频特有 /channels
        ss << "/" << channels;
    ss << "\r\n";
}

// a=fmtp：有 codec_param 的才输出
for (auto& codec : _codecs) {
    if (!codec->codec_param.empty()) {
        ss << "a=fmtp:" << codec->id;
        for (auto& [k, v] : codec->codec_param)
            ss << " " << k << "=" << v << ";";
        ss << "\r\n";
    }
}

// a=ssrc：遍历 _send_streams
for (auto& stream : _send_streams)
    for (auto ssrc : stream.ssrcs) { ... }

// a=sendonly/recvonly
if (_direction == k_send_only) ss << "a=sendonly\r\n";
```

---

## 一个完整的 SDP 最终长什么样

推流场景，Step 1 要生成的最终结果：

```
v=0
o=- 12345678 0 IN IP4 0.0.0.0
s=-
t=0 0
a=group:BUNDLE audio video

m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8
a=mid:audio
a=rtpmap:111 opus/48000/2
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=ssrc:12345678 cname:streamaudio
a=ssrc:12345678 msid:audio audio
a=sendonly

m=video 9 UDP/TLS/RTP/SAVPF 96
a=mid:video
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=42e01f;
a=ssrc:87654321 cname:streamvideo
a=ssrc:87654321 msid:video video
a=sendonly
```

**Step 1 只需要上面这些**（没有 ICE、DTLS、rtcp-fb）。这正是 `to_string()` 的输出目标。

---

## SSRC、Stream、Track 三者的层级关系

这是 WebRTC 中最容易混淆的一组概念，理清楚了后面写代码就很顺畅。

### 一句话总结

```
一个 PeerConnection（连接）→ 包含多个 MediaStream（媒体流）
→ 每个 MediaStream 包含多个 MediaStreamTrack（媒体轨：音频轨/视频轨）
→ 每个 Track 由一个或多个 SSRC 标识的 RTP 流承载
```

### 在 SDP 中的对应

看下面这段 SDP：

```
m=audio 9 UDP/TLS/RTP/SAVPF 111          ← MediaSection（媒体段），对应一个 m= 行
a=mid:audio
a=msid:stream1 audio_track_1              ← MediaStream ID + MediaStreamTrack ID
a=ssrc:1000 cname:user_1                  ← SSRC + CNAME
a=ssrc:1000 msid:stream1 audio_track_1    ← SSRC → MediaStream + MediaStreamTrack 的映射
a=ssrc:1000 mslabel:stream1               ← (已废弃)
a=ssrc:1000 label:audio_track_1           ← (已废弃)

m=video 9 UDP/TLS/RTP/SAVPF 96
a=mid:video
a=msid:stream1 video_track_1
a=ssrc:2000 cname:user_1
a=ssrc:2000 msid:stream1 video_track_1
```

### 各个层级的含义

| 层级 | 类名（对应代码） | 说明 |
|------|---------------|------|
| **PeerConnection** | `PeerConnection` | 一个端到端的 WebRTC 连接，对应一个 `RTCPeerConnection` JS 对象 |
| **MediaStream** | （SDP 中用 `msid` 标识） | 一组相关轨道的集合，比如一个用户的"摄像头+麦克风"归为同一个 Stream |
| **MediaStreamTrack** | （SDP 中用 `msid:<stream_id> <track_id>` 标识） | 单个音频或视频轨道，如麦克风音频、摄像头视频、屏幕共享视频 |
| **StreamParams** | `StreamParams`（代码中的结构体） | 对应 SDP 中一个 `a=ssrc:` 块描述的**一组 SSRC 的集合**。一个音/视频轨道可以用多个 SSRC（如 simulcast 分层），它们组成一个 StreamParams |
| **SsrcGroup** | `SsrcGroup` | 一组有逻辑关系的 SSRC，如 SIM（simulcast 分层）、FID（retransmission 流） |
| **SSRC** | `uint32_t` | RTP 包头中的 32 位同步源标识符，唯一标识一路 RTP 流 |

### 为什么要有 StreamParams

假设推流端支持 Simulcast（同时发三个分辨率的视频流）：

```
m=video 9 UDP/TLS/RTP/SAVPF 96 97 98
a=ssrc:3000 cname:user1
a=ssrc:3001 cname:user1
a=ssrc:3002 cname:user1
a=ssrc-group:SIM 3000 3001 3002
```

这里三个 SSRC 属于同一个 Video Track 的三个 simulcast 层，代码中用：
- `StreamParams.ssrcs = {3000, 3001, 3002}`
- `StreamParams.ssrc_groups = [SsrcGroup("SIM", {3000, 3001, 3002})]`

### 在 xrtc-server 的代码中

```
SDP 的 m= 行
  └── MediaContentDescription（AudioContentDescription / VideoContentDescription）
        ├── _codecs: vector<CodecInfo>        ← 该媒体支持的编码格式
        ├── _direction: RtpDirection          ← 方向（sendonly/recvonly/...）
        └── _send_streams: vector<StreamParams> ← 该媒体的发送流
              ├── id: "audio" / "video"
              ├── ssrcs: {0x12345678}
              ├── ssrc_groups: []             ← 暂不使用
              ├── cname: "streamaudio"
              └── stream_id: "audio"
```

也就是说，在代码中：
- **一个 m= 行** → `AudioContentDescription` 或 `VideoContentDescription`
- **一个音频/视频流** → 往 `_send_streams` 里加一个 `StreamParams`
- **一个 SSRC** → `StreamParams.ssrcs` 中的一个元素

### 在代码中的使用场景

创建 PushStream 的 SDP 时：

```cpp
// 视频轨道：一个 SSRC 就够（没有 simulcast）
StreamParams video_stream;
video_stream.id = "video";
video_stream.ssrcs = {0x87654321};
video_stream.cname = "streamvideo";

auto video_content = std::make_shared<VideoContentDescription>();
video_content->add_stream(video_stream);
```

当后续实现 PullStream（拉流）时，你要接收客户端发来的 RTP 包，SSRC 就是用来区分来自不同推流端的依据。

---

## FAQ：代码实现中的疑问

### Q1：build_ssrc 对应 SDP 中的什么？StreamParams / track / SSRC 的关系？

**回答：**

`build_ssrc()` 负责生成 SDP 中所有 `a=ssrc:` 和 `a=ssrc-group:` 行。

**代码中数据结构的对应关系：**

```
MediaContentDescription（一个 m= 段）
  └── _send_streams: vector<StreamParams>     ← 该 m= 段中的所有 track
        └── StreamParams（一个 track，如"音频轨"或"视频轨"）
              ├── ssrcs[]                     ← 该 track 的所有 SSRC（可能多个，如 simulcast）
              ├── ssrc_groups[]               ← SSRC 分组关系
              │     └── SsrcGroup{semantics, ssrcs[]}
              ├── cname                       ← 规范名称
              ├── id                          ← track id
              └── stream_id                   ← media stream id
```

**SDP 中的实际体现：**

```
a=ssrc-group:FID 1000 1001                   ← FID 分组：主流(1000) + 重传流(1001)
a=ssrc:1000 cname:streamaudio                ← SSRC=1000 的 cname
a=ssrc:1000 msid:stream1 track1              ← SSRC=1000 所属的 media stream + track
a=ssrc:1001 cname:streamaudio                ← SSRC=1001 的 cname
a=ssrc:1001 msid:stream1 track1
```

**关键理解：**
- `StreamParams` = SDP 中的一个 **track**（音频轨或视频轨）
- 一个 track 可以有**多个 SSRC**（例如主编码流 + 重传流），它们共享同一个 cname 和 msid
- `ssrc_groups` 描述这些 SSRC 之间的逻辑关系
- **Step 1 中不会触发 `build_ssrc`**，因为 `PushStream::create_offer()` 没有调用 `add_stream()`，`streams()` 返回空列表

### Q2：SsrcGroup::semantics 是什么？

**回答：**

`semantics` 是 SSRC 分组的语义类型，定义在 RFC 5576 中，告诉接收方"这一组 SSRC 是什么关系"。

**常见的 semantics 值：**

| semantics | 含义 | 说明 |
|-----------|------|------|
| `FID` | Flow Identification | 主流 + 重传流（rtx）配对。如 H264(96) 和 rtx(97) 共用一个传输通道 |
| `FEC` | Forward Error Correction | 主码流 + FEC 冗余包配对 |
| `FEC-FR` | FEC Flow Repair | FEC 修复流 |
| `SIM` | Simulcast | 分层编码的各层 SSRC 分组 |

**SDP 中的对应：**

```
a=ssrc-group:FID 1000 1001       ← semantics = "FID"
a=ssrc-group:SIM 2000 2001 2002  ← semantics = "SIM"
```

**代码中的构造：**

```cpp
// FID 分组：主流(1000) + 重传流(1001)
SsrcGroup fid_group("FID", {1000, 1001});

// SIM 分组：三个 simulcast 层
SsrcGroup sim_group("SIM", {2000, 2001, 2002});
```

**当前 Step 1 用不到**，`SsrcGroup` 是为后续步骤（当客户端发来 ANSWER 带 SSRC 信息时）预留的。

---

## 下一步

写代码的时候对照这个文档，理解每一行 `to_string()` 输出的 SDP 对应什么含义。后面的 step 每扩展 SDP 的一个方面（ICE, DTLS, codec feedback），都会在 note 目录补充对应的背景知识。
