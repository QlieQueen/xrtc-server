#include "stream/rtc_stream_manager.h"

#include "base/conf.h"
#include "base/event_loop.h"
#include "stream/push_stream.h"

extern xrtc::GeneralConf* g_conf;

namespace xrtc {

RtcStreamManager::RtcStreamManager(EventLoop* el) :
    _el(el),
    _allocator(new PortAllocator())
{
    if (g_conf) {
        _allocator->set_port_range(g_conf->ice_min_port, g_conf->ice_max_port);
    }
}

RtcStreamManager::~RtcStreamManager() {
}

int RtcStreamManager::create_push_stream(uint64_t uid, const std::string& stream_name, 
    bool audio, bool video, uint32_t log_id,
    rtc::RTCCertificate* certificate,
    std::string& offer)
{
    PushStream* stream = new PushStream(_el, _allocator.get(), uid, stream_name, audio, video, log_id);
    stream->start(certificate);
    offer = stream->create_offer();

    _push_streams[stream_name] = stream;

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