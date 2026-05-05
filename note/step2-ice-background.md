# Step 2 背景知识：WebRTC 中的 ICE

## 为什么需要 ICE

回到 Step 1 你已经做了的事：PUSH 请求进来，服务端返回了一段 SDP offer。但这个 SDP 里只有 codec 信息（opus、H264），没有告诉客户端**数据发到哪里去**。

ICE（Interactive Connectivity Establishment，交互式连接建立）解决的核心问题就是：

> **SDP 交换后，双方的媒体数据走哪条路？**

具体来说，ICE 要回答三个问题：

1. **对方在哪里？**（IP 地址和端口是什么）
2. **怎么加密？**（DTLS 握手的证书是什么）
3. **数据通路健壮吗？**（能发能收吗？双全工？）

Step 2 只解决第 1 个问题：**告诉对方我在哪个 IP:端口等着收 UDP 数据**。第 2 个问题是 Step 3（DTLS fingerprint），第 3 个问题是 Step 4（ICE 连通性检查）。

---

## ICE 的核心概念

### 1. Candidate（候选地址）

Candidate 是一个可能的通信端点。一个 candidate 本质上就是"一个 IP + 一个端口 + 一个传输协议"的三元组。

```
candidate:1 1 UDP 2130706431 192.168.1.100 54321 typ host
   ↑       ↑ ↑   ↑       ↑          ↑        ↑      ↑
foundation 组件 协议   优先级       IP       端口    类型
```

**candidate 的各字段含义：**

| 字段 | 含义 | 在我们代码中的值 |
|------|------|-----------------|
| foundation | 该 candidate 的唯一标识（相同网卡的同类型 candidate 共享 foundation） | 网卡名，如 `"eth0"` |
| component_id | RTP=1, RTCP=2（rtcp-mux 启用时只用 1） | `1` |
| protocol | 传输协议 | `"UDP"` |
| priority | 优先级，按候选类型和网卡偏好计算 | 公式后面给 |
| IP | 通信目标的 IP 地址 | 本机网卡 IP |
| port | UDP 端口 | 程序绑定的端口 |
| type | 候选类型 | `"host"`（当前阶段） |

### 2. Candidate 的四种类型

| 类型 | 缩写 | 含义 | 优先级基值 | 是否需要外部服务 |
|------|------|------|-----------|----------------|
| **host** | HOST | 本机网卡直连地址 | 2130706431 | 不需要 |
| server reflexive | SRFLX | NAT 映射后的公网地址 | 1694498815 | 需要 STUN 服务器 |
| peer reflexive | PRFLX | 对端看到的你的地址 | 1845493759 | 连接过程中自动发现 |
| relay | RELAY | TURN 服务器中转地址 | 1694498815 | 需要 TURN 服务器 |

**Step 2 我们只实现 host candidate**，即服务端直接告诉客户端"我的内网 IP 是这个，端口是这个，你往这里发 UDP 数据"。

对于同一局域网中的客户端（调试场景），host candidate 就够了。

### 3. Candidate 优先级计算公式

ICE RFC 5245 中定义的优先级公式：

```
priority = (2^24) * type_preference
         + (2^8) * local_preference
         + (2^0) * (256 - component_id)
```

对于 host candidate：
- `type_preference` = 126（最大值给 host）
- `local_preference` = 网卡索引（第一个网卡取最大值 65535）
- `component_id` = 1（只做 rtcp-mux）

```
priority = 2^24 * 126 + 2^8 * 65535 + (256 - 1)
         = 2113929216 + 16776960 + 255
         = 2130706431
```

这就是参考项目中看到的 `2130706431` 这个数字的来源。

---

## ICE 在 SDP 中的体现

加上 ICE 信息后，SDP 的媒体段中会增加这些行：

