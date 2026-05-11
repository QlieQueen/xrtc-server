# Phase 8-14 学习计划：从 STUN 到完整推拉流

## 当前状态

Phase 1-7 已完成，89 个测试全部通过，与 Windows 客户端联调验证通过：

- **Phase 1-2**: TCP xhead 解析 + 跨线程路由 (CRC32 + LockFreeQueue)
- **Phase 3**: SDP offer 生成 (codec + ICE credentials + BUNDLE)
- **Phase 4**: UDPPort + Candidate 生成 (async socket + port bind)
- **Phase 5**: DTLS fingerprint 填入 SDP
- **Phase 6**: PeerConnection + TransportController + IceAgent + IceTransportChannel
- **Phase 7**: ANSWER flow + set_remote_sdp (SSRC/SSRC group/transport info 解析)

**当前消息路径**:

```
客户端 → signaling(Go) → xhead → xrtc-server
  ├── PUSH: create_offer() → SDP offer (codec + ICE + fingerprint + candidate typ host)
  └── ANSWER: set_remote_sdp() → 解析客户端 SDP → ICE params 下发到 IceTransportChannel
```

**关键断点**: UDPPort 已在监听 (`172.17.54.49:10025`)，但 `_on_read_packet()` 是空函数 —— 客户端发的 STUN Binding Request 到达后无人处理。

---

## 参考项目

参考项目: `/home/ydqun/workspace/lession/xrtcserver`
起始 commit: `ca762a97ce02132630d1effcebffdb085288c537` (验证STUN指纹)
终点 commit: `8e8a514` (与webrtc_client联调通过)
共 76 个 commit，本节从中学习 Phase 8-14 的实现。

---

## Phase 8: STUN 消息编解码

### 学习目标

理解 STUN 协议 (RFC 5389) 的二进制格式，实现消息的读写、属性解析、MESSAGE-INTEGRITY 验证和 FINGERPRINT 校验。理解 UDPPort 如何识别 STUN 包并完成验证流水线。

### 参考 commit 范围

```
7012b73 Stun message读取
fb675f2 Stun ByteString
ba73fa3 解析并验证USERNAME
92bfad0 解析并验证MI属性
615e9ee STUN绑定请求异常处理
fde76e2 创建peer反射地址
a70fdf5 创建IceConnection
98e68ce 构造binding响应
7a08e53 添加MI属性
ef60ffe 添加指纹属性
c18f33d 发送binding响应
```

### 消息追踪

```
客户端 STUN Binding Request (UDP)
  → AsyncUdpSocket::signal_read_packet
    → UDPPort::_on_read_packet()
      → StunMessage::validate_fingerprint()     ← CRC32 校验
      → StunMessage::read(ByteBufferReader)      ← 解析 20 字节头 + 属性
      → 检查 USERNAME 属性 (local_ufrag:remote_ufrag)
      → 检查 MESSAGE-INTEGRITY (HMAC-SHA1 with ice_pwd)
      → signal_unknown_address → IceTransportChannel 处理
```

### STUN 消息格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|0 0|     STUN Message Type     |         Message Length        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Magic Cookie                          |
|                         0x2112A442                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Transaction ID (96 bits)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          Attributes                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### STUN Attributes

| Type | Name | Value Type | 用途 |
|------|------|-----------|------|
| 0x0006 | USERNAME | ByteString | `local_ufrag:remote_ufrag` |
| 0x0008 | MESSAGE-INTEGRITY | ByteString | HMAC-SHA1(ice_pwd, message) |
| 0x0020 | XOR-MAPPED-ADDRESS | Address (XOR) | 客户端映射地址 |
| 0x0024 | PRIORITY | UInt32 | candidate 优先级 |
| 0x0025 | USE-CANDIDATE | (无 value) | 提名此连接 |
| 0x8028 | FINGERPRINT | UInt32 | CRC32(message) ^ 0x5354554E |
| 0x802A | ICE-CONTROLLING | UInt64 | ICE 角色 (controlling/controlled) |

### 新增文件

