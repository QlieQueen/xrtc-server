#include "ice/stun.h"

#include <rtc_base/byte_order.h>
#include <rtc_base/crc32.h>

namespace xrtc {

const char EMPTY_TRANSACTION_ID[] = "000000000000"; // 96bits
const size_t STUN_FINGERPRINT_XOR_VALUE = 0x5354554e;

StunMessage::StunMessage() :
    _type(0),
    _length(0),
    _transaction_id(EMPTY_TRANSACTION_ID)
{

}

StunMessage::~StunMessage() = default;

bool StunMessage::validate_fingerprint(const char* data, size_t len) {
    // 检查长度
    // 1. 长度必须是4的整数倍（不是也会填充）
    // 2. stun包至少包含一个fingerprint的属性
    size_t fingerprint_attr_size = k_stun_attribute_header_size +
        StunUint32Attribute::SIZE;
    if (len % 4 != 0 || len < k_stun_head_size + fingerprint_attr_size) {
        return false;
    }

    // 检查magic_cookie
    // (为什么要先定位到transaction id的起始地址，在减去magic cookie的大小这种方式？直接偏移 type 和 length的长度不行吗？)
    const char* magic_cookie = data + k_stun_transaction_id_offset -
        k_stun_magic_cookie_length; // 定位到magic cookie的位置
    if (rtc::GetBE32(magic_cookie) != k_stun_magic_cookie) {
        return false;
    }

    // 检查attr type和length
    // fingerprint属性必须是stun报文的最后一个属性(RFC 5389 15.5节)
    // 所以可以直接从报文末尾回退来定位，无需遍历所有属性
    const char* fingerprint_attr_data = data + len - fingerprint_attr_size;
    if (rtc::GetBE16(fingerprint_attr_data) != STUN_ATTR_FINGERPRINT ||
        rtc::GetBE16(fingerprint_attr_data + sizeof(uint16_t)) !=
        StunUint32Attribute::SIZE)
    {
        return false;
    }

    // 检查fingerprint的值
    uint32_t fingerprint = rtc::GetBE32(fingerprint_attr_data +
        k_stun_attribute_header_size); // 获取报文中fingerprint属性的value值
    
    // STUN FINGERPRINT 校验：
    // RFC 5389 第15.5节规定：
    //   1. FINGERPRINT必须是消息的最后一个属性
    //   2. CRC32计算范围是从消息头开始到FINGERPRINT属性之前（即不包含FINGERPRINT属性本身）
    //   3. 计算完成后，结果需要与 STUN_FINGERPRINT_XOR_VALUE(0x5354554e) 进行异或，再与报文中的值比较
    //
    // fingerprint_attr_size = 属性头(4字节) + 属性值(4字节) = 8字节
    // data + len 指向报文末尾
    // data + len - fingerprint_attr_size 定位到 FINGERPRINT 属性的起始位置
    // len - fingerprint_attr_size 是 CRC32 计算的数据长度（排除整个FINGERPRINT属性）
    return (fingerprint ^ STUN_FINGERPRINT_XOR_VALUE) == 
        rtc::ComputeCrc32(data, len - fingerprint_attr_size);
}

bool StunMessage::read(rtc::ByteBufferReader* buf) {
    // 1. 读 type (2字节)
    if (!buf->ReadUInt16(&_type)) return false;

    // 2. 排除 RTP/RTCP (前2位=10 的不是 STUN 包)
    // 00 1000 0000 0000
    if (_type & 0x800) return false;

    // 3. 读 length (2字节)
    if (!buf->ReadUInt16(&_length)) return false;

    // 4. 读 magic cookie (4字节)
    std::string magic_cookie;
    if (!buf->ReadString(&magic_cookie, k_stun_magic_cookie_length)) {
        return false;
    }

    // 5. 读 transaction id
    std::string transaction_id;
    if (!buf->ReadString(&transaction_id, k_stun_transaction_id_length)) {
        return false;
    }

    // 6. 兼容经典 STUN： magic cookie 不等于 0x2112A442时
    //    把 magic_cookie 的4字节并入 transaction_id（经典STUN的transaction id是128 bits）
    uint32_t magic_cookie_int;
    memcpy(&magic_cookie_int, magic_cookie.data(), sizeof(magic_cookie_int));
    if (rtc::NetworkToHost32(magic_cookie_int) != k_stun_magic_cookie) {
        transaction_id.insert(0, magic_cookie);
    }
    _transaction_id = transaction_id;

    // 7. 头部读完后，buf 剩余的数据就是value，其长度应该等于解析出来的长度 _length (属性总长度)
    if (buf->Length() != _length) return false;

    _attrs.resize(0);
    while (buf->Length() > 0) {
        // 每个属性都是TLV格式， T->type，L->length，V-> value(属性值)
        uint16_t attr_type, attr_length;
        if (!buf->ReadUInt16(&attr_type)) return false;
        if (!buf->ReadUInt16(&attr_length)) return false;
        
        // 工厂方法创建属性 -- 目前返回 nullptr （后续 commit 才填充）
        std::unique_ptr<StunAttribute> attr = _create_attribute(attr_type, attr_length);
        if (!attr) { // 不认识的属性
            // 4 字节对齐()
            if (attr_length % 4 != 0) {
                attr_length += (4 - (attr_length % 4));
            }
            if (!buf->Consume(attr_length)) return false;
        } else {
            if (!attr->read(buf)) return false;
            _attrs.push_back(std::move(attr));
        }
    }
    return true;
}

// 工厂模式 暂时未实现
std::unique_ptr<StunAttribute> StunMessage::_create_attribute(uint16_t attr_type, uint16_t attr_length) {
    return nullptr;
}


} // namespace xrtc
