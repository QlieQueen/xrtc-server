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

    int init(rtc::RTCCertificate* certificate);
    void destroy();

    std::string create_offer(const RTCOfferAnswerOptions& options);
    int set_remote_sdp(const std::string& sdp);

public:
    sigslot::signal2<PeerConnection*, PeerConnectionState> signal_connection_state;

private:
    // 延迟析构: 禁止外部直接 delete pc, 必须通过 destroy() → timer → delete pc
    // 否则在 ICE timer 回调链中析构 PC 会导致 re-entrant destruction coredump
    // (timer 回调还在 _on_check_and_ping 调用栈中, this 已被析构)
    friend void destroy_timer_cb(EventLoop* el, TimerWatcher* w, void* data);
    ~PeerConnection();
    void _on_candidate_allocate_done(TransportController*,
            const std::string& transport_name,
            IceCandidateComponent component,
            const std::vector<Candidate>& candidates);
    void _on_connection_state(TransportController*, PeerConnectionState state);

private:
    EventLoop* _el;
    std::unique_ptr<SessionDescription> _local_desc;
    std::unique_ptr<SessionDescription> _remote_desc;
    rtc::RTCCertificate* _certificate = nullptr;
    std::unique_ptr<TransportController> _transport_controller;
    TimerWatcher* _destroy_timer = nullptr;
};

} // namespace xrtc

#endif // __PEER_CONNECTION_H_