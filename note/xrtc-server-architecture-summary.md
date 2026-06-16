# xrtc-server 全链路走读：从 PUSH 到 RTP 转发

这篇文档以一条推流+拉流的完整时间线，串起 xrtc-server 的所有核心模块。

---

## 全局线程布局

```
main() 线程
  │
  ├─ SignalingServer 线程（1 个 libev loop）
  │    └─ accept TCP:9000 → round-robin → SignalingWorker
  │
  ├─ SignalingWorker 线程 ×N（libev loop）
  │    └─ xhead + JSON 解析 → 构造 RtcMsg → 发给 RtcServer
  │
  ├─ RtcServer 线程（1 个 libev loop）
  │    └─ 注入证书 + CRC32(stream_name) % N 路由 → RtcWorker
  │
  └─ RtcWorker 线程 ×N（libev loop）
       └─ PushStream / PullStream / PeerConnection /
          ICE / DTLS / SRTP / RTP转发 全部在这一个线程里
```

关键设计：**媒体面（RTP 收→解密→加密→发）全程在同一 RtcWorker 线程，零跨线程开销。**

---

## 第一段：PUSH 请求 → TCP 协议解析

```
客户端 TCP 连上来
  │
  ▼
SignalingServer::accept_new_connect()
  → tcp_accept() 拿到 conn_fd
  → _dispatch_new_conn(conn_fd)
    → index = _next_worker_index++ % worker_num
    → workers[index]->notify_new_conn(fd)
      → LockFreeQueue<int> 入队 + pipe write 唤醒 worker
```

SignalingWorker 被唤醒后：

```
SignalingWorker::_new_conn(fd)
  → new TcpConnection(sds buffer)
  → 注册 conn_io_cb (READ | WRITE) + 100ms 超时 timer

客户端发来数据 → conn_io_cb → _read_query(fd) → _process_query_buffer(c)

状态机：STATE_HEAD (读 36 字节 xhead_t)
  → 校验 magic_num == 0xfb202202
  → 提取 body_len
  → 切换到 STATE_BODY

STATE_BODY (读 body_len 字节 JSON)
  → _process_request(c)
  → json["cmdno"] → CMDNO_PUSH
  → _process_push(c, json):
      提取 uid, stream_name, audio, video
      构造 RtcMsg{cmdno=1, uid, stream_name, audio, video, worker=this, conn=c}
      调用 g_rtc_server->send_rtc_msg(msg)
```

**RtcMsg 是这个系统里跨线程传递的核心消息结构**（`src/xrtc_server_def.h`）：

```cpp
struct RtcMsg {
    int cmdno;              // 1=PUSH, 2=PULL, 3=ANSWER, 4=STOP_PUSH, 5=STOP_PULL
    uint64_t uid;
    std::string stream_name;
    std::string stream_type; // "push" / "pull" (ANSWER 时区分)
    int audio, video;
    uint32_t log_id;
    void* worker;            // SignalingWorker* → 回包用
    void* conn;              // TcpConnection*  → TCP write 用
    std::string sdp;         // offer/answer 的 SDP 字符串
    int err_no;
    void* certificate;       // RtcServer 注入的 DTLS 证书
};
```

---

## 第二段：跨线程路由 → RtcWorker

```
SignalingWorker::_process_push()
  → g_rtc_server->send_rtc_msg(msg)
    → 入队 std::queue (mutex guard)
    → pipe write("RTC_MSG") 唤醒 RtcServer

RtcServer 线程被唤醒:
  _process_rtc_msg():
    1. msg 出队
    2. _generate_and_check_certificate() → msg->certificate = DTLS 证书
    3. _get_worker(stream_name):
         idx = rtc::ComputeCrc32(stream_name) % worker_num
         return _workers[idx]
    4. worker->send_rtc_msg(msg)
       → LockFreeQueue<RtcMsg> + pipe write 唤醒 RtcWorker
```

**CRC32 路由是 PULL 能找到 PUSH 的基础**：同一 stream_name 的 PUSH 和 PULL 必然落在同一 RtcWorker。

---

## 第三段：RtcWorker → PushStream → PeerConnection → 生成 SDP Offer

```
RtcWorker 被唤醒:
  _process_rtc_msg() → cmdno == PUSH → _process_push(msg)

_process_push(msg):
  offer = _rtc_stream_mgr->create_push_stream(
      uid, stream_name, audio, video, msg->certificate, ...)
  msg->sdp = offer
  msg->worker->send_rtc_msg(msg)  // 回包给 SignalingWorker
```

**RtcStreamManager::create_push_stream()** 做了什么：

