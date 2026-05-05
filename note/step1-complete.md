# Step 1 完成总结 — PUSH 返回带 codec 的 SDP offer

## 目标

客户端发 PUSH 请求 → xrtc-server 返回一个有真实 codec 信息的 SDP offer 文本。

---

## 完整数据流

```
signaling 服务 (Go) ──TCP/9000──→ xrtc-server
                                        │
                        1. SignalingWorker 收到 TCP 数据
                        2. 读 xhead 头部（36字节），校验 magic_num
                        3. 读 body（JSON），解析出 cmdno=1（PUSH）
                        4. 构造 RtcMsg → 发给 RtcServer
                        5. RtcServer CRC32(stream_name) 路由到 RtcWorker
                        6. RtcWorker → RtcStreamManager::create_push_stream()
                                            ↓
                              7. 创建 PushStream 对象
                              8. PushStream::create_offer()
                                            ↓
                              9. 构造 SessionDescription 对象
                                 ├── AudioContentDescription（opus 111）
                                 ├── VideoContentDescription（H264 96 + rtx 97）
                                 ├── BUNDLE group
                                 ├── 方向设 recvonly（服务端是接收方）
                                 └── to_string() 序列化成 SDP 文本
                                            ↓
                             10. SDP 文本原路返回(SignalingWorker) → signaling 服务 → 客户端
```

---

## 新增 / 修改的文件

### 新建 4 个文件

| 文件 | 作用 |
|------|------|
| `src/stream/rtc_stream.h` | 所有流（Push/Pull）的基类，声明 `create_offer()` 纯虚函数 |
| `src/stream/rtc_stream.cpp` | 基类构造函数，存储 `uid` / `stream_name` / `audio` / `video` / `log_id` |
| `src/stream/push_stream.h` | 推流类声明，继承 `RtcStream` |
| `src/stream/push_stream.cpp` | `create_offer()` 核心实现：构造 `SessionDescription` → 填入 codec → `to_string()` |

### 修改 2 个文件

| 文件 | 变更 |
|------|------|
| `src/pc/session_description.h` | `MediaContentDescription` 增加 `add_codec()` 公共方法 |
| `src/stream/rtc_stream_manager.cpp` | `create_push_stream()` 创建 `PushStream` 并调用 `create_offer()` |

---

## 类间关系

```
RtcStream（基类）
  └── PushStream（子类）
        └── create_offer()
              └── SessionDescription（SDP 构建器）
                    ├── AudioContentDescription
                    │     └── CodecInfo（opus/PCMU/PCMA）
                    ├── VideoContentDescription
                    │     └── CodecInfo（H264/rtx）
                    └── to_string() → 标准 SDP 文本
```

---

## 关键设计决策

| 决策 | 原因 |
|------|------|
| `PushStream::create_offer()` 直接构造 SessionDescription，不是调用 PeerConnection | 当前还没有 PeerConnection 对象（那是 Step 6 的事），直接构造 SDP 文本更简单 |
| 方向用 `k_recv_only` | 推流端（客户端）是发送方，服务端是接收方。SDP 里写 `a=recvonly` |
| 没有 SSRC | 只有**发送方**需要在 SDP 中声明 SSRC，接收方不需要。服务端既然是 `recvonly`，就不需要声明 SSRC |
| `AudioContentDescription` 构造函数自带 opus(111) | 构造函数中已经添加了 opus，`create_offer()` 中通过 `add_codec()` 可以追加 PCMU/PCMA |
| `VideoContentDescription` 构造函数自带 H264(96) + rtx(97) | 已经包含了主要视频 codec 和重传流 |
| 不加 ICE/DTLS 信息 | 这是 Step 2/3 的目标，Step 1 只关注 codec 信息 |

---

## PushStream::create_offer() 伪代码

```
SessionDescription offer(SdpType::k_offer)
ContentGroup bundle_group("BUNDLE")

if (_audio):
    auto audio = new AudioContentDescription()
    audio.set_direction(k_recv_only)
    audio.set_rtcp_mux(true)
    // 构造函数已含 opus(111)，可选追加 PCMU(0) / PCMA(8)
    offer.add_content(audio)
    bundle_group.add_content_name("audio")

if (_video):
    auto video = new VideoContentDescription()
    video.set_direction(k_recv_only)
    video.set_rtcp_mux(true)
    // 构造函数已含 H264(96) + rtx(97)
    offer.add_content(video)
    bundle_group.add_content_name("video")

offer.add_group(bundle_group)
return offer.to_string()
```

---

## SessionDescription::to_string() 输出结构

```
v=0
o=- 0 2 IN IP4 127.0.0.0    ← 会话信息
s=-                          ← 会话名称
t=0 0                        ← 活动时间（永久）
a=msid-semantic: WMS

m=audio 9 UDP/TLS/RTP/SAVPF 111   ← 音频媒体行
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=mid:audio
a=recvonly                         ← 服务端只接收
a=rtcp-mux
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=fmtp:111 minptime=10;useinbandfec=1

m=video 9 UDP/TLS/RTP/SAVPF 96 97 ← 视频媒体行
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=mid:video
a=recvonly
a=rtcp-mux
a=rtpmap:96 H264/90000
a=rtcp-fb:96 goog-remb
a=rtcp-fb:96 transport-cc
a=rtcp-fb:96 ccm fir
a=rtcp-fb:96 nack
a=rtcp-fb:96 nack pli
a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f
a=rtpmap:97 rtx/90000
a=fmtp:97 apt=96
```

---

## 验证方法

启动 xrtc-server，用 Python 模拟 signaling 服务发送 xhead 协议请求：

```python
import struct, json, socket

body = json.dumps({"cmdno": 1, "uid": 12345,
    "stream_name": "test", "audio": 1, "video": 1})
hdr = struct.pack('<HHI16sIII', 0, 0, 1001,
    b'\x00'*16, 0xfb202202, 0, len(body))

s = socket.socket()
s.connect(('127.0.0.1', 9000))
s.sendall(hdr + body.encode())

# 读响应
data = s.recv(36)
body_len = struct.unpack_from('<I', data, 32)[0]
resp = s.recv(body_len)
print(resp.decode())
s.close()
```

预期：JSON 响应中包含 `err_no: 0` 和 `offer` 字段，offer 是合法的 SDP 文本。

---

## 已修复的问题

- `session_description.cpp` 第 288 行 `"0=-"` → `"o=-"`（o 被误写成了数字 0）

---

## 下一步预告

Step 2 将在 SDP 中加入 ICE 传输信息：
- `a=ice-ufrag:xxxx` / `a=ice-pwd:yyyy`
- `a=candidate:1 1 UDP 2130706431 192.168.x.x 54321 typ host`

这样客户端收到 SDP 后才知道往哪个 IP:端口发 UDP 数据。
