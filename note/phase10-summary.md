# Phase 10 总结：DTLS 握手 + SRTP

## 完成情况

10 个实现 commit（`1.5.70` ~ `1.5.80`），新增约 400 行代码，集中在 `DtlsTransport` 一个类。

## 架构全景

```
DtlsTransport                                     ← DTLS 状态机 + OpenSSL 胶水层
  │
  ├── StreamInterfaceChannel                      ← ICE ↔ OpenSSL 适配器
  │     ├── BufferQueue _packets                  ← OpenSSL 读数据的 fifo 缓冲
  │     ├── on_received_packet()                  ← 注入加密包 + SignalEvent 唤醒
  │     ├── Read()                                ← OpenSSL 消费 BufferQueue
  │     └── Write()                               ← ICE channel->send_packet()
  │
  ├── SSLStreamAdapter (_dtls)                    ← OpenSSL 引擎
  │     ├── SetIdentity / SetPeerCertificateDigest
  │     ├── SetDtlsSrtpCryptoSuites               ← SRTP 密码套件协商
  │     ├── StartSSL()                            ← 启动握手
  │     ├── SignalEvent                          ← SE_OPEN / SE_READ / SE_CLOSE
  │     └── SignalSSLHandshakeError              ← 握手失败
  │
  ├── Signal/Slot 对外接口
  │     ├── signal_dtls_state(DtlsTransportState)
  │     ├── signal_writable_state(bool)
  │     ├── signal_receiving_state(bool)
  │     ├── signal_read_packet(buf, len, ts)       ← RTP 向上转发
  │     └── signal_closed                         ← remote close
  │
  └── ICE 层订阅
        ├── signal_read_packet → _on_read_packet   ← ICE 收包
        ├── signal_writable_state_change           ← ICE 连通追踪
        └── signal_receiving_state_change          ← ICE 接收状态 mirror
```

## 组件层次

| 层 | 职责 | 关键方法 |
|----|------|---------|
| **DtlsTransport** | 生命周期管理：证书→指纹→握手→状态信号 | `set_local_certificate()`, `set_remote_fingerprint()`, `_maybe_start_dtls()`, `_on_dtls_event()` |
| **StreamInterfaceChannel** | 适配器：BufferQueue 缓冲 + ICE send_packet | `on_received_packet()`, `Read()`, `Write()` |
| **SSLStreamAdapter** | OpenSSL 封装：DTLS 握手协议 + 加密解密 | 由 rtcbase 提供 |

## 数据流

### 1. 加密方向（DTLS 握手包进入 OpenSSL）

```
ICE 收到 UDP 包
  └─ signal_read_packet → DtlsTransport::_on_read_packet()
       ├─ k_new + ClientHello → _catched_client_hello 缓存
       │                         → _setup_dtls（如证书已设）
       └─ k_connecting/k_connected + DTLS Record
            └─ _handle_dtls_packet()
                 ├─ 逐条校验 Record Header length 字段
                 └─ _downward->on_received_packet()
                      ├─ BufferQueue.WriteBack()
                      └─ SignalEvent(SE_READ) → 唤醒 OpenSSL 消费
```

### 2. 解密方向（OpenSSL 写回数据）

```
OpenSSL SSL_write()
  └─ BIO → StreamInterfaceChannel::Write()
       └─ _channel->send_packet() → ICE 发送
```

### 3. 握手完成 → 派生 SRTP 密钥

```
DTLS 握手完成
  └─ _dtls->SignalEvent(SE_OPEN) → DtlsTransport::_on_dtls_event()
       ├─ _set_writable_state(true)
       └─ _set_dtls_state(k_connected)
            └─ _on_read_packet() 现在放行 RTP/RTCP
```

## 核心难点

### 1. ClientHello 与 answer SDP 的竞态（两种时序路径）

DTLS 启动有三个条件：本地证书、远程指纹、ICE writable。但三者到达顺序不确定：

**路径 A — answer 先到（正常）：**
```
set_local_certificate → set_remote_fingerprint → _setup_dtls（指纹就绪，一把配好）
  → ICE writable → _maybe_start_dtls → StartSSL → replay ClientHello
```

**路径 B — ClientHello 先到（UDP 不等 ICE）：**
```
set_local_certificate → ClientHello → _on_read_packet 缓存 + _setup_dtls（指纹空，跳过 SetPeerCertificateDigest）
  → answer 到 → set_remote_fingerprint → 补 SetPeerCertificateDigest 到已有 _dtls
  → ICE writable → _maybe_start_dtls → StartSSL
```

`set_remote_fingerprint` 里 `if (_dtls && !fingerprint_change)` 分支专为路径 B 设计：`_dtls` 已创建但缺指纹，不重建，直接补调 `SetPeerCertificateDigest`。

### 2. 两个 SignalEvent 的区别（核心混淆点）

| 谁发射 | 谁订阅 | 方向 | 含义 |
|--------|--------|------|------|
| **我们**调 `_downward->SignalEvent(SE_READ)` | `_dtls`(OpenSSLStreamAdapter) 订阅 | → `_dtls` | "BufferQueue 有数据了，来读" |
| **`_dtls`** 调 `SignalEvent(SE_OPEN/READ/CLOSE)` | **我们**(DtlsTransport) 订阅 | ← 我们 | "握手完成/解密数据就绪/关闭" |

