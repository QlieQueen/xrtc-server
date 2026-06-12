# Phase 14 背景知识：加密发送 + 流转发 + 联调

## 核心概念：SRTP 加密发送路径

Phase 13 完成了**接收解密路径**：UDP 收到加密包 → 解复用 → `srtp_unprotect` 解密 → 信号转发到 RtcStreamManager。

Phase 14 完成**加密发送路径**：RtcStreamManager 收到明文 RTP → 下发到对应流 → `srtp_protect` 加密 → UDP 发出。

### 1.1 加解密对称性

```
Phase 13 (收): unprotect_rtp → srtp_unprotect → 原地解密 → auth_tag 被截掉
Phase 14 (发): protect_rtp  → srtp_protect  → 原地加密 → auth_tag 被追加
```

两者都是原地操作，但关键区别：

| | unprotect (解密) | protect (加密) |
|---|---|---|
| 输入 buffer | 加密包 (RTP + encrypted payload + auth_tag) | 明文包 (RTP + payload) |
| 输出 buffer | 明文包 (RTP + payload) — 变短 | 加密包 (RTP + encrypted payload + auth_tag) — 变长 |
| buffer 容量 | in_len 够用 | max_len 必须 >= in_len + auth_tag_len |
| 返回值 out_len | 缩短后的长度 | 加密后的长度 (in_len + auth_tag_len) |

### 1.2 SRTP 加密后包结构 (`srtp_protect`)

```
加密前 (明文): [RTP Header 12B] [Payload N bytes]              len = 12+N
加密后 (SRTP): [RTP Header 12B] [Encrypted Payload N bytes] [Auth Tag 10B]
                                                             len = 12+N+10
```

RTP Header 不加密（SRTP 只加密 payload），所以中间的传输层和网络设备仍能读取 sequence number、SSRC 等字段。

RTCP 类似，但 auth tag 长度为 `_rtcp_auth_tag_len + sizeof(uint32_t)`，因为 SRTCP 多了 4 字节的 SRTCP index。

## 2. 发送路径调用链

```
RtcStreamManager::on_rtp_packet_received()
    │  push stream → 找到 pull stream
    │
    ▼
PullStream::send_rtp() / PushStream::send_rtp()
    │
    ▼
RtcStream::send_rtp(data, len)
    │
    ▼
PeerConnection::send_rtp(data, len)
    │
    ▼
TransportController::send_rtp("audio", data, len)
    │  _get_dtls_srtp_transport("audio")
    │
    ▼
DtlsSrtpTransport::send_rtp(data, len)
    │  1. 分配带 auth_tag 空间的 CopyOnWriteBuffer
    │  2. protect_rtp() — srtp_protect 原地加密
    │  3. _rtp_dtls_transport->send_packet() → UDP 发出
    │
    ▼
DtlsTransport::send_packet(data, len)
    │
    ▼
IceTransportChannel::send_packet(data, len) → UDP socket
```

## 3. 流转发逻辑

```
push stream (推流端) → RTP/RTCP 收到 → 查找同名 pull stream → 转发
pull stream (拉流端) → RTCP 收到 → 查找同名 push stream → 转发 (RTCP 双向对称)
```

RTCP 的特殊之处：它是双向的。推流端发来的 RTCP 要转发给拉流端，拉流端发来的 RTCP 也要转发给推流端。

## 4. protect_rtp / protect_rtcp (srtp_protect)

```cpp
bool SrtpSession::protect_rtp(void* p, int in_len, int max_len, int* out_len) {
    // 1. 自动计算需要的输出长度 = in_len + _rtp_auth_tag_len
    // 2. 检查 max_len >= need_len，不够则失败
    // 3. *out_len = in_len; srtp_protect(_session, p, out_len);
    //    srtp_protect 原地修改，out_len 变长 = in_len + auth_tag_len
}

bool SrtpSession::protect_rtcp(void* p, int in_len, int max_len, int* out_len) {
    // 同上，但 need_len = in_len + _rtcp_auth_tag_len + sizeof(uint32_t)
    // RTCP 的 auth tag 后面还有 4 字节 SRTCP index
}
```

## 5. 发送 buffer 空间分配

加密前需要预先分配足够的 buffer 空间来容纳 auth tag：

