# 面试速讲稿：ICE / epoll / STUN / TURN

> 用法：每块先背"开场"（30 秒讲完，第一时间讲清楚），面试官追问时从"弹药"里取。
> 与 `interview-sfu-prep.md` 知识库配套：这里是骨架和口语，那里是细节和代码行号。

---

## 1. ICE 模块速讲（30 秒开场）

**开场：**

> ICE 解决的是"双方各有一堆候选地址，到底走哪条路"的问题——本质是**广撒网 + 择优**。
> 我的项目里分四层：UDPPort（一个 UDP socket 绑定一个本地地址）、IceConnection（一条候选路径，即一个四元组）、IceTransportChannel（聚合一个端口下所有连接）、IceController（选出最优路径）。
> 路径怎么验通？用 **STUN binding 探测**——我发 ping，对端回 pong，能收到 pong 就是通；通了的路径还要排序，排序规则是：**writable 优先，再看 receiving，再看优先级，最后看 RTT**。
> 选中的路径就是"最优路径"，媒体从这条路走。如果这条路断了，2.5 秒没收到数据或 5 秒 ping 不回，立刻切到备选；15 秒全部失联，宣告连接失败、释放资源。

**追问弹药：**

| 追问 | 答 |
|------|----|
| 谁发起探测？ | 双方都发，主动探测。我的项目里服务器始终是 controlling 角色，无角色冲突问题 |
| 探测包是什么？ | STUN binding request，带 USERNAME（对端 ufrag:本地 ufrag）、PRIORITY、ICE-CONTROLLING、USE-CANDIDATE、MESSAGE-INTEGRITY、FINGERPRINT |
| 怎么判断一条路径死没死？ | 三个独立状态：writable（我→对端，ping 回复判定）、receiving（对端→我，2.5s 内有数据）、ICE 提名状态（WAITING→IN_PROGRESS→SUCCEEDED）。UDP 可能单向通，所以分开看 |
| 切换为什么不立刻切？ | RTT 切换有 10ms 防抖，新路径 RTT 必须比当前小 10ms 以上才切，防止抖动造成来回切 |
| ping 太频繁怎么办？ | 两档限速：弱连接 48ms 高频探（快速找路），稳定连接 2500ms 保活 |
| 对端从没声明过的地址发来探测？ | peer-reflexive candidate，运行时动态创建连接并回 binding response，不需要信令 |
| 排序细节？ | 5 级：writable > write_state > receiving > priority > RTT |

---

## 2. epoll 面试表达（30 秒开场）

**开场：**

> 我的服务器是 thread-per-core 模型，每个线程一个事件循环，底层是 libev 封装、Linux 上走 **epoll**（水平触发）。
> 面试官最关心的其实是**监听策略**，我总结成一句话：**读常驻、写按需**。
> 读为什么能常驻？因为没数据时 fd 不可读，水平触发不触发回调，常驻零开销；而数据随时可能到，必须长期监听。
> 写为什么不能常驻？因为发送缓冲区几乎总是可写，水平触发下写回调**每个事件循环迭代都会触发**，常驻写监听就是 busy loop、CPU 空转。
> 所以写路径是**乐观发送**：先直接发，发不出去（缓冲区满）才挂写监听，缓冲区排空立刻卸掉监听。
> 读路径虽然是水平触发，但我用 **while 循环读到 EAGAIN**，一次事件把内核缓冲区读空，模拟边缘触发的效果，减少回调次数。

**追问弹药：**

| 追问 | 答 |
|------|----|
| LT 还是 ET？为什么？ | libev 默认 LT。LT 语义简单、不会丢事件；ET 必须一次读完否则饿死，配合 while drain 才能用。我的读路径用 LT + drain 兼容两者优点 |
| 为什么不裸写 epoll？ | libev 自动选后端（epoll/select 回退）、统一 io/timer/pipe 抽象、每线程一 loop 契合 thread-per-core |
| 跨线程怎么唤醒？ | pipe fd，往对端线程的 loop 写 1 个字节，写端也是常驻读监听，被唤醒后从队列取消息 |
| 为什么不用每连接一个线程？ | 连接数大时线程切换成本高；thread-per-core 固定 N 个线程（worker_num），事件驱动，可扩展性好（详见知识库 B3 追问三） |

---

## 3. STUN 速讲（30 秒开场）

**开场：**

