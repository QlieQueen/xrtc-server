# Phase 10 背景：DTLS 握手 + SRTP

## 1. 本 Phase 解决什么问题

Phase 9 结束后，ICE 连接可以收发 UDP 数据。但这些数据是**明文**的——RTP 媒体流和 DTLS 握手包都裸露在网络上。

本 Phase 在 ICE 之上叠加 DTLS 层，完成：
1. DTLS 握手 → 两端互相认证，派生对称密钥
2. 从 DTLS 导出 SRTP 密钥 → 加密/解密 RTP/RTCP 媒体流

## 2. DTLS Record 层格式

DTLS（Datagram TLS）是 UDP 上的 TLS，数据以 Record 为单位传输。每个 Record 有 13 字节固定头部：

```
Byte 0:     ContentType      20=ChangeCipherSpec, 21=Alert, 22=Handshake, 23=ApplicationData
                             合法范围: 20~63
Byte 1-2:   Version          DTLS 1.2 = 0xFEFD
Byte 3-4:   Epoch            0=初始, 1=加密
Byte 5-10:  SequenceNumber   6 字节, 防重放
Byte 11-12: Length           Payload 长度 (大端)
Byte 13+:   Payload          变长
```

### 2.1 如何识别 DTLS 包

```cpp
bool is_dtls_packet(buf, len) {
    return len >= 13 && (buf[0] > 19 && buf[0] < 64);
    // ContentType 必须在 20..63 之间
}
```

### 2.2 如何识别 ClientHello

ClientHello 是 DTLS 握手的第一个包。Record 头部后面是 Handshake 子协议头：

```
Byte 0-12:   DTLS Record Header
Byte 13:     HandshakeType    1 = ClientHello
Byte 14-16:  HandshakeLength  3 字节 (大端)
Byte 17+:    ClientHello 体
```

```cpp
bool is_dtls_client_hello_packet(buf, len) {
    // ContentType == 22 (Handshake) 且 HandshakeType == 1 (ClientHello)
    return len > 17 && (buf[0] == 22 && buf[13] == 1);
}
```

`> 17` 确保至少有 Record Header(13) + Handshake T(1) + Handshake L(3) = 17 字节。

## 3. StreamInterfaceChannel — ICE 与 OpenSSL 的适配器

OpenSSL 通过 `rtc::StreamInterface` 抽象来读写数据。`StreamInterfaceChannel` 继承这个接口，把 OpenSSL 的"读/写"请求适配到实际的 ICE 连接上。

```
OpenSSL (SSL_Read / SSL_Write)
  └─ rtc::SSLStreamAdapter
       └─ rtc::StreamInterface      ← 抽象接口
            └─ StreamInterfaceChannel ← 我们的适配器
                 ├─ Read()  → 从 BufferQueue 取数据
                 ├─ Write() → ice_channel->send_packet()
                 └─ on_received_packet() → 往 BufferQueue 塞数据
```

**为什么需要这个适配器？** OpenSSL 不认识 `IceConnection` 也不认识 `IceTransportChannel`。它只知道 `StreamInterface::Read(buf, len)`。适配器把 ICE 层的收包变成 BufferQueue，把 OpenSSL 的 Write 变成 ICE 的发送。

### 3.1 `_dtls` 与 `_downward` 的关系（易混淆点）

**不是同一个东西：**

- **`_dtls`** (`SSLStreamAdapter`) — OpenSSL 引擎，负责 DTLS 握手协议和加密/解密
- **`_downward`** (`StreamInterfaceChannel*`) — ICE 适配器裸指针，负责缓冲队列和数据收发

`_dtls` **拥有** `_downward` 的所有权：

```cpp
std::unique_ptr<StreamInterfaceChannel> downward = std::make_unique<StreamInterfaceChannel>(_channel);
_downward = downward.get();  // 保存裸指针
_dtls = rtc::SSLStreamAdapter::Create(std::move(downward));  // 所有权转移给 _dtls
```

保留 `_downward` 裸指针的原因是：`_handle_dtls_packet` 需要调用 `_downward->on_received_packet()` 往 BufferQueue 灌数据，这个方法在 `StreamInterfaceChannel` 上，不在 `rtc::StreamInterface` 接口里，通过 `_dtls` 调不到。

### 3.2 OpenSSL 如何调用到 Read()：BIO 回调链

