#include "stream/push_stream.h"

#include "pc/session_description.h"
#include "ice/ice_credentials.h"
#include "ice/port_allocator.h"

#include <rtc_base/logging.h>

namespace xrtc {


PushStream::PushStream(EventLoop* el, PortAllocator* alloctor, uint64_t uid, const std::string& stream_name,
    bool audio, bool video, uint32_t log_id) :
    RtcStream(el, alloctor, uid, stream_name, audio, video, log_id)
{

}

PushStream::~PushStream() {
    for (auto port : _ports) {
        delete port;
    }
    _ports.clear();
}

std::string PushStream::create_offer() {
    SessionDescription offer(SdpType::k_offer);
    bool use_rtp_mux = true;

    IceParameters ice_params = IceCredentials::create_random_ice_credentials();

    // --- 创建UDP端口，生成Candidate ---
    std::vector<Candidate> candidates;
    auto networks = _allocator->get_networks();
    for (auto network : networks) {
        auto* port = new UDPPort(_el, "audio", IceCandidateComponent::RTP, ice_params);
        Candidate c;
        int ret = port->create_ice_candidate(network, _allocator->min_port(),
            _allocator->max_port(), c);
        if (ret != 0) {
            RTC_LOG(LS_WARNING) << "create ice candidate failed, network: "
                << network->to_string();
            delete port;
            continue;
        }
        _ports.push_back(port);
        candidates.push_back(c);
    }

    if (_audio) {
        auto audio = std::make_shared<AudioContentDescription>();
        audio->set_direction(RtpDirection::k_recv_only);
        audio->set_rtcp_mux(use_rtp_mux);
        audio->add_candidates(candidates);
        offer.add_content(audio);
        offer.add_transport_info(audio->mid(), ice_params, _certificate);
    }

    if (_video) {
        auto video = std::make_shared<VideoContentDescription>();
        video->set_direction(RtpDirection::k_recv_only);
        video->set_rtcp_mux(use_rtp_mux);
        video->add_candidates(candidates);
        offer.add_content(video);
        offer.add_transport_info(video->mid(), ice_params, _certificate);
    }

    if (use_rtp_mux) {
        ContentGroup bundle_group("BUNDLE");
        for (auto& content : offer.contents()) {
            bundle_group.add_content_name(content->mid());
        }
    }

    return offer.to_string();
}

} // namespace xrtc

