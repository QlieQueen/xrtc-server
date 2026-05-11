# Phase 8 背景知识：STUN 协议详解

## STUN 在 WebRTC 连接建立中的位置

回顾 Phase 1-7 我们已经做了的事：

```
1. Offer  (服务端 → 客户端): SDP 含 codec + ICE ufrag/pwd + candidate + fingerprint
2. Answer (客户端 → 服务端): SDP 含客户端的 ICE ufrag/pwd + candidate + fingerprint + SSRC
3. ★ ICE 连通性检查 ★ ← Phase 8+9 要做的
4. DTLS 握手
5. SRTP 加密媒体传输
```

Phase 7 完成时，两端的信息已经齐全：

| 信息 | 服务端 | 客户端 |
|------|--------|--------|
| 自己的 IP:端口 | 172.17.54.49:10025 | 从 answer SDP candidate 行解析 |
| 对端的 IP:端口 | 从 answer SDP candidate 行解析 | 从 offer SDP candidate 行解析 |
| 自己的 ufrag/pwd | 随机生成 (如 oQoD/GYn6...) | 随机生成 (如 3WyL/x5FN...) |
| 对端的 ufrag/pwd | 从 answer 解析 | 从 offer 解析 |

**但双方还没有互相验证过对方的可达性。** Phase 8+9 就是做这件事：

> 双方通过 STUN Binding Request/Response 互相发 ping/pong，确认对方确实在 candidate 地址上监听，且拥有正确的 ICE 密码。

**Phase 8 的角色**：实现 STUN 协议的编解码 —— 让服务端能读懂客户端发来的 STUN 包，以及构造正确的 STUN 回复。

---

## 什么是 STUN

STUN（Session Traversal Utilities for NAT，RFC 5389）是一个 **客户端-服务端协议**，用于：

1. **连通性检查**（ICE 的核心用途）：一方发 Binding Request，另一方回 Binding Response
2. **地址发现**：Response 中携带 XOR-MAPPED-ADDRESS，告诉对方"从外面看到你的地址是什么"

在 ICE 场景中，双方互为 STUN 服务端和客户端 —— 两边都发 Request，也都回 Response。

---

## STUN 消息的二进制格式

STUN 消息由 **20 字节固定头** + **0 或多个属性** 组成：

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
|                          Attributes                           |
|                      (variable length)                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 头部字段详解

#### Message Type (2 字节)

Message Type 的 14 位编码方式（来自 RFC 5389，参考代码中 `k_stun_method_mask = 0x3EEF`、`k_stun_class_mask = 0x0110`）：

```
 0                   1
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|0 0|M11|M10|M9|M8|M7|C1|M6|M5|M4|C0|M3|M2|M1|M0|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **bit 14-15**：固定为 `00`
- **Method 位**（M0-M11，共 12 位）：定义方法（如 Binding = 0x001）。散落在 class 位的间隙中
- **Class 位**：C0 在 bit 4，C1 在 bit 8

```
Class 值（C1 C0）:
  0 0 = Request（请求）         → STUN_CLASS_REQUEST    = 0x000
  0 1 = Indication（指示）      → STUN_CLASS_INDICATION = 0x010  (bit 4)
  1 0 = Success Response（响应） → STUN_CLASS_SUCCESS    = 0x100  (bit 8)
  1 1 = Error Response（错误）   → STUN_CLASS_ERROR      = 0x110  (bit 4 | bit 8)
```

**为什么 Method 位和 Class 位交织？**
这样设计的目的是让 Binding Request (method=0x001, class=Request) 的 type 值是 0x0001 —— 与旧的 RFC 3489 STUN 保持兼容。如果 Method 和 Class 各自连续排列，新旧 STUN 的 type 值就无法对齐。

**常见的 Message Type 值**：

| 十进制 | 十六进制 | 含义 |
|--------|---------|------|
| 0x0001 | 1 | Binding Request |
| 0x0101 | 257 | Binding Success Response |
| 0x0111 | 273 | Binding Error Response |

**为什么 Request 和 Response 的 transaction ID 必须一致？**

```
服务端                                       客户端
   |                                          |
   | ← Binding Request                        |
   |    (type=0x0001, transaction_id=0xA1B2)  |
   |                                          |
   | Binding Response →                       |
   | (type=0x0101, transaction_id=0xA1B2)     |
   |                                          |