```
1. new PushStream(event_loop, options)
     → 构造函数里 new PeerConnection(event_loop)
     → 注册 signal_connection_state / signal_rtp_packet_received / signal_rtcp_packet_received

2. stream->register_listener(this)
     → RtcStreamManager 订阅这个 stream 的事件

3. stream->start(certificate)
     → _pc->init(certificate):
         _certificate = certificate
         _transport_controller->set_local_certificate(certificate)
     → 启动 30 秒 ICE 超时计时器（超时则 on_stream_exception）

4. offer = stream->create_offer()
```

**PushStream::create_offer()**：

```
→ 设置 transceiver 方向:
    recv_audio = true, recv_video = true   // 推流端 = 收流
    send_audio = false, send_video = false
→ return _pc->create_offer(options)
```

**PeerConnection::create_offer()** —— 核心 SDP 生成逻辑：

```
1. 创建 _local_desc (SessionDescription, type=k_offer)

2. 生成 IceParameters:
     ice_ufrag = random 4 bytes
     ice_pwd = random 24 bytes

3. 构造 AudioContentDescription (mid="audio"):
     → 添加 codec: opus/48000/2, payload=111
     → 添加 feedback: transport-cc
     → 设置 direction: recvonly

4. 构造 VideoContentDescription (mid="video"):
     → 添加 codec: H264/90000, payload=96
     → 添加 rtx: payload=97, apt=96
     → 添加 feedback: goog-remb, transport-cc, ccm fir, nack, pli
     → 设置 direction: recvonly

5. 设置 BUNDLE group (audio + video 复用同一传输通道)

6. 调用 _transport_controller->set_local_description(desc)
     ↓ 这是搭建 ICE + DTLS + SRTP 管道的关键步骤 ↓

7. 序列化 SDP → to_string() → 返回 offer 字符串
```

---

## 第四段：TransportController 搭建传输管道 + ICE 收集 Candidate

**TransportController::set_local_description()**：

```
for each media content (audio, video — 但 BUNDLE 下只建一套传输):

  1. channel = _ice_agent->create_channel(mid, component=1)
       → new IceTransportChannel(el, port_allocator, ice_params)
       → 存入 _channels

  2. channel->set_ice_params(local_ice_params)
       → 存储 _local_ice_params (ufrag + pwd)

  3. dtls = new DtlsTransport(channel)
       → 订阅 channel 的 signal_read_packet (ICE 收到的数据 → DTLS)
       → 订阅 channel 的 signal_writable_state (ICE 连通 → 启动 DTLS)

  4. dtls_srtp = new DtlsSrtpTransport()
       → dtls_srtp->set_dtls_transport(dtls)
       → 订阅 dtls 的 signal_dtls_state (握手完成 → 导出 SRTP 密钥)
       → 订阅 dtls 的 signal_read_packet (RTP/RTCP → 解密)

  5. 串联信号链路:
       ICE channel → DtlsTransport → DtlsSrtpTransport → TransportController

  6. _ice_agent->gathering_candidate()  ← 开始收集本地候选地址
```

### IceAgent::gathering_candidate() — UDP 候选地址收集

```
IceAgent::gathering_candidate():
  for each channel:
    channel->gathering_candidate()

IceTransportChannel::gathering_candidate():
  for each local network (PortAllocator 枚举网卡):
    port = new UDPPort()
    port->create_ice_candidate(ice_params, component=1)

UDPPort::create_ice_candidate():
  1. create_udp_socket() + sock_bind()  → 绑定 UDP 端口
  2. 包装为 AsyncUdpSocket → 注册 READ 事件到 event loop
  3. 构造 Candidate:
       type = "host"
       component = 1 (RTP)
       protocol = "udp"
       priority = 126 << 24 | (65535 - local_port) << 8 | (256 - 1)
       username = ice_ufrag + ":" + local_ufrag
       foundation = CRC32(type + protocol + base_address)
  4. _local_candidates.push_back(candidate)
  5. signal_candidate_allocate_done ↑ 上报
```

候选地址逐层上回调：

```
UDPPort → IceTransportChannel::signal_candidate_allocate_done
  → IceAgent::_on_candidate_allocate_done
    → TransportController::_on_candidate_allocator_done
      → PeerConnection::_on_candidate_allocate_done
        → 将 candidate 写入 _local_desc 的 MediaContentDescription
```

---

## 第五段：SDP Offer 回传给客户端

SDP offer 生成完成后，沿调用栈返回：

```
PushStream::create_offer() → RtcStreamManager::create_push_stream()
  → RtcWorker::_process_push()
    → msg->sdp = offer
    → msg->worker->send_rtc_msg(msg)  // 发给 SignalingWorker
```

SignalingWorker 收到响应：