`_dtls` 构造时把 `_downward` 包进 OpenSSL 的 BIO（Basic I/O）层：

```cpp
// openssl_stream_adapter.cc 构造函数
stream_->SignalEvent.connect(this, &OpenSSLStreamAdapter::OnEvent);

// 初始化时创建 BIO
static BIO* BIO_new_stream(StreamInterface* stream) {
    BIO* ret = BIO_new(BIO_stream_method());
    BIO_set_data(ret, stream);  // 把 StreamInterfaceChannel* 存进 BIO
    return ret;
}
```

OpenSSL 读数据时的完整调用链：

```
OpenSSL SSL_read()
  → BIO_read()
    → stream_read(BIO* b, ...)           // openssl_stream_adapter.cc:207
      → stream = BIO_get_data(b)         // 取出之前存的 StreamInterfaceChannel*  // :211
        → stream->Read(out, ...)         // 调用你的 Read()                      // :215
          → _packets.ReadFront()         // 从 BufferQueue 取数据
```

**数据方向总结：**

```
灌数据（生产）:
  _handle_dtls_packet → _downward->on_received_packet() → BufferQueue → SignalEvent(SE_READ)

取数据（消费）:
  OpenSSL → BIO → stream_read() → _downward->Read() → BufferQueue
```

`on_received_packet` 是注入方法（生产），`Read` 是消费方法。`SignalEvent(SE_READ)` 是通知机制——OpenSSL 不知道 BufferQueue 里有数据，必须由 `on_received_packet` 发射信号唤醒 OpenSSL 来消费。`SignalEvent` 不是同步调用 `Read()`，而是异步唤醒 OpenSSL 的握手循环，OpenSSL 被唤醒后去调 `SSL_read` → `Read()`。如果 BufferQueue 为空，`Read()` 返回 `SR_BLOCK` → `SSL_ERROR_WANT_READ`，OpenSSL 退回去等下一个信号。

### 3.3 为什么需要订阅 ICE 的 writable 状态变化

DTLS 启动条件之一是 `_channel->writable()`。ICE 连通是异步的——以下场景会丢启动时机：

```
_setup_dtls() → _maybe_start_dtls() → ICE 还没 writable → 跳过
      ↓
  过了几秒...
      ↓
  ICE 通了 → 没人再调 _maybe_start_dtls() → DTLS 永远不启动
```

`_on_writable_state` 补这个缺口。ICE 变 writable 时自动收到通知：

| `_dtls_state` | `_dtls_active` | 行为 |
|--------------|----------------|------|
| — | false | 直接 mirror ICE writable（DTLS 还没准备好） |
| `k_new` | true | `_maybe_start_dtls()`（DTLS 已准备好，终于等到 ICE 通） |
| `k_connected` | true | mirror ICE writable（DTLS 已在跑） |
| 其他 | — | 不处理 |

注意：此时 `_maybe_start_dtls` 内部有 guard `if (_dtls && _channel->writable())`，所以即使 `k_new` 时 `_dtls` 还不存在也是安全的。

## 4. DTLS 握手完整流程

### 4.1 组件关系

```
DtlsTransport                          ← DTLS 状态机
  ├── _dtls (SSLStreamAdapter)         ← OpenSSL 封装
  ├── _downward (StreamInterfaceChannel) ← ICE 适配器
  ├── _catched_client_hello            ← ClientHello 缓存
  ├── _local_certificate               ← 本地证书 (从 TransportController 传入)
  └── _remote_fingerprint              ← 对端指纹 (从 SDP answer 解析)
```

### 4.2 时序

```
客户端                                服务端
  │                                    │
  │──── ClientHello ────────────────→   │ ICE 还没 ready → _catched_client_hello 缓存
  │                                    │
  │   [客户端发送 SDP answer]           │
  │──── SDP answer (fingerprint) ──→   │ set_remote_fingerprint() → 指纹存入
  │                                    │ _setup_dtls() → 创建 SSLStreamAdapter
  │                                    │ _maybe_start_dtls() → 检查 ICE writable
  │                                    │
  │   [ICE writable = true]            │
  │                                    │ StartSSL() → OpenSSL 开始握手
  │                                    │ 重放 _catched_client_hello → dtls->ProcessData()
  │                                    │
  │←──── ServerHello + Certificate ─── │
  │──── ClientKeyExchange ──────────→  │
  │←──── Finished ──────────────────── │
  │                                    │
  │                       握手完成     │ DTLS state = k_connected
  │                                    │ 导出 SRTP 密钥
```

