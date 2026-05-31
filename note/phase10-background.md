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

### 2.3 如何识别 RTP 包

```cpp
bool is_rtp_packet(buf, len) {
    // RTP version == 2 (bits 6-7), 最小头 12 字节
    return len >= 12 && ((buf[0] & 0xC0) == 0x80);
}
```

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

`on_received_packet` 是注入方法（生产），`Read` 是消费方法。`SignalEvent(SE_READ)` 是通知机制——OpenSSL 不知道 BufferQueue 里有数据，必须由 `on_received_packet` 发射信号唤醒 OpenSSL 来消费。`SignalEvent` 不是同步调用 `Read()`，而是异步唤醒 OpenSSL 的握手循环。如果 BufferQueue 为空，`Read()` 返回 `SR_BLOCK` → `SSL_ERROR_WANT_READ`，OpenSSL 退回去等下一个信号。

### 3.3 两个 SignalEvent 的区别（核心混淆点）

`StreamInterfaceChannel` 和 `SSLStreamAdapter` 各有一个 `SignalEvent`，方向相反，极易混淆：

| 谁发射 | 谁订阅 | 方向 | 含义 |
|--------|--------|------|------|
| **我们**调 `_downward->SignalEvent(SE_READ)` | `_dtls`(OpenSSLStreamAdapter) 订阅 | → 通知 `_dtls` | "我把加密数据写入 BufferQueue 了，你来读" |
| **`_dtls`** 调 `SignalEvent(SE_OPEN/READ/CLOSE)` | **我们** (DtlsTransport) 订阅 | ← 通知我们 | "握手完成 / 有解密数据 / 关闭" |

虽然都叫 `SignalEvent`，但区别在**谁点火、谁接收**：
- 往下灌加密数据 → 我们点火，`_dtls` 接收
- 往上收解密数据 → `_dtls` 点火，我们接收

```cpp
// _setup_dtls 中订阅两个信号：
_dtls->SignalEvent.connect(this, &DtlsTransport::_on_dtls_event);
_dtls->SignalSSLHandshakeError.connect(this, &DtlsTransport::_on_dtls_handshake_error);
```

### 3.4 `_dtls->SignalEvent` 的三种事件

| 事件 | 含义 | DtlsTransport 行为 |
|------|------|-------------------|
| `SE_OPEN` | DTLS 握手完成 | `_set_writable_state(true)` + `_set_dtls_state(k_connected)` |
| `SE_READ` | 有解密数据可读 | 循环 `_dtls->Read()` 读出解密后的明文 |
| `SE_CLOSE` | 连接关闭 | `k_closed`(无 error) 或 `k_failed`(有 error) |

### 3.5 数据的两条路径（加密 vs 解密）

```
加密方向（DTLS 握手包 / 加密 RTP）：
  ICE → _on_read_packet → _handle_dtls_packet → _downward->on_received_packet()
    → BufferQueue → _downward->SignalEvent(SE_READ) 通知 _dtls
      → _dtls Read 消费

解密方向（握手完成后解密数据）：
  _dtls 解密完成 → _dtls->SignalEvent(SE_READ) 通知我们
    → _on_dtls_event → _dtls->Read() 读出明文
```

注意：DTLS 握手完成后客户端发送的是 SRTP 加密的 RTP（非 DTLS ApplicationData），所以 `_on_read_packet` 收到的是非 DTLS 包 → 走 RTP 分支 → `is_rtp_packet` 校验 → `signal_read_packet` 向上层转发。解密后的数据走 `_dtls->Read()` 是从 DTLS 控制通道读出来的（Alert 等控制消息），跟 RTP 数据不在同一条路径上。

### 3.6 为什么需要订阅 ICE 的 writable 状态变化

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

### 3.7 为什么接收状态也要 mirror

ICE 层有 `receiving` 状态（对端在发数据），DtlsTransport 通过 `_on_receiving_state` 订阅 ICE 的 `signal_receiving_state`，mirror 到 `_receiving`。上层（PeerConnection）需要用它来聚合整体 PC 状态。

### 3.8 为什么抓包"少了 Encrypted Alert"

抓包中缺少的 "Encrypted Alert" 是 DTLS 的 `close_notify` Alert（ContentType=21）。客户端没有调用 `pc->Close()`，进程退出时 DTLS 连接被 OS 直接回收，不会发送 close_notify。服务端因此收不到 `_dtls->Read() == SR_EOS`，`_on_dtls_event(SE_CLOSE)` 也不会触发。

客户端需要：`pc->Close()` 或者在析构函数中关闭 PeerConnection。

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

### 4.2 时序（三种路径）

**路径 A — answer SDP 先到（正常）：**

```
answer → _setup_dtls → _maybe_start_dtls (ICE 未通，跳过)
ClientHello 到 → 缓存
ICE writable → _on_writable_state → _maybe_start_dtls → StartSSL
  → replay ClientHello → 握手开始
```

**路径 B — ClientHello 先到：**