```

客户端用 transaction_id 匹配 Request 和 Response：收到 Response 后查找对应的 Request，计算 RTT（往返时间），确认连通性。

#### Message Length (2 字节)

**属性部分的总长度**（不包含 20 字节头部）。必须能被 4 整除（因为 STUN 属性是 4 字节对齐的）。

#### Magic Cookie (4 字节)

固定值 `0x2112A442`，用于：
1. 区分 STUN 和其他协议（前两位是 00 且 cookie 匹配）
2. XOR-MAPPED-ADDRESS 计算时的 XOR 操作数

#### Transaction ID (12 字节)

96 位随机数，唯一标识一个 STUN 事务。Request 和对应的 Response 必须使用相同的 Transaction ID。

**新版本 STUN 格式**（我们只实现这种）：
- Transaction ID 占满 12 字节
- Magic Cookie 在前面 4 字节

**旧版本 STUN 格式**（RFC 3489，不实现但需要兼容检测）：
- 没有 Magic Cookie
- Transaction ID 是 16 字节（Magic Cookie 位置 + Transaction ID 位置）

在 `StunMessage::read()` 中需要处理这种情况：如果 Magic Cookie 位置的值不是 `0x2112A442`，则将这 4 字节视为 Transaction ID 的一部分。

---

## STUN 属性的 TLV 格式

每个属性有相同的头部格式，后面跟具体的 value：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             Type              |            Length             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Value (variable)                ....
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Type**：属性类型（见下表）
- **Length**：value 的字节数（**不包含** 4 字节属性头，**不包含** padding）
- **Value**：实际数据，必须是 **4 字节对齐**（如果 length 不是 4 的倍数，后面补 0）

### ICE 使用的 STUN 属性一览

| Type | Name | Value 类型 | Value 长度 | 方向 | 用途 |
|------|------|-----------|-----------|------|------|
| 0x0006 | USERNAME | ByteString | 可变 | Request | ICE ufrag 组合：`remote_ufrag:local_ufrag` |
| 0x0008 | MESSAGE-INTEGRITY | ByteString | 20 字节 | Both | HMAC-SHA1(ice_pwd, 消息内容) |
| 0x0009 | ERROR-CODE | ErrorCode | 可变 (≥4) | Error Response | 错误码 + 原因描述 |
| 0x0020 | XOR-MAPPED-ADDRESS | Address (XOR) | 8 (IPv4) | Response | 客户端地址（XOR 编码） |
| 0x0024 | PRIORITY | UInt32 | 4 字节 | Request | candidate 优先级值 |
| 0x0025 | USE-CANDIDATE | (无 value) | 0 字节 | Request | 提名此连接为最终选路 |
| 0x8028 | FINGERPRINT | UInt32 | 4 字节 | Both | CRC32 校验值 |
| 0x802A | ICE-CONTROLLING | UInt64 | 8 字节 | Request | ICE 角色标记 (controlling agent) |

**属性 type 范围的约定**：
- `0x0000 - 0x3FFF`：标准属性，理解后必须处理
- `0x4000 - 0x7FFF`：标准属性，可选处理
- `0x8000 - 0xFFFF`：可选属性，理解后必须处理

FINGERPRINT (0x8028) 属于"理解后必须处理"范围。

### 属性的 value 类型

STUN 属性虽然类型很多，但 value 的存储格式只有几种：

| Value 类型 | 存储格式 | 对应 C++ 类型 | 属性举例 |
|-----------|---------|-------------|---------|
| UInt32 | 4 字节大端无符号整数 | `uint32_t` | PRIORITY, FINGERPRINT |
| UInt64 | 8 字节大端无符号整数 | `uint64_t` | ICE-CONTROLLING |
| ByteString | 可变长字节序列 | `char*` + length | USERNAME, MESSAGE-INTEGRITY |
| Address | 1+1+2+(4 或 16) 字节 | `SocketAddress` | MAPPED-ADDRESS |
| XorAddress | 同上但 IP/端口 XOR Magic Cookie | `SocketAddress` | XOR-MAPPED-ADDRESS |
| ErrorCode | 4 字节 code + 变长 reason | `int` + `string` | ERROR-CODE |

---

## MESSAGE-INTEGRITY 详解

MESSAGE-INTEGRITY 是 ICE 安全的核心机制。它用 HMAC-SHA1 对 STUN 消息进行完整性校验，key 是 ICE 的 password。

### 为什么重要

只有知道 ice-pwd 的人才能正确生成和验证 MESSAGE-INTEGRITY。这保证了：
- 收到 Binding Request 的一方可以确认 **对方知道正确的密码**（确实是通过信令交换过的对端）
- 消息在传输过程中没有被篡改

### 计算方式

```
HMAC-SHA1(key = ice_pwd, data = STUN消息内容)
输出: 20 字节 HMAC
```

**关键细节**：HMAC 的计算范围是从 STUN 消息开头到 **MESSAGE-INTEGRITY 属性本身之前**（不包含 MESSAGE-INTEGRITY 属性及其之后的属性）。

```
STUN 消息结构:
┌─────────────────┐
│   STUN 头部      │  ┐
│   (20 bytes)    │  │
├─────────────────┤  │
│   USERNAME      │  ├── HMAC 计算范围
│   (属性)         │  │
├─────────────────┤  │
│   PRIORITY      │  │
│   (属性)         │  ┘
├─────────────────┤
│ MESSAGE-INTEGRITY│  ← HMAC 放在这里
│   (属性)         │
├─────────────────┤
│   FINGERPRINT   │  ← 不在 HMAC 计算范围内
│   (属性)         │
└─────────────────┘
```

### 验证时对 Message Length 的调整

因为 MESSAGE-INTEGRITY 属性在计算 HMAC 时还不存在，且它后面的属性也不在 HMAC 范围内，所以验证时：
- STUN 头部的 Message Length 字段包含的是**所有属性**的长度（含 MI 和 FINGERPRINT）
- 但在验证 HMAC 时，需要把 Message Length 调整为**到 MI 属性开始前的长度**（不含 MI 自己）

### 生成流程

```
add_message_integrity(password):
  1. 创建一个临时的 StunByteStringAttribute(STUN_ATTR_MESSAGE_INTEGRITY, 20字节全0)
  2. 先把它添加到 _attrs 列表中（这样才能确定 MI 在消息中的位置）
  3. write(ByteBufferWriter) 序列化整个消息
  4. 计算 HMAC-SHA1(password, 消息内容从开头到 MI 的 value 之前)
  5. 把 HMAC 结果填回 MI 属性的 bytes 中（覆盖之前的全 0）