```
m=audio 9 UDP/TLS/RTP/SAVPF 111
c=IN IP4 0.0.0.0
a=mid:audio
a=ice-ufrag:bmOu           ← ★ 新增：ICE 用户名片段
a=ice-pwd:gN7glSPNwmh1J0uo+Olrdgcd  ← ★ 新增：ICE 密码
a=candidate:1 1 UDP 2130706431 192.168.1.100 54321 typ host  ← ★ 新增：候选地址
a=recvonly
a=rtcp-mux
a=rtpmap:111 opus/48000/2
```

### `a=ice-ufrag` / `a=ice-pwd`

这两行是 ICE 的**安全凭证**。当客户端收到 SDP 后，**后续所有的 STUN 消息都必须携带这个 ufrag/pwd**，服务端用 pwd 验证消息的 MESSAGE-INTEGRITY 属性。

- ufrag 是明文传输的，用于对端识别是哪个 ICE session 的流量
- pwd 是保密的，用于 HMAC-SHA1 完整性校验
- ufrag 和 pwd 每次重新 ICE 都重新随机生成

### `a=candidate`

向对端广告"我在这上面等着"。每一行就是一个 candidate。

```
a=candidate:eth0 1 UDP 2130706431 192.168.1.100 54321 typ host
```

---

## ICE 完整状态机

这个图在后面 Step 4 会非常有用，现在先预览：

```
         +-----+
         | NEW |    ← IceTransportChannel 刚创建
         +-----+
            |
            v
       +----------+
       | CHECKING |   ← 开始发 STUN Binding Request
       +----------+
          /      \
         v        v
  +-----------+  +------------+
  | CONNECTED |  |  FAILED    |  ← 成功/失败
  +-----------+  +------------+
       |
       v
  +-----------+
  | COMPLETED |   ← 选路完成
  +-----------+
```

**Step 2 不做连通性检查**，只是生成 candidate 放到 SDP 里。真正建立连接是 Step 4 的事。

---

## ICE 与 PushStream 的完整交互流程

把一个 PUSH 请求从开始到结束的 ICE 相关流程拆解开：

```
1. PushStream::create_offer() 被调用
   │
2. PortAllocator::get_udp_ports()
   │  └── NetworkManager 枚举本机网卡
   │  └── 对每个网卡创建 UDPPort
   │      └── 创建 socket + bind() 到某个端口
   │      └── 从 UDPPort 信息创建 Candidate
   │
3. Candidate 填到 SDP 的 MediaContentDescription 中
   │  └── content->add_candidates(candidates)
   │  └── 序列化后出现 a=candidate: 行
   │
4. SDP 返回给客户端
   │
5. [Step 4] 客户端发 STUN Binding Request 到 candidate 的 IP:端口
   │
6. [Step 4] 服务端 UDPPort 收到 STUN 消息，回复 Binding Response
   │
7. [Step 4] ICE 状态机推进：new → checking → connected
   │
8. [Step 5] DTLS 握手开始在 ICE 通道上跑
```

理解这个流程就理解了 Step 2 在整个链路中的位置——**它只在第 2、3 步发挥作用**：分配端口、生成 candidate、写入 SDP。

---

## 底层技术细节

### getifaddrs()

POSIX 标准 API，枚举系统所有网络接口。返回值是一个**单向链表**：

```c
struct ifaddrs *ifaddr;
getifaddrs(&ifaddr);

for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if (ifa->ifa_addr->sa_family == AF_INET) {
        // IPv4 地址
        struct sockaddr_in *addr = (struct sockaddr_in*)ifa->ifa_addr;
        // addr->sin_addr 就是 IP 地址（网络字节序）
    }
}

freeifaddrs(ifaddr);  // 必须释放
```

### 端口分配策略

当前阶段我们用**固定端口模式**：
- 每个 `UDPPort` 绑定到一个系统分配的端口（port = 0，内核自动分配）
- 读 candidate 时取出 `getsockname()` 获得的实际端口

### UDP 绑定详解

创建 UDP socket 并绑定端口的 POSIX 流程：