```
SignalingWorker::_process_rtc_msg(msg):
  → _response_server_offer(msg):
      构造 JSON: {"errno":0, "err_msg":"success", "offer":"<SDP字符串>"}
      复用 xhead_t（更新 body_len）
      _add_reply(c, reply):
        → c->reply_list.push_back(reply)
        → 开启 EventLoop::WRITE watcher

  → _write_query(fd):
      从 reply_list 取数据
      sock_write_data(fd, data, len)
      写完关闭 WRITE watcher
```

**这条 offer 里有什么**（客户端看到的内容）：

```
v=0
o=- ... IN IP4 0.0.0.0
s=-
t=0 0
a=group:BUNDLE audio video            ← 音视频复用同一传输
m=audio 9 UDP/TLS/RTP/SAVPF 111      ← DTLS-SRTP
c=IN IP4 0.0.0.0
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=ice-ufrag:xxxx
a=ice-pwd:yyyyyyyyyyyyyyyyyyyyyyyy
a=fingerprint:sha-256 XX:XX:...       ← DTLS 证书指纹
a=setup:actpass
a=mid:audio
a=recvonly
a=candidate:... typ host ...          ← 服务端 UDP 候选地址
a=rtcp-mux
m=video 9 UDP/TLS/RTP/SAVPF 96 97
... (video codec + 同上)
```

---

## 第六段：ANSWER 到达，客户端应答

ANSWER 的处理链路与 PUSH 相同（TCP → SignalingWorker → RtcServer → RtcWorker）：

```
客户端发 ANSWER:
  SignalingWorker::_process_request()
    → cmdno == ANSWER → _process_answer(c, json):
        解析 uid, stream_name, stream_type("push"), answer_sdp
        RtcMsg{cmdno=ANSWER, sdp=answer_sdp, stream_type="push"}
        g_rtc_server->send_rtc_msg(msg)

路由: RtcServer → CRC32(stream_name) → RtcWorker (与 PUSH 同一 worker!)

RtcWorker::_process_answer(msg):
  → _rtc_stream_mgr->set_answer(uid, stream_name, sdp, "push")

RtcStreamManager::set_answer():
  push_stream = _push_streams[stream_name]
  push_stream->set_remote_sdp(answer_sdp)
```

**PeerConnection::set_remote_sdp()** —— 解析远端 SDP：

```
逐行解析 answer SDP:
  1. a=group:BUNDLE audio video  → 确认复用组
  2. m=audio ...  /  m=video ... → 创建媒体内容描述
  3. a=ice-ufrag:xxx / a=ice-pwd:xxx  → remote ICE 参数
  4. a=fingerprint:sha-256 XX:XX:...  → remote DTLS 指纹
  5. a=ssrc:123456 cname:client_cname → 客户端 SSRC
  6. a=ssrc-group:FID 123456 123457   → FEC 组
  7. a=candidate:...  → 客户端候选地址（可选，ICE 的 trickle 模式下可能没有）

→ 构造 _remote_desc (SdpType::k_answer)
→ 提取 TransportDescription (ICE ufrag/pwd + DTLS fingerprint)
→ 提取 StreamParams (SSRCs, cname, track_id, stream_id)
→ _transport_controller->set_remote_description(remote_desc)
```

**TransportController::set_remote_description()**：

```
for each media content:
  1. _ice_agent->set_remote_ice_params(mid, component, IceParameters)
     → IceTransportChannel::set_remote_ice_params():
         存储 _remote_ice_params
         通知所有已有连接 maybe_set_remote_ice_params()
         _sort_connections_and_update_state()  // 连接状态重新排序

  2. dtls_transport->set_remote_fingerprint(algorithm, digest)
     → 存储远端指纹
     → 可能触发 _maybe_start_dtls() (如果 ICE 已经 writable)
```

**重点：ANSWER 和 ICE/DTLS 是异步的**。客户端可能在收到 offer 后、发送 answer 之前就开始发 STUN Binding Request 和 DTLS ClientHello。服务端必须处理这些乱序到达。

---

## 第七段：STUN 绑定请求 → 创建 peer reflexive candidate

客户端发出 STUN Binding Request（UDP 面）：

```
UDPPort::_on_read_packet(data, size, remote_addr):
  1. 收到 UDP 数据包
  2. 是否来自已有 IceConnection？
     - 是 → conn->on_read_packet(data, size)  // 交给已有连接处理
     - 否 → 尝试 STUN 解析 ↓

  3. get_stun_message(data, size):
       a. validate_fingerprint()  → CRC32 快速检查（非 STUN 直接返回）
       b. StunMessage::read():    → 解析 20 字节头 (type, length, transaction_id)
                                  → TLV 属性循环解析
       c. validate_message_integrity(ice_pwd)
          → HMAC-SHA1 验证，排除 MI 属性和 FINGERPRINT 后的长度
       d. 提取 USERNAME (格式: remote_ufrag:local_ufrag)
       e. 成功 → 返回 StunMessage

  4. 是 Binding Request → signal_unknown_address(remote_addr, stun_msg)
```