```

### 验证流程

```
validate_message_integrity(password):
  1. 在 _attrs 中查找 STUN_ATTR_MESSAGE_INTEGRITY
  2. 定位 MI 属性在原始 buffer 中的位置
  3. 复制原始 buffer 中 MI 之前的所有内容到临时 buffer
  4. 如果 MI 后面还有属性（如 FINGERPRINT），调整临时 buffer 头部的 length 字段
  5. 对临时 buffer 计算 HMAC-SHA1
  6. 比较计算结果与 MI 属性中存储的值
```

---

## FINGERPRINT 详解

FINGERPRINT 是一个 CRC32 校验，用于快速识别 STUN 包和区分 STUN 与其他协议（如 RTP）。

### 计算方式

```
fingerprint = CRC32(data from start to FINGERPRINT attribute value) ^ 0x5354554E
```

其中 `0x5354554E` 是 "STUN" 的 ASCII 编码（'S'=0x53, 'T'=0x54, 'U'=0x55, 'N'=0x4E）。

### 为什么 XOR 0x5354554E

RFC 5389 规定这个 XOR 值，这样即使 CRC32 结果是 0，XOR 后也不会和"属性不存在"混淆。

### 验证流程

```
validate_fingerprint(data, len):
  1. 检查 len 是否至少为 20 + 8（头部 + fingerprint 属性最小大小）
  2. 定位 Magic Cookie，验证是否是 0x2112A442
  3. 定位到消息最后 8 字节（FINGERPRINT 属性一定在最后）
  4. 验证这 8 字节的 type 是否等于 STUN_ATTR_FINGERPRINT (0x8028)
  5. 验证这 8 字节的 length 是否等于 4 (UInt32 的长度)
  6. 取出 value 部分（4 字节），这就是报文中存储的 fingerprint
  7. 计算 CRC32(data, len - 8) — 对 FINGERPRINT 属性之前的所有内容做 CRC32
  8. 验证: (存储的 fingerprint) == CRC32(data, len - 8) ^ 0x5354554E
