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

} // namespace xrtc

