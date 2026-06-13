# Phase 14 进度 (完成)

## 最终状态: 6/6 commits ✅

| # | commit | 内容 | 状态 |
|---|--------|------|------|
| 1 | `1.5.105` | 转发 RTP 数据骨架 | ✅ |
| 2 | `1.5.106` | 加密 RTP 发送 | ✅ |
| 3 | `1.5.107-108` | 加密 RTP + RTCP + 双向转发 | ✅ |
| 4 | `1.5.109` | 停止拉流 | ✅ |
| 5 | `1.5.110` | SrtpSession 析构 + ICE 超时保护 | ✅ |
| 6 | `1.5.111` | on_stream_exception 替代直接 delete | ✅ |

## 关键 bug 修复

1. **send_rtcp 调了 protect_rtp** — `dtls_srtp_transport.cpp:84` 误用 RTP 加密函数导致客户端 auth fail
2. **RTCP 双向转发** — pull→push 方向缺失导致 PLI 无法到达推流端，视频不渲染

## Phase 14 全链路完成

```
UDP 加密包
  → IceTransportChannel
    → DtlsTransport
      → DtlsSrtpTransport::_on_read_packet (解复用)
        → unprotect (解密)
        → signal → TransportController → PeerConnection → RtcStream → RtcStreamManager
          → 转发:
            push RTP  → pull->send_rtp  → protect_rtp  → DtlsTransport::send_packet → UDP
            push RTCP → pull->send_rtcp → protect_rtcp → DtlsTransport::send_packet → UDP
            pull RTCP → push->send_rtcp → protect_rtcp → DtlsTransport::send_packet → UDP
```
