#include "stream/push_stream.h"

#include "pc/session_description.h"

#include <rtc_base/logging.h>

namespace xrtc {


PushStream::PushStream(EventLoop* el, uint64_t uid, const std::string& stream_name,
    bool audio, bool video, uint32_t log_id) :
    RtcStream(el, uid, stream_name, audio, video, log_id)
{

}

PushStream::~PushStream() {

}

std::string PushStream::create_offer() {
    SessionDescription offer(SdpType::k_offer);
    bool use_rtp_mux = true;

    if (_audio) {
        auto audio = std::make_shared<AudioContentDescription>();
        audio->set_direction(RtpDirection::k_recv_only);
        audio->set_rtcp_mux(use_rtp_mux);
        offer.add_content(audio);
    }

    if (_video) {
        auto video = std::make_shared<VideoContentDescription>();
        video->set_direction(RtpDirection::k_recv_only);
        video->set_rtcp_mux(use_rtp_mux);
        offer.add_content(video);
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