**IceTransportChannel::_on_unknown_address()** —— 创建 prflx candidate：

```
1. 从 STUN PRIORITY 属性提取优先级
2. 构造 Candidate:
     type = "prflx"  (peer reflexive)
     address = remote_addr (数据包来源 IP:Port)
     priority = STUN PRIORITY 属性值
3. UDPPort::create_connection(remote_candidate)
     → new IceConnection(_local_candidate, remote_candidate)
     → _add_connection(conn):
         注册到 IceController
         加入 _unpinged_connections 集合
4. conn->handle_stun_binding_request()
     → send_stun_binding_response():
         构造 STUN Binding Success Response
         添加 XOR-MAPPED-ADDRESS (客户端的反射地址)
         添加 MESSAGE-INTEGRITY + FINGERPRINT
         _port->send_to() → UDP 发出
5. _sort_connections_and_update_state()
     → IceController 排序 → 可能更新 selected connection
```

---

## 第八段：ICE Ping 周期 + 候选对选优

IceTransportChannel 有一个 48ms 的定时器持续驱动 ICE 检查：

```
IceTransportChannel::ice_ping_cb() → _on_check_and_ping():

1. _update_connection_states():
   for each conn:
     conn->update_state(now):
       writable 状态下连续 N 次 ping 失败 + 5s → STATE_WRITE_UNRELIABLE
       UNRELIABLE + 15s 无响应 → STATE_WRITE_TIMEOUT (dead)

2. _ice_controller->select_connection_to_ping(last_ping_sent_ms):
   a. 判断 channel 强弱:
        selected connection 不 writable 或丢失 → weak (48ms)
        正常 → strong (480ms)
   b. _find_next_pingable_connection(now):
        优先从 _unpinged_connections 选
        round-robin 遍历
   c. 通过 _is_pingable(conn, now) 检查:
        需要 remote ufrag+pwd 已知
        channel weak → 无条件可 ping
        channel strong → 必须过 connection 级间隔门控

3. _ping_connection(conn):
   conn->ping(last_ping_sent_ms)
```

### IceConnection — 发送 STUN Binding Request

```
IceConnection::ping():

1. 创建 ConnectionRequest (STUN Binding Request):
     → 自动添加 USERNAME, PRIORITY, MESSAGE-INTEGRITY, FINGERPRINT
     → 如果处于提名阶段, 添加 USE-CANDIDATE
2. request->prepare() → 序列化 STUN 消息
3. _on_stun_send_packet(buf, size):
     → _port->send_to(buf, size, remote_addr)
4. set_state(IN_PROGRESS)
```

### IceConnection — 处理 STUN Binding Response

```
收到对方 STUN 响应:
  conn->on_read_packet(data, size):
    → 判断是 STUN response
    → 验证 MESSAGE-INTEGRITY
    → _request_manager.check_response(transaction_id)
      → on_connection_request_response():
          rtt = now_ms - sent_time_ms
          received_ping_response(rtt):
            _rtt = (_rtt * 3 + rtt) / 4   // 指数移动平均
            set_write_state(STATE_WRITABLE)
            set_state(SUCCEEDED)
            update_receiving()
```

### IceController — 连接排序与优选

5 级排序权重：

```
1. writable > not writable
2. write_state: WRITABLE > UNRELIABLE > TIMEOUT
3. receiving > not receiving
4. priority 高的优先
5. RTT 低的优先
```

切换防抖：新连接 RTT 必须比当前 selected connection 至少好 10ms 才切换，避免 ping-pong 振荡。

选中的连接通过 `_selected_connection` 指针持有，后续所有 RTP/RTCP 发送都通过它。

---

## 第九段：IceTransportChannel — ICE 传输通道状态聚合

IceTransportChannel 聚合所有 IceConnection 的状态，计算出通道级状态：

```
_compute_ice_transport_state():
  - 有 WRITABLE 连接    → k_connected
  - 有 RECEIVING 连接   → k_connected
  - 全部 TIMEOUT        → k_failed
  - 其他                → k_disconnected
```

状态变化通过 signal 链路上报：

```
IceTransportChannel::signal_ice_transport_state
  → IceAgent → TransportController
    → _update_state() → signal_connection_state
      → PeerConnection → RtcStream
        → k_failed → RtcStreamManager::on_stream_exception()
```

---

## 第十段：DTLS 握手

### DTLS 启动时机

DtlsTransport 订阅 ICE channel 的两个信号：

```
signal_receiving_state  → _on_receiving_state()
signal_writable_state   → _on_writable_state()
```

当 ICE channel 变为 writable：

