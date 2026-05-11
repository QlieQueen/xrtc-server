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


} // namespace xrtc
