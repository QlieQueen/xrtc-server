#include "stream/pull_stream.h"

#include "pc/peer_connection_def.h"
#include "ice/port_allocator.h"

#include <rtc_base/logging.h>

namespace xrtc {


PullStream::PullStream(EventLoop* el, PortAllocator* alloctor, uint64_t uid, const std::string& stream_name,
    bool audio, bool video, uint32_t log_id) :
    RtcStream(el, alloctor, uid, stream_name, audio, video, log_id)
{

}

PullStream::~PullStream() {
    RTC_LOG(LS_INFO) << to_string() << ": Pull stream destroy.";
}

std::string PullStream::create_offer() {
    RTCOfferAnswerOptions options;
    options.recv_audio = false;
    options.recv_video = false;
    options.send_audio = _audio;
    options.send_video = _video;
    options.use_rtp_mux = true;
    options.use_rtcp_mux = true;
    options.dtls_on = true;

    return _pc->create_offer(options);
}

void PullStream::add_audio_source(const std::vector<StreamParams>& source) {
    if (!_pc) {
        return;
    }
    _pc->add_audio_source(source);
}

void PullStream::add_video_source(const std::vector<StreamParams>& source) {
    if (!_pc) {
        return;
    }
    _pc->add_video_source(source);
}

} // namespace xrtc

