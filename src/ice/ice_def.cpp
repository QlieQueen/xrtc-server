#include "ice/ice_def.h"

namespace xrtc {

const int ICE_UFRAG_LENGTH = 4;
const int ICE_PWD_LENGTH = 24;

// 单个 STUN 包大小 (60 bytes * 8 = 480 bits)，带宽 10kbps(weak) / 1kbps(strong)
const int STUN_PACKET_SIZE = 60 * 8;
const int WEAK_PING_INTERVAL = 1000 * STUN_PACKET_SIZE / 10000; // 48ms
const int STRONG_PING_INTERVAL = 1000 * STUN_PACKET_SIZE / 1000; // 480ms


} // namespace xrtc