```
ClientHello 到 → _on_read_packet(k_new) → 缓存 + _setup_dtls (指纹仍空)
answer 到 → set_remote_fingerprint → 补设指纹到已有 _dtls
ICE writable → _on_writable_state → _maybe_start_dtls → StartSSL
```

**握手完成 → k_connected → 接收 RTP：**

```
SE_OPEN → _set_dtls_state(k_connected) → 解开 RTP k_connected 守卫
  → RTP 包经 _on_read_packet → is_rtp_packet 校验 → signal_read_packet 向上层转发
```

### 4.3 关键点：ClientHello 缓存

ClientHello 可能在 ICE 连通之前就到达。如果不缓存：
- ICE 连通后，OpenSSL 没收到 ClientHello → 握手永远不启动
- 客户端不会重发 ClientHello（除非超时重传，但这不可靠）

### 4.4 关键点：条件启动

`_maybe_start_dtls()` 三个条件缺一不可：
1. `_dtls` 已创建（_setup_dtls 完成）
2. `_channel->writable()` — ICE 路径可用
3. 指纹已设置

### 4.5 关键点：k_connected 守卫 RTP

握手完成前不能接收 RTP——在此之前的数据要么是 DTLS 握手包，要么是不该出现的包。`_on_read_packet` 里 RTP 分支用 `_dtls_state != k_connected` 守卫，直到 `_on_dtls_event` 收到 `SE_OPEN` 才解开。

## 5. DTLS → SRTP 密钥派生

DTLS 握手完成后，双方拥有相同的对称密钥材料。SRTP 密钥不单独协商，而是从 DTLS 握手导出：

```
DTLS 握手 → 主密钥
                │
                ├─ SRTP 加密密钥
                ├─ SRTP 认证密钥
                └─ SRTP Salt
```

OpenSSL 通过 `SSL_export_keying_material` 导出。rtcbase 封装为 `SSLStreamAdapter::ExportKeyingMaterial(label, ...)`，其中 label 固定为 `"EXTRACTOR-dtls_srtp"`。

`SetDtlsSrtpCryptoSuites` 告诉 OpenSSL 握手完成后需要导出哪些 SRTP 密码套件的密钥材料。不设置的话 DTLS 握手能完成但 SRTP 密钥导不出来。

## 6. DtlsTransportState 状态机

```
k_new → (StartSSL) → k_connecting → (SE_OPEN) → k_connected
  ↓                      ↓                         ↓
k_failed              k_failed              k_closed / k_failed
```

## 7. DtlsSrtpTransport — 最后的胶水层

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

## 8. Phase 10 完成总结

### 实现 commits (11/12)

| # | 你的 commit | 参考 commit | 内容 |
|---|-----------|-----------|------|
| 1 | 1.5.70 | `b01ce7f` | DtlsTransport 封装 + signal_read_packet 链路 |
| 2 | 1.5.71 | `2e08091` | ClientHello 缓存 |
| 3 | 1.5.72 | `045d357` | 安装 DTLS — StreamInterfaceChannel + _setup_dtls |
| 4 | 1.5.73 | `504560b` | 设置本地证书 + _dtls_active |
| 5 | 1.5.74 | `cce2dfc` | 设置远程指纹（两条时序路径） |
| 6 | 1.5.75 | `7786a0f` | 启动 DTLS — _maybe_start_dtls + ClientHello replay + _handle_dtls_packet |
| 7 | 1.5.76 | `2d7ec3c` | DTLS 数据读取 — BufferQueue + _on_writable_state |
| 8 | 1.5.77 | `257e6fa` | DTLS 数据写入 — Write() + ICE send_packet 链路 + _on_read_packet 扩展 |
| 9 | 1.5.78 | `36e9a85` | SRTP 密码套件 + is_rtp_packet + signal_read_packet |
| 10 | 1.5.79 | `e36132d` | DTLS 传输状态 — _on_dtls_event SE_OPEN/SE_READ/SE_CLOSE + k_connected/k_closed |
| 11 | 1.5.80 | `f5719ec` | DTLS 接收状态 — _on_receiving_state mirror ICE receiving |
| 12 | — | `092c650` | DtlsSrtpTransport（留到 Phase 11） |

### 核心理解

1. **`_dtls` vs `_downward`**：前者拥有后者所有权，裸指针用于 `on_received_packet` 注入
2. **BIO 回调链**：`BIO_get_data` → `stream->Read()` → `BufferQueue`
3. **两个 SignalEvent**：`_downward->SignalEvent` 通知 OpenSSL 来读，`_dtls->SignalEvent` 通知我们握手完成/解密数据就绪
4. **`_on_writable_state`**：补偿 ICE 延迟连通的 DTLS 启动时机
5. **`_on_dtls_event` SE_OPEN**：握手完成 → `k_connected` → 解开 RTP 守卫
6. **远程关闭检测**：需要客户端调用 `pc->Close()` 发送 close_notify
