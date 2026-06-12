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

    SessionDescription* remote_desc() { return _remote_desc.get(); }
    SessionDescription* local_desc() { return _local_desc.get(); }

    void add_audio_source(const std::vector<StreamParams>& source) {
        _audio_source = source;
    }

    void add_video_source(const std::vector<StreamParams>& source) {
        _video_source = source;
    }

    int send_rtp(const char* data, size_t len);

public:
    sigslot::signal2<PeerConnection*, PeerConnectionState> signal_connection_state;
    sigslot::signal3<PeerConnection*, rtc::CopyOnWriteBuffer*, int64_t>
        signal_rtp_packet_received;
    sigslot::signal3<PeerConnection*, rtc::CopyOnWriteBuffer*, int64_t>
        signal_rtcp_packet_received;

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
    void _on_rtp_packet_received(TransportController*,
            rtc::CopyOnWriteBuffer* buffer, int64_t ts);
    void _on_rtcp_packet_received(TransportController*,
            rtc::CopyOnWriteBuffer* buffer, int64_t ts);

private:
    EventLoop* _el;
    std::unique_ptr<SessionDescription> _local_desc;
    std::unique_ptr<SessionDescription> _remote_desc;
    rtc::RTCCertificate* _certificate = nullptr;
    std::unique_ptr<TransportController> _transport_controller;
    TimerWatcher* _destroy_timer = nullptr;
    std::vector<StreamParams> _audio_source;
    std::vector<StreamParams> _video_source;
};

} // namespace xrtc

#endif // __PEER_CONNECTION_H_