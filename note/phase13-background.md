# Phase 13 背景知识：SRTP 加解密

## 核心概念：DTLS-SRTP（RFC 5764）

**DTLS 只做握手 + 密钥交换，不做媒体加密。**

```
握手阶段：DTLS 握手 → 导出 SRTP 密钥
媒体阶段：RTP → SRTP 加密 → 直接走 UDP（不经过 DTLS）
```

不是"DTLS 加密 + SRTP 加密"双层结构。DTLS 握手完成后，数据面只用 SRTP。

## 1. 数据流全景

```
推流端 SRTP 加密包
    │
    ▼ UDP
IceTransportChannel::_on_read_packet()
    │
    ▼ signal_read_packet
DtlsTransport::_on_read_packet()
    │  is_dtls_packet()? → NO（SRTP 包不是 DTLS Record）
    │  is_rtp_packet()?  → YES
    │
    ▼ signal_read_packet
DtlsSrtpTransport::_on_read_packet()
    │  infer_rtp_packet_type() → RTP or RTCP
    │
    ├─ RTP:  srtp_unprotect()  → signal_rtp_packet_received
    └─ RTCP: srtp_unprotect_rtcp() → signal_rtcp_packet_received
         │
         ▼
    TransportController → PeerConnection → RtcStreamManager → PullStream.send_rtp()
         │
         ▼ SRTP 加密
    DtlsSrtpTransport::send_rtp()
         │  srtp_protect()
         │  _rtp_dtls_transport->send_packet()
         ▼
    IceTransportChannel → UDP → 拉流端
```

## 2. DTLS 如何导出 SRTP 密钥

DTLS 握手完成后，通过 `SSL_export_keying_material`（RFC 5705）导出密钥材料：

```
export_keying_material("EXTRACTOR-dtls_srtp", ...)
  → raw key material: client_write_key(16) + server_write_key(16)
                     + client_write_salt(12) + server_write_salt(12)
```

xrtc-server 作为 DTLS server：
- **发送密钥** = server_write_key + server_write_salt
- **接收密钥** = client_write_key + client_write_salt

以 `SRTP_AEAD_AES_128_GCM` 为例：key=16 字节，salt=12 字节，合起来 28 字节。

## 3. 类层次结构

```
SrtpTransport (基类)
  ├─ _send_session (SrtpSession*)
  ├─ _recv_session (SrtpSession*)
  ├─ set_rtp_params() — 创建 send/recv session
  └─ protect/unprotect 代理
       │
       ▼
DtlsSrtpTransport (子类)
  ├─ _rtp_dtls_transport (DtlsTransport*)    — 不继承，持有指针
  ├─ _rtcp_dtls_transport (DtlsTransport*)   — RTCP 独立通道（rtcp-mux 时为 nullptr）
  ├─ _extract_params() — DTLS 导出密钥
  ├─ _setup_dtls_srtp() — 创建 SRTP session
  ├─ _on_read_packet() — RTP/RTCP 解复用 + 解密
  ├─ send_rtp() / send_rtcp() — 加密 + 发送
  └─ signal_rtp/rtcp_packet_received — 解密后的原始数据
```

### 创建时序（TransportController::set_local_description）

```
1. IceAgent::create_channel()        → IceTransportChannel
2. new DtlsTransport(ice_channel)    → 处理 DTLS 握手
3. new DtlsSrtpTransport("audio")    → 包装 DTLS
4. dtls_srtp->set_dtls_transport(dtls)
       │ 订阅 dtls 的 signal_dtls_state
       │ 订阅 dtls 的 signal_read_packet
       ▼
5. DTLS 握手完成 → signal_dtls_state(k_connected)
       │
       ▼
6. _maybe_setup_dtls_srtp()
     → _extract_params() — 导出密钥
     → set_rtp_params() — 创建 SrtpSession(send) + SrtpSession(recv)
```

## 4. SrtpSession — libsrtp 封装

```cpp
class SrtpSession {
    srtp_ctx_t* _session;
    
    set_send(crypto_suite, key, key_len)   → srtp_create(ssrc_any_outbound)
    set_recv(crypto_suite, key, key_len)   → srtp_create(ssrc_any_inbound)
    protect_rtp(buf, in_len, max_len, out_len)  → srtp_protect()
    unprotect_rtp(buf, in_len, out_len)         → srtp_unprotect()
    protect_rtcp(...)                           → srtp_protect_rtcp()
    unprotect_rtcp(...)                         → srtp_unprotect_rtcp()
};
```