### 4.3 关键点：ClientHello 缓存

ClientHello 可能在 ICE 连通之前就到达。如果不缓存：
- ICE 连通后，OpenSSL 没收到 ClientHello → 握手永远不启动
- 客户端不会重发 ClientHello（除非超时重传，但这不可靠）

所以**必须缓存**，等 DTLS 准备好后重放给 OpenSSL。

### 4.4 关键点：条件启动

`_maybe_start_dtls()` 三个条件缺一不可：
1. `_dtls` 已创建（_setup_dtls 完成）
2. `_ice_channel->writable()` — ICE 路径可用
3. 指纹已设置

## 5. DTLS → SRTP 密钥派生

DTLS 握手完成后，双方拥有相同的对称密钥材料。SRTP 密钥**不单独协商**，而是从 DTLS 握手导出：

```
DTLS 握手 → 主密钥
                │
                ├─ SRTP 加密密钥
                ├─ SRTP 认证密钥
                └─ SRTP Salt
```

OpenSSL 通过 `SSL_export_keying_material` 导出。rtcbase 封装为 `SSLStreamAdapter::ExportKeyingMaterial(label, ...)`，其中 label 固定为 `"EXTRACTOR-dtls_srtp"`。

## 6. DtlsSrtpTransport — 最后的胶水层

Phase 10 最后一个 commit 引入 `DtlsSrtpTransport`，它是 DTLS 和 SRTP 的胶水：

```
DtlsSrtpTransport
  ├── DtlsTransport*                ← DTLS 握手层
  └── SrtpTransport*                ← SRTP 加密/解密层
```

职责：
- 监听 DTLS 握手完成 → 导出 SRTP 密钥 → 设置 SrtpTransport
- 监听 DTLS 收到应用数据（ContentType=23）→ 转发给 SrtpTransport 解密 → 上层
- 监听上层要发送 RTP 数据 → SrtpTransport 加密 → DTLS 发送（通过 ICE）

## 7. 涉及的 rtcbase 组件

| 组件 | 用途 |
|------|------|
| `rtc::SSLStreamAdapter` | DTLS 实现，封装 OpenSSL |
| `rtc::StreamInterface` | 抽象 I/O 接口 |
| `rtc::BufferQueue` | 线程安全的 fifo 队列 |
| `rtc::RTCCertificate` | X.509 证书 |
| `rtc::SSL_MODE_DTLS` | 模式：DTLS (非 TLS) |
| `rtc::SSL_PROTOCOL_DTLS_12` | DTLS 1.2 |
| `rtc::SSL_SERVER` | 服务端角色 |
| `rtc::Buffer` | 二进制缓冲区 |

## 8. Phase 10 参考 commits（12 个）

| # | Commit | 内容 |
|---|--------|------|
| 1 | `b01ce7f` | 封装 DtlsTransport 类 |
| 2 | `2e08091` | 缓存 ClientHello 包 |
| 3 | `045d357` | 安装 DTLS — StreamInterfaceChannel + _setup_dtls |
| 4 | `504560b` | 设置本地证书 |
| 5 | `cce2dfc` | 设置远程指纹 |
| 6 | `7786a0f` | 启动 DTLS + _handle_dtls_packet |
| 7 | `2d7ec3c` | 实现 DTLS 数据读取 — BufferQueue |
| 8 | `257e6fa` | 实现 DTLS 数据写入 — ice_channel->send_packet |
| 9 | `36e9a85` | 设置 SRTP 密码套件 |
| 10 | `e36132d` | 设置 DTLS 传输状态信号 |
| 11 | `f5719ec` | 设置 DTLS 接收状态 |
| 12 | `092c650` | 创建 DtlsSrtpTransport 加密传输通道 |

## 9. 你已有代码的承接点

| 你已有的 | Phase 10 要用 |
|----------|-------------|
| `TransportController` | `set_local_description` 中创建 DtlsTransport（commit 1 已完成） |
| `TransportController::set_local_certificate` | 传入 `DtlsTransport::set_local_certificate` |
| `IceTransportChannel::signal_read_packet` | DtlsTransport 订阅（commit 1 已完成） |
| `SessionDescription` 解析 | 从中提取 fingerprint |
| `IceTransportChannel::set_remote_ice_params` | 已存在，DTLS 启动的前置条件 |
