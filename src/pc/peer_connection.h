#ifndef __PEER_CONNECTION_H_
#define __PEER_CONNECTION_H_

#include <memory>
#include <vector>
#include <string>
#include <rtc_base/rtc_certificate.h>
#include <rtc_base/third_party/sigslot/sigslot.h>

#include "base/event_loop.h"
#include "ice/port_allocator.h"
#include "pc/session_description.h"
#include "pc/peer_connection_def.h"
#include "pc/transport_controller.h"

namespace xrtc {

class PeerConnection : public sigslot::has_slots<> {
public:
    PeerConnection(EventLoop* _el, PortAllocator* allocator);
    ~PeerConnection();

    int init(rtc::RTCCertificate* certificate);
    std::string create_offer(const RTCOfferAnswerOptions& options);
    int set_remote_sdp(const std::string& sdp);

private:
    void _on_candidate_allocate_done(TransportController*,
            const std::string& transport_name,
            IceCandidateComponent component,
            const std::vector<Candidate>& candidates);

private:
    EventLoop* _el;
    std::unique_ptr<SessionDescription> _local_desc;
    std::unique_ptr<SessionDescription> _remote_desc;
    rtc::RTCCertificate* _certificate = nullptr;
    std::unique_ptr<TransportController> _transport_controller;
};

} // namespace xrtc

#endif // __PEER_CONNECTION_H_