#ifndef __ICE_CANDIDATE_H_
#define __ICE_CANDIDATE_H_

#include <string>
#include <stdint.h>

namespace xrtc {

struct Candidate {
    std::string foundation;
    int component_id = 1;
    std::string protocol;
    uint32_t priority;
    std::string address;
    uint16_t port;
    std::string type;
};

} // namespace xrtc

#endif // __ICE_CANDIDATE_H_