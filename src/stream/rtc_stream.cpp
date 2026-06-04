#include "stream/rtc_stream.h"

#include <rtc_base/logging.h>

#include "base/event_loop.h"

namespace xrtc {

RtcStream::RtcStream(EventLoop* el, PortAllocator* allocator, uint64_t uid, const std::string& stream_name,
        bool audio, bool video, uint32_t log_id) :
        _el(el), _uid(uid), _stream_name(stream_name),
        _audio(audio), _video(video), _log_id(log_id),
        _pc(new PeerConnection(_el, allocator))
{
    _pc->signal_connection_state.connect(this, &RtcStream::_on_connection_state);
}

// 订阅 PC 状态信号, 打日志; commit 5 将在 k_failed 时触发资源清理
void RtcStream::_on_connection_state(PeerConnection*, PeerConnectionState state) {
    if (_state == state) {
        return;
    }

    RTC_LOG(LS_INFO) << "PeerConnectionState change from " << _state
        << " to " << state;
    _state = state;
}

RtcStream::~RtcStream() {
    if (_pc) {
        delete _pc;
        _pc = nullptr;
    }

}

void RtcStream::start(rtc::RTCCertificate* certificate) {
    _pc->init(certificate);
}

int RtcStream::set_remote_sdp(const std::string& sdp) {
    return _pc->set_remote_sdp(sdp);
}

} // namespace xrtc