| 文件 | 行数(参考) | 角色 |
|------|-----------|------|
| `src/ice/stun.h` | ~300 | StunMessage + StunAttribute 类体系声明 |
| `src/ice/stun.cpp` | ~690 | read/write, HMAC, CRC32, 属性工厂方法 |

### StunMessage 核心接口

```cpp
class StunMessage {
public:
    // 解析 UDP 数据为 STUN 消息
    bool read(rtc::ByteBufferReader* buf);

    // 序列化 STUN 消息为 UDP 数据
    bool write(rtc::ByteBufferWriter* buf) const;

    // 验证 FINGERPRINT 属性 (CRC32)
    static bool validate_fingerprint(const char* data, size_t len);
    bool add_fingerprint();

    // 验证 MESSAGE-INTEGRITY (HMAC-SHA1)
    IntegrityStatus validate_message_integrity(const std::string& password);
    bool add_message_integrity(const std::string& password);

    // 属性访问
    const StunByteStringAttribute* get_byte_string(uint16_t type);
    const StunUInt32Attribute* get_uint32_t(uint16_t type);
    void add_attribute(std::unique_ptr<StunAttribute> attr);
};
```

### StunAttribute 类体系

```
StunAttribute (抽象基类)
  ├── StunUInt32Attribute       — PRIORITY, FINGERPRINT, USE-CANDIDATE
  ├── StunUInt64Attribute       — ICE-CONTROLLING
  ├── StunByteStringAttribute   — USERNAME, MESSAGE-INTEGRITY
  ├── StunAddressAttribute      — MAPPED-ADDRESS
  ├── StunXorAddressAttribute   — XOR-MAPPED-ADDRESS
  └── StunErrorCodeAttribute    — ERROR-CODE
```

### 修改文件

| 文件 | 变更 |
|------|------|
| `src/ice/udp_port.h` | 增加 `get_stun_message()`, `send_binding_error_response()`, `create_stun_username()`, `_parse_stun_username()`, `signal_unknown_address`, `AddressMap _connections` |
| `src/ice/udp_port.cpp` | `_on_read_packet()` 实现 STUN 验证流水线 |

### UDPPort STUN 验证流水线

```
_on_read_packet(buf, len, addr):
  1. 如果 addr 已有 IceConnection → conn->on_read_packet() (Phase 9)
  2. 否则:
     a. StunMessage::validate_fingerprint(data, len)
     b. StunMessage::read(ByteBufferReader)
     c. 如果是 BINDING_REQUEST:
        - 检查 USERNAME 属性存在
        - 检查 MESSAGE-INTEGRITY 属性存在
        - _parse_stun_username(): 提取 local_ufrag, remote_ufrag
        - 验证 local_ufrag == _ice_params.ice_ufrag
        - validate_message_integrity(_ice_params.ice_pwd)
        - signal_unknown_address(port, addr, stun_msg, remote_ufrag)
     d. 任何验证失败 → send_binding_error_response()
```

### 验证点

1. 编译通过，单元测试通过
2. 启动 server，客户端连接后，`_on_read_packet` 收到 STUN 包并打印日志
3. STUN binding error response 在验证失败时正确发送

---

## Phase 9: ICE 连接状态机 + Controller + Ping 机制

### 学习目标

理解 ICE 连通性检查的全过程：创建 IceConnection、发送 STUN Binding Request (ping)、处理 Binding Response (pong)、更新连接状态、选择最佳连接、保活机制。

### 参考 commit 范围

```
a0bdda8 实现UDP包高性能发送
af25839 完成ice保活
da9e320 发送stun错误响应消息
89b5985 实现服务侧的连通性检查
97afa2c ICE传输通道的ping周期
d95f7ea ICE连接的ping优先级
c365708 选择一个连接执行ping请求
410644e 构造STUN绑定请求
6225d4f ICE普通提名和积极提名
183ca73 发送STUN ping请求
c90a868 处理STUN响应
3710be1 输出rtt和ping id
d26dce4 更新Ice连接的读写状态
f017815 实现选中连接切换策略
cb60f45 切换策略考虑连接的优先级
722f876 开始切换selected连接
a60d5ee STUN请求并处理错误响应
b808117 设置Candidate对状态
63b5f45 处理Ice ping周期问题
9bc4471 实现Ice连接探活机制
9bb997d 更新Ice传输通道的状态
```

