#include "ice/stun.h"

#include <rtc_base/byte_order.h>
#include <rtc_base/crc32.h>
#include <rtc_base/message_digest.h>
#include <rtc_base/logging.h>

namespace xrtc {

// ============================================================================
// STUN 常量
// ============================================================================

// 未初始化时使用的空 Transaction ID (12 个 '0')
const char EMPTY_TRANSACTION_ID[] = "000000000000"; // 96 bits

// FINGERPRINT 属性的 XOR 常量 (RFC 5389 第 15.5 节)
// CRC32 计算结果需要与这个值做异或再写入属性
const size_t STUN_FINGERPRINT_XOR_VALUE = 0x5354554e;

const char STUN_ERROR_REASON_BAD_REQUEST[] = "Bad Request";
const char STUN_ERROR_REASON_UNAUTHORIZED[] = "Unauthorized";
const char STUN_ERROR_REASON_SERVER_ERROR[] = "Server Error";

std::string stun_method_to_string(int type) {
    switch (type) {
        case STUN_BINDING_REQUEST:
            return "BINDING REQUEST";
        case STUN_BINDING_RESPONSE:
            return "BINDING RESPONSE";
        case STUN_BINDING_ERROR_RESPONSE:
            return "BINDING ERROR RESPONSE";
        default:
            return "Unknown<" + std::to_string(type) + ">";
    }
}

// ============================================================================
// StunMessage — 构造函数 / 析构函数
// ============================================================================

StunMessage::StunMessage() :
    _type(0),
    _length(0),
    _transaction_id(EMPTY_TRANSACTION_ID)
{
}

StunMessage::~StunMessage() = default;

// ============================================================================
// StunMessage::validate_fingerprint — 快速指纹校验
//
// 在 read() 之前独立调用，用于快速校验 STUN 包的合法性。
// 如果能通过这个校验，几乎可以确定是一个合法的 STUN 包。
//
// RFC 5389 第 15.5 节规定:
//   1. FINGERPRINT 必须是消息的最后一个属性
//   2. CRC32 计算范围: 从消息头开始，到 FINGERPRINT 属性之前结束 (不含 FINGERPRINT 属性本身)
//   3. 计算公式: CRC32(data, len - (header_size + mi_attr_value_size)) ^ 0x5354554E == fingerprint_value
//
// 校验步骤:
//   1. 长度检查: 必须是 4 的倍数 + 至少包含一个 FINGERPRINT 属性
//   2. Magic Cookie 检查: 必须是 0x2112A442
//   3. 属性头检查: 最后一个属性必须是 FINGERPRINT (type=0x8028, length=4)
//   4. CRC32 检查: 用消息内容计算 CRC32，与指纹值比对
// ============================================================================
bool StunMessage::validate_fingerprint(const char* data, size_t len) {
    // 1. 长度检查
    // fingerprint 属性 = attr_header(4) + uint32(4) = 8 字节
    size_t fingerprint_attr_size = k_stun_attribute_header_size +
        StunUint32Attribute::SIZE;

    // STUN 包长度必须是 4 的倍数 (RFC 5389 第 7 节: 消息长度对齐)
    // 包总长度必须 >= 头部(20) + FINGERPRINT 属性(8)
    if (len % 4 != 0 || len < k_stun_header_size + fingerprint_attr_size) {
        return false;
    }

    // 2. Magic Cookie 检查
    // k_stun_transaction_id_offset = 8, k_stun_magic_cookie_length = 4
    // → magic_cookie 位于 data + 8 - 4 = data + 4 (即头部第 4 个字节)
    const char* magic_cookie = data + k_stun_transaction_id_offset -
        k_stun_magic_cookie_length; // 定位到 magic cookie 的位置
    if (rtc::GetBE32(magic_cookie) != k_stun_magic_cookie) {
        return false;
    }

    // 3. FINGERPRINT 属性头和长度检查
    // fingerprint 是最后一个属性，所以从报文末尾回退来定位即可，无需遍历所有属性
    const char* fingerprint_attr_data = data + len - fingerprint_attr_size;

    // 检查属性 type 是否为 FINGERPRINT (0x8028)
    if (rtc::GetBE16(fingerprint_attr_data) != STUN_ATTR_FINGERPRINT) {
        return false;
    }
    // 检查属性 length 是否等于 4 (uint32 的大小)
    if (rtc::GetBE16(fingerprint_attr_data + sizeof(uint16_t)) !=
        StunUint32Attribute::SIZE) {
        return false;
    }

    // 4. CRC32 校验
    // fingerprint_attr_data + k_stun_attribute_header_size = 指纹 value 的起始位置
    uint32_t fingerprint = rtc::GetBE32(fingerprint_attr_data +
        k_stun_attribute_header_size);

    // data + len = 报文末尾
    // data + len - fingerprint_attr_size = FINGERPRINT 属性起始位置
    // len - fingerprint_attr_size = 需要计算 CRC32 的数据长度 (排除整个 FINGERPRINT 属性)
    return (fingerprint ^ STUN_FINGERPRINT_XOR_VALUE) ==
        rtc::ComputeCrc32(data, len - fingerprint_attr_size);
}

bool StunMessage::add_fingerprint() {
    // 1. 创建占位属性: value=0 (后续计算 CRC32 后替换)
    auto fingerprint_attr_ptr = std::make_unique<StunUint32Attribute>(
        STUN_ATTR_FINGERPRINT, 0);

    // 2. 保存裸指针 — move 后 unique_ptr 所有权转移，但裸指针仍有效
    StunUint32Attribute* fingerprint_origin_ptr = fingerprint_attr_ptr.get();
    add_attribute(std::move(fingerprint_attr_ptr));

    // 3. 序列化整条消息
    rtc::ByteBufferWriter buf;
    if (!write(&buf)) {
        return false;
    }

    // 4. CRC32 计算范围: 从消息头到 FINGERPRINT 属性之前 (不含 FINGERPRINT 自身)
    size_t msg_len_for_crc32 = buf.Length() - k_stun_attribute_header_size
        - fingerprint_origin_ptr->length();
    uint32_t c = rtc::ComputeCrc32(buf.Data(), msg_len_for_crc32);
    // 5. XOR 替换占位值
    fingerprint_origin_ptr->set_value(c ^ STUN_FINGERPRINT_XOR_VALUE);
    return true;
}

// ============================================================================
// StunMessage::validate_message_integrity — MESSAGE-INTEGRITY 验证 (RFC 5389 §15.4)
//
// MESSAGE-INTEGRITY 用于验证 STUN 消息未被篡改。
// 机制: 发送方用 ice_pwd 作为密钥，对消息内容计算 HMAC-SHA1(20 字节)，
//       写入 MESSAGE-INTEGRITY 属性。接收方用同样的密钥重新计算 HMAC 并比对。
//
// 关键规则 (RFC 5389 §15.4):
//   1. MESSAGE-INTEGRITY 始终放在 FINGERPRINT 之前
//   2. HMAC 计算前，消息头中的 length 必须调整为"指向 MI 属性末尾"
//      (包含 MI 自身，但不含后面的 FINGERPRINT)
//   3. HMAC key = ice_pwd（STUN 长期凭据密码）
//
// 返回 IntegrityStatus:
//   - k_not_set: 未调用此方法
//   - k_no_integrity: 消息中没有 MESSAGE-INTEGRITY 属性
//   - k_integrity_ok: HMAC 验证通过
//   - k_integrity_bad: HMAC 验证失败 (消息被篡改或密码不匹配)
// ============================================================================
StunMessage::IntegrityStatus StunMessage::validate_message_integrity(const std::string& password) {
    _password = password;

    // 1. 检查消息中是否包含 MESSAGE-INTEGRITY 属性
    if (get_byte_string(STUN_ATTR_MESSAGE_INTEGRITY)) {
        // 2. 对原始字节数据 _buffer 做 HMAC 验证
        //    注意: 必须传 _buffer (原始字节)，不能用 read() 解析后的 _attrs 列表，
        //    因为 HMAC 计算依赖精确的字节布局 (属性顺序、padding 等)
        if (_validate_message_integrity_of_type(STUN_ATTR_MESSAGE_INTEGRITY,
                    k_stun_message_integrity_size,
                    _buffer.c_str(), _buffer.length(),
                    password))
        {
            _integrity = IntegrityStatus::k_integrity_ok;
        } else {
            _integrity = IntegrityStatus::k_integrity_bad;
        }
    } else {
        _integrity = IntegrityStatus::k_no_integrity;
    }

    return _integrity;
}

bool StunMessage::add_message_integrity(const std::string& password) {
    return _add_message_integrity_of_type(STUN_ATTR_MESSAGE_INTEGRITY,
        k_stun_message_integrity_size, password.c_str(), password.size());
}

// 构造 MESSAGE-INTEGRITY 属性并写入消息
// 流程: 创建 20 字节占位属性 → 加入消息 → 序列化整条消息到 buffer
// 注意: commit 10 会在序列化后计算 HMAC 并替换占位符
bool StunMessage::_add_message_integrity_of_type(uint16_t attr_type, uint16_t attr_size,
        const char* key, size_t key_len)
{
    // 1. 创建占位属性: 20 字节 '0' (后续 commit 会用真实 HMAC 替换)
    auto mi_attr_ptr = std::make_unique<StunByteStringAttribute>(attr_type,
        std::string(attr_size, '0'));

    // 2. 保存裸指针 — move 后 unique_ptr 所有权转移，但裸指针仍有效
    StunByteStringAttribute* mi_attr_origin_ptr = mi_attr_ptr.get();
    add_attribute(std::move(mi_attr_ptr));

    // 3. 序列化整条消息 (header + 目前插入的所有属性)
    rtc::ByteBufferWriter buf;
    if (!write(&buf)) {
        return false;
    }

    uint32_t msg_len_for_hmac = buf.Length() - k_stun_attribute_header_size -
        mi_attr_origin_ptr->length();
    char hmac[attr_size] = {0};
    if (rtc::ComputeHmac(rtc::DIGEST_SHA_1, 
        key, key_len, buf.Data(), msg_len_for_hmac, hmac, attr_size)
        != attr_size)
    {
        RTC_LOG(LS_WARNING) << "compute hmac error";
        return false;
    }
    mi_attr_origin_ptr->copy_bytes(hmac, attr_size);
    _password.assign(key, key_len);
    _integrity = IntegrityStatus::k_integrity_ok;

    return true;
}

// ============================================================================
// _validate_message_integrity_of_type — HMAC-SHA1 验证核心
//
// 参数:
//   mi_attr_type: MESSAGE-INTEGRITY 属性 type (0x0008)
//   mi_attr_size: MESSAGE-INTEGRITY value 长度 (20 字节)
//   data / size:  完整 STUN 消息的原始字节
//   password:     ice_pwd (用作 HMAC key)
//
// 验证步骤:
//   1. 基本合法性检查 (size 对齐 + header length 一致性)
//   2. 遍历属性找到 MESSAGE-INTEGRITY 所在位置
//   3. 拷贝 [0, mi_pos) 的原始数据为 temp_data
//   4. 若 MI 之后还有属性 (如 FINGERPRINT)，修正 temp_data 头部的 length 字段,
//      使其指向 MI 属性末尾 (包含 MI，但不含 FINGERPRINT)
//   5. 对 temp_data 计算 HMAC-SHA1(password, temp_data)
//   6. 比较计算结果与 MI 属性中的 value
// ============================================================================
bool StunMessage::_validate_message_integrity_of_type(uint16_t mi_attr_type,
        size_t mi_attr_size, const char* data, size_t size,
        const std::string& password)
{
    // ---- 第 1 步: 基本合法性检查 ----
    // STUN 消息长度必须是 4 的倍数 (RFC 5389 §7)
    if (size % 4 != 0 || size < k_stun_header_size) {
        return false;
    }

    // 消息头中的 length 字段必须与实际 size 一致
    // data[2] 是 Message Length 字段 (2 字节，不含 20 字节头)
    uint16_t length = rtc::GetBE16(&data[2]);
    if (length + k_stun_header_size != size) {
        return false;
    }

    // ---- 第 2 步: 遍历属性，定位 MESSAGE-INTEGRITY 位置 ----
    // 从头部之后 (偏移 20) 开始扫描 TLV 属性
    // 需要找到 MI 属性在原始字节中的偏移位置 mi_pos
    size_t current_pos = k_stun_header_size;
    bool has_message_integrity = false;
    while (current_pos + k_stun_attribute_header_size <= size) {
        uint16_t attr_type;
        uint16_t attr_length;
        attr_type = rtc::GetBE16(&data[current_pos]);
        attr_length = rtc::GetBE16(&data[current_pos + sizeof(attr_type)]);

        if (attr_type == mi_attr_type) {
            has_message_integrity = true;
            break;  // current_pos 就是 MI 属性的起始偏移
        }

        // 跳过当前属性: header(4) + value(attr_length) + padding
        current_pos += k_stun_attribute_header_size + attr_length;
        // RFC 5389: 属性 value 按 4 字节对齐，不足则填充
        if (attr_length % 4 != 0) {
            current_pos += (4 - (attr_length % 4));
        }
    }

    if (!has_message_integrity) {
        return false;
    }

    // ---- 第 3 步: 拷贝消息中 MI 之前的部分 ----
    // mi_pos = MI 属性在原始数据中的起始偏移
    // 拷贝 data[0..mi_pos) → temp_data
    // 这段数据包含: 消息头 + MI 之前的所有属性 (如 USERNAME)
    size_t mi_pos = current_pos;
    std::unique_ptr<char[]> temp_data(new char[mi_pos]);
    memcpy(temp_data.get(), data, mi_pos);

    // ---- 第 4 步: 修正 temp_data 头部的 length 字段 ----
    // RFC 5389 §15.4: HMAC 计算时，消息头中的 length 必须调整为
    // "指向 MI 属性末尾" — 即包含 MI 自身，但不含其后的任何属性。
    //
    // 场景: 消息属性 = [USERNAME] [MESSAGE-INTEGRITY] [FINGERPRINT]
    //   原始 length = USERNAME + MI(4+20) + FINGERPRINT(8) = 44
    //   修正 length = USERNAME + MI(4+20) = 36 (只去掉 FINGERPRINT)
    //
    // 如果 MI 之后还有属性 (如 FINGERPRINT)：
    //   extra_size = FINGERPRINT 占用的字节数
    //   adjust_new_len = 消息总长 - extra_size - 消息头(20)
    //                  = 所有属性中 MI(含) 之前的属性总长
    if (size > current_pos + k_stun_attribute_header_size + k_stun_message_integrity_size) {
        size_t extra_pos = mi_pos + k_stun_attribute_header_size + mi_attr_size;
        // extra_pos = MI 属性之后的第一个字节偏移
        size_t extra_size = size - extra_pos;
        // extra_size = MI 之后所有属性的总字节数 (如 FINGERPRINT: 4+4=8)
        size_t adjust_new_len = size - extra_size - k_stun_header_size;
        // adjust_new_len = MI 之前的属性总长 = mi_pos - 20
        // 写入 temp_data 头部偏移 2 处，替换原来的 length 字段
        rtc::SetBE16(temp_data.get() + 2, adjust_new_len);
    }

    // ---- 第 5 步: 计算 HMAC-SHA1 ----
    // key   = ice_pwd (password)
    // data  = temp_data (消息头 + MI 之前的属性)
    // 结果  = 20 字节 hmac
    char hmac[k_stun_message_integrity_size];
    size_t ret = rtc::ComputeHmac(rtc::DIGEST_SHA_1, password.c_str(),
            password.length(), temp_data.get(), current_pos, hmac,
            sizeof(hmac));
    if (ret != k_stun_message_integrity_size) {
        return false;
    }

    // ---- 第 6 步: 比对 HMAC ----
    // 计算出的 hmac 与 MI 属性中的 value 逐字节比较
    // mi_pos + 4 (跳过 attr type + attr length) = MI value 的位置
    return memcmp(data + mi_pos + k_stun_attribute_header_size, hmac, mi_attr_size) == 0;
}

// ============================================================================
// StunMessage::read — 解析 STUN 消息
//
// 从 ByteBufferReader 中读取完整的 STUN 消息:
//   1. 读头部: type(2) + length(2) + magic_cookie(4) + transaction_id(12)
//   2. 循环读取属性: attr_type(2) + attr_length(2) + value(attr_length) + padding
//
// 属性通过 _create_attribute() 工厂创建，不认识的类型会被跳过 (Consume 掉数据)
// ============================================================================
bool StunMessage::read(rtc::ByteBufferReader* buf) {
    if (!buf) return false;

    _buffer.assign(buf->Data(), buf->Length());

    // --- 读消息头部 (20 字节) ---

    // 1. 读 type (2 字节)
    if (!buf->ReadUInt16(&_type)) return false;

    // 2. 排除 RTP/RTCP:
    //    RTP 包前 2 位固定为 10 (version=2)
    //    STUN 包前 2 位固定为 00
    //    bit 11 (0x0800) = 1 表示这不是 STUN (是 RTP/RTCP)
    if (_type & 0x0800) return false;

    // 3. 读 length (2 字节): 属性部分的总长度 (不含 20 字节头部)
    if (!buf->ReadUInt16(&_length)) return false;

    // 4. 读 Magic Cookie (4 字节): 固定值 0x2112A442
    std::string magic_cookie;
    if (!buf->ReadString(&magic_cookie, k_stun_magic_cookie_length)) {
        return false;
    }

    // 5. 读 Transaction ID (12 字节 = 96 bits)
    std::string transaction_id;
    if (!buf->ReadString(&transaction_id, k_stun_transaction_id_length)) {
        return false;
    }

    // 6. 兼容经典 STUN (RFC 3489):
    //    经典 STUN 没有 Magic Cookie，其 transaction ID 是 128 bits
    //    如果 Magic Cookie 不等于 0x2112A442，说明这是一个经典 STUN 包，
    //    把 magic_cookie 的 4 字节并入 transaction_id 前部 (共 16 字节 = 128 bits)
    uint32_t magic_cookie_int;
    memcpy(&magic_cookie_int, magic_cookie.data(), sizeof(magic_cookie_int));
    if (rtc::NetworkToHost32(magic_cookie_int) != k_stun_magic_cookie) {
        transaction_id.insert(0, magic_cookie);
    }
    _transaction_id = transaction_id;

    // 7. 校验剩余长度:
    //    buf->Length() 是读完头部后剩余的数据量
    //    它必须等于 _length (属性部分的声明长度)
    if (buf->Length() != _length) return false;

    _attrs.resize(0);

    // --- 循环读取属性 (TLV 格式) ---
    // 每个属性: Type(2) + Length(2) + Value(Length 字节) + Padding(0~3 字节)
    while (buf->Length() > 0) {
        uint16_t attr_type, attr_length;
        if (!buf->ReadUInt16(&attr_type)) return false;
        if (!buf->ReadUInt16(&attr_length)) return false;

        // 工厂方法: 根据 attr_type 创建对应的 StunAttribute 子类
        // 返回 nullptr 表示不认识的属性类型
        std::unique_ptr<StunAttribute> attr(_create_attribute(attr_type, attr_length));
        if (!attr) {
            // --- 不认识的属性: 跳过 ---
            // STUN 要求 value 长度是 4 字节对齐 (RFC 5389 第 15 节)
            // 不足 4 的倍数则填充到 4 的倍数，所以消费的时候也要考虑对齐
            if (attr_length % 4 != 0) {
                attr_length += (4 - (attr_length % 4));
            }
            // Consume: 直接从缓冲区中丢弃 attr_length 字节
            if (!buf->Consume(attr_length)) return false;
        } else {
            // --- 认识的属性: 读取 value ---
            // 调用子类的 read() 方法解析 value 数据
            if (!attr->read(buf)) return false;
            _attrs.push_back(std::move(attr));
        }
    }

    return true;
}

// 序列化 StunMessage 为 STUN 二进制格式 (RFC 5389 §6)
// 顺序: type(2) + length(2) + magic_cookie(4) + transaction_id(12) + 属性列表(TLV)
bool StunMessage::write(rtc::ByteBufferWriter* buf) const {
    if (!buf) {
        return false;
    }

    buf->WriteUInt16(_type);
    buf->WriteUInt16(_length);
    buf->WriteUInt32(k_stun_magic_cookie);
    buf->WriteString(_transaction_id);

    for (const auto& attr : _attrs) {
        buf->WriteUInt16(attr->type());
        buf->WriteUInt16(attr->length());
        if (!attr->write(buf)) {
            return false;
        }
    }

    return true;
}

void StunMessage::add_attribute(std::unique_ptr<StunAttribute> attr) {
    size_t attr_len = attr->length();
    if (attr_len % 4 != 0) {
        attr_len += (4 - (attr_len % 4));
    }

    _length += (attr_len + k_stun_attribute_header_size);

    _attrs.push_back(std::move(attr));
}

// ============================================================================
// StunMessage::get_attribute_value_type — 属性类型映射
//
// 将 attribute type ID 映射为 value 的存储类型。
// 这是工厂模式的关键: 看到 type ID 就知道要创建哪种 StunAttribute 子类。
// ============================================================================
StunAttributeValueType StunMessage::get_attribute_value_type(int type) {
    switch (type) {
        case STUN_ATTR_USERNAME:
        case STUN_ATTR_MESSAGE_INTEGRITY:
            // USERNAME 和 MESSAGE-INTEGRITY 的 value 都是字节串
            return STUN_VALUE_BYTE_STRING;
        case STUN_ATTR_PRIORITY:
            return STUN_VALUE_UINT32;
        default:
            return STUN_VALUE_UNKNOWN;
    }
}

// ============================================================================
// StunMessage::get_byte_string — 按类型查找 ByteString 属性
//
// 在 _attrs 中查找 type 匹配的属性，并 static_cast 为 StunByteStringAttribute*
// 用于 stun binding request 中检查 USERNAME / MESSAGE-INTEGRITY 属性是否存在
// ============================================================================
const StunByteStringAttribute* StunMessage::get_byte_string(uint16_t type) {
    return static_cast<const StunByteStringAttribute*>(_get_attribute(type));
}

const StunUint32Attribute* StunMessage::get_uint32_t(uint16_t type) {
    return static_cast<const StunUint32Attribute*>(_get_attribute(type));
}

// ============================================================================
// StunMessage::_get_attribute — 遍历查找属性
//
// 遍历 _attrs 列表，找到第一个 type 匹配的属性。
// 如果没有找到，返回 nullptr。
// ============================================================================
StunAttribute* StunMessage::_get_attribute(uint16_t type) {
    for (const auto& attr : _attrs) {
        if (attr->type() == type) {
            return attr.get();
        }
    }
    return nullptr;
}

// ============================================================================
// StunMessage::_create_attribute — 属性工厂
//
// 两步:
//   1. get_attribute_value_type() — 查 type → value_type 映射
//   2. StunAttribute::create()     — 根据 value_type 实例化具体子类
//
// 如果 type 不在任何映射中 (返回 STUN_VALUE_UNKNOWN)，返回 nullptr，
// read() 会自动跳过该属性。
// ============================================================================
StunAttribute* StunMessage::_create_attribute(uint16_t attr_type, uint16_t attr_length) {
    StunAttributeValueType value_type = get_attribute_value_type(attr_type);
    if (STUN_VALUE_UNKNOWN == value_type) {
        return nullptr;
    }
    return StunAttribute::create(value_type, attr_type, attr_length, this);
}

// ============================================================================
// StunAttribute — 属性基类实现
// ============================================================================

StunAttribute::StunAttribute(uint16_t type, uint16_t length)
    : _type(type), _length(length) {}

StunAttribute::~StunAttribute() = default;

// ============================================================================
// StunAttribute::create — 静态工厂方法
//
// 根据 value_type 创建具体的 StunAttribute 子类实例。
// 返回裸指针，调用方用 unique_ptr 接管所有权。
//
// ============================================================================
StunAttribute* StunAttribute::create(StunAttributeValueType value_type,
        uint16_t type, uint16_t length, void* /*owner*/)
{
    switch (value_type) {
        case STUN_VALUE_BYTE_STRING:
            return new StunByteStringAttribute(type, length);
        case STUN_VALUE_UINT32:
            return new StunUint32Attribute(type);
        default:
            return nullptr;
    }
}

std::unique_ptr<StunErrorCodeAttribute> StunAttribute::create_error_code() {
    return std::make_unique<StunErrorCodeAttribute>(
        STUN_ATTR_ERROR_CODE, StunErrorCodeAttribute::MIN_SIZE);
}

// ============================================================================
// StunAttribute::consume_padding — 消费填充字节
//
// RFC 5389 第 15 节: 属性 value 长度必须是 4 的倍数。
// 如果实际 value 长度不是 4 的倍数，会在末尾填充 0 补齐。
// 这个方法消费这些填充字节。
//
// 例如: attr_length=7, 7%4=3, 填充 4-3=1 字节
//       总占用 = 7 + 1 = 8 字节 (4 字节对齐)
// ============================================================================
void StunAttribute::consume_padding(rtc::ByteBufferReader* buf) {
    int remain = length() % 4;
    if (remain > 0) {
        buf->Consume(4 - remain);
    }
}

// RFC 5389 §15: 属性 value 长度必须是 4 的倍数，不足填充 0 字节
void StunAttribute::write_padding(rtc::ByteBufferWriter* buf) {
    int remain = length() % 4;
    if (remain > 0) {
        char zeros[4] = {0};
        buf->WriteBytes(zeros, 4 - remain);
    }
}

// Address
StunAddressAttribute::StunAddressAttribute(uint16_t type,
        const rtc::SocketAddress& addr) :
    StunAttribute(type, 0) // 长度暂时为0，IPv4: 4字节
{
    set_address(addr);
}

// 根据地址族自动设置属性 value 长度: IPv4=8, IPv6=20
// STUN 地址属性格式: 0x00(1) + Family(1) + Port(2) + IP(4或16)
void StunAddressAttribute::set_address(const rtc::SocketAddress& addr) {
    _address = addr;

    switch (family()) {
        case STUN_ADDRESS_IPV4:
            set_length(SIZE_IPV4);
            break;
        case STUN_ADDRESS_IPV6:
            set_length(SIZE_IPV6);
            break;
        default:
            set_length(SIZE_UNDEF);
            break;
    }
}

// 映射系统地址族 (AF_INET/AF_INET6) 到 STUN Address Family (0x01/0x02)
StunAddressFamily StunAddressAttribute::family() {
    switch (_address.family()) {
        case AF_INET:
            return STUN_ADDRESS_IPV4;
        case AF_INET6:
            return STUN_ADDRESS_IPV6;
        default:
            return STUN_ADDRESS_UNDEF;
    }
}

bool StunAddressAttribute::read(rtc::ByteBufferReader* buf) {
    return true;
}

/*
 0                   1                   2                   3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|X X X X X X X X|    Family     |            Port               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                 Address (32 bits for IPv4)                    |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
bool StunAddressAttribute::write(rtc::ByteBufferWriter* buf) {
    StunAddressFamily stun_family = family();
    if (stun_family == STUN_ADDRESS_UNDEF) {
        RTC_LOG(LS_WARNING) << "write address attribute error: unknown family";
        return false;
    }

    buf->WriteUInt8(0);
    buf->WriteUInt8(stun_family);
    buf->WriteUInt16(_address.port());

    switch (_address.family()) {
        case AF_INET: {
            in_addr v4addr = _address.ipaddr().ipv4_address();
            buf->WriteBytes((const char*)&v4addr, sizeof(v4addr));
            break;

        }
        case AF_INET6: {
            in6_addr v6addr = _address.ipaddr().ipv6_address();
            buf->WriteBytes((const char*)&v6addr, sizeof(v6addr));
            break;
        }
        default:
            return false;
            break;
    }

    return true;
}

// Xor Address
StunXorAddressAttribute::StunXorAddressAttribute(uint16_t type,
        const rtc::SocketAddress& addr) :
    StunAddressAttribute(type, addr)
{
}


bool StunXorAddressAttribute::write(rtc::ByteBufferWriter* buf) {
    StunAddressFamily stun_family = family();
    if (stun_family == STUN_ADDRESS_UNDEF) {
        RTC_LOG(LS_WARNING) << "write address attribute error: unknown family";
        return false;
    }

    rtc::IPAddress xored_ip = _get_xored_ip();
    if (xored_ip.family() == AF_UNSPEC) {
        return false;
    }

    buf->WriteUInt8(0);
    buf->WriteUInt8(stun_family);
    // 异或magic_cookie的高16位
    buf->WriteUInt16(_address.port() ^ (k_stun_magic_cookie) >> 16);

    switch (_address.family()) {
        case AF_INET: {
            in_addr v4addr = xored_ip.ipv4_address();
            buf->WriteBytes((const char*)&v4addr, sizeof(v4addr));
            break;

        }
        case AF_INET6: {
            in6_addr v6addr = xored_ip.ipv6_address();
            buf->WriteBytes((const char*)&v6addr, sizeof(v6addr));
            break;
        }
        default:
            return false;
            break;
    }

    return true;
}

rtc::IPAddress StunXorAddressAttribute::_get_xored_ip() {
    rtc::IPAddress ip = _address.ipaddr();
    switch (_address.family()) {
        case AF_INET: {
            in_addr v4addr = ip.ipv4_address();
            v4addr.s_addr = (v4addr.s_addr ^ rtc::HostToNetwork32(k_stun_magic_cookie));
            return rtc::IPAddress(v4addr);
        }
        case AF_INET6:
            // no support yet
            break;
        default:
            break;
    }
    return rtc::IPAddress();
}

// ============================================================================

// ============================================================================
StunUint32Attribute::StunUint32Attribute(uint16_t type) :
    StunAttribute(type, SIZE) {}

StunUint32Attribute::StunUint32Attribute(uint16_t type, uint32_t value) :
    StunAttribute(type, SIZE), _bits(value) {}

StunUint32Attribute::~StunUint32Attribute() {}


bool StunUint32Attribute::read(rtc::ByteBufferReader* buf) {
    if (length() != SIZE || !buf->ReadUInt32(&_bits)) {
        return false;
    }
    return true;
}

// 写入 4 字节无符号整数 (FINGERPRINT, PRIORITY 等)
bool StunUint32Attribute::write(rtc::ByteBufferWriter* buf) {
    buf->WriteUInt32(_bits);
    return true;
}

// ============================================================================
// StunByteStringAttribute — 字节串属性实现
//
// 用于 USERNAME (local_ufrag:remote_ufrag) 和 MESSAGE-INTEGRITY (HMAC-SHA1 值)
// 数据存储在堆上的 _bytes 数组中，长度由基类的 _length 记录。
// ============================================================================

StunByteStringAttribute::StunByteStringAttribute(uint16_t type, uint16_t length) :
    StunAttribute(type, length) {}

StunByteStringAttribute::StunByteStringAttribute(uint16_t type, const std::string& str) :
    StunAttribute(type, 0)
{
    copy_bytes(str.c_str(), str.size());
}

StunByteStringAttribute::~StunByteStringAttribute() {
    if (_bytes) {
        delete []_bytes;
        _bytes = nullptr;
    }
}

// 替换属性 value: 释放旧 _bytes，复制新数据，更新 _length
// 用于 _add_message_integrity_of_type 中: 先占位 '0'*20，HMAC 算出后替换
void StunByteStringAttribute::copy_bytes(const char* bytes, size_t len) {
    char* new_bytes = new char[len];
    memcpy(new_bytes, bytes, len);
    _set_bytes(new_bytes);
    set_length(len);
}

// 安全替换内部字节指针: 先 delete[] 旧值，再赋值新指针
void StunByteStringAttribute::_set_bytes(char* bytes) {
    if (_bytes) {
        delete[] _bytes;
    }
    _bytes = bytes;
}

// read: 从 buf 中读取 length() 字节到 _bytes，然后消费 padding
bool StunByteStringAttribute::read(rtc::ByteBufferReader* buf) {
    _bytes = new char[length()];
    if (!buf->ReadBytes(_bytes, length())) {
        return false;
    }

    // 消费属性 value 的填充字节 (不足 4 倍数的补齐部分)
    consume_padding(buf);

    return true;
}


// 写入 value 字节，然后填充 0 使总长度对齐到 4 的倍数
bool StunByteStringAttribute::write(rtc::ByteBufferWriter* buf) {
    buf->WriteBytes(_bytes, length());
    write_padding(buf);
    return true;
}

// ============================================================================
// StunErrorCodeAttribute — RFC 5389 第 15.6 节
//
// 属性格式 (4 + reason 长度):
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |           Reserved (21 bits)           |Class|     Number    |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |      Reason Phrase (variable)                                ...
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Class:  百位数字 (3 bits, 值 3-6)
// Number: 十位+个位 (8 bits, 值 0-99)
// 例: 401 → class=4, number=1, wire format = (4 << 8) | 1 = 0x0401
//
// MIN_SIZE = 4: Reserved(21) + Class(3) + Number(8) = 32 bits
// ============================================================================

const uint16_t StunErrorCodeAttribute::MIN_SIZE = 4;

StunErrorCodeAttribute::StunErrorCodeAttribute(uint16_t type, uint16_t length) :
    StunAttribute(type, length), _class(0), _number(0) {}

void StunErrorCodeAttribute::set_code(int code) {
    _class = code / 100;    // 百位: 401 / 100 = 4
    _number = code % 100;   // 十位+个位: 401 % 100 = 1
}

void StunErrorCodeAttribute::set_reason(const std::string& reason) {
    _reason = reason;
    set_length(MIN_SIZE + reason.size());
}

bool StunErrorCodeAttribute::read(rtc::ByteBufferReader* buf) {
    // todo: 错误响应是服务端发出的，暂时不需要解析
    return false;
}

bool StunErrorCodeAttribute::write(rtc::ByteBufferWriter* buf) {
    // wire format: 高 21 bits 为 0 (Reserved), bits 10-8 为 Class, bits 7-0 为 Number
    buf->WriteUInt32(_class << 8 | _number);
    buf->WriteString(_reason);
    write_padding(buf);
    return true;
}

} // namespace xrtc