```
_on_writable_state():
  if _dtls_active && _dtls_state == k_new:
    _maybe_start_dtls()
```

**_setup_dtls()** —— 搭建 DTLS 握手环境：

```
1. 创建 StreamInterfaceChannel (ICE ↔ OpenSSL 的适配器)
     Write()  → ICE channel::send_packet()  (加密数据发出)
     Read()   → 从 BufferQueue 取 OpenSSL 需要的数据

2. 创建 SSLStreamAdapter (OpenSSL 封装)
     设置: DTLS 1.2, server role
     设置: 本地证书 (self-signed)
     设置: 远端指纹 (sha-256)
     设置: SRTP crypto suites (srtp_profile)

3. _dtls->StartSSL()
     → OpenSSL DTLS 握手开始
     → set_dtls_state(k_connecting)
```

### DTLS 数据流 — StreamInterfaceChannel 胶水层

```
[上行] ICE 收到加密 DTLS 包:
  IceTransportChannel::signal_read_packet
    → DtlsTransport::_on_read_packet()
      → is_dtls_packet(): 检查 ContentType 范围 20-63
      → _handle_dtls_packet():
          校验每个 DTLS record header 长度
          _downward->on_received_packet(data, size)
            → StreamInterfaceChannel → 入队 BufferQueue
            → OpenSSL 通过 Read() 消费

[下行] OpenSSL 产出数据:
  SSLStreamAdapter 回调 → _on_dtls_event():
    - SE_WRITE: 加密数据就绪
        → StreamInterfaceChannel::Write()
          → _channel->send_packet() → ICE → UDP 发出
    - SE_READ:  解密数据就绪
        → 上层读取明文字节
    - SE_OPEN:  DTLS 握手完成!
        → set_dtls_state(k_connected)
        → signal_dtls_state 发射 → DtlsSrtpTransport 开始工作
```

### ClientHello 早到的处理

客户端可能在 ICE writable 之前就发来 DTLS ClientHello：

```
_on_read_packet() 中:
  if _dtls_state == k_new && !_dtls_active:
    // ICE 还没 writable → 缓存
    _catched_client_hello = Buffer(data, size)
    return

等到 _on_writable_state → _maybe_start_dtls():
  _setup_dtls() 创建好 DTLS 环境后:
    if _catched_client_hello.size() > 0:
      _handle_dtls_packet(_catched_client_hello)  // 重放
```

---

## 第十一段：DtlsSrtpTransport — SRTP 密钥导出与安装

DTLS 握手完成后：

```
signal_dtls_state(k_connected) → DtlsSrtpTransport::_on_dtls_state()
  → _maybe_setup_dtls_srtp()
    → _setup_dtls_srtp():

1. _extract_params():
     a. 获取 SRTP crypto suite (从 DTLS 握手协商结果)
     b. key_len + salt_len 查表
     c. dtls_transport->export_keying_material("EXTRACTOR-dtls_srtp", ...)
        → OpenSSL SSL_export_keying_material()
     d. 切分密钥材料:
          [client_write_key(16) | server_write_key(16) |
           client_write_salt(12) | server_write_salt(12)]
     e. 服务端角色 (DTLS server):
          send_key = server_write_key + server_write_salt
          recv_key = client_write_key + client_write_salt

2. set_rtp_params(send_key, recv_key):
     → _send_session = new SrtpSession(send_key, send=true)
     → _recv_session = new SrtpSession(recv_key, send=false)
```

### SrtpSession 的方向隔离

```
srtp_policy_t.ssrc.type:
  send_session → ssrc_any_outbound   (只能加密)
  recv_session → ssrc_any_inbound    (只能解密)
```

一个 SrtpSession 只能做一种操作，不能混用。强制方向隔离防止错误。

---

## 第十二段：RTP/RTCP 包解复用与加解密

### 收包路径（解密）

```
UDP 收到 RTP/RTCP 包:
  UDPPort::_on_read_packet()
    → 非 STUN → signal_read_packet
    → IceTransportChannel → DtlsTransport::_on_read_packet()
      → is_dtls_packet()? 否
      → signal_read_packet → DtlsSrtpTransport::_on_read_packet()

DtlsSrtpTransport::_on_read_packet():
  → infer_rtp_packet_type(data, len):
      RTP:  payload_type 在 64-127 范围
      RTCP: payload_type 在 192-223 范围
  → RTP:  _on_rtp_packet_received(data, len)
  → RTCP: _on_rtcp_packet_received(data, len)

_on_rtp_packet_received(data, len):
  1. _recv_session->unprotect_rtp(data, &len)  // SRTP 原地解密
     → 解密成功后 len 变短（减掉 auth tag）
     → packet.SetSize(out_len) 截掉尾部
  2. signal_rtp_packet_received(data, len) → 上层转发

_on_rtcp_packet_received(data, len):
  1. _recv_session->unprotect_rtcp(data, &len)  // SRTCP 原地解密
     → RTCP 加密比 RTP 多一个 SRTCP index (4 字节)
  2. signal_rtcp_packet_received(data, len)
```