```cpp
// RTP 发送 buffer: 明文大小 + auth_tag_len
rtc::CopyOnWriteBuffer packet(buf, size, size + rtp_auth_tag_len);

// RTCP 发送 buffer: 明文大小 + auth_tag_len + 4 (SRTCP index)
rtc::CopyOnWriteBuffer packet(buf, size, size + rtcp_auth_tag_len + sizeof(uint32_t));
```

`CopyOnWriteBuffer(buf, size, capacity)` 构造：初始化内容为 `buf` 的前 `size` 字节，但内部 buffer 预分配 `capacity`。

## 6. get_auth_tag_len / get_send_auth_tag_len

加密前需要知道 `_send_session` 的 auth_tag_len 来预分配 buffer。调用链：

```
DtlsSrtpTransport::send_rtp()
  → get_send_auth_tag_len(&rtp_auth_tag_len, nullptr)  // SrtpTransport
    → _send_session->get_auth_tag_len(&rtp, &rtcp)     // SrtpSession
      → *_rtp_auth_tag_len / *_rtcp_auth_tag_len       // 读取 _do_set_key 存的
```

## 7. TransportController 管理 DtlsSrtpTransport

和 DtlsTransport 一样的按名索引管理：

```cpp
std::map<std::string, DtlsSrtpTransport*> _dtls_srtp_transport_by_name;

void _add_dtls_srtp_transport(DtlsSrtpTransport* dtls_srtp);
DtlsSrtpTransport* _get_dtls_srtp_transport(const std::string& transport_name);
```

在 `set_local_description` 创建 DtlsSrtpTransport 后注册，在析构函数中统一释放。

## 8. 异常处理与完善 (commit 5a1e89b)

- **SrtpSession 析构**：销毁 `_session` (srtp_ctx_t)，递减引用计数，到 0 时 `srtp_shutdown()`
- **RtcStream::_on_connection_state** 增加 `k_connected` 日志
- **RtcStream::destroy** 方法：主动销毁流的接口

## 9. 联调修正 (commit 8e8a514)

- **SessionDescription**：修复获取 direction 的逻辑
- **RTCP 转发**：只转发到 k_push 类型的流（修正 commit 1 的逻辑）
- **目标流不存在**：打印 warning 而非静默忽略

## 10. 和 Phase 13 的关系

| | Phase 13 (收) | Phase 14 (发) |
|---|---|---|
| 核心操作 | `srtp_unprotect` | `srtp_protect` |
| 数据方向 | UDP → 应用层 | 应用层 → UDP |
| buffer 变化 | 变短 (去掉 auth_tag) | 变长 (追加 auth_tag) |
| SrtpSession | `_recv_session` | `_send_session` |
| 信号链路 | signal_read_packet 向上 | send_rtp 向下 |

## 11. Phase 14 参考文件

| 文件 | 改动内容 |
|------|---------|
| `pc/srtp_session.h/.cpp` | protect_rtp/protect_rtcp + get_auth_tag_len + 析构清理 |
| `pc/srtp_transport.h/.cpp` | protect_rtp/protect_rtcp 代理 + get_send_auth_tag_len |
| `pc/dtls_srtp_transport.h/.cpp` | send_rtp/send_rtcp 完整实现 |
| `pc/dtls_transport.h/.cpp` | send_packet 方法 |
| `pc/transport_controller.h/.cpp` | DtlsSrtpTransport 管理 + send_rtp/send_rtcp |
| `pc/peer_connection.h/.cpp` | send_rtp/send_rtcp |
| `stream/rtc_stream.h/.cpp` | send_rtp/send_rtcp + destroy |
| `stream/rtc_stream_manager.h/.cpp` | 流转发逻辑（push → pull） |
| `stream/pull_stream.cpp` | 停止拉流处理 |
| `server/signaling_worker.h/.cpp` | STOP_PULL 命令处理 |
| `server/rtc_worker.h/.cpp` | RtcStreamManager 获取/销毁 pull stream |

## 12. 关键注意点

1. **protect 前必须预留 auth_tag 空间** — buffer 不够大 srtp_protect 会失败
2. **protect 是原地操作** — 和 unprotect 一样，不分配新内存
3. **RTCP protect 需要额外 4 字节** — SRTCP index 占 sizeof(uint32_t)
4. **send_packet 不走 DTLS 握手** — 直接通过 ICE channel 发 UDP，DTLS 只加解密
5. **RTCP 双向转发** — push→pull 和 pull→push 都要转发 RTCP
6. **析构时 srtp_shutdown** — 最后一个 SrtpSession 销毁时清理 libsrtp 全局状态
