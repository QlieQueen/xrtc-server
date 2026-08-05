# STUN / TURN / NAT 穿透

## 一句话本质（面试版）

**STUN = 一面镜子**：客户端的内网包经去程 SNAT 被 NAT 改写成公网 IP:port 送到服务器，服务器用 recvfrom 拿到这个"经 NAT 改写后的源地址"，原样回写进 XOR-MAPPED-ADDRESS 属性；回程经 DNAT 送回客户端——客户端照见自己的公网映射地址，通过 SDP 信令告诉对端"来这个地址找我"，实现 NAT 穿透。

## STUN 协议机制（RFC 5389）

### 协议做了什么

STUN 协议本身只做一件事：**服务器把自己视角看到的 request 源地址（source transport address）回写给客户端**。

- SNAT/DNAT 是 **NAT 设备**的行为，不是 STUN 协议的一部分
- NAT 的存在让"服务器看到的源地址" ≠ "客户端的内网地址"，客户端因此能学到自己的公网映射

### 完整流程

```
客户端(内网 192.168.1.100:5000)
    │ 发送 STUN BINDING REQUEST
    ▼
NAT 设备 ── 去程 SNAT：源地址改写为公网出口 200.1.1.10:6001
    │
    ▼
STUN 服务器 recvfrom 拿到源地址 200.1.1.10:6001  ← 公网地址的来源就是 recvfrom
    │ 构造 BINDING RESPONSE：
    │   XOR-MAPPED-ADDRESS = 200.1.1.10:6001
    │   MESSAGE-INTEGRITY (服务器自己的 pwd)
    │   FINGERPRINT
    ▼
NAT 设备 ── 回程 DNAT：目的地址改写回内网 192.168.1.100:5000
    │
    ▼
客户端收到 response，学到自己的公网映射地址 200.1.1.10:6001
```

### 响应构造要点

- XOR-MAPPED-ADDRESS 填的是 **request 的实际源地址**（经 NAT 改写后），XOR 编码用 magic cookie（IPv4：`addr ^ 0x2112A442`）
- 客户端拿到后把该地址作为 server-reflexive (srflx) candidate 写进 SDP 的 `a=candidate ... typ srflx` 行
- 通过信令服务器交换 offer/answer 完成 candidate 交换，然后 ICE 做连通性检查

## xrtc-server 代码对照

**结论：xrtc-server 构造了 XOR-MAPPED-ADDRESS，但取值不是 recvfrom 的源地址，而是 SDP 声明的对端地址——因为该架构内网直连、无 NAT，两者等价。**

| 代码位置 | 内容 |
|---------|------|
| `src/base/socket.cpp:258-259` | `sock_recv_from` → `recvfrom`，`addr` 是 out 参数，拿到包源地址 |
| `src/ice/udp_port.cpp:119-120` | `_on_read_packet(..., const SocketAddress& addr, ...)` — 源地址一路传到这，但只用于 `get_connection(addr)` 查找连接 |
| `src/ice/ice_connection.cpp:59-61` | `send_stun_binding_response` — 构造 `StunXorAddressAttribute(STUN_ATTR_XOR_MAPPED_ADDRESS, remote_candidate().address)` |
| `src/ice/stun_request.cpp:109-138` | `ConnectionRequest::prepare` — binding request 属性：USERNAME / ICE_CONTROLLING / USE_CANDIDATE / PRIORITY / MESSAGE-INTEGRITY / FINGERPRINT，**无 XOR-MAPPED-ADDRESS**（request 不该有，它是 response 属性） |

参考项目 `xrtcserver/src/ice/ice_connection.cpp:294-295` 同样用 `remote_candidate().address`，两边一致。

**若将来要做公网穿透**：recvfrom 源地址已在 `_on_read_packet` 的 `addr` 参数中，需一路传入 `send_stun_binding_response` 并用它填 XOR-MAPPED-ADDRESS。届时 `remote_candidate().address`（SDP 内网地址）会错得离谱。

### srflx vs prflx

| | srflx (server-reflexive) | prflx (peer-reflexive) |
|---|---|---|
| 地址来源 | 主动向 STUN 服务器查询学得 | 被动收到对端发来的 STUN binding request，从包源地址动态学得 |
| 告知方式 | SDP 信令交换（candidate 行） | 无需信令，ICE 运行时动态创建 |
| 代码路径 | — | `udp_port.cpp:136` `signal_unknown_address` → 上层创建 prflx candidate |

