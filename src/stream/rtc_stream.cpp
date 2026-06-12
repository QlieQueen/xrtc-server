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
    _pc->signal_rtp_packet_received.connect(this, &RtcStream::_on_rtp_packet_received);
    _pc->signal_rtcp_packet_received.connect(this, &RtcStream::_on_rtcp_packet_received);
}

// 订阅 PC 状态信号, 打日志; commit 5 将在 k_failed 时触发资源清理
void RtcStream::_on_connection_state(PeerConnection*, PeerConnectionState state) {
    if (_state == state) {
        return;
    }

    RTC_LOG(LS_INFO) << to_string() << ": PeerConnectionState change from " << _state
        << " to " << state;
    _state = state;

    if (_listener) {
        _listener->on_connection_state(this, state);
    }

}

void RtcStream::_on_rtp_packet_received(PeerConnection*,
    rtc::CopyOnWriteBuffer* packet, int64_t /*ts*/)
{
    if (_listener) {
        _listener->on_rtp_packet_received(this, (const char*)packet->data(), packet->size());
    }
}

void RtcStream::_on_rtcp_packet_received(PeerConnection*,
    rtc::CopyOnWriteBuffer* packet, int64_t /*ts*/)
{
    if (_listener) {
        _listener->on_rtcp_packet_received(this, (const char*)packet->data(), packet->size());
    }
}

// 不直接 delete _pc: ~PeerConnection() 是 private 的, 必须走 destroy()
// destroy() 将 delete pc 延迟到 10ms timer 中执行, 避免在 ICE/DTLS timer
// 回调链中 re-entrant destruction 导致 coredump (crash 路径见 peer_connection.cpp)
RtcStream::~RtcStream() {
    _pc->destroy();
}

void RtcStream::start(rtc::RTCCertificate* certificate) {
    _pc->init(certificate);
}

int RtcStream::set_remote_sdp(const std::string& sdp) {
    return _pc->set_remote_sdp(sdp);
}

int RtcStream::send_rtp(const char* data, size_t len) {
    return _pc ? _pc->send_rtp(data, len) : -1;
}

std::string RtcStream::to_string() {
    std::stringstream ss;
    ss << "Stream[" << this << "|" << _uid << "|" << _stream_name << "]";
    return ss.str();
}



} // namespace xrtc