### 消息追踪

```
UDPPort::signal_unknown_address
  → IceTransportChannel::_on_unknown_address()
    → port->create_connection(remote_candidate)     ← 创建 IceConnection
    → _add_connection(conn)
      → IceController::add_connection(conn)
    → _sort_connections_and_update_state()
    → _maybe_state_pinging()                        ← 启动 ping timer
        ↓
  TimerWatcher 触发 ice_ping_cb()
    → IceController::select_connection_to_ping()
      → 选一个未 ping 过的或需要重试的 connection
    → _ping_connection(conn)
      → conn->ping(now)
        → ConnectionRequest::send()
          → 构造 STUN BINDING_REQUEST
            - USERNAME = remote_ufrag:local_ufrag
            - PRIORITY = local_candidate.priority
            - USE-CANDIDATE (如果被选中)
            - ICE-CONTROLLING (服务端是 controlled)
          → MESSAGE-INTEGRITY + FINGERPRINT
          → UDPPort::send_to() 发送
            ↓
  客户端回复 STUN BINDING_RESPONSE
    → UDPPort::_on_read_packet()
    → conn->on_read_packet()
      → StunRequestManager::check_response()
        → ConnectionRequest::on_request_response()
          → conn->received_ping_response(rtt)
          → conn->update_receiving(now)
          → conn->set_write_state(STATE_WRITABLE)
          → signal_state_change → IceTransportChannel
            → _update_state() → k_connected
```

### IceConnection 状态机

```
              +----------+
              | WAITING  |  ← 创建但未开始 ping
              +----------+
                    |
                    ↓ (被 IceController 选中 ping)
              +------------+
              | IN_PROGRESS|  ← 已发 Binding Request，等待 Response
              +------------+
                   /    \
                  v      v
           +----------+ +-------+
           | SUCCEEDED| | FAILED|  ← 超时未收到回复
           +----------+ +-------+
```

### WriteState 状态机

```
STATE_WRITE_INIT → STATE_WRITABLE (收到 pong) → STATE_WRITE_UNRELIABLE (超时未收到) → STATE_WRITE_TIMEOUT
```

### IceTransportChannel 状态机

```
k_new → k_checking (开始 ping) → k_connected (至少一个 connection 成功) → k_completed (选路完成)
                                                                         → k_failed (全部失败)
```

### Ping 周期策略

| 状态 | 间隔 | 常量 |
|------|------|------|
| 弱连接 (weak) | 250ms | `WEAK_PING_INTERVAL` |
| 强连接 (stable) | 2500ms | `STRONG_PING_INTERVAL` |

### IceController 排序策略

`sort_and_switch_connection()` 排序优先级:

1. **nominated** (USE-CANDIDATE) + writable + receiving > 其他
2. **candidate type**: host > prflx > srflx > relay
3. **priority** 值降序
4. **RTT** 升序

### 新增文件

| 文件 | 行数(参考) | 角色 |
|------|-----------|------|
| `src/ice/ice_connection_info.h` | ~15 | `IceCandidatePairState` 枚举 |
| `src/ice/ice_connection.h` | ~125 | IceConnection: ping/pong, RTT, 状态 |
| `src/ice/ice_connection.cpp` | ~400 | 实现细节 |
| `src/ice/stun_request.h` | ~60 | StunRequest + StunRequestManager |
| `src/ice/stun_request.cpp` | ~100 | 请求管理实现 |
| `src/ice/ice_controller.h` | ~60 | 连接排序, ping 选择 |
| `src/ice/ice_controller.cpp` | ~250 | 排序策略实现 |

### 修改文件