```

**为什么 FINGERPRINT 总是在最后一个属性？**
- 它的计算范围包含所有其他属性
- 如果它不在最后，后续属性的增减会导致 CRC32 失效
- 所以生成时先添加所有属性（含 MI），最后加 FINGERPRINT

### 生成流程

```
add_fingerprint():
  1. 创建一个临时的 StunUInt32Attribute(STUN_ATTR_FINGERPRINT, 0)
  2. 添加到 _attrs 列表末尾
  3. write(ByteBufferWriter) 序列化
  4. 计算 CRC32(序列化数据从开头到 FINGERPRINT value 之前)
  5. value = CRC32 ^ 0x5354554E
  6. 更新属性中的 _bits 为计算出的 value
```

---

## XOR-MAPPED-ADDRESS 详解

当服务端收到客户端的 STUN Binding Request 时，需要在 Response 中告诉客户端"我看到你的地址是 X"。这个地址放在 XOR-MAPPED-ADDRESS 属性中。

### 为什么用 XOR 而不是明文

RFC 3489 使用的是 MAPPED-ADDRESS（明文），但 NAT 设备可能会修改明文 IP 地址（ALG - Application Layer Gateway），导致地址错误。XOR 混淆后 NAT 设备无法识别，避免了这个问题。

### XOR 计算

```
XOR-MAPPED-ADDRESS 格式:
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |0 0 0 0 0 0 0 0|    Family     |         X-Port                |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                X-Address (32 bits for IPv4)
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Family**：0x01 = IPv4, 0x02 = IPv6
- **X-Port**：`实际端口 ^ (Magic_Cookie >> 16)` = `port ^ 0x2112`
- **X-Address (IPv4)**：`实际 IP ^ Magic_Cookie` = `ip ^ 0x2112A442`

### 代码实现

```cpp
// 写入 XOR-MAPPED-ADDRESS
bool StunXorAddressAttribute::write(ByteBufferWriter* buf) {
    buf->WriteUInt8(0);                         // 保留
    buf->WriteUInt8(0x01);                      // IPv4
    buf->WriteUInt16(port ^ (0x2112A442 >> 16)); // X-Port
    
    in_addr v4addr = addr.ipv4_address();
    v4addr.s_addr ^= htonl(0x2112A442);          // X-Address
    buf->WriteBytes(&v4addr, sizeof(v4addr));
    return true;
}
```

---

## USERNAME 属性详解

USERNAME 在 Binding Request 中**必须存在**（用于服务端识别这是哪个 ICE session 的流量）。

### 格式

```
remote_ufrag:local_ufrag
```

例如客户端发送时：
```
服务端的 ufrag:客户端的 ufrag
= "oQoD:3WyL"
```

### 为什么格式是 "remote_ufrag:local_ufrag"

- 发送方用对方的 ufrag 作为第一部分，自己的 ufrag 作为第二部分
- 接收方解析时：
  - 第一部分 = 自己的 ufrag（验证是否匹配）
  - 第二部分 = 对方的 ufrag（用于查找对应的 ICE session）

### 服务端的验证流程

```
收到 Binding Request → 取出 USERNAME → 按 ':' 分割
  → fields[0] 应该是 服务端自己的 ice_ufrag
  → fields[1] 应该是 客户端的 ice_ufrag
```

---

## PRIORITY 属性详解

PRIORITY 告诉对方这个 candidate 的优先级。用于 ICE 的选路排序。

### 格式

4 字节大端无符号整数，使用候选地址优先级公式计算：

```
priority = (type_preference << 24) | (local_preference << 8) | (256 - component_id)

type_preference: host=126, prflx=110, srflx=100, relay=0
local_preference: 网卡优先数，默认 65535
component_id: RTP=1
```

---

## USE-CANDIDATE 属性

这是一个**没有 value** 的属性（length = 0），用于 "提名" (nomination) 一个 candidate pair 为最终选路。

在 ICE 普通提名（regular nomination）流程中：
- Controlling agent 在连通性检查成功后，会发一个带 USE-CANDIDATE 的 Binding Request
- Controlled agent 收到后知道"这个 candidate pair 被选中了"
- 之后双方停止检查其他 candidate pair，只用这个

---

## ICE-CONTROLLING / ICE-CONTROLLED 属性

用于解决 ICE 角色冲突（双方都认为自己是 controlling）。通过比较 tie-breaker 值决定。

| 属性 | Type | Value | 说明 |
|------|------|-------|------|
| ICE-CONTROLLING | 0x802A | UInt64 (8字节随机数) | "我是 controlling agent" |
| ICE-CONTROLLED | 0x8029 | UInt64 (8字节随机数) | "我是 controlled agent" |

