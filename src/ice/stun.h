#ifndef __ICE_STUN_H_
#define __ICE_STUN_H_

#include <string>
#include <vector>
#include <memory>
#include <stdint.h>

#include <rtc_base/byte_buffer.h>

namespace xrtc {

// ============================================================================
// STUN (Session Traversal Utilities for NAT) — RFC 5389
//
// STUN 消息二进制格式:
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |0 0|M11|M10|M9|M8|M7|C1|M6|M5|M4|C0|M3|M2|M1|M0|  ← Message Type (14+2 bits)
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |          Message Length       |   ← 不含 20 字节头部的属性总长度
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                         Magic Cookie                          |
// |                  固定值 0x2112A442 (网络序)                    |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                                                               |
// |                Transaction ID (96 bits = 12 bytes)            |
// |                                                               |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                                                               |
// |                  Attributes (TLV 格式，可变长度)               |
// |                                                               |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Message Type 编码: 14 位方法 + 2 位 class 交织
//   Class bits: C0=bit4(0x010), C1=bit8(0x100)
//   Class mask: 0x0110, Method mask: 0x3EEF
//   Request=0x000, Indication=0x010, Success=0x100, Error=0x110
//
// 每个 Attribute 格式 (TLV):
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |         Attribute Type        |      Attribute Length         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                    Attribute Value (Length 字节)              |
// |                  (如果 Length 不是 4 的倍数，填充 0)           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// ============================================================================

// --- STUN 消息头部常量 ---
const size_t k_stun_head_size = 20;                // 头部固定 20 字节
const size_t k_stun_attribute_header_size = 4;     // 属性头部: type(2) + length(2)
const size_t k_stun_transaction_id_offset = 8;     // Transaction ID 在头部的起始偏移
const size_t k_stun_transaction_id_length = 12;    // Transaction ID 长度: 96 bits
const uint32_t k_stun_magic_cookie = 0x2112A442;   // Magic Cookie 固定值
const size_t k_stun_magic_cookie_length = sizeof(k_stun_magic_cookie);

// --- STUN 消息类型 (type 字段的解析值, 方法 + class) ---
enum StunMessageType {
    STUN_BINDING_REQUEST = 0x0001,  // Binding 请求: method=Binding(0x001) + class=Request(0x000)
};

// --- STUN 属性类型 (attr_type, RFC 5389 第 18 节) ---
enum StunAttributeType {
    STUN_ATTR_USERNAME           = 0x0006,   // 用户名: local_ufrag:remote_ufrag
    STUN_ATTR_MESSAGE_INTEGRITY  = 0x0008,   // 消息完整性: HMAC-SHA1(key=ice_pwd, data=message)
    STUN_ATTR_FINGERPRINT        = 0x8028,   // 指纹: CRC32(message) ^ 0x5354554E
};

// --- 属性值的存储类型分类 ---
// 不同 attribute type 的 value 存储方式不同:
//   USERNAME → 字节串
//   MESSAGE-INTEGRITY → 字节串 (20 字节 HMAC)
//   FINGERPRINT → uint32
//   PRIORITY → uint32
//   等等...
enum StunAttributeValueType {
    STUN_VALUE_UNKNOWN     = 0,
    STUN_VALUE_UINT32      = 1,   // 4 字节无符号整数
    STUN_VALUE_BYTE_STRING = 2,   // 可变长度字节串
};

// 前置声明
class StunAttribute;
class StunByteStringAttribute;

// ============================================================================
// StunMessage — STUN 消息的编码/解码
// ============================================================================
class StunMessage {
public:
    StunMessage();
    ~StunMessage();

    // --- 快速校验 (read 之前调用) ---
    // 检查 FINGERPRINT 属性的 CRC32 值是否合法
    // 这是最快速的合法性检查，能过滤掉绝大部分非法包
    static bool validate_fingerprint(const char* data, size_t len);

    // --- 消息解析 ---
    // 从 ByteBufferReader 中读取并解析完整 STUN 消息 (头部 + 所有属性)
    bool read(rtc::ByteBufferReader* buf);

    // --- 访问器 ---
    int type() { return _type; }
    size_t length() { return _length; }

    // --- 属性类型映射 ---
    // 根据 attribute type 返回其 value 的存储类型
    // 用于工厂方法在解析时创建正确的 StunAttribute 子类
    StunAttributeValueType get_attribute_value_type(int type);

    // --- 属性查询 ---
    // 获取指定 type 的 ByteString 属性 (用于 USERNAME / MESSAGE-INTEGRITY)
    const StunByteStringAttribute* get_byte_string(uint16_t type);

private:
    // 工厂方法: 根据 type + length 创建对应的 StunAttribute 子类
    // 返回 nullptr 表示不认识的属性类型 (read() 会跳过它)
    StunAttribute* _create_attribute(uint16_t attr_type, uint16_t attr_length);

    // 在 _attrs 中查找指定 type 的属性
    StunAttribute* _get_attribute(uint16_t type);

private:
    uint16_t _type;                                 // 消息类型 (方法 + class)
    uint16_t _length;                               // 属性部分总长度 (不含 20 字节头部)
    std::string _transaction_id;                    // 事务 ID (96 bits)
    std::vector<std::unique_ptr<StunAttribute>> _attrs;  // 已解析的属性列表
};

// ============================================================================
// StunAttribute — 属性抽象基类
// ============================================================================
class StunAttribute {
public:
    virtual ~StunAttribute();

    // 属性类型 ID (如 USERNAME=0x0006)
    int type() const { return _type; }
    // 属性 value 的字节数 (不含 padding)
    size_t length() const { return _length; }

    // 工厂方法: 根据 value_type 创建具体的 StunAttribute 子类
    // owner 参数保留用于未来扩展 (如访问 StunMessage 的上下文)
    static StunAttribute* create(StunAttributeValueType value_type,
            uint16_t type, uint16_t length, void* owner);

    // 从 ByteBufferReader 中读取属性 value (子类实现)
    virtual bool read(rtc::ByteBufferReader* buf) = 0;

protected:
    // 构造函数 protected — 只允许子类调用
    StunAttribute(uint16_t type, uint16_t length);

    // 消费 4 字节对齐产生的 padding 字节
    // RFC 5389: 属性 value 长度必须是 4 的倍数, 不足则填充 0
    void consume_padding(rtc::ByteBufferReader* buf);

private:
    uint16_t _type;
    uint16_t _length;
};

// ============================================================================
// StunUint32Attribute — uint32 类型属性 (FINGERPRINT, PRIORITY 等)
// ============================================================================
class StunUint32Attribute : public StunAttribute {
public:
    static const size_t SIZE = 4;
};

// ============================================================================
// StunByteStringAttribute — 字节串类型属性 (USERNAME, MESSAGE-INTEGRITY 等)
// ============================================================================
class StunByteStringAttribute : public StunAttribute {
public:
    StunByteStringAttribute(uint16_t type, uint16_t length);
    ~StunByteStringAttribute() override;

    // 从 buf 中读取 length() 字节到 _bytes
    bool read(rtc::ByteBufferReader* buf) override;

    std::string get_string() const { return std::string(_bytes, length()); }

private:
    char* _bytes = nullptr;
};

} // namespace xrtc

#endif // __ICE_STUN_H_