| 文件 | 变更 |
|------|------|
| `src/ice/ice_transport_channel.h/.cpp` | 加入 `IceController`, ping timer, 状态机, STUN 信号处理, `_on_unknown_address`, `_ping_connection`, `_switch_selected_connection`, `_update_state`, `_compute_ice_transport_state`, `signal_writable_state`, `signal_receiving_state`, `signal_ice_state_changed`, `signal_read_packet` |
| `src/ice/ice_agent.h/.cpp` | 加入 `IceTransportState`, `signal_ice_state`, `_on_ice_state_changed`, `_update_state` |
| `src/ice/udp_port.h/.cpp` | 加入 `create_connection()`, `get_connection()`, `AddressMap _connections`, `_on_read_packet` 路由到 connection |

### 核心信号链

```
UDPPort::signal_unknown_address
  → IceTransportChannel::_on_unknown_address
    → create IceConnection → IceController::add_connection

IceConnection::signal_state_change
  → IceTransportChannel::_on_connection_state_change
    → _update_state → signal_ice_state_changed
      → IceAgent::_on_ice_state_changed → _update_state → signal_ice_state
        → TransportController → PeerConnection → signal_connection_state

IceConnection::signal_read_packet
  → IceTransportChannel::_on_read_packet
    → signal_read_packet → TransportController → DtlsTransport (Phase 10)
```

### 验证点

1. 编译通过，单元测试通过
2. 客户端连接后，服务端日志显示:
   - `Received BINDING REQUEST from <addr>`
   - `send BINDING REQUEST to <addr>` (服务端 ping 客户端)
   - `Received BINDING RESPONSE from <addr>` (客户端 pong)
   - ICE state: `k_new → k_checking → k_connected → k_completed`
3. `UDPPort::_on_read_packet` 将 STUN 包正确路由到已有 IceConnection

---

## Phase 10: DTLS 握手 + SRTP 加密

### 学习目标

理解 DTLS 握手如何在 ICE 连接上运行、如何从 DTLS 导出 SRTP 密钥、libsrtp2 的基本用法。

### 参考 commit 范围

```
b01ce7f 封装Dtls传输类
2e08091 缓存ClientHello包
045d357 安装DTLS
504560b 设置本地证书
cce2dfc 设置远程指纹
7786a0f 启动DTLS
2d7ec3c 实现dtls数据的读取
257e6fa 实现DTLS数据的写入
36e9a85 设置SRTP密码套件
e36132d 设置Dtls传输状态
f5719ec 设置Dtls接收状态
c442539 创建Srtp会话并设置参数
7f79ec1 引入libsrtp库
ad8bbde 初始化libsrtp库
79057c3 创建srtp上下文结构
c68cac3 完成srtp的设置和更新方法
a177c95 开始安装DTLS-SRTP
d52a949 从DTLS中导出密钥
092c650 创建Dtls加密传输通道
```

### 消息追踪

```
ICE state = k_connected (Phase 9)
  → IceTransportChannel::signal_ice_state_changed
    → TransportController::_on_ice_state()
      → 创建/启动 DtlsTransport (如果还没有)
        → DtlsTransport 开始 DTLS 握手
          - ClientHello ← (客户端是 DTLS client)
          - ServerHello + Certificate → (服务端是 DTLS server)
          - 握手完成
            ↓
        → SSLStreamAdapter::GetSrtpParams()
          → 导出 send_key + recv_key (SRTP master keys)
            ↓
        → SrtpTransport::set_rtp_params(send_key, recv_key)
          → SrtpSession::set_send(key)
          → SrtpSession::set_recv(key)
            → srtp_create() 初始化 libsrtp2
            → SRTP 通道就绪
```

### DTLS/STUN/RTP 包区分逻辑

```cpp
// UDP 端口收到数据后判断包类型
if (buf[0] >= 20 && buf[0] <= 63) {
    // ContentType 20-63 → DTLS (20=ChangeCipherSpec, 21=Alert, 22=Handshake, 23=App)
    return k_dtls;
} else if ((buf[0] & 0xC0) == 0x00) {
    // 前两位 00 → STUN
    return k_stun;
} else if ((buf[0] & 0xC0) == 0x80) {
    // 前两位 10 → RTP/SRTP
    return k_rtp;
}
```

### 数据路径 (加密/解密)

