#include "stream/rtc_stream_manager.h"

#include "base/event_loop.h"
#include "stream/push_stream.h"

namespace xrtc {

RtcStreamManager::RtcStreamManager(EventLoop* el) : _el(el) {

}

RtcStreamManager::~RtcStreamManager() {
}

int RtcStreamManager::create_push_stream(uint64_t uid, const std::string& stream_name, 
    bool audio, bool video, uint32_t log_id,
    rtc::RTCCertificate* certificate,
    std::string& offer)
{
    PushStream* stream = new PushStream(_el, uid, stream_name, audio, video, log_id);
    offer = stream->create_offer();    

    return 0;
}

int RtcStreamManager::create_pull_stream(uint64_t uid, const std::string& stream_name, 
    bool audio, bool video, uint32_t log_id,
    rtc::RTCCertificate* certificate,
    std::string& offer)
{
    return 0;
}



} // namespace xrtc