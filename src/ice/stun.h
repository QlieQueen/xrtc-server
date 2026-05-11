#ifndef __ICE_STUN_H_
#define __ICE_STUN_H_

#include <string>
#include <vector>
#include <memory>
#include <stdint.h>

namespace xrtc {
/*
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
*/
const size_t k_stun_head_size = 20;
const size_t k_stun_attribute_header_size = 4; // 属性中 type + length的长度
const size_t k_stun_transaction_id_offset = 8; // Transaction ID的偏移量
const uint32_t k_stun_magic_cookie = 0x2112A442; // magic cookie的固定取值 0x2112A442
const size_t k_stun_magic_cookie_length = sizeof(k_stun_magic_cookie); // magic cookie的长度

enum StunAttributeValue {
    STUN_ATTR_FINGERPRINT = 0x8028,
};

class StunMessage {
public:
    StunMessage();
    ~StunMessage();

    static bool validate_fingerprint(const char* data, size_t len);

private:
    uint16_t _type;
    uint16_t _length;
    std::string _transaction_id;
    std::vector<std::unique_ptr<StunAttribute>> _attrs;

};

class StunAttribute {

};

class StunUint32Attribute : public StunAttribute {
public:
    static const size_t SIZE = 4;
};

} // namespace xrtc

#endif // __ICE_STUN_H_