```
发送方向: RTP → SrtpSession::protect_rtp() → DtlsTransport → IceConnection → UDPPort

接收方向: UDPPort → IceConnection → DtlsTransport → SrtpSession::unprotect_rtp() → RTP
```

### 新增文件

| 文件 | 行数(参考) | 角色 |
|------|-----------|------|
| `src/pc/dtls_transport.h` | ~80 | DTLS 握手封装 |
| `src/pc/dtls_transport.cpp` | ~350 | SSLStreamAdapter 操作 |
| `src/pc/dtls_srtp_transport.h` | ~50 | DTLS+SRTP 桥接 |
| `src/pc/dtls_srtp_transport.cpp` | ~150 | 信号连接, key 导出 |
| `src/pc/srtp_session.h` | ~50 | libsrtp2 封装 |
| `src/pc/srtp_session.cpp` | ~150 | protect/unprotect |
| `src/pc/srtp_transport.h` | ~55 | send/recv session 管理 |
| `src/pc/srtp_transport.cpp` | ~120 | 参数设置 |

### 修改文件

| 文件 | 变更 |
|------|------|
| `src/pc/transport_controller.h/.cpp` | 持有 DtlsTransport map, DTLS 信号连接, `set_local_certificate`, `set_remote_description` 中创建 DtlsTransport |
| `CMakeLists.txt` | 链接 libsrtp2.a |

### 验证点

1. DTLS 握手完成日志
2. SRTP key 导出成功日志
3. 客户端和服务端之间可以收发 SRTP 加密的 RTP 数据 (Phase 13 验证)

---

## Phase 11: PC/ICE/Agent 状态管理

### 学习目标

理解状态聚合的层次结构：IceConnection → IceTransportChannel → IceAgent → PeerConnection。

### 参考 commit 范围

```
86a58c9 计算pc的状态
9047639 计算Ice传输通道的状态
c98be95 计算IceAgent的状态
d77f345 重新计算pc状态
fc5ae77 实现pc失败状态下的资源清理
```

### 状态聚合规则

```
IceTransportChannel state:
  - 若有 connection SUCCEEDED + writable + receiving → k_connected
  - 全部 FAILED → k_failed
  - 正在 ping → k_checking
  - 从未连接 → k_new

IceAgent state:
  - merged(c1, c2): 用 max(k_connected > k_completed > k_checking > k_new > k_failed)

PeerConnection state:
  - 根据 IceAgent state 映射
  - k_new/k_checking → k_connecting
  - k_connected → k_connected
  - k_completed → k_connected (简化)
  - k_failed → k_failed
  - k_disconnected → k_disconnected
```

### 修改文件

| 文件 | 变更 |
|------|------|
| `src/pc/peer_connection.h/.cpp` | 加入 `PeerConnectionState`, `_update_state()`, `signal_connection_state`, 失败资源清理 |
| `src/ice/ice_transport_channel.h/.cpp` | `_compute_ice_transport_state()`, `_update_state()`, `_set_writable()`, `_set_receiving()` |
| `src/ice/ice_agent.h/.cpp` | `_update_state()`, `signal_ice_state` |

---

## Phase 12: PULL 流 + STOP 命令

### 学习目标

理解拉流与推流的差异 (方向相反)，实现完整的流生命周期管理。

### 参考 commit 范围

```
5a0f518 音视频转发方案设计
f494372 解析sdp的ssrc信息
a0bb515 解析ssrc组信息
a391d98 创建音视频track
8025d5e 实现获取音视频源的方法
e925bf0 实现设置音视频源的方法
29eb026 sdp中增加ssrc的描述
306556c 处理pull命令
d73cd98 分发服务支持停止推流
84b1752 处理停止拉流命令
```

### PULL vs PUSH 的区别

| | PUSH (推流) | PULL (拉流) |
|------|------|------|
| direction | recvonly (服务端收) | sendonly (服务端发) |
| RTCOfferAnswerOptions | send=false, recv=true | send=true, recv=false |
| SSRC | 不需要 (接收方) | 需要 (发送方) |

### 新增文件

