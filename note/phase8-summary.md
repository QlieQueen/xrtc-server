# Phase 8 总结：STUN 消息编解码与 Binding Request/Response

## 目标

实现完整的 STUN (RFC 5389) binding request 接收校验 → binding response 构造发送的链路。

## 数据流全景

```
UDP 到达
  └─ AsyncUdpSocket::recv_data() → signal_read_packet
       └─ UDPPort::_on_read_packet()
            ├─ validate_fingerprint()        ← CRC32 快速合法性检查
            ├─ StunMessage::read()           ← 解析 header + 所有属性
            ├─ 校验 USERNAME 格式            ← local_ufrag:remote_ufrag
            ├─ validate_message_integrity()  ← HMAC-SHA1 验证
            └─ signal_unknown_address → 上层创建 prflx candidate

                   ↓ handle_stun_binding_request()

            StunMessage response;
            ├─ set_type(STUN_BINDING_RESPONSE)
            ├─ add_attribute(XOR-MAPPED-ADDRESS)
            ├─ add_message_integrity(ice_pwd)   ← HMAC-SHA1 签名
            ├─ add_fingerprint()                ← CRC32 指纹
            └─ send_response_message()
                 ├─ write(&buf)                 ← 序列化为二进制
                 └─ AsyncUdpSocket::send_to()   ← UDP 发送
```

## 提交清单（12 commits）

| # | Commit | 内容 |
|---|--------|------|
| 1 | `1.5.36` | `StunMessage::read()` + `validate_fingerprint()` + `StunByteStringAttribute` |
| 2 | `1.5.37` | USERNAME 解析验证 (`local_ufrag:remote_ufrag`) |
| 3 | `1.5.38` | MESSAGE-INTEGRITY 验证 (HMAC-SHA1) |
| 4 | `1.5.39` | Binding request 异常处理 — 缺属性/ufrag 不匹配/MI 失败发 error response |
| 5 | `1.5.40` | 创建 peer 反射地址 — `signal_unknown_address` + PRIORITY 解析 |
| 6 | `1.5.41` | `StunUInt32Attribute` 补充 + PRIORITY 类型映射 |
| 7 | `1.5.42` | `IceConnection` — 跟踪每个对端地址的连接状态 |
| 8 | `1.5.43` | Binding response 去重 + 构造 (XOR-MAPPED-ADDRESS + MI + FINGERPRINT 调用) |
| 9 | `1.5.44` | `StunMessage::write()` 序列化 + `_add_message_integrity_of_type` 占位 |
| 10 | `1.5.45` | HMAC 计算 + FINGERPRINT CRC32 + Address 属性序列化 |
| 11 | `1.5.46` | `send_response_message()` + `to_string()` — 真正发送 UDP response |
| 12 | `1.5.47` | 修复 `AsyncUdpSocket` 异步发送 — `_send_data_from_list` 返回值避免无效发送 |

## 核心数据结构

### StunMessage

```
StunMessage {
    uint16_t _type;               // 消息类型 (BINDING_REQUEST=0x0001, BINDING_RESPONSE=0x1001)
    uint16_t _length;             // 属性部分总长度 (不含 20 字节 header)
    string   _transaction_id;     // 96 bits
    vector<unique_ptr<StunAttribute>> _attrs;  // 属性列表
    IntegrityStatus _integrity;   // MI 验证状态
    string   _password;           // ice_pwd
    string   _buffer;             // read() 时保存的原始字节 (用于 MI 验证)
}
```

### StunAttribute 继承体系

```
StunAttribute (abstract)
  ├── StunByteStringAttribute    — USERNAME, MESSAGE-INTEGRITY (可变长字节串)
  ├── StunUint32Attribute        — FINGERPRINT, PRIORITY (4 字节整数)
  ├── StunAddressAttribute       — MAPPED-ADDRESS (明文 IP/Port)
  └── StunXorAddressAttribute    — XOR-MAPPED-ADDRESS (XOR 混淆 IP/Port)
```

### IceConnection

```cpp
IceConnection {
    EventLoop* _el;
    UDPPort*   _port;                  // 归属的本地端口
    Candidate  _remote_candidate;      // 对端 prflx candidate
}
```

## 关键技术细节

### 1. STUN 消息二进制格式

