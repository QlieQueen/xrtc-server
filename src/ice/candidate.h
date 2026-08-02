#ifndef __ICE_CANDIDATE_H_
#define __ICE_CANDIDATE_H_

#include <string>
#include <stdint.h>

#include <rtc_base/socket_address.h>

#include "ice/ice_def.h"

namespace xrtc {

class Candidate {
public:
    uint32_t get_priority(uint32_t type_preference,
        int network_adapter_preference,
        int relay_preference);
    
    std::string to_string() const;

public:
    // //a=candidate:1975680953 1 udp 2113937151 <your_public_ip> 10028 typ host
    std::string foundation;
    IceCandidateComponent component;
    std::string protocol;    // UDP
    uint32_t priority;
    rtc::SocketAddress address;
    uint16_t port = 0;
    std::string username;
    std::string password;
    std::string type;        // host
};

} // namespace xrtc

#endif // __ICE_CANDIDATE_H_