| 文件 | 角色 |
|------|------|
| `src/stream/pull_stream.h/.cpp` | PullStream (direction: k_send_only) |

### 修改文件

| 文件 | 变更 |
|------|------|
| `src/stream/rtc_stream_manager.h/.cpp` | `create_pull_stream()`, `stop_push()`, `stop_pull()` |
| `src/server/rtc_worker.h/.cpp` | `_process_pull()`, `_process_stop_push()`, `_process_stop_pull()` |
| `src/server/signaling_worker.cpp` | 完整 STOP_PUSH/STOP_PULL 响应处理 |

---

## Phase 13: RTP/RTCP 数据转发

### 学习目标

理解 RTP/RTCP 包的判别方法，实现 SRTP 加密的 RTP 数据在 DTLS 通道上的收发。

### 参考 commit 范围

```
e43ac54 解复用rtp和rtcp包
0cc8f93 实现rtp包的判断方法
8764293 rtp数据包解密
b0ad147 rtcp数据包解密 + 获取rtp和rtcp数据包
01880eb 转发rtp数据
1cddb36 实现加密的rtp发送
3372f65 加密rtp数据包 + 发送加密的rtcp数据包
```

### 数据路径

```
接收: UDP → IceConnection → DtlsSrtpTransport
        → SrtpSession::unprotect_rtp() → RTP data
        → signal_rtp_packet_received → RtcStream → 应用层

发送: 应用层 → RtcStream → DtlsSrtpTransport
        → SrtpSession::protect_rtp() → SRTP data
        → DtlsTransport → IceConnection → UDPPort → UDP
```

### 新增文件

| 文件 | 角色 |
|------|------|
| `src/module/rtp_rtcp/rtp_utils.h/.cpp` | `infer_rtp_packet_type()`, `parse_rtp_ssrc()`, `parse_rtp_sequence_number()` |

### PacketType 判别

```cpp
enum PacketType { k_rtp, k_rtcp, k_stun, k_dtls, k_unknown };

PacketType infer_rtp_packet_type(const char* buf, size_t len) {
    // DTLS: ContentType in [20, 63]
    if (buf[0] >= 20 && buf[0] <= 63) return k_dtls;
    // STUN: 前两位 00
    if ((buf[0] & 0xC0) == 0x00) return k_stun;
    // RTCP: payload type in [200, 211]
    if ((buf[0] & 0xC0) == 0x80 && buf[1] >= 200 && buf[1] <= 211) return k_rtcp;
    // RTP: version=2, payload type < 200
    if ((buf[0] & 0xC0) == 0x80) return k_rtp;
    return k_unknown;
}
```

---

## Phase 14: 异常处理 + 完整联调

### 参考 commit 范围

```
5a1e89b 异常处理、代码完善
8e8a514 与webrtc_client联调通过，可以进行推拉流
```

### 内容

- `stop_push()` / `stop_pull()` 资源释放
- PeerConnection 销毁时的状态清理
- ICE 资源释放 (UDPPort, IceConnection, IceTransportChannel)
- DTLS/SRTP 资源释放
- 完整推拉流端到端测试

---

## 完整消息路径 (Phase 14 最终态)