### 发包路径（加密）

```
send_rtp(data, len):
  1. need_len = len + auth_tag_len
  2. packet.SetSize(need_len)  // 扩大缓冲区
  3. _send_session->protect_rtp(data, &len)  // SRTP 原地加密
  4. _rtp_dtls_transport->send_packet(data, len)
     → DtlsTransport → StreamInterfaceChannel::Write()
       → IceTransportChannel::send_packet()
         → _selected_connection->send_packet()
           → UDPPort::send_to()
             → AsyncUdpSocket::send_to()  // 内核 UDP 发送
```

### AsyncUdpSocket 发送优化

```
send_to(data, len, addr):
  1. optimistic send: 直接 sendto()，成功就返回
  2. 失败 (EAGAIN/EWOULDBLOCK) → 入队 _send_queue
  3. 开启 EventLoop::WRITE watcher:
     _on_write_event() → 从队列取数据 sendto()
     队列空了 → 关闭 WRITE watcher (避免 busy loop)
```

---

## 第十三段：PULL 流 — 拉流客户端接入

### PULL 请求处理

```
客户端 PULL 请求:
  SignalingWorker::_process_pull()
    → RtcMsg{cmdno=CMDNO_PULL, uid, stream_name, audio, video}
    → g_rtc_server->send_rtc_msg(msg)
    → CRC32(stream_name) → 与 PUSH 同一个 RtcWorker!

RtcWorker::_process_pull(msg):
  → _rtc_stream_mgr->create_pull_stream(uid, stream_name, audio, video)

RtcStreamManager::create_pull_stream():
  1. 查找同名 push_stream
  2. 从 push_stream 提取音视频源:
     push_stream->get_audio_source(source):
       → _pc->remote_desc()->audio_content()->streams()
       → 返回 StreamParams 列表 (包含 SSRC, cname, track_id)
     push_stream->get_video_source(source):
       → 同上, 从 video content 提取
  3. new PullStream(el, options)
  4. pull_stream->add_audio_source(source)
     → _pc->add_audio_source(source) → 存入 _audio_source 列表
  5. pull_stream->add_video_source(video_source)
     → 同理
  6. pull_stream->start(certificate)
     → _pc->init(certificate) → 启动 ICE 超时计时器
  7. offer = pull_stream->create_offer()
```

### PullStream SDP Offer — 带 SSRC 信息

```
PullStream::create_offer():
  → send_audio = true, send_video = true  (与 PUSH 相反: sendonly)
  → recv_audio = false, recv_video = false
  → _pc->create_offer(options)

PeerConnection::create_offer() 中:
  if options.send_audio:
    for stream in _audio_source:  // 之前注入的 StreamParams
      audio_content->add_stream(stream)
  if options.send_video:
    for stream in _video_source:
      video_content->add_stream(stream)

to_string() 序列化 → build_ssrc():
  a=ssrc:4012345678 cname:push_client_cname
  a=ssrc:4012345678 msid:stream_id track_id
  a=ssrc-group:FID 4012345678 4012345679
```

**这样拉流客户端看到的 SSRC 就是推流端实际使用的 SSRC**，拉流端可以正确解码。

### Pull 的 ANSWER / ICE / DTLS / SRTP

与 PUSH 完全相同的流程：

```
ANSWER(type="pull") → RtcStreamManager::set_answer()
  → pull_stream->set_remote_sdp(answer)
  → _pc->set_remote_sdp(sdp)
  → _transport_controller->set_remote_description()
  → ICE → DTLS 握手 → SRTP 密钥导出
```

---

## 第十四段：RTP/RTCP 转发 — 推流 → 拉流

### 转发触发

RtcStreamManager 订阅了 push 和 pull stream 的信号：

```
RtcStreamManager::on_rtp_packet_received(stream, data, len):
  if stream->type() == k_push:
    pull_stream = _find_pull_stream(stream_name)
    if pull_stream:
      pull_stream->send_rtp(data, len)
        → _pc->send_rtp(data, len)
          → _transport_controller->send_rtp("audio", data, len)
            → DtlsSrtpTransport::send_rtp(data, len)
              1. protect_rtp() → SRTP 加密 (用 Pull 端的 send_key)
              2. DTLS → ICE → UDP → 发给拉流客户端

RtcStreamManager::on_rtcp_packet_received(stream, data, len):
  双向转发:
    k_push → pull_stream->send_rtcp()   // SR/RR 给拉流端
    k_pull → push_stream->send_rtcp()   // PLI 给推流端 (请求 I 帧)
```