在我们的 PUSH 场景中，服务端作为 passive 方，通常是 controlled agent。但 Phase 8 可以先不深入角色协商，Phase 9 的 ICE 连接状态机会处理。

---

## ERROR-CODE 属性

当 STUN 请求验证失败时，服务端返回 Binding Error Response，携带 ERROR-CODE 说明原因。

### 格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Reserved                            |Class | Number |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      Reason Phrase (variable)                                ..
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Reserved**: 21 位，必须为 0
- **Class**: 3 位，错误码百位数 (4, 5, 6)
- **Number**: 8 位，错误码的十位和个位
- **code** = Class * 100 + Number

### 常见错误码

| Code | 含义 | 使用场景 |
|------|------|--------|
| 400 | Bad Request | 缺少必要属性（USERNAME 或 MESSAGE-INTEGRITY） |
| 401 | Unauthorized | local_ufrag 不匹配或 MESSAGE-INTEGRITY 验证失败 |
| 500 | Server Error | 服务端内部错误 |
| 600 | Global Failure | 通用失败 |

**关键注意**：返回 400 和 401 错误时，响应**不包含 MESSAGE-INTEGRITY**（因为客户端还未能证明它知道正确的密码）。返回其他错误时才加 MI。

---

## 4 字节对齐（Padding）

STUN 协议要求属性 value 按 4 字节对齐。如果属性的 value 长度不是 4 的倍数，需要补 0 字节。

```
属性实际长度 = k_stun_attribute_header_size(4) + value_length + padding

padding = (4 - value_length % 4) % 4
```

代码实现：

```cpp
// 读取属性时消费 padding
void StunAttribute::consume_padding(ByteBufferReader* buf) {
    int remain = length() % 4;
    if (remain > 0) {
        buf->Consume(4 - remain);  // Consume 跳过补 0 字节
    }
}

// 写入属性时填充 padding
void StunAttribute::write_padding(ByteBufferWriter* buf) {
    int remain = length() % 4;
    if (remain > 0) {
        char zeros[4] = {0};
        buf->WriteBytes(zeros, 4 - remain);
    }
}
```

---

## STUN 读取流程（StunMessage::read）

```
1. 保存原始 buffer 到 _buffer（后续 HMAC 验证需要）
2. 读取 Message Type (2 字节，大端)
   → 检查 _type & 0x0800 → 前两位必须为 00
3. 读取 Message Length (2 字节，大端)
4. 读取 Magic Cookie (4 字节)
5. 读取 Transaction ID (12 字节)
   → 如果 Magic Cookie 不等于 0x2112A442:
       将 Magic Cookie 部分也视为 Transaction ID 的一部分（旧版本 STUN）
6. 验证剩余 buffer 长度 == _length
7. 循环读取属性:
   while (buf->Length() > 0):
     a. 读取 attr_type (2 字节)
     b. 读取 attr_length (2 字节)
     c. 通过 _create_attribute(attr_type, attr_length) 创建属性
        如果无法识别类型 → Consume(attr_length + padding) 跳过
     d. 如果能识别 → attr->read(buf) 解析 value
     e. 处理 padding
```

## STUN 写入流程（StunMessage::write）

```
1. WriteUInt16(_type)
2. WriteUInt16(_length)
3. WriteUInt32(k_stun_magic_cookie)
4. WriteString(_transaction_id, 12)
5. 循环写入属性:
   for each attr in _attrs:
     a. WriteUInt16(attr->type())
     b. WriteUInt16(attr->length())
     c. attr->write(buf)
```

---

## STUN 和 DTLS/媒体数据的区分

UDP 端口上可能收到三种数据：STUN、DTLS、RTP/RTCP。区分方法看第一个字节：

```
DTLS ContentType 范围: 20-63 (0x14-0x3F)
  - 20 (0x14): ChangeCipherSpec
  - 21 (0x15): Alert
  - 22 (0x16): Handshake
  - 23 (0x17): Application Data

STUN: 前 2 位 = 00 → (byte[0] & 0xC0) == 0x00
  例如 0x0001 (Binding Request) → byte[0]=0x00, byte[1]=0x01

RTP: version=2 → (byte[0] & 0xC0) == 0x80
```

注意：DTLS 的 ContentType 22 (0x16) 前两位是 `00`，会和 STUN 混淆！所以判断 STUN 不能只看前两位，还得验证 Magic Cookie。

