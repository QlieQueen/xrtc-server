#ifndef __TRANSPORT_CONTROLLER_H_
#define __TRANSPORT_CONTROLLER_H_

#include <map>
#include <rtc_base/third_party/sigslot/sigslot.h>

#include "base/event_loop.h"
#include "ice/ice_agent.h"
#include "ice/candidate.h"
#include "ice/port_allocator.h"
#include "pc/session_description.h"

namespace xrtc {

class DtlsTransport;

class TransportController : public sigslot::has_slots<> {
public:
    TransportController(EventLoop* el, PortAllocator* allocator);
    ~TransportController();

    int set_local_description(SessionDescription* desc);
    int set_remote_description(SessionDescription* remote_desc);

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
    void _add_dtls_transport(DtlsTransport* dtls_transport);
    DtlsTransport* _get_dtls_transport(const std::string& transport_name);

private:
    EventLoop* _el;
    IceAgent* _ice_agent;
    rtc::RTCCertificate* _local_certificate = nullptr;
    std::map<std::string, DtlsTransport*> _dtls_transport_by_name;
};



} // namespace xrtc

#endif // __TRANSPORT_CONTROLLER_H_
