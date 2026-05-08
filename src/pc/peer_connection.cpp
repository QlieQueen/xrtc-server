#include "pc/peer_connection.h"

#include <rtc_base/logging.h>
#include <rtc_base/third_party/sigslot/sigslot.h>

#include "pc/peer_connection_def.h"

namespace xrtc {

PeerConnection::PeerConnection(EventLoop* el, PortAllocator* allocator) : 
    _el(el),
    _transport_controller(new TransportController(el, allocator))
{
    _transport_controller->signal_candidate_allocate_done.connect(this,
        &PeerConnection::_on_candidate_allocate_done);
}

PeerConnection::~PeerConnection() {
}

int PeerConnection::init(rtc::RTCCertificate* certificate) {
    _certificate = certificate;
    _transport_controller->set_local_certificate(certificate);
    return 0;
}

// create_offer() - 构造SDP -> 调 TransportController -> 返回 SDP
std::string PeerConnection::create_offer(const RTCOfferAnswerOptions& options) {
    if (options.dtls_on && !_certificate) {
        RTC_LOG(LS_WARNING) << "certificate is null";
        return "";
    }

    _local_desc = std::make_unique<SessionDescription>(SdpType::k_offer);

    IceParameters ice_params = IceCredentials::create_random_ice_credentials();

    if (options.recv_audio) {
        auto audio = std::make_shared<AudioContentDescription>();
        audio->set_direction(RtpDirection::k_recv_only);
        audio->set_rtcp_mux(options.use_rtp_mux);
        _local_desc->add_content(audio);
        _local_desc->add_transport_info(audio->mid(), ice_params, _certificate);
    } 

    if (options.recv_video) {
        auto video = std::make_shared<VideoContentDescription>();
        video->set_direction(RtpDirection::k_recv_only);
        video->set_rtcp_mux(options.use_rtp_mux);
        _local_desc->add_content(video);
        _local_desc->add_transport_info(video->mid(), ice_params, _certificate);
    } 

    if (options.use_rtp_mux) {
        ContentGroup bundle_group("BUNDLE");
        for (auto& content : _local_desc->contents()) {
            bundle_group.add_content_name(content->mid());
        }

        if (!bundle_group.content_names().empty()) {
            _local_desc->add_group(bundle_group);
        }
    }

    _transport_controller->set_local_description(_local_desc.get());

    return _local_desc->to_string();
}

// 信号回调 -- 把 candidate填入到 _local-desc

void PeerConnection::_on_candidate_allocate_done(TransportController*,
        const std::string& transport_name,
        IceCandidateComponent component,
        const std::vector<Candidate>& candidates)
{
    if (!_local_desc) {
        return;
    }

    auto content = _local_desc->get_content(transport_name);
    if (content) {
        content->add_candidates(candidates);
    }
}

} // namespace xrtc
