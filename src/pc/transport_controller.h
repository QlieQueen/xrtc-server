#ifndef __TRANSPORT_CONTROLLER_H_
#define __TRANSPORT_CONTROLLER_H_

#include <rtc_base/third_party/sigslot/sigslot.h>

#include "base/event_loop.h"
#include "ice/ice_agent.h"
#include "ice/candidate.h"
#include "ice/port_allocator.h"
#include "pc/session_description.h"

namespace xrtc {

class TransportController : public sigslot::has_slots<> {
public:
    TransportController(EventLoop* el, PortAllocator* allocator);
    ~TransportController();

    int set_local_description(SessionDescription* desc);
    void set_local_certificate(rtc::RTCCertificate* cert); // 先存着，DTLS握手时使用

public:
    // 信号转发： IceAgent -> TransportController -> PeerConnection
    sigslot::signal4<TransportController*, const std::string&, IceCandidateComponent,
        const std::vector<Candidate>&> signal_candidate_allocate_done;
    
private:
    void _on_candidate_allocator_done(IceAgent* agent,
            const std::string& transport_name,
            IceCandidateComponent component,
            const std::vector<Candidate>& candidates);
private:
    EventLoop* _el;
    IceAgent* _ice_agent;
    rtc::RTCCertificate* _local_certificate = nullptr;
};



} // namespace xrtc

#endif // __TRANSPORT_CONTROLLER_H_