```c
// 1. 创建 UDP socket
int fd = socket(AF_INET, SOCK_DGRAM, 0);

// 2. 设置非阻塞（跨平台惯例，libev 的 IOWatcher 可以用非阻塞）
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);

// 3. 绑定到特定网卡 + 端口
struct sockaddr_in addr;
addr.sin_family = AF_INET;
addr.sin_addr = network->ip（二进制格式）;
addr.sin_port = htons(port ? port : 0);  // 0 = 自动分配
bind(fd, (struct sockaddr*)&addr, sizeof(addr));

// 4. 获取实际分配的端口
socklen_t len = sizeof(addr);
getsockname(fd, (struct sockaddr*)&addr, &len);
实际端口 = ntohs(addr.sin_port);
```

### STUN 协议基础（Step 4 才深入，先了解）

STUN（Session Traversal Utilities for NAT）是 ICE 的核心协议，用于：

1. **连通性检查**：发一个 Binding Request 过去，对方回 Binding Response
2. **地址发现**：Binding Response 中携带 XOR-MAPPED-ADDRESS，告诉发送方"你从外面看是这个地址"（用于 srflx）

STUN 消息的二进制格式：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|0 0|     STUN Message Type     |         Message Length        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Magic Cookie                          |
|                         0x2112A442                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                     Transaction ID (96 bits)                  |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

重要字段：
- **Message Type**：0x0001 = Binding Request, 0x0101 = Binding Response
- **Magic Cookie**：固定 0x2112A442
- **Transaction ID**：12 字节随机数，Request 和 Response 的 transaction ID 必须一致
- **Attributes**：跟在头部后面，TLV 格式（Type-Length-Value）

**当前 Step 2 不需要实现 STUN 编解码**，UDPPort 先创建 socket，收发函数先留空。Step 4 才实现 STUN message 的 read/write。

---

## 代码结构对应关系

### 新增文件与 ICE 概念的对应

| 概念 | 对应代码 | 作用 |
|------|---------|------|
| 网络接口 | `Network` + `NetworkManager` | 枚举本机网卡和 IP |
| UDP 端点 | `UDPPort` | 绑定 UDP socket，收发 STUN/媒体数据 |
| 候选地址 | `Candidate` | SDP 中的 `a=candidate:` 行 |
| ICE 凭证 | `IceParameters` + `IceCredentials` | SDP 中的 `a=ice-ufrag:` / `a=ice-pwd:` |
| Candidate 分配器 | `PortAllocator` | 持有 NetworkManager，为每个网卡创建 UDPPort |
| 异步 UDP | `AsyncUdpSocket` | 封装 EventLoop 的 IOWatcher，提供异步 UDP 收发 |

### 类间关系图

```
PortAllocator
  ├── NetworkManager
  │     └── vector<Network*>          ← 本机所有网卡
  └── get_udp_ports()
        └── for each Network:
              └── new UDPPort(network)
                    ├── new AsyncUdpSocket(fd, el)  ← UDP socket + IOWatcher
                    └── create_candidate()
                          └── Candidate              ← 填充 foundation/priority/address/port/type

PushStream::create_offer()
  ├── PortAllocator::get_udp_ports() → vector<UDPPort*>
  ├── for each UDPPort → Candidate → content->add_candidates()
  └── offer.to_string() → SDP 文本含 a=candidate: 行
```

---

## ICE 的 SDP 序列化过程

当 `SessionDescription::to_string()` 处理一个 `MediaContentDescription` 时，在 Step 2 后它输出的流程是：

```
m=audio 9 UDP/TLS/RTP/SAVPF 111
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
                                 ← 现在会在这里插入 a=candidate: 行
a=ice-ufrag:bmOu                 ← 从 TransportDescription 取
a=ice-pwd:gN7glSPNwmh1J0uo+...  ← 从 TransportDescription 取
a=mid:audio
a=recvonly
a=rtcp-mux
a=rtpmap:111 opus/48000/2
...
```

`to_string()` 的第 325 行已经调用了 `build_candidate(content, ss)` 来输出 `a=candidate:` 行。关键是我们需要确保 `content->candidates()` 里面有数据。

---

## 前后流程串讲

把从客户端发送 PUSH 请求到返回含 ICE candidate 的 SDP 的完整路径走一遍：