**面试一句话**：SDP 信令交换的是"预先可知"的地址（host/srflx），prflx 是运行时靠收到的 STUN binding request 动态学到的地址——一个靠"说"，一个靠"探"。

## NAT 分类与穿透可行性

### 非对称 NAT（Full-Cone / Restricted-Cone / Port-Restricted-Cone）

映射可复用：**对任何目的地址发包都用同一个公网出口**。

```
客户端A ──STUN──> 学到公网出口 (srflx)
客户端B ──STUN──> 学到公网出口 (srflx)
双方把 host + srflx candidate 写进 SDP
通过信令服务器交换 offer/answer (candidate 交换)
ICE 按对方 candidate 逐一做连通性检查，选通一条路径
```

STUN 学到的映射地址可直接用于和任意对端通信 → 穿透成功。

### 对称 NAT（Symmetric）

行为：**每往一个新的目的地址发包，就分配一个新的公网出口（端口变化）**。就像一个人每次给不同的收件人写信，都换一个不同的寄件地址。

#### 失败全过程（数字示例）

场景：A、B 都在对称 NAT 后，各自内网 192.168.1.x:5000。

1. **B 问 STUN**：B → STUN 服务器，NAT 分配出口 `200.1.1.10:6001`。STUN 答复"你的公网地址是 200.1.1.10:6001"，B 写进 SDP 发给 A。（A 同理学到 200.2.2.20:7001 发给 B。）
2. **ICE 检查，B → A**：B 向 `200.2.2.20:7001` 发包，NAT 一看目的地址 ≠ STUN 服务器 → 分配**新出口** `200.1.1.10:6002`。A 收到来自 6002 的包——SDP 里 B 说的是 6001！
3. **A 回包**：A 向 `200.1.1.10:6002` 回包，A 的 NAT 也换新出口 → 从 `200.2.2.20:7002` 发出。B 的 NAT 只放行"B 主动发包过去的地址"的回信，B 主动找过 7001，来信却是 7002 → **拒收，丢包**。
4. **反向同理**：两边永远对不上号。

#### 精确结论

- **换出口本身不是死因**：单侧对称 NAT 时，对侧可通过 prflx 动态学到新出口 Y，回包给 Y，此跳能通。
- **死因是双侧**：A 向 Y 回包时，A 的 NAT 又换出口 Y'；B 的 NAT 会话表里没有 (Y' → B) → 丢包。**双向都换，互相够不着。**
- 只有 TURN 中继能救：服务器地址固定，两边都只认服务器这一个地址。

### 决策树

```
客户端能否直接互通？
├─ 公网直连 / 非对称 NAT → 直接打洞成功，不需要 TURN
├─ 单侧对称 NAT → prflx 兜底可能成功
└─ 双侧对称 NAT → 打洞必败，必须 TURN 中继
```

## 面试速记

| 问题 | 一句话答案 |
|------|-----------|
| STUN 是什么 | 一面镜子：服务器把 recvfrom 看到的包源地址（经 SNAT 的公网 IP:port）写进 XOR-MAPPED-ADDRESS 回给客户端，客户端照见自己的公网映射 |
| 公网地址怎么来 | 去程 SNAT 改写源地址，服务器 recvfrom 拿到，回程 DNAT 送回 |
| 拿到之后呢 | 作为 srflx candidate 写进 SDP，信令交换给对端，ICE 检查建连 |
| STUN 解决什么 | 只解决映射可复用的非对称 NAT；对称 NAT 下 srflx 失效 |
| 对称 NAT 为何不通 | 每往新目的发包就换出口，STUN 学到的地址对 A 无效；回包时对侧也换出口，NAT 会话表不认 |
| prflx 是什么 | 运行时收到对端 STUN request 动态学到的地址，不需要信令 |
| TURN 什么时候用 | 双侧对称 NAT（或企业防火墙限制），直接打洞失败，走服务器中继转发 |

## 参考

- RFC 5389：STUN 协议，XOR-MAPPED-ADDRESS 定义（§15.2，填充 request 的源地址）
- RFC 8445：ICE，srflx / prflx candidate 定义
- 本仓库代码：`src/ice/stun_request.cpp`（request 构造）、`src/ice/ice_connection.cpp`（response 构造）、`src/ice/udp_port.cpp`（收包入口）、`src/base/socket.cpp`（recvfrom）