libsrtp 初始化是引用计数的——第一个 SrtpSession 创建时 `srtp_init()`，最后一个销毁时 `srtp_shutdown()`。

## 5. RTP/RTCP 包格式与解复用

### 5.1 RTP 包头（RFC 3550 Section 5.1）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            contributing source (CSRC) identifiers             |
|                             ....                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         payload ...                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Byte 0, bits 6-7**: Version (V) = 2
- **Byte 0, bit 5**: Padding (P) — 1 表示尾部有填充
- **Byte 0, bit 4**: Extension (X) — 1 表示有扩展头
- **Byte 0, bits 0-3**: CSRC Count (CC)
- **Byte 1, bit 7**: Marker (M) — 帧边界标记
- **Byte 1, bits 0-6**: Payload Type (PT) — RTP 负载类型，动态范围 96~127
- **Byte 2-3**: Sequence Number
- **Byte 4-7**: Timestamp
- **Byte 8-11**: SSRC
- **最小长度**: 12 字节（固定头）

### 5.2 RTCP 包头（RFC 3550 Section 6.1）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|    RC   |   PT          |             length            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         packet-specific data                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Byte 0, bits 6-7**: Version (V) = 2
- **Byte 0, bit 5**: Padding (P)
- **Byte 0, bits 0-4**: Reception Report Count (RC)
- **Byte 1 (full 8 bits)**: Payload Type (PT) — RTCP 包类型
  - SR = 200 (Sender Report)
  - RR = 201 (Receiver Report)
  - SDES = 202
  - BYE = 203
  - APP = 204
- **Byte 2-3**: Length (以 4 字节为单位，不含自身)
- **最小长度**: 4 字节

### 5.3 RTP vs RTCP 区分规则（RFC 5761 Section 4）

RTP 和 RTCP 复用同一 UDP 端口时，靠 PT 字段的低 7 位区分：

```
RTCP PT (完整 8 bit) = 192 ~ 223
RTCP PT (低 7 bit)   = 192 & 0x7F ~ 223 & 0x7F = 64 ~ 95

RTP PT (低 7 bit)    = 96 ~ 127
```

判断逻辑:

```
1. 长度 >= 12 或 >= 4
2. Byte 0 >> 6 == 2（version bits）
3. (Byte 1 & 0x7F) ∈ [64, 96) → RTCP
   (Byte 1 & 0x7F) ∉ [64, 96) → RTP
```

## 6. 和 Phase 10/11 的关系

Phase 10 完成了 `DtlsTransport`（DTLS 握手）。Phase 13 在 DTLS 之上加 SRTP 层：

| 组件 | Phase | 职责 |
|------|-------|------|
| `DtlsTransport` | 10 | DTLS 握手、导出密钥材料 |
| `DtlsSrtpTransport` | 13 | SRTP 加解密、RTP/RTCP 解复用 |
| `SrtpSession` | 13 | libsrtp 封装 |

当前 `signal_read_packet` 直接转发原始 RTP（Phase 10 的 `is_rtp_packet` + `k_connected` 守卫）。Phase 13 会插入 `DtlsSrtpTransport` 做 SRTP 解密后再向上传。

## 7. Phase 13 参考文件

| 文件 | 内容 |
|------|------|
| `pc/dtls_srtp_transport.h/.cpp` | DtlsSrtpTransport 完整实现 |
| `pc/srtp_transport.h/.cpp` | SrtpTransport 基类 |
| `pc/srtp_session.h/.cpp` | SrtpSession — libsrtp 封装 |
| `pc/transport_controller.h/.cpp` | 创建 DtlsSrtpTransport、send_rtp/send_rtcp |
| `module/rtp_rtcp/rtp_utils.h/.cpp` | RTP/RTCP 解复用 |

## 8. 关键注意点

1. **DTLS 不加密媒体数据**——握手后媒体走 SRTP 直接在 UDP 上传输
2. **DtlsSrtpTransport 不继承 DtlsTransport**——它持有 DtlsTransport 指针，订阅其信号
3. **服务器是 DTLS server role**——`send_key = server_write`、`recv_key = client_write`
4. **rtcp-mux 时不需要 RTCP 独立 DTLS 通道**——`_rtcp_dtls_transport = nullptr`
5. **SRTP 解保后包变小**——auth tag 被去除，in_len > out_len