**正确的判断顺序**：

```cpp
// 1. 检查 FINGERPRINT 确认是 STUN
if (StunMessage::validate_fingerprint(data, len)) {
    return k_stun;
}
// 2. 否则检查是否是 DTLS
if (data[0] >= 20 && data[0] <= 63) {
    return k_dtls;
}
// 3. 最后检查是否是 RTP
if ((data[0] & 0xC0) == 0x80) {
    return k_rtp;
}
```

---

## UDPPort 中的 STUN 消息处理流水线

Phase 8 要实现的核心是 `UDPPort::_on_read_packet()` 中的 STUN 验证流水线。当前这个函数是空的，我们要把它填充成如下逻辑：

```
UDPPort::_on_read_packet(buf, len, addr):
  
  // ★ 第一步：如果有已建立的 IceConnection，直接路由给它处理
  if (IceConnection* conn = get_connection(addr)) {
      conn->on_read_packet(buf, len, ts);
      return;  // Phase 9 实现
  }
  
  // ★ 第二步：验证 FINGERPRINT（快速区分 STUN 和非 STUN）
  if (!StunMessage::validate_fingerprint(data, len)) {
      // 非 STUN 消息（可能是 DTLS 或其他），暂不处理
      return;
  }
  
  // ★ 第三步：解析 STUN 消息
  StunMessage stun_msg;
  ByteBufferReader buf(data, len);
  if (!stun_msg.read(&buf) || buf.Length() != 0) {
      return;
  }
  
  // ★ 第四步：检查是否 Binding Request
  if (stun_msg.type() != STUN_BINDING_REQUEST) {
      return;
  }
  
  // ★ 第五步：检查必要属性存在
  if (!stun_msg.get_byte_string(STUN_ATTR_USERNAME) ||
      !stun_msg.get_byte_string(STUN_ATTR_MESSAGE_INTEGRITY)) {
      // 缺少 USERNAME 或 MI → 返回 400 Bad Request
      send_binding_error_response(&stun_msg, addr, STUN_ERROR_BAD_REQUEST, ...);
      return;
  }
  
  // ★ 第六步：解析 USERNAME → 验证 local_ufrag 是否匹配
  std::string local_ufrag, remote_ufrag;
  _parse_stun_username(&stun_msg, &local_ufrag, &remote_ufrag);
  if (local_ufrag != _ice_params.ice_ufrag) {
      // ufrag 不匹配 → 返回 401 Unauthorized
      send_binding_error_response(&stun_msg, addr, STUN_ERROR_UNAUTHORIZED, ...);
      return;
  }
  
  // ★ 第七步：验证 MESSAGE-INTEGRITY
  if (stun_msg.validate_message_integrity(_ice_params.ice_pwd) 
      != StunMessage::IntegrityStatus::k_integrity_ok) {
      // MI 验证失败 → 返回 401 Unauthorized
      send_binding_error_response(&stun_msg, addr, STUN_ERROR_UNAUTHORIZED, ...);
      return;
  }
  
  // ★ 第八步：通过验证 → 触发信号，由 IceTransportChannel 处理
  signal_unknown_address(this, addr, &stun_msg, remote_ufrag);
```

**关键设计决策**：Phase 8 在 UDPPort 中做验证，验证通过后通过 `signal_unknown_address` 交给 IceTransportChannel 创建 IceConnection 并回复 Binding Response（Phase 9）。

---

## 属性工厂方法

STUN 的属性类型很多，但 value 的存储格式只有几种。`_create_attribute()` 用工厂方法创建正确的子类：

```cpp
StunAttribute* StunMessage::_create_attribute(uint16_t type, uint16_t length) {
    StunAttributeValueType value_type = get_attribute_value_type(type);
    switch (value_type) {
        case STUN_VALUE_BYTE_STRING:
            return new StunByteStringAttribute(type, length);
        case STUN_VALUE_UINT32:
            return new StunUInt32Attribute(type);
        default:
            return nullptr;  // 未知类型 → read() 中会跳过
    }
}

StunAttributeValueType get_attribute_value_type(int type) {
    switch (type) {
        case STUN_ATTR_USERNAME:          return STUN_VALUE_BYTE_STRING;
        case STUN_ATTR_MESSAGE_INTEGRITY: return STUN_VALUE_BYTE_STRING;
        case STUN_ATTR_PRIORITY:          return STUN_VALUE_UINT32;
        case STUN_ATTR_FINGERPRINT:       return STUN_VALUE_UINT32;
        case STUN_ATTR_USE_CANDIDATE:     return STUN_VALUE_UINT32;
        case STUN_ATTR_ICE_CONTROLLING:   return STUN_VALUE_UINT64;
        default:                          return STUN_VALUE_UNKNOWN;
    }
}
```