### 请求进入

```
TCP 连接 → SignalingServer → SignalingWorker → _process_request()
  → cmdno=1 (PUSH)
  → _process_push()
      → 构造 RtcMsg { cmdno=1, uid, stream_name, audio=1, video=1 }
      → g_rtc_server->send_rtc_msg(msg)       // 发到 RtcServer
```

### 路由

```
RtcServer::_process_rtc_msg()
  → pop_msg()
  → _get_worker(stream_name)          // CRC32("test") % 2 = 某个 worker
  → rtc_worker->send_rtc_msg(msg)     // msg 入 LockFreeQueue，pipe 写一个 int
```

### 处理

```
RtcWorker::_process_rtc_msg()
  → pop_msg()
  → switch(cmdno=1): _process_push()
      → _rtc_stream_mgr->create_push_stream(uid, stream_name, audio, video, log_id, certificate, offer)
          → new PushStream(el, uid, stream_name, audio, video, log_id)
          → PushStream::create_offer()
              → IceCredentials::create_random_ice_credentials()    ← 生成 ufrag/pwd
              → PortAllocator::get_udp_ports()                     ← 创建 UDPPort + candidates
              → MediaContentDescription->add_candidates(candidates) ← candidates 填入 content
              → SessionDescription::add_transport_info()            ← 填入 ice-ufrag/pwd 到 transport_info
              → SessionDescription::to_string()                     ← 序列化（含 a=candidate: 行）
          → offer 字符串通过 RtcMsg->sdp 返回
  → ((SignalingWorker*)msg->worker)->send_rtc_msg(msg)  // 下行
```

### 响应返回

```
SignalingWorker::_process_rtc_msg()
  → pop_msg()
  → _response_server_offer(msg)
      → 构造 JSON { err_no: 0, offer: "<sdp 文本>" }
      → _add_reply(json_str, conn)
      → TCP socket 写回 signaling 服务
```

---

## 总结：Step 2 到底做了什么

一句话概括：

> **Step 2 让服务端的 SDP 中出现了 ICE 传输信息，包括 ice-ufrag、ice-pwd 和 host candidate，告诉客户端"我的 UDP 端口开在这里，数据发到这个 IP:端口"。**

具体：
1. 用 `getifaddrs()` 枚举本机网卡，拿到 IP 地址
2. 在每个网卡上创建 UDP socket 并绑定端口
3. 从绑定的 socket 提取 IP:端口，生成 `Candidate` 对象
4. 随机生成 ICE ufrag 和 pwd
5. 把 candidate 和 ufrag/pwd 填入 `SessionDescription`
6. `to_string()` 序列化时输出 `a=candidate:` / `a=ice-ufrag:` / `a=ice-pwd:` 行

**但此时还没有连通性检查**。客户端拿到 SDP 后发的 STUN Binding Request，服务端会收到包到 UDP socket 上，但还没有解析和回复的能力——这是 Step 4 的内容。

---

## 下一步代码实现

读完这篇后，你应该按这个顺序写代码：

1. **`src/base/network.h + .cpp`** → Network / NetworkManager（getifaddrs 枚举网卡）
2. **`src/ice/candidate.cpp`** → Candidate 的 get_priority()（注意：当前 Candidate 只是个结构体，可以通过辅助函数计算 priority，不是成员方法）
3. **`src/base/async_udp_socket.h + .cpp`** → AsyncUdpSocket（EventLoop + IOWatcher 异步 UDP）
4. **`src/ice/port_allocator.h + .cpp`** → PortAllocator（持有 NetworkManager）
5. **`src/ice/udp_port.h + .cpp`** → UDPPort（socket 绑定 + candidate 生成）
6. **修改 `push_stream.cpp`** → 使用 PortAllocator 获取 UDPPort，生成 candidate 填入 SDP
7. **修改 `rtc_stream_manager.h + .cpp`** → 持有 PortAllocator，传给 create_offer()

每个文件我们一步一步来，先从 `Network` / `NetworkManager` 开始。
