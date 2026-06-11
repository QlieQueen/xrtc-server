#include "module/rtp_rtcp/rtp_utils.h"

#include <rtc_base/byte_io.h>

namespace xrtc {

// RTP/RTCP 包头常量 (RFC 3550)
const uint8_t k_rtp_version = 2;
const size_t k_min_rtp_packet_size = 12;   // RTP 固定头 12 字节
const size_t k_min_rtcp_packet_size = 4;   // RTCP 最小 4 字节

// 检查 RTP 版本号 — byte 0 的高 2 位必须 == 2
bool has_correct_rtp_version(rtc::ArrayView<const uint8_t> packet) {
    return (packet[0] >> 6) == k_rtp_version;
}

// RFC 5761 Section 4: RTCP 的 PT 完整 8 bit = 192~223, 取低 7 bit = 64~95
bool payload_type_is_reserved_for_rtcp(uint8_t payload_type) {
    return 64 <= payload_type && payload_type < 96;
}

// RTP 判断: 长度 >= 12 + version == 2 + PT 不在 64~95 范围
bool is_rtp_packet(rtc::ArrayView<const uint8_t> packet) {
    return packet.size() >= k_min_rtp_packet_size &&
        has_correct_rtp_version(packet) &&
        !payload_type_is_reserved_for_rtcp(packet[1] & 0x7F);
}

// RTCP 判断: 长度 >= 4 + PT 在 64~95 范围
bool is_rtcp_packet(rtc::ArrayView<const uint8_t> packet) {
    return packet.size() >= k_min_rtcp_packet_size &&
        payload_type_is_reserved_for_rtcp(packet[1] & 0x7F);
}

// 解复用入口 — 先判 RTP 再判 RTCP，都不匹配返回 k_unknown
RtpPacketType infer_rtp_packet_type(rtc::ArrayView<const char> packet) {
    if (is_rtp_packet(rtc::reinterpret_array_view<const uint8_t>(packet))) {
        return RtpPacketType::k_rtp;
    }

    if (is_rtcp_packet(rtc::reinterpret_array_view<const uint8_t>(packet))) {
        return RtpPacketType::k_rtcp;
    }

    return RtpPacketType::k_unknown;
}

// 读取 RTP header 的 sequence number — 大端序，位于 byte 2-3
uint16_t parse_rtp_sequence_number(const rtc::ArrayView<const uint8_t>& packet) {
    return rtc::ByteReader<uint16_t>::ReadBigEndian(packet.data() + 2);
}

// 读取 RTP header 的 SSRC — 大端序，位于 byte 8-11
uint32_t parse_rtp_ssrc(const rtc::ArrayView<const uint8_t>& packet) {
    return rtc::ByteReader<uint32_t>::ReadBigEndian(packet.data() + 8);
}

} // namespace xrtc