### 完整端到端数据流

```
推流客户端                       xrtc-server                     拉流客户端
─────────                       ───────────                     ──────────

① PUSH ────────────→  TCP:xhead+JSON
                      CRC32路由
                      创建 PushStream
                         ICE收集candidate
                         DTLS证书指纹
② ←────────────────  SDP offer (recvonly, ICE, DTLS fingerprint)

③ ANSWER ─────────→  set_remote_sdp()
                      解析remote ICE/DTLS
                      提取SSRC

④ STUN req ───────→  UDPPort接收
    ↕ ping/pong       创建prflx candidate
                      创建IceConnection
    ↕                  ↕ ICE连接选优(IceController) ↕
                      IceTransportChannel聚合状态

⑤ ClientHello ────→  DtlsTransport::_on_read_packet
    ↕ 握手            _setup_dtls()
    ↕                 OpenSSL ↔ StreamInterfaceChannel ↔ ICE
                      set_dtls_state(k_connected)

⑥                   DTLS-SRTP密钥导出
                      创建SrtpSession(send+recv)
                      DtlsSrtpTransport就绪

RTP/RTCP加密 ──────→  SRTP解密
⑥                     on_rtp_packet_received()

                      ⑦ PULL ──────────────→  TCP:xhead+JSON
                         提取push的SSRC
                         创建PullStream
                      ⑧ ←────────────────  SDP offer (sendonly, with SSRCs)
                      ⑨ ANSWER ←─────────  set_remote_sdp()
                          ↕ ICE/DTLS/SRTP ↕

⑥                     ⑩ pull_stream->send_rtp()
                         SRTP加密(Pull端send_key) ──→ 拉流端收到RTP
⑩                     ⑪ ↔ RTCP双向转发 ↔
                         PLI → push / SR → pull
```

---

## 第十五段：停止 + 异常处理

### 停止命令

```
STOP_PUSH → RtcWorker → _rtc_stream_mgr->stop_push(name)
  → 销毁 PushStream
  → 通知对应 PullStream 也停止

STOP_PULL → RtcWorker → _rtc_stream_mgr->stop_pull(name)
  → 销毁 PullStream
```

### 异常处理路径

```
1. ICE 30秒超时:
     RtcStream::_ice_timeout_timer_cb
       → RtcStreamManager::on_stream_exception()  // 销毁流

2. ICE 连接退化:
     IceConnection::update_state():
       WRITABLE → UNRELIABLE (5次ping失败+5s) → TIMEOUT (15s)
     IceTransportChannel 聚合为 k_failed
       → TransportController::_update_state() → k_failed
       → PeerConnection → RtcStreamManager → 销毁流

3. DTLS 握手失败:
     DtlsTransport::_on_dtls_handshake_error()
       → set_dtls_state(k_failed)
       → TransportController::_update_state() → k_failed

4. PeerConnection 延迟销毁:
     destroy() 创建 10ms 延迟 timer:
       防止在 ICE timer 回调栈内析构导致 use-after-free
       (ICE timer 回调返回后代码还要访问 IceController)
```

---

## 完整文件索引（按消息流顺序）

