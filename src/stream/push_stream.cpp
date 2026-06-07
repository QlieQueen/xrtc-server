#include "stream/push_stream.h"

#include "pc/peer_connection_def.h"
#include "ice/port_allocator.h"

#include <rtc_base/logging.h>

namespace xrtc {


PushStream::PushStream(EventLoop* el, PortAllocator* alloctor, uint64_t uid, const std::string& stream_name,
    bool audio, bool video, uint32_t log_id) :
    RtcStream(el, alloctor, uid, stream_name, audio, video, log_id)
{

}

PushStream::~PushStream() {
    RTC_LOG(LS_INFO) << to_string() << ": Push stream destroy.";
}

std::string PushStream::create_offer() {
    RTCOfferAnswerOptions options;
    options.recv_audio = _audio;
    options.recv_video = _video;
    options.send_audio = false;
    options.send_video = false;
    options.use_rtp_mux = true;
    options.use_rtcp_mux = true;
    options.dtls_on = true;

    return _pc->create_offer(options);
}

// 从 remote SDP 提取 audio track（StreamParams），供 PullStream 生成拉流 offer 时透传 SSRC
bool PushStream::get_audio_source(std::vector<StreamParams>& source) {
    return _get_source("audio", source);
}

bool PushStream::get_video_source(std::vector<StreamParams>& source) {
    return _get_source("video", source);
}

bool PushStream::_get_source(const std::string& mid, std::vector<StreamParams>& source) {
    if (!_pc) {
        return false;
    }

    auto remote_desc = _pc->remote_desc();
    if (!remote_desc) {
        return false;
    }

    auto content = remote_desc->get_content(mid);
    if (!content) {
        return false;
    }

    source = content->streams();
    return true;
}

} // namespace xrtc

