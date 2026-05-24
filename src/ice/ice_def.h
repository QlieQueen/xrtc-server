#ifndef __ICE_DEF_H_
#define __ICE_DEF_H_

namespace xrtc {

#define LOCAL_PORT_TYPE  "host"
#define PRFLX_PORT_TYPE  "prflx"

extern const int ICE_UFRAG_LENGTH;
extern const int ICE_PWD_LENGTH;

// ICE ping 间隔 (ms)，由 STUN packet size 和目标带宽计算
//   WEAK_PING_INTERVAL   = 1000 * 480bit / 10000bps = 48ms  (channel weak → 加速探测)
//   STRONG_PING_INTERVAL = 1000 * 480bit / 1000bps  = 480ms (channel strong → 节省带宽)
extern const int WEAK_PING_INTERVAL;
extern const int STRONG_PING_INTERVAL;
extern const int STABLING_CONNECTION_PING_INTERVAL; // 900ms
extern const int STABLE_CONNECTION_PING_INTERVAL; // 2500m
extern const int MIN_PINGS_AT_WEAK_PING_INTERVAL;
extern const int WEAK_CONNECTION_RECEIVE_TIMEOUT;

enum IceCandidateComponent {
    RTP = 1,
    RTCP = 2
};

// ICE candidate 类型优先级 (RFC 5245 §4.1.2.1): 类型优先值占 priority 的高 8 位
// 值越高优先级越高: host > prflx > srflx > relay
enum IcePriorityValue {
    ICE_TYPE_PREFERENCE_RELAY_UDP = 2,
    ICE_TYPE_PREFERENCE_SRFLX     = 100,
    ICE_TYPE_PREFERENCE_PRFLX     = 110,
    ICE_TYPE_PREFERENCE_HOST      = 126,
};

} // namespace xrtc

#endif // __ICE_DEF_H_