**注意**：Phase 8 可以先只实现 `STUN_VALUE_BYTE_STRING` 和 `STUN_VALUE_UINT32`，`STUN_VALUE_UINT64` 和 `STUN_VALUE_ADDRESS` 可以后续按需补充。

---

## 代码结构对应关系

### 新增文件与 STUN 概念的对应

| 概念 | 对应代码 | 作用 |
|------|---------|------|
| STUN 消息 | `StunMessage` | 消息头 + 属性集合的读写 |
| STUN 属性 (UInt32) | `StunUInt32Attribute` | PRIORITY, FINGERPRINT |
| STUN 属性 (UInt64) | `StunUInt64Attribute` | ICE-CONTROLLING |
| STUN 属性 (ByteString) | `StunByteStringAttribute` | USERNAME, MESSAGE-INTEGRITY |
| STUN 属性 (Address) | `StunAddressAttribute` | MAPPED-ADDRESS |
| STUN 属性 (XorAddress) | `StunXorAddressAttribute` | XOR-MAPPED-ADDRESS |
| STUN 属性 (ErrorCode) | `StunErrorCodeAttribute` | ERROR-CODE |
| HMAC-SHA1 计算 | `rtc::ComputeHmac` | MESSAGE-INTEGRITY |
| CRC32 计算 | `rtc::ComputeCrc32` | FINGERPRINT |

### 类间关系图

```
StunMessage
  ├── _type / _length / _transaction_id (头部字段)
  └── vector<unique_ptr<StunAttribute>> _attrs
        ├── StunByteStringAttribute (USERNAME)
        ├── StunUInt32Attribute (PRIORITY)
        ├── StunByteStringAttribute (MESSAGE-INTEGRITY)
        └── StunUInt32Attribute (FINGERPRINT)

UDPPort
  ├── _ice_params (ufrag + pwd — 用于验证)
  ├── signal_unknown_address (通过验证后触发)
  └── _on_read_packet()
        └── get_stun_message() → StunMessage::read()
                                  → validate_fingerprint()
                                  → validate_message_integrity()
```

---

## Phase 8 的边界

### Phase 8 要实现

1. `stun.h` — StunMessage + 6 个 StunAttribute 子类
2. `stun.cpp` — read/write, validate_fingerprint, validate/add_message_integrity
3. UDPPort 的 `_on_read_packet()` — STUN 验证流水线
4. UDPPort 的 `get_stun_message()` — 完整的验证逻辑
5. UDPPort 的 `send_binding_error_response()` — 错误响应发送
6. `signal_unknown_address` 信号声明和触发

### Phase 8 不实现（留给 Phase 9）

1. IceConnection 的创建和管理（`create_connection` / `get_connection`）
2. Binding Response 的构造和发送（`send_stun_binding_response`）
3. 主动发送 STUN Binding Request (ping)
4. ICE 状态机推进
5. StunRequest / StunRequestManager

### Phase 8 验证通过后，消息到达哪里

```
UDPPort::signal_unknown_address
  → (Phase 9) IceTransportChannel::_on_unknown_address
    → create IceConnection
    → send Binding Response
    → 启动 ICE ping 机制
```

---

## 总结

Phase 8 的本质工作：

> **实现 STUN 协议的编解码，让服务端的 UDPPort 能读懂客户端发来的 STUN Binding Request，验证其身份（通过 ice ufrag/pwd），验证通过后发出信号交给上层创建 ICE 连接。**

关键实现点：
1. StunMessage 的二进制 read/write（20 字节头 + TLV 属性列表）
2. FINGERPRINT 的 CRC32 快速校验（区分 STUN 和其他协议）
3. MESSAGE-INTEGRITY 的 HMAC-SHA1 完整性校验（ICE 安全核心）
4. USERNAME 的解析和 ufrag 验证（识别对端身份）
5. 4 字节对齐的 padding 处理
6. 验证失败时的 Binding Error Response 发送

完成后，服务端就具备了对 STUN 包的基本解析和验证能力，为 Phase 9 的 ICE 连接管理打下基础。