```
 0                   1                   2                   3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Message Type (14bit method + 2bit class)                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Message Length (不含 20B header)                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Magic Cookie (0x2112A442)                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Transaction ID (96 bits = 12 bytes)                     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Attributes (TLV) ...                                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- Class bits: C0=bit4(0x010), C1=bit8(0x100)
- Request=0x000, Success=0x100, Error=0x110

### 2. validate_fingerprint — 快速包校验

在 `read()` 之前独立调用，用 CRC32 快速过滤非法包：
1. 长度检查 (4 的倍数 + 至少 28 字节)
2. Magic Cookie 检查 (0x2112A442)
3. 最后一个属性必须是 FINGERPRINT (type=0x8028, length=4)
4. `CRC32(data, len-8) ^ 0x5354554E == fingerprint_value`

### 3. MESSAGE-INTEGRITY — HMAC-SHA1

**验证** (`_validate_message_integrity_of_type`):
1. 从头遍历属性找到 MI 位置
2. 拷贝 [0, mi_pos) → temp_data
3. 若 MI 之后还有属性 (FINGERPRINT)，修正 temp_data 头部的 length 指向 MI 末尾
4. `HMAC-SHA1(ice_pwd, temp_data)` → 20 字节
5. 与 MI 属性 value 比对

**构造** (`_add_message_integrity_of_type`):
1. 创建 20 字节占位属性 ('0') 加入消息
2. `write(&buf)` 序列化
3. `msg_len_for_hmac = buf.Length() - 4(attr_header) - 20(mi_value)`
4. `HMAC-SHA1(ice_pwd, buf.Data(), msg_len_for_hmac)` → 20 字节
5. `copy_bytes(hmac)` 替换占位符

**关键差异**: 构造时 MI 是最后一个属性 (FINGERPRINT 还没加)，所以 length 天然正确，无需修正。验证时 MI 后面有 FINGERPRINT，必须修正 length。

### 4. FINGERPRINT — CRC32

```cpp
bool StunMessage::add_fingerprint() {
    // 1. 创建占位属性 value=0
    // 2. add_attribute → write(&buf)
    // 3. msg_len_for_crc32 = buf.Length() - 4 - 4
    // 4. ComputeCrc32(buf.Data(), msg_len_for_crc32)
    // 5. set_value(crc ^ 0x5354554E)
}
```

### 5. XOR-MAPPED-ADDRESS

二进制格式 (IPv4 = 8 bytes): `0x00 | family(0x01) | port^0x2112 | ip^0x2112A442`

**为什么要 XOR？** 防止网络中间的 ALG (Application Layer Gateway) 误识别并篡改明文 IP/Port。

### 6. 序列化 `write()` 顺序

```cpp
buf->WriteUInt16(_type);           // 2 bytes
buf->WriteUInt16(_length);         // 2 bytes (所有属性 header + value + padding 之和)
buf->WriteUInt32(k_stun_magic_cookie);  // 4 bytes
buf->WriteString(_transaction_id); // 12 bytes

for (auto& attr : _attrs) {
    buf->WriteUInt16(attr->type());     // 2 bytes
    buf->WriteUInt16(attr->length());   // 2 bytes
    attr->write(buf);                   // value + padding
}
```

### 7. `add_attribute` 与 `_length` 维护

```cpp
_length += attr->length()(padded) + k_stun_attribute_header_size(4)
```

每个属性对 `_length` 的贡献 = type(2) + length(2) + value + padding。

### 8. 属性 value 的 padding

RFC 5389 要求属性 value 长度是 4 的倍数。不足用 0 填充：

```cpp
write_padding: remain = length % 4; if (remain > 0) WriteBytes(zeros, 4-remain);
consume_padding: remain = length % 4; if (remain > 0) Consume(4-remain);
```

### 9. copy_bytes 与 HMAC 占位替换

`StunByteStringAttribute::copy_bytes()` 安全替换内部字节指针：
1. `new char[len]` + `memcpy`
2. `_set_bytes(new_bytes)` — delete[] 旧值，赋值新指针
3. `set_length(len)` — 更新 length

### 10. AsyncUdpSocket 异步发送设计

- **READ 常驻** — 持续监听 UDP 到达
- **WRITE 按需** — 有数据待发才开，队列清空立即关，避免 busy loop
- **乐观发送** — `_add_udp_packet` 先清队列，再尝试直接 `sock_send_to`，失败才入队
- **队列状态反馈** — `_send_data_from_list` 返回 bool，告知队列是否清空，避免无效发送

## 涉及文件

| 文件 | 新增/修改 |
|------|-----------|
| `src/ice/stun.h` | StunMessage, StunAttribute, StunAddressAttribute, StunXorAddressAttribute, StunUInt32Attribute, StunByteStringAttribute 完整声明 |
| `src/ice/stun.cpp` | read/write/add_fingerprint/add_message_integrity/validate_fingerprint 全部实现 |
| `src/ice/ice_connection.h/.cpp` | IceConnection — handle_stun_binding_request + send_response_message + to_string |
| `src/ice/udp_port.h/.cpp` | UDPPort — get_stun_message + send_to + create_connection + signal_unknown_address |
| `src/ice/ice_credentials.h/.cpp` | IceParameters — ice_ufrag + ice_pwd |
| `src/ice/candidate.h` | Candidate — address/port/type/priority/foundation |
| `src/base/async_udp_socket.h/.cpp` | AsyncUdpSocket + UdpPacketData — 异步 UDP 收发 |

## 与参考项目的对应

参考 xrtcserver commits: `7012b73` → `c18f33d` (11 commits)
你的 xrtc-server commits: `485b6bb` → `2333896` (12 commits，含补充和修复)
