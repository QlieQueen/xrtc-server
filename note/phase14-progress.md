# Phase 14 进度 (断点备忘)

## 当前进度

- ✅ commit 1: 转发 RTP 骨架
- ✅ commit 2: 加密 RTP 发送 (protect_rtp)
- ✅ commit 3: 加密 RTP + send_packet + push→pull RTP 转发 (合并提交 1.5.107)
- ⬜ commit 4: STOP_PULL 命令 (84b1752)
- ⬜ commit 5: 异常处理、代码完善 (5a1e89b)
- ⬜ commit 6: 联调通过 (8e8a514)

## 当前阻塞问题

视频不渲染，原因是 RTCP 双向转发未实现。Pull 端收不到 I 帧 → 发 PLI → `on_rtcp_packet_received` 空实现 → PLI 丢弃 → 推流端不发 I 帧 → 死循环。

## 下次继续的任务

RTCP 完整链路需要补：

| 文件 | 新增 |
|------|------|
| `SrtpSession` | `protect_rtcp` — `srtp_protect_rtcp` 包装 (need_len = in_len + _rtcp_auth_tag_len + sizeof(uint32_t)) |
| `SrtpTransport` | `protect_rtcp` — 代理到 `_send_session` |
| `DtlsSrtpTransport` | `send_rtcp` — buffer 分配 + encrypt + send_packet |
| `TransportController` | `send_rtcp` |
| `PeerConnection` | `send_rtcp` |
| `RtcStream` | `send_rtcp` |
| `RtcStreamManager` | RTCP 双向转发: push→pull (push stream RTCP → pull stream) + pull→push (pull stream PLI → push stream) |

参考: `/home/ydqun/workspace/lession/xrtcserver` commit `3372f65`

**Why:** RTCP 缺失导致 pull 端的 PLI 请求无法到达 push 端，推流端不发送 I 帧，视频永远渲染不了。

**How to apply:** 下次从 `RtcStreamManager::on_rtcp_packet_received` 入口开始，按调用链逐层补 `protect_rtcp` + `send_rtcp`。

参考: note/phase13-summary.md, note/phase14-background.md