> STUN 本质是一面**镜子**。客户端在内网，想照见自己被 NAT 改写后的公网地址，就向 STUN 服务器发一个 binding request。
> 包在去程被 NAT 做**源地址转换**（SNAT）：内网 IP:port 改写为公网出口 IP:port；服务器用 **recvfrom** 拿到这个经 NAT 改写后的源地址，把它填进 XOR-MAPPED-ADDRESS 属性回给客户端；回程再经 **目的地址转换**（DNAT）送回。
> 客户端照见自己的公网映射地址，把它作为 server-reflexive candidate 写进 SDP，通过信令告诉对端"来这个地址找我"。

**追问弹药：**

| 追问 | 答 |
|------|----|
| NAT 有哪几类？ | 按映射和过滤分：Full-Cone、Restricted-Cone、Port-Restricted-Cone（非对称）、Symmetric（对称） |
| 对称 NAT 为什么 STUN 失效？ | 对称 NAT 每往一个新目的地址发包就换一个新出口。STUN 学到的映射只对"STUN 服务器"有效，发给对端时 NAT 又换了地址；对端回包时自己也换出口，这边 NAT 会话表不认 → 双方永远对不上号（详见 stun-turn-nat-punching.md 的数字示例） |
| 非对称 NAT 就能通？ | 对，映射可复用，一个公网出口对谁都一样 |
| 拿到公网地址后怎么告诉对端？ | SDP 信令，a=candidate 行，typ srflx |
| XOR 是什么意思？ | 地址字段与 magic cookie 异或，防止 NAT 设备把地址当数据改写；客户端异或回去还原 |
| STUN 和 ICE 什么关系？ | ICE 用 STUN binding 做连通性检查；STUN 单独用还能做 NAT 探测（学地址） |

---

## 4. TURN 速讲（30 秒开场）

**开场：**

> TURN 是 STUN 不够用时兜底的：**双侧对称 NAT 或企业防火墙下，直接打洞必败，只能让服务器当中继**。
> 原理：客户端向 TURN 服务器发 **Allocate** 请求，服务器分配一个**中继地址**（公网 IP:port）；之后客户端把媒体数据发到中继地址，服务器转发给对端——对端也只认这一个地址，两边都只跟服务器这一个固定地址通信，绕开了 NAT 映射对不上的问题。
> 代价是**多一跳**：数据绕道服务器，延迟变高、服务器带宽成本翻倍，所以实践中只在打洞失败时回退 TURN，平时走直连（ICE 的候选优先级里 relay 也是最低的）。

**追问弹药：**

| 追问 | 答 |
|------|----|
| Allocate 具体流程？ | 客户端发 Allocate request（带用户名密码鉴权）→ 服务器返回中继地址 + 分配的 5 元组；Allocate 有寿命，靠 Refresh 续期 |
| 谁来都能往我的中继地址发吗？ | 不是。对端必须先被授权：客户端发 Permission 请求，把对端的 IP（+端口）加入服务器白名单，Permission 大约 5 分钟过期。这是 TURN 的安全机制（防垃圾流量） |
| 数据怎么在中继地址间流转？ | 客户端→服务器用 Send Indication 带对端地址；服务器→客户端用 Data Indication，包里有数据实际来源（经 RFC 5766 的 XOR-PEER-ADDRESS 标识） |
| TURN 和 STUN 什么关系？ | TURN 是 STUN 的扩展：同样的消息格式和机制，多了 Allocate/Refresh/Permission 等方法，多了中继类属性（RFC 5766） |
| 什么时候用 TURN 什么时候用 STUN？ | 先试直连（host），再试 STUN 学到的 srflx，都失败才上 relay。ICE 的候选优先级天然排好序：host > srflx/prflx > relay |
| 服务器怎么知道要不要转发给对端？ | Permission 授权 + 目的地址匹配，命中才转，否则丢弃 |

---

## 速讲顺序建议（面试官问"讲讲你的媒体服务器"）

```
1. 整体架构（30s）  →  2. ICE 选路（30s）  →  3. DTLS/SRTP 加密（20s）
  →  4. RTP/RTCP 处理与转发（30s）  →  5. 收尾：I/O 模型 + 线程模型（20s）
```

被问到 NAT 穿透 → 切到 STUN/TURN 块；被问到高并发 → 切到 epoll/线程模型块。

## 参考文档

- `interview-sfu-prep.md`：完整知识库（A4 ICE、A10 libev/epoll 细节含行号）
- `stun-turn-nat-punching.md`：STUN/TURN 完整原理 + 对称 NAT 失败数字示例