| 步骤 | 文件 | 核心类/函数 |
|------|------|------------|
| TCP accept | `src/server/signaling_server.cpp` | `SignalingServer::_dispatch_new_conn()` |
| 协议解析 | `src/server/signaling_worker.cpp` | `_read_query()` → `_process_query_buffer()` → `_process_push()` |
| 协议头 | `src/base/xhead.h` | `xhead_t` (36字节, magic=0xfb202202) |
| 连接状态机 | `src/server/tcp_connection.h/.cpp` | `TcpConnection` (STATE_HEAD/STATE_BODY) |
| 跨线程消息 | `src/xrtc_server_def.h` | `RtcMsg` (cmdno, uid, stream_name, sdp, certificate...) |
| CRC32路由 | `src/server/rtc_server.cpp` | `RtcServer::_get_worker()` → `ComputeCrc32() % N` |
| 流管理 | `src/server/rtc_worker.cpp` | `RtcWorker::_process_push/pull/answer()` |
| 流管理 | `src/stream/rtc_stream_manager.cpp` | `RtcStreamManager::create_push_stream/pull_stream/set_answer()` |
| 流基类 | `src/stream/rtc_stream.cpp` | `RtcStream` (PC封装, ICE超时, RTP/RTCP send) |
| 推流 | `src/stream/push_stream.cpp` | `PushStream::create_offer()` (recvonly) |
| 拉流 | `src/stream/pull_stream.cpp` | `PullStream::create_offer()` (sendonly + SSRC) |
| PC | `src/pc/peer_connection.cpp` | `PeerConnection::create_offer/set_remote_sdp/send_rtp` |
| SDP | `src/pc/session_description.cpp` | `SessionDescription`, codec, SSRC序列化 |
| 传输控制 | `src/pc/transport_controller.cpp` | `TransportController::set_local/remote_description()` |
| ICE代理 | `src/ice/ice_agent.cpp` | `IceAgent::create_channel/gathering_candidate()` |
| ICE通道 | `src/ice/ice_transport_channel.cpp` | `IceTransportChannel::_on_check_and_ping()` |
| ICE控制 | `src/ice/ice_controller.cpp` | `IceController::select_connection_to_ping/sort_and_switch()` |
| ICE连接 | `src/ice/ice_connection.cpp` | `IceConnection::ping/on_read_packet/update_state()` |
| UDP端口 | `src/ice/udp_port.cpp` | `UDPPort::create_ice_candidate/_on_read_packet()` |
| STUN | `src/ice/stun.h/.cpp` | `StunMessage` (read, validate, HMAC, fingerprint) |
| DTLS | `src/pc/dtls_transport.cpp` | `DtlsTransport` (OpenSSL, StreamInterfaceChannel) |
| SRTP | `src/pc/dtls_srtp_transport.cpp` | `DtlsSrtpTransport` (密钥导出, protect/unprotect) |
| RTP/RTCP转发 | `src/stream/rtc_stream_manager.cpp` | `on_rtp_packet_received/on_rtcp_packet_received()` |
| RTP分拣 | `src/pc/dtls_srtp_transport.cpp` | `infer_rtp_packet_type()` |
| UDP发送 | `src/ice/async_udp_socket.cpp` | `AsyncUdpSocket` (optimistic send + 队列兜底) |

## 关键队列一览

| 从 | 到 | 队列 | 唤醒 |
|----|----|------|------|
| SignalingServer | SignalingWorker | `LockFreeQueue<int>` (SPSC) | pipe write |
| SignalingWorker | RtcServer | `std::queue + mutex` | pipe write |
| RtcServer | RtcWorker | `LockFreeQueue<RtcMsg>` (SPSC) | pipe write |
| RtcWorker | SignalingWorker | `LockFreeQueue<RtcMsg>` (SPSC) | pipe write |

## Trickle ICE 下的异步时序处理

三个关键异步场景：

1. **DTLS ClientHello 先于 Answer 到达** → `DtlsTransport::_catched_client_hello` 缓存
2. **SRTP 安装等待 DTLS 握手完成** → signal_dtls_state + 立即检查双重触发
3. **ICE 连接建立后才开始 DTLS** → `_on_writable_state` + `_on_receiving_state` 双路径触发 `_maybe_start_dtls`

## 几个"为什么"的答案

**为什么 CRC32 路由？** 保证同一 stream_name 的 PUSH 和 PULL 落在同一 RtcWorker，转发路径可以同步调用，零跨线程开销。

**为什么 PULL 的 SDP offer 要带 SSRC？** 因为 xrtc-server 不做转码，不解码编码层。它只是 SRTP 解密→重加密。SSRC 直接透传给拉流端，让拉流端知道要解码哪些流。

**为什么 DTLS 握手完成后才算 media 就绪？** SRTP 的加密密钥是从 DTLS 握手主密钥导出的（RFC 5705）。DTLS 不完成，SRTP 就没有密钥。

**为什么 PeerConnection 析构要延迟 10ms？** ICE timer 回调栈内触发状态变化 → k_failed → delete stream → 析构 IceController。但回调返回后调用方还要访问 IceController → coredump。10ms 定时器让 delete 发生在下个事件循环迭代。

**为什么 ICE 连接排序要有 10ms RTT 防抖？** 防止两个连接 RTT 接近时反复切换（ping-pong 振荡），每次切换都有丢包风险。

**SDP 中的 fingerprint 是什么？起什么作用？** WebRTC 的 DTLS 证书都是自签名的，没有 CA 证书链验证。fingerprint 是 DTLS 证书的 SHA-256 哈希，通过 SDP（信令通道）交换，替代 CA 完成身份认证。链路：`RTCCertificate → SSLFingerprint::CreateFromCertificate() → TransportDescription::identity_fingerprint → to_string() → a=fingerprint:sha-256 XX:XX:...`。收到对端 SDP 后，解析出 fingerprint 调 `SSL_set_peer_cert_digest()` 设入 OpenSSL——握手时 OpenSSL 自动对对端证书算哈希并比对，匹配则握手继续，不匹配则握手失败（等同于 MITM 检测）。两端各自用自己的证书生成 fingerprint（offer 端放 offer 端、answer 端放 answer 端），生成方式完全对称。
