# Step 3 深入理解：已完成代码的全链路详解

本文档覆盖 Phase 1~5 的所有已实现代码，按消息处理路径逐层深入。

---

## 目录

1. [Phase 1 — TCP 数据到达 + xhead 协议解析](#phase-1--tcp-数据到达--xhead-协议解析)
2. [Phase 2 — 跨线程路由 + CRC32 分发](#phase-2--跨线程路由--crc32-分发)
3. [Phase 3 — SDP 生成：codec + ICE 凭证 + BUNDLE](#phase-3--sdp-生成codec--ice-凭证--bundle)
4. [Phase 4 — UDP 端口分配 + Candidate 生成](#phase-4--udp-端口分配--candidate-生成)
   - 4.1 [NetworkManager：网卡枚举](#41-networkmanager网卡枚举)
   - 4.2 [PortAllocator：端口分配器](#42-portallocator端口分配器)
   - 4.3 [UDPPort：socket 创建 + bind + candidate](#43-udpportsocket-创建--bind--candidate)
   - 4.4 [AsyncUdpSocket：事件驱动 UDP I/O](#44-asyncudpsocket事件驱动-udp-io)
   - 4.5 [Candidate：候选地址](#45-candidate候选地址)
5. [Phase 5 — DTLS Fingerprint 传递链](#phase-5--dtls-fingerprint-传递链)
6. [完整调用栈（TCP → SDP 返回）](#六完整调用栈tcp--sdp-返回)
7. [潜在问题和注意事项](#七潜在问题和注意事项)

---

## Phase 1 — TCP 数据到达 + xhead 协议解析

### 消息流

```
TCP 客户端 connect → signaling_server accept → fd 入 LockFreeQueue
  → signaling_worker EventLoop IOWatcher READ 触发
    → _read_query() → TcpConnection 状态机
      → STATE_HEAD: 读 36 字节 xhead，校验 magic_num == 0xfb202202
      → STATE_BODY: 读 body_len 字节 JSON
      → _process_request() → switch(cmdno) → _process_push()
```

### 关键数据结构

```cpp
// xhead_t — 36 字节二进制协议头
struct xhead_t {
    uint16_t id;           // 消息 ID
    uint16_t version;      // 版本号
    uint32_t log_id;       // 日志 ID（网络字节序）
    char provider[16];     // 服务提供商
    uint32_t magic_num;    // 固定 0xfb202202
    uint32_t reserved;     // 保留字段
    uint32_t body_len;     // JSON body 长度
};
```

### xhead 为什么是 36 字节？

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          id(2)              |     version(2)     log_id(4)    |            ← 4B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                        provider(16)                           | 			 ← 8B
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           magic_num(4)      |       	reserved(4)	   			|	 	     ← 4B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       body_len(4)                             | 			 ← 4B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

总计：4 + 4 + 16 + 8 + 4 = **36 字节**

### 短连接模型

每个 TCP 连接只处理**一个请求**。处理完一个请求后，`butes_processed` 被设为 65536，阻止后续读。这种模型简化了连接管理，但每请求都有 TCP 连接/断开的开销。

---

## Phase 2 — 跨线程路由 + CRC32 分发

### 消息流

```
SignalingWorker::_process_push()
  → 构造 RtcMsg { cmdno=1, uid, stream_name, audio=1, video=1, worker, conn, fd }
  → g_rtc_server->send_rtc_msg(msg)
       ↓
RtcServer::_q_msg.push(msg)          ← mutex 保护的多生产者队列
write(pipe_send_fd, &msg, 1)         ← 1 字节通知 EventLoop
       ↓
RtcServer recv_notify 回调触发
  → pop_msg()                        ← 出队
  → CRC32(stream_name) % worker_num  ← 路由计算
  → rtc_worker->send_rtc_msg(msg)
       ↓
RtcWorker LockFreeQueue 入队         ← SPSC 无锁队列
write(pipe_send_fd, &msg, 1)         ← 通知
       ↓
RtcWorker recv_notify 回调
  → pop_msg() → switch(cmdno)
    → CMDNO_PUSH → _process_push()
```

### 线程间通信机制

| 环节 | 数据结构 | 生产者 | 消费者 | 说明 |
|------|---------|--------|--------|------|
| SignalingWorker → RtcServer | `std::queue + std::mutex` | 多个 SignalingWorker | RtcServer | 多生产者，需要锁 |
| RtcServer → RtcWorker | `LockFreeQueue<shared_ptr<RtcMsg>>` | RtcServer（单生产者） | RtcWorker | SPSC，无锁 |

为什么 RtcServer 用 mutex 而 RtcWorker 不用？

- **RtcServer** 的队列可能被 N 个 SignalingWorker 线程同时写入 → 多生产者 → 需要互斥锁
- **RtcWorker** 的队列只被 RtcServer（单生产者）写入 → SPSC → `LockFreeQueue` 足够

为什么用 pipe + write(int) 而不是 condition_variable？

- EventLoop 线程阻塞在 `ev_run()` 中，无法用 condition_variable 唤醒
- pipe 的读端注册了 IOWatcher，写端写入后 libev 检测到 fd 可读，自动唤醒目标线程
- 所有跨线程通知都走这个模式（SignalingServer → SignalingWorker 也一样）

### CRC32 路由

```cpp
// rtc_server.cpp:268-276
RtcWorker* RtcServer::_get_worker(const std::string& stream_name) {
    uint32_t num = rtc::ComputeCrc32(stream_name);
    size_t index = num % _options.worker_num;
    return _workers[index];
}
```

同一条流的 PUSH/ANSWER/STOP_PUSH 消息都会路由到同一个 worker，因为 `CRC32(stream_name)` 是确定的。

### 证书传递

```cpp
// rtc_server.cpp:221-225
_process_rtc_msg() {
    _generate_and_check_certificate();
    msg->certificate = _certificate.get();  // scoped_refptr → 原始指针
    // ...
    worker->send_rtc_msg(msg);
}
```

注意：`msg->certificate` 是 `void*` 类型，传递时做了强制转换。RtcServer 的 `_certificate` 是 `rtc::scoped_refptr`，持有引用计数，保证证书在工作期间不被销毁。

---

## Phase 3 — SDP 生成：codec + ICE 凭证 + BUNDLE

### PushStream::create_offer() 完整流程

```cpp
std::string PushStream::create_offer() {
    SessionDescription offer(SdpType::k_offer);

    // 1. 生成 ICE 凭证
    IceParameters ice_params = IceCredentials::create_random_ice_credentials();

    // 2. 创建 UDP 端口，生成 Candidate（Phase 4 细节）
    std::vector<Candidate> candidates;
    auto networks = _allocator->get_networks();
    for (auto network : networks) {
        auto* port = new UDPPort(_el, "audio", IceCandidateComponent::RTP, ice_params);
        Candidate c;
        port->create_ice_candidate(network, _allocator->min_port(), _allocator->max_port(), c);
        _ports.push_back(port);
        candidates.push_back(c);
    }

    // 3. 添加音频媒体段
    if (_audio) {
        auto audio = std::make_shared<AudioContentDescription>();
        audio->set_direction(RtpDirection::k_recv_only);   // ← 服务端只收
        audio->set_rtcp_mux(true);
        audio->add_candidates(candidates);
        offer.add_content(audio);
        offer.add_transport_info(audio->mid(), ice_params, _certificate);
    }

    // 4. 添加视频媒体段
    if (_video) {
        auto video = std::make_shared<VideoContentDescription>();
        video->set_direction(RtpDirection::k_recv_only);
        video->set_rtcp_mux(true);
        video->add_candidates(candidates);
        offer.add_content(video);
        offer.add_transport_info(video->mid(), ice_params, _certificate);
    }

    // 5. BUNDLE 分组
    ContentGroup bundle_group("BUNDLE");
    for (auto& content : offer.contents()) {
        bundle_group.add_content_name(content->mid());
    }
    offer.add_group(bundle_group);

    return offer.to_string();
}
```

### AudioContentDescription 构造函数

```cpp
// session_description.cpp:15-26
AudioContentDescription::AudioContentDescription() {
    auto codec = std::make_shared<AudioCodecInfo>(111, "opus", 48000, 2);
    codec->feedback_param.push_back(FeedBackParam("transport-cc"));
    codec->codec_param["minptime"] = "10";
    codec->codec_param["useinbandfec"] = "1";
    _codecs.push_back(codec);
}
```

生成：
```
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=fmtp:111 minptime=10;useinbandfec=1
```

### VideoContentDescription 构造函数

```cpp
// session_description.cpp:30-47
VideoContentDescription::VideoContentDescription() {
    auto h264 = std::make_shared<VideoCodecInfo>(96, "H264", 90000);
    h264->feedback_param.push_back(FeedBackParam("goog-remb"));
    h264->feedback_param.push_back(FeedBackParam("transport-cc"));
    h264->feedback_param.push_back(FeedBackParam("ccm", "fir"));
    h264->feedback_param.push_back(FeedBackParam("nack"));
    h264->feedback_param.push_back(FeedBackParam("nack", "pli"));
    h264->codec_param["level-asymmetry-allowed"] = "1";
    h264->codec_param["packetization-mode"] = "1";
    h264->codec_param["profile-level-id"] = "42e01f";
    _codecs.push_back(h264);

    auto rtx = std::make_shared<VideoCodecInfo>(97, "rtx", 90000);
    rtx->codec_param["apt"] = std::to_string(96);
    _codecs.push_back(rtx);
}
```

生成：
```
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

### rtcp-fb 各行含义

| rtcp-fb 行 | 含义 | 作用 |
|-----------|------|------|
| `goog-remb` | Google Receiver Estimated Max Bitrate | 接收端估算带宽并通知发送端 |
| `transport-cc` | Transport Wide Congestion Control | 传输级拥塞控制 |
| `ccm fir` | Full Intra Request | 请求发送端立即发一个关键帧 |
| `nack` | Generic NACK | 丢包通知，请求重传 |
| `nack pli` | Picture Loss Indication | 视频帧丢失，请求关键帧 |

### BUNDLE 分组的作用

```cpp
ContentGroup bundle_group("BUNDLE");
for (auto& content : offer.contents()) {
    bundle_group.add_content_name(content->mid());
}
```

BUNDLE 的意思是 audio 和 video 共用**同一个传输通道**（同一个 UDP 端口、同一个 ICE 会话、同一个 DTLS 加密通道）。这减少了端口占用，也简化了 ICE 连接管理。

生成的 SDP 行：`a=group:BUNDLE audio video`

### to_string() 序列化流程

```cpp
// session_description.cpp:278-361
std::string SessionDescription::to_string() {
    // v=0\r\n
    // o=- 0 2 IN IP4 127.0.0.0\r\n
    // s=-\r\n
    // t=0 0\r\n
    // a=group:BUNDLE audio video\r\n
    // a=msid-semantic: WMS\r\n

    for (auto& content : _contents) {
        // m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n
        // c=IN IP4 0.0.0.0\r\n
        // a=rtcp:9 IN IP4 0.0.0.0\r\n

        // a=candidate:...\r\n                   ← UDPPort 创建
        // a=ice-ufrag:...\r\n
        // a=ice-pwd:...\r\n
        // a=fingerprint:...\r\n                  ← 证书
        // a=setup:actpass\r\n

        // a=mid:audio\r\n
        // a=recvonly\r\n
        // a=rtcp-mux\r\n
        // a=rtpmap:...\r\n
        // a=rtcp-fb:...\r\n
        // a=fmtp:...\r\n
    }
}
```

**SDP 行顺序是有讲究的**：`m=` 行在最前，然后是连接/传输信息（candidate、ICE、DTLS），然后是媒体参数（rtpmap、fmtp）。

---

## Phase 4 — UDP 端口分配 + Candidate 生成

这是消息路径中最关键的转折点——从纯内存操作变为涉及 I/O 的操作。

```
之前（Phase 3）：create_offer() 直接在内存中构造 SessionDescription，同步返回
现在（Phase 4）：需要创建 UDP socket → bind → getsockname → 创建 Candidate

PushStream::create_offer()
  → _allocator->get_networks()     ← 获取本机所有网卡
  → new UDPPort(..., ice_params)
    → create_udp_socket()          ← socket(AF_INET, SOCK_DGRAM, 0)
    → sock_setnonblock()           ← fcntl(O_NONBLOCK)
    → sock_bind()                  ← bind() + getsockname()
    → new AsyncUdpSocket(fd, el)   ← 注册 IOWatcher READ
    → create_ice_candidate()       ← 从 UDPPort 信息创建 Candidate
  → candidates 填入 MediaContentDescription
  → to_string() → SDP 中现在有 a=candidate: 行了
```

### 4.1 NetworkManager：网卡枚举

```cpp
// network.cpp:16-38
NetworkManager::NetworkManager() {
    struct ifaddrs* ifaddr = nullptr;
    getifaddrs(&ifaddr);           // POSIX 系统调用，返回网卡信息链表

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;  // 只取 IPv4
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;     // 排除回环

        struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
        rtc::IPAddress ip_address(addr->sin_addr);
        _networks.push_back(new Network(ifa->ifa_name, ip_address));
    }

    freeifaddrs(ifaddr);           // 必须释放链表
}
```

**设计要点：**
- 构造函数中完成全量枚举，此后 `get_networks()` 只是返回已填充的 `vector`
- 只取 IPv4 + 非回环地址
- `Network` 结构体只存 `{name, ip}`，ip 用了 `rtc::IPAddress`（来自 rtcbase 项目）

`Network` 结构体：
```cpp
struct Network {
    std::string _name;       // 网卡名，如 "eth0"
    rtc::IPAddress _ip;      // IP 地址
};
```

### 4.2 PortAllocator：端口分配器

```cpp
// port_allocator.cpp
PortAllocator::PortAllocator() : _network_manager(new NetworkManager()) {}

void PortAllocator::set_port_range(int min_port, int max_port) {
    if (min_port > 0) { _min_port = min_port; }
    if (max_port > 0) { _max_port = min_port; }   // ← BUG: 应该是 max_port
}

const std::vector<Network*>& PortAllocator::get_networks() {
    return _network_manager->get_networks();
}
```

PortAllocator 是一个非常薄的层，当前只负责持有 NetworkManager 并把端口范围传递下去。`get_networks()` 只是一个透传。

它由 RtcStreamManager 持有：
```cpp
// rtc_stream_manager.cpp:11-18
RtcStreamManager::RtcStreamManager(EventLoop* el) :
    _el(el),
    _allocator(new PortAllocator())
{
    if (g_conf) {
        _allocator->set_port_range(g_conf->ice_min_port, g_conf->ice_max_port);
    }
}
```

### 4.3 UDPPort：socket 创建 + bind + candidate

`src/ice/udp_port.cpp` 是当前最核心的 90 行代码。

#### 成员变量

```cpp
class UDPPort {
    EventLoop* _el;
    std::string _transport_name;       // "audio" 或 "video"
    IceCandidateComponent _component;  // RTP = 1
    IceParameters _ice_params;         // ufrag + pwd
    int _socket = -1;                  // UDP socket fd
    std::unique_ptr<AsyncUdpSocket> _async_socket;
    rtc::SocketAddress _local_addr;    // 绑定的本地地址
    std::vector<Candidate> _candidates;
};
```

#### create_ice_candidate() 完整流程

```cpp
// udp_port.cpp:35-91
int UDPPort::create_ice_candidate(Network* network, int min_port, int max_port, Candidate& c) {
    // ① 创建 UDP socket
    _socket = create_udp_socket(network->ip().family());
    // → socket(AF_INET, SOCK_DGRAM, 0)

    // ② 设为非阻塞
    sock_setnonblock(_socket);
    // → fcntl(fd, F_SETFL, flags | O_NONBLOCK)

    // ③ bind 到端口
    struct sockaddr_in addr_in;
    addr_in.sin_family = network->ip().family();
    addr_in.sin_addr.s_addr = INADDR_ANY;     // ★ 绑定到所有接口
    sock_bind(_socket, &addr_in, sizeof(addr_in), min_port, max_port);

    // ④ 获取实际分配的端口
    sock_get_address(_socket, nullptr, &port);
    // → getsockname(fd, &addr) → ntohs(addr.sin_port)

    _local_addr.SetIP(network->ip());
    _local_addr.SetPort(port);

    // ⑤ 创建 AsyncUdpSocket，注册 IOWatcher
    _async_socket = std::make_unique<AsyncUdpSocket>(_el, _socket);
    _async_socket->signal_read_packet.connect(this, &UDPPort::_on_read_packet);

    // ⑥ 生成 Candidate
    c.component = _component;
    c.protocol = "udp";
    c.address = _local_addr;
    c.port = port;
    c.priority = c.get_priority(ICE_TYPE_PREFERENCE_HOST, 0, 0);
    c.username = _ice_params.ice_ufrag;
    c.password = _ice_params.ice_pwd;
    c.type = LOCAL_PORT_TYPE;           // "host"
    c.foundation = compute_foundation(c.type, c.protocol, c.address);
    // → CRC32("host" + "IP字符串" + "udp")

    _candidates.push_back(c);
    return 0;
}
```

#### sock_bind() 端口选择策略

```cpp
// socket.cpp:216-234
int sock_bind(int sock, struct sockaddr* addr, socklen_t len, int min_port, int max_port) {
    int ret = -1;
    if (0 == min_port && 0 == max_port) {
        ret = bind(sock, addr, len);          // 系统自动选端口
    } else {
        struct sockaddr_in* addr_in = (struct sockaddr_in*)addr;
        for (int port = min_port; port <= max_port && ret != 0; port++) {
            addr_in->sin_port = htons(port);
            ret = bind(sock, addr, len);      // 逐个尝试
        }
    }
    return ret;
}
```

两种模式：
- `min_port=0, max_port=0`：交给内核自动分配（默认行为）
- `min_port>0`：在 `[min_port, max_port]` 范围内逐一尝试 bind

#### 关于 INADDR_ANY 的说明

UDPPort 调用 `sock_bind()` 时传入的是 `INADDR_ANY` 而不是具体的网卡 IP。这意味着 **socket 能收到所有网卡的 UDP 包**，但 candidate 中写入的 IP 却是 `network->ip()`。

为什么这样设计？

在 ICE 中，服务端有多个网卡（如 eth0=192.168.1.100, eth1=10.0.0.1）时，会为每个网卡创建一个 UDPPort（每个有自己的 socket 和端口）。每个 UDPPort 的 candidate 写入对应网卡的 IP。但实际上每个 socket 绑的都是 INADDR_ANY，意味着 eth0 的 socket 也能收到发到 eth1 的包的。这在多网卡环境下不会造成问题，因为 ICE 连通性检查时客户端只会向 candidate 中声明的 IP:端口发包。

### 4.4 AsyncUdpSocket：事件驱动 UDP I/O

#### 构造函数：立即注册 READ 事件

```cpp
// async_udp_socket.cpp:26-34
AsyncUdpSocket::AsyncUdpSocket(EventLoop* el, int socket) :
    _el(el), _socket(socket),
    _buf(new char[MAX_BUF_SIZE]), _size(MAX_BUF_SIZE)
{
    _socket_watcher = _el->create_io_event(async_udp_socket_io_cb, this);
    _el->start_io_event(_socket_watcher, _socket, EventLoop::READ);
}
```

**关键设计：构造时立即注册 READ 事件。** 这意味着 socket 创建后就开始监听收包，即使 SDP 还在构造过程中。

#### recv_data()：循环读取

```cpp
// async_udp_socket.cpp:49-67
void AsyncUdpSocket::recv_data() {
    while (true) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int len = sock_recv_from(_socket, _buf, _size, (struct sockaddr*)&addr, addr_len);
        if (len <= 0) {
            return;   // EAGAIN 或出错 → 退出循环
        }

        int64_t ts = sock_get_recv_timestamp(_socket);  // SIOCGSTAMP_OLD
        rtc::SocketAddress remote_addr(ip, port);
        signal_read_packet(this, _buf, len, remote_addr, ts);
    }
}
```

**为什么用 while(true) + 非阻塞？**
- EventLoop 是单线程的，recv_data() 在 I/O 回调中执行
- 内核可能积累了多个 UDP 包（burst 场景），while 循环一次读完
- 读完的标志是 `recvfrom` 返回 EAGAIN（`sock_recv_from` 映射为 `<= 0`）
- 避免了反复触发 I/O 回调的开销

**sock_get_recv_timestamp() 的用途：**
```cpp
// socket.cpp:299-305
int64_t sock_get_recv_timestamp(int sock) {
    struct timeval time;
    ioctl(sock, SIOCGSTAMP_OLD, &time);  // 获取内核时间戳
    return time.tv_sec * 1000000 + time.tv_usec;
}
```

`SIOCGSTAMP_OLD` 返回内核为数据包打的时间戳（精确到微秒），用于后续的 RTT 计算和 jitter 估计。

#### send_to()：写优化策略

```cpp
// async_udp_socket.cpp:106-143
int AsyncUdpSocket::send_to(const char* data, size_t size, const rtc::SocketAddress& addr) {
    // 1. 先发掉队列中积压的数据
    _send_data_from_list();

    // 2. 尝试直接发送当前数据
    sent = sock_send_to(_socket, data, size, MSG_NOSIGNAL, ...);
    if (sent < 0)  return -1;
    if (0 == sent)  goto SEND_AGAIN;   // EAGAIN
    return size;

SEND_AGAIN:
    // 3. 加入缓存队列，开启 WRITE 事件监控
    _udp_packet_list.push_back(new UdpPacketData(data, size, addr));
    _el->start_io_event(_socket_watcher, _socket, EventLoop::WRITE);
    return size;
}
```

**设计要点：**
- **优先直接发送**（不走队列），只在 `EAGAIN` 时才入队
- **`MSG_NOSIGNAL`** 标志：防止 `sendto` 写已关闭的对端时触发 SIGPIPE 信号
- **写队列 + WRITE 事件**：只有写缓冲区满时才开启 WRITE 事件，send_data() 回调中发完后立即 `stop_io_event(WRITE)`
- **这同 Phase 1 中的 WRITE spin bug 是同一类问题**：不停止 WRITE 事件会导致回调反复触发空转 CPU

#### send_data() 回调

```cpp
void AsyncUdpSocket::send_data() {
    _send_data_from_list();
    if (_udp_packet_list.empty()) {
        _el->stop_io_event(_socket_watcher, _socket, EventLoop::WRITE);
    }
}
```

发完队列数据后必须停掉 WRITE 事件，否则会在 EventLoop 的每次循环中反复回调。

#### UdpPacketData

```cpp
// async_udp_socket.h:15-38
class UdpPacketData {
    char* _data;                  // 动态分配的 buffer
    size_t _size;
    rtc::SocketAddress _addr;     // 对端地址
};
```

`UdpPacketData` 封装了一个待发送的 UDP 数据包。它复制传入的数据到堆上（`mempcpy`），因为原始数据可能来自临时栈变量。

### 4.5 Candidate：候选地址

```cpp
// candidate.h
class Candidate {
    std::string foundation;         // CRC32("host" + IP + "udp")
    IceCandidateComponent component; // RTP = 1
    std::string protocol;           // "udp"
    uint32_t priority;
    rtc::SocketAddress address;     // IP + 端口
    uint16_t port;
    std::string username;           // ice_ufrag
    std::string password;           // ice_pwd
    std::string type;               // "host"
};
```

#### priority 计算公式

```cpp
// candidate.cpp:20-27
uint32_t Candidate::get_priority(uint32_t type_preference,
    int network_adapter_preference, int relay_preference)
{
    int addr_ref = rtc::IPAddressPrecedence(address.ipaddr()) + relay_preference;
    int local_pref = (network_adapter_preference << 8) | addr_ref;
    return (type_preference << 24) | (local_pref << 8) | (256 - (int)component);
}
```

UDPPort 中的调用：`c.get_priority(ICE_TYPE_PREFERENCE_HOST, 0, 0)`

- `type_preference = 126`（`ICE_TYPE_PREFERENCE_HOST`）
- `network_adapter_preference = 0`
- `component = 1`（RTP）

所以：`(126 << 24) | ((0 << 8) | addr_ref) << 8 | (256 - 1)` = `2113929216 + (addr_ref << 8) + 255`

其中 `addr_ref = rtc::IPAddressPrecedence(ip)` — 这是网卡 IP 的地址优先级（RFC 3484），全局单播地址优先级最高。

#### foundation 计算

```cpp
// udp_port.cpp:26-33
static std::string compute_foundation(const std::string& type,
    const std::string& protocol, const rtc::SocketAddress& base)
{
    std::stringstream ss;
    ss << type << base.HostAsURIString() << protocol;
    return std::to_string(rtc::ComputeCrc32(ss.str()));
}
```

foundation = `CRC32("host" + "192.168.1.100" + "udp")` → 一个 32 位整数转字符串。

同一网卡同类型的所有 candidate 共享同一个 foundation（作用是将来自同一个网卡的候选地址分组）。

#### candidate SDP 序列化

```cpp
// session_description.cpp:219-232
static void build_candidate(content, ss) {
    for (auto& c : content->candidates()) {
        ss << "a=candidate:" << c.foundation
           << " " << c.component
           << " " << c.protocol
           << " " << c.priority
           << " " << c.address.ipaddr().ToString()
           << " " << c.port
           << " " << c.type
           << "\r\n";
    }
}
```

生成 SDP 行：`a=candidate:1234567890 1 udp 2113937151 192.168.1.100 54321 typ host`

---

## Phase 5 — DTLS Fingerprint 传递链

### DTLS 证书生命周期

```
RtcServer::init()
  → 创建 DTLS 证书（ECDSA 密钥，有效期 1 年）
  → _certificate 存储为 scoped_refptr（引用计数管理）

RtcServer::_process_rtc_msg()
  → _generate_and_check_certificate()    ← 懒生成，检查是否过期
  → msg->certificate = _certificate.get()  ← 原始指针传下去

RtcWorker::_process_push()
  → (rtc::RTCCertificate*)(msg->certificate)  ← void* 转回

RtcStreamManager::create_push_stream()
  → stream->start(certificate)
    → _certificate = certificate    ← 存入 PushStream

PushStream::create_offer()
  → offer.add_transport_info("audio", ice_params, _certificate)
  → offer.add_transport_info("video", ice_params, _certificate)
```

### 证书生成

```cpp
// rtc_server.cpp:55-73
int RtcServer::_generate_and_check_certificate() {
    if (!_certificate || _certificate->HasExpired(time(NULL) * 1000)) {
        rtc::KeyParams key_params;
        _certificate = rtc::RTCCertificateGenerator::GenerateCertificate(
            key_params, k_year_in_ms);
        // 默认密钥：ECDSA（rtcbase）
        // 有效期：365 * 24 * 3600 * 1000 毫秒 = 1 年
    }
}
```

### add_transport_info 中的 fingerprint 处理

```cpp
// session_description.cpp:105-123
bool SessionDescription::add_transport_info(const std::string& mid,
    const IceParameters& ice_param, rtc::RTCCertificate* certificate)
{
    auto tdesc = std::make_shared<TransportDescription>();
    tdesc->mid = mid;
    tdesc->ice_ufrag = ice_param.ice_ufrag;
    tdesc->ice_pwd = ice_param.ice_pwd;

    if (certificate) {
        // 从证书创建 SSLFingerprint（提取 sha-256 指纹）
        tdesc->identity_fingerprint =
            rtc::SSLFingerprint::CreateFromCertificate(*certificate);
    }

    // offer 方角色设为 actpass（把 DTLS 角色选择权给客户端）
    tdesc->connection_role = (_sdp_type == SdpType::k_offer) ? ACTPASS : ACTIVE;

    _transport_infos.push_back(tdesc);
    return true;
}
```

### fingerprint 在 SDP 中的序列化

```cpp
// session_description.cpp:335-339
if (transport_info->identity_fingerprint) {
    ss << "a=fingerprint:" << transport_info->identity_fingerprint->algorithm
       << " " << transport_info->identity_fingerprint->GetRfc4572Fingerprint()
       << "\r\n";
    ss << "a=setup:" << connection_role_to_string(transport_info->connection_role)
       << "\r\n";
}
```

生成：
```
a=fingerprint:sha-256 72:6D:FC:06:53:D0:64:AE:54:D2:3F:9F:6F:AC:C2:7E:...\r\n
a=setup:actpass\r\n
```

### setup:actpass 的含义

`setup:actpass` 表示本端（服务端）**既可以做 DTLS 客户端也可以做服务端**，把角色选择权交给客户端。客户端在 answer 中回复：
- `a=setup:active` → 客户端做 DTLS 客户端，服务端做 DTLS 服务端（最常见）
- `a=setup:passive` → 客户端做 DTLS 服务端，服务端做 DTLS 客户端

后续 DTLS 握手时，服务方根据 `setup` 角色决定谁先发 `ClientHello`。

---

## 六、完整调用栈（TCP → SDP 返回）

```
signaling_worker.cpp
  _read_query(fd)                          ← TCP 数据到达
    _process_query_buffer(c)               ← 按帧处理
      STATE_HEAD: 读 36 字节 xhead         ← 校验 magic_num=0xfb202202
      STATE_BODY: 读 body_len 字节 JSON    ← 解析 cmdno, uid, stream_name
      _process_request(c)
        _process_push(msg)                 ← cmdno==1
          g_rtc_server->send_rtc_msg(msg)  ← RtcServer 队列 + pipe

rtc_server.cpp
  _process_rtc_msg()                       ← pipe 通知
    _generate_and_check_certificate()      ← 懒创建证书
    msg->certificate = _certificate.get()  ← 传下去
    _get_worker(stream_name)               ← CRC32(stream_name) % N
    worker->send_rtc_msg(msg)              ← LockFreeQueue + pipe

rtc_worker.cpp
  _process_rtc_msg()
    _process_push(msg)
      _rtc_stream_mgr->create_push_stream(..., certificate, offer)

rtc_stream_manager.cpp
  create_push_stream()
    new PushStream(el, allocator, uid, name, audio, video, log_id)
    stream->start(certificate)             ← 保存证书
    offer = stream->create_offer()         ← ★ 核心

push_stream.cpp
  create_offer()
    SessionDescription offer(SdpType::k_offer)
    IceCredentials::create_random_ice_credentials()
      → ufrag = rtc::CreateRandomString(4)   ← 4 字节
      → pwd  = rtc::CreateRandomString(24)   ← 24 字节

    _allocator->get_networks()             ← NetworkManager 枚举结果
    for each network:
      new UDPPort(el, "audio", RTP, ice_params)
        create_udp_socket(family)           ← socket(AF_INET, SOCK_DGRAM, 0)
        sock_setnonblock(fd)                ← fcntl(O_NONBLOCK)
        sock_bind(fd, INADDR_ANY, port_range) ← bind() + getsockname()
        new AsyncUdpSocket(el, fd)          ← 注册 IOWatcher READ
        port->create_ice_candidate(network, ..., c)
          → Candidate {foundation, component, protocol, priority, address, port, type, username, password}
        _ports.push_back(port)
        candidates.push_back(c)

    if _audio:
      auto audio = new AudioContentDescription()
        → codecs = {opus 111}              ← 构造函数自带
      audio->set_direction(k_recv_only)
      audio->add_candidates(candidates)     ← ★ candidate 填入
      offer.add_content(audio)
      offer.add_transport_info("audio", ice_params, _certificate)
        → TransportDescription {ice_ufrag, ice_pwd, fingerprint, setup=actpass}

    if _video:
      auto video = new VideoContentDescription()
        → codecs = {H264 96, rtx 97}       ← 构造函数自带
      video->set_direction(k_recv_only)
      video->add_candidates(candidates)
      offer.add_content(video)
      offer.add_transport_info("video", ice_params, _certificate)

    ContentGroup("BUNDLE") → {audio, video} ← 共用传输通道
    offer.add_group(bundle_group)

    return offer.to_string()               ← ★ SDP 序列化

rtc_stream_manager.cpp
  _push_streams[stream_name] = stream       ← 保存流

rtc_worker.cpp
  msg->sdp = offer                          ← SDP 写入响应
  ((SignalingWorker*)msg->worker)->send_rtc_msg(msg)  ← 回传

signaling_worker.cpp
  _process_rtc_msg()
    _response_server_offer(msg)
      → JSON {err_no:0, offer: "<sdp 文本>"}
      → _add_reply(json, conn)
        → _write_query(fd)                 ← TCP 写回客户端
```

---

## 七、潜在问题和注意事项

### 7.1 PortAllocator::set_port_range() bug

```cpp
// port_allocator.cpp:18
void PortAllocator::set_port_range(int min_port, int max_port) {
    if (max_port > 0) {
        _max_port = min_port;   // ← bug: 应该是 max_port
    }
}
```

`_max_port` 被赋值为 `min_port`，导致端口范围退化到 `[min_port, min_port]`（只尝试一个端口）。如果不配置端口范围（min=max=0，系统自动分配），不影响。如果配了范围就错了。

### 7.2 INADDR_ANY vs 具体网卡 IP

```cpp
// udp_port.cpp:51
addr_in.sin_addr.s_addr = INADDR_ANY;
```

socket 绑定到所有接口，但 candidate 中写入的 IP 是 `network->ip()`。多网卡环境下，每个 UDPPort 的 candidate 写不同 IP，但 socket 都收所有网卡的包。这在当前阶段不是问题，因为 ICE 连通性检查时客户端只向 candidate 中的 IP:端口发包。

### 7.3 _on_read_packet 目前是空的

```cpp
// udp_port.cpp:101-105
void UDPPort::_on_read_packet(AsyncUdpSocket*, char*, size_t,
    const rtc::SocketAddress&, int64_t)
{
    // 空实现！UDP 数据到达后什么都不做
}
```

AsyncUdpSocket 的 IOWatcher 已经注册了 READ 事件，数据到达时 `signal_read_packet` 会触发，但接收方 `UDPPort::_on_read_packet` 是空的。需要 Phase 8（STUN 消息处理）才补实现。

### 7.4 Certificate 生命周期隐患

```cpp
// rtc_server.cpp:225
msg->certificate = _certificate.get();  // scoped_refptr → 原始指针
```

`msg->certificate` 是 `void*` 类型，传递的是原始指针。RtcServer 的 `_certificate` 是 `scoped_refptr`，确保证书在工作期间不被销毁。但后续 Phase 6 引入异步 candidate 采集后，证书可能需要在多个异步回调期间保持有效，那时需要更仔细的生命周期管理。

### 7.5 `m=` 行端口固定为 9

```cpp
// session_description.cpp:320
ss << "m=" << content->mid() << " 9 " << k_media_protocol_dtls_savpf << fmt << "\r\n";
```

WebRTC 惯例：`m=` 行的端口固定写 9 作为占位，实际通信端口由 ICE candidate 提供。

### 7.6 `o=` 行 IP 写死

```cpp
// session_description.cpp:288
ss << "o=- 0 2 IN IP4 127.0.0.0\r\n";
```

`o=`（origin）行的 IP 写死了 `127.0.0.0`，没有写真实 IP。这在 WebRTC 中是允许的，因为 `o=` 行只作为会话标识，真正的传输地址在 candidate 中。

### 7.7 build_ssrc 当前不会输出任何行

```cpp
// session_description.cpp:252-275
static void build_ssrc(content, ss) {
    for (auto& track : content->streams()) {
        // 当前 streams() 为空，因为 create_offer() 没有 add_stream()
    }
}
```

PushStream::create_offer() 没有往 `MediaContentDescription` 中添加 StreamParams，所以 SDP 中不会有 `a=ssrc:` 行。这需要在 Phase 7（set_remote_sdp 解析 answer 中的 SSRC）时才用到。

---

## 生成的完整 SDP 示例

```
v=0
o=- 0 2 IN IP4 127.0.0.0
s=-
t=0 0
a=group:BUNDLE audio video
a=msid-semantic: WMS

m=audio 9 UDP/TLS/RTP/SAVPF 111
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=candidate:1234567890 1 udp 2113937151 192.168.1.100 54321 typ host
a=ice-ufrag:bmOu
a=ice-pwd:gN7glSPNwmh1J0uo+Olrdgcd
a=fingerprint:sha-256 72:6D:FC:06:53:D0:64:AE:54:D2:3F:9F:6F:AC:C2:7E:7C:...
a=setup:actpass
a=mid:audio
a=recvonly
a=rtcp-mux
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=fmtp:111 minptime=10;useinbandfec=1

m=video 9 UDP/TLS/RTP/SAVPF 96 97
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=candidate:1234567890 1 udp 2113937151 192.168.1.100 54321 typ host
a=ice-ufrag:bmOu
a=ice-pwd:gN7glSPNwmh1J0uo+Olrdgcd
a=fingerprint:sha-256 72:6D:FC:06:53:D0:64:AE:54:D2:3F:9F:6F:AC:C2:7E:7C:...
a=setup:actpass
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