```
                     CMDNO_PUSH 消息的完整生命周期

TCP 数据到达 (xhead + JSON)
 → SignalingWorker 解析
 → RtcServer CRC32 路由
   → RtcWorker → RtcStreamManager::create_push_stream()
     → PushStream → PeerConnection::create_offer()
       → SessionDescription (codec + ICE ufrag/pwd + fingerprint)
       → TransportController::set_local_description()
         → IceAgent → IceTransportChannel → PortAllocator
           → UDPPort → socket + bind → Candidate (host)
       → to_string() → SDP 文本返回
 ← 响应经原路径返回客户端

客户端收到 SDP offer → 生成 answer → 发 CMDNO_ANSWER
 → SignalingWorker → RtcServer → RtcWorker
   → RtcStreamManager::set_answer()
     → PushStream::set_remote_sdp()
       → PeerConnection::set_remote_sdp()
         → 解析 transport info + SSRC + SSRC group
         → TransportController::set_remote_description()
           → IceAgent::set_remote_ice_params()
           → 启动 ICE 连通性检查

          ★ ICE 连通性检查 (Phase 9) ★
客户端 STUN Binding Request (UDP)
 → UDPPort::_on_read_packet()
   → StunMessage::validate_fingerprint()
   → StunMessage::read()
   → validate USERNAME + MESSAGE-INTEGRITY
   → signal_unknown_address
     → IceTransportChannel::_on_unknown_address()
       → create IceConnection
       → IceController 开始 ping

服务端 STUN Binding Request (UDP)
 ← IceConnection::ping() → ConnectionRequest → UDPPort::send_to()

客户端 STUN Binding Response (UDP)
 → IceConnection::on_read_packet() → received_ping_response()
   → write_state = WRITABLE, receiving = true
   → IceTransportChannel state = k_connected

          ★ DTLS 握手 (Phase 10) ★
 → DtlsTransport 启动握手
   → ClientHello + ServerHello + Certificate
   → DTLS 连接建立
   → SSLStreamAdapter::GetSrtpParams() 导出 SRTP key
   → SrtpSession::set_send/recv() 初始化

          ★ RTP 数据流 (Phase 13) ★
客户端 RTP (UDP, SRTP encrypted)
 → IceConnection → DtlsSrtpTransport
   → SrtpSession::unprotect_rtp() → 解密 RTP
   → signal_rtp_packet_received → PeerConnection → 应用层
```

---

## 文件清单总览

### 新增文件 (14 个)

```
src/ice/stun.h
src/ice/stun.cpp
src/ice/stun_request.h
src/ice/stun_request.cpp
src/ice/ice_connection.h
src/ice/ice_connection.cpp
src/ice/ice_connection_info.h
src/ice/ice_controller.h
src/ice/ice_controller.cpp
src/pc/dtls_transport.h
src/pc/dtls_transport.cpp
src/pc/dtls_srtp_transport.h
src/pc/dtls_srtp_transport.cpp
src/pc/srtp_session.h
src/pc/srtp_session.cpp
src/pc/srtp_transport.h
src/pc/srtp_transport.cpp
src/module/rtp_rtcp/rtp_utils.h
src/module/rtp_rtcp/rtp_utils.cpp
src/stream/pull_stream.h
src/stream/pull_stream.cpp
```

### 修改文件 (11 个)

```
src/ice/udp_port.h                  — STUN 处理, Connection 管理
src/ice/udp_port.cpp                — _on_read_packet 实现
src/ice/ice_transport_channel.h     — 状态机, ping timer, controller
src/ice/ice_transport_channel.cpp   — 完整 ICE 逻辑
src/ice/ice_agent.h                 — 状态聚合
src/ice/ice_agent.cpp               — signal_ice_state
src/pc/transport_controller.h       — DTLS 管理
src/pc/transport_controller.cpp     — DTLS 信号连接
src/pc/peer_connection.h            — 状态管理
src/pc/peer_connection.cpp          — _update_state
src/stream/rtc_stream_manager.h     — PULL + STOP
src/stream/rtc_stream_manager.cpp   — 完整流管理
src/server/rtc_worker.cpp           — _process_pull/stoppush/stoppull
src/server/signaling_worker.cpp     — STOP 响应
CMakeLists.txt                      — 链接 libsrtp2
```

---

## 执行建议

1. **Phase 8+9 紧密耦合**，建议作为一个大阶段实现。STUN codec 只有被 ICE connection 使用才有意义。
2. **Phase 10 依赖 Phase 9** 的 `k_connected` 状态来触发 DTLS 握手。
3. **Phase 11** 是状态机完善，代码量小但涉及多个文件的信号连接。
4. **Phase 12** 可以部分独立开发 (PULL + STOP 不依赖 DTLS/SRTP)。
5. **Phase 13** 依赖 Phase 10 的 SRTP 通道就绪。
6. **Phase 14** 是端到端验证和清理。

推荐顺序: **8 → 9 → 10 → 11 → 12 → 13 → 14**