虽然都叫 `SignalEvent`，关键在**谁点火、谁接收**。

### 3. BIO 回调链（数据如何到达 Read）

```
OpenSSL SSL_read()
  → BIO_read()
    → stream_read(BIO* b, ...)           // openssl_stream_adapter.cc
      → stream = BIO_get_data(b)         // 取出 StreamInterfaceChannel*
        → stream->Read(out, ...)         // 调用我们的 Read()
          → _packets.ReadFront()         // 从 BufferQueue 取
```

### 4. `_dtls` 与 `_downward` 的关系

不是同一个东西：
- `_dtls` — `SSLStreamAdapter`，OpenSSL 引擎，**拥有** `_downward`
- `_downward` — `StreamInterfaceChannel*`，ICE 适配器裸指针

保留裸指针是因为 `_handle_dtls_packet` 需要调 `_downward->on_received_packet()`，这个方法在 `StreamInterfaceChannel` 上，不在 `rtc::StreamInterface` 接口里，通过 `_dtls` 调不到。

### 5. ICE writable 事件补偿

ICE 连通是异步的。如果 DTLS 先准备好而 ICE 还没 writable：

```
_setup_dtls → _maybe_start_dtls → ICE 没 writable → 跳过
...
ICE 通了 → 没人再调 _maybe_start_dtls → DTLS 永远不启动
```

`_on_writable_state` 补这个缺口。订阅 `signal_writable_state_change`，ICE 变 writable 时自动触发 `_maybe_start_dtls`（仅限 `k_new` 状态）。

### 6. 不是 STUN 就是 DTLS 就是 RTP

`_on_read_packet` 的分发逻辑：ICE 层已经把 STUN 过滤掉，到达此处的包按状态分发：

| 状态 | 包类型 | 行为 |
|------|--------|------|
| `k_new` | ClientHello | 缓存 + 可能触发 `_setup_dtls` |
| `k_new` | 其他 | 丢弃 |
| `k_connecting` / `k_connected` | DTLS Record | 注入 OpenSSL |
| `k_connected` | RTP/RTCP | `is_rtp_packet` 校验 → `signal_read_packet` 向上转发 |
| `k_connected` | 非 DTLS 非 RTP | 丢弃并警告 |

### 7. remote close 检测

SE_READ 循环中 `ret == rtc::SR_EOS` 表示对端发送了 `close_notify` Alert（正常关闭）。SE_CLOSE 事件表示连接关闭（有 error → k_failed，无 error → k_closed）。

注意：客户端直接退出（不调 pc->Close()）不会发 close_notify，服务端收不到这两种信号。抓包缺少 "Encrypted Alert" 就是这个原因。

## Bug 记录

| Bug | 表现 | 根因 |
|-----|------|------|
| `_maybe_start_dtls` 无返回值 | 编译报错 | 空函数体无 return |
| `set_remote_fingerprint` 返回 void | 调用方无法判断成功/失败 | 应返回 bool |
| `set_remote_fingerprint` 参数类型不匹配 | 编译报错 | `const unsigned char*` vs `const char*` |
| `_is_fingerprint_change` 判断用错成员 | 指纹变更检测逻辑错误 | 应为 `_remote_fingerprint_alg.size() > 0` 而非 `_remote_fingerprint_value.size() > 0`（参考 commit 后的正确实现） |
| `ice_transport_channel::receiving()` 遗漏 | 编译报错 | 新增 `_on_receiving_state` 但忘加访问器 |

## 关键设计决策

1. **所有权转移**：`StreamInterfaceChannel` 的所有权通过 `std::make_unique` → `SSLStreamAdapter::Create(std::move(...))` 转移给 `_dtls`，保留裸指针 `_downward` 用于 `on_received_packet`。
2. **`_dtls_active` 门控**：保证证书先于指纹设置。指纹未变 + `_dtls` 已存在时补调 `SetPeerCertificateDigest`，指纹变更时销毁 `_dtls` 重建。
3. **BufferQueue 容量限制**：`k_max_pending_packets = 2`，防止 OpenSSL 来不及消费时缓冲无限增长。
4. **Signal/Slot 解耦**：DtlsTransport 不直接向上层暴露状态字段，而是通过信号通知（`signal_dtls_state`、`signal_writable_state`、`signal_receiving_state`），上层 PeerConnection 订阅聚合。
5. **RTP 守卫**：`k_connected` 之前拒绝非 DTLS 包，防止握手未完成的密文被当作 RTP 转发。

## 与 Phase 9 的关系

| Phase 9 | Phase 10 |
|---------|---------|
| ICE 连通性检查 + selected 连接切换 | 在 selected 连接之上建立 DTLS 加密隧道 |
| `signal_read_packet` 发射非 STUN 的 UDP 数据 | `_on_read_packet` 订阅此信号，分发 DTLS/RTP |
| `IceTransportChannel::writable` | DTLS 启动的前置条件 |
| `IceTransportChannel::send_packet` | `StreamInterfaceChannel::Write` 的出口 |

## 下一步

Phase 10 剩余：commit 12 — 创建 DtlsSrtpTransport 胶水层。Phase 11: PC/ICE/Agent 状态聚合。
