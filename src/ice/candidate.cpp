#include "ice/candidate.h"

#include <sstream>

namespace xrtc {

/*
priority = (2^24)*(type preference) +
           (2^8)*(local preference) +
           (2^0)*(256 - component ID)
*/
//Local preference: 2 个字节
//0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
//+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//| NIC Pref | Addr Pref |
//+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//NIC type：3G/wifi
//Addr pref: 定义在 RFC3484

uint32_t Candidate::get_priority(uint32_t type_preference,
    int network_adapter_preference,
    int relay_preference)
{
    int addr_ref = rtc::IPAddressPrecedence(address.ipaddr()) + relay_preference;
    int local_pref = (network_adapter_preference << 8 | addr_ref);
    return (type_preference << 24) | (local_pref << 8) | (256 - (int)component);
}

std::string Candidate::to_string() const {
    std::stringstream ss;
    ss << "Candidate[" << foundation << ":" << (int)component << ":" << protocol
       << ":" << priority << ":" << address.ToString() << ":" << type 
       << ":" << username << ":" << password << "]";
    return ss.str();
}



} // namespace xrtc
