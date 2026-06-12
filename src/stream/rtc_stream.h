#ifndef __RTC_STREAM_H_
#define __RTC_STREAM_H_

#include <memory>
#include <string>
#include <stdint.h>

#include <rtc_base/rtc_certificate.h>
#include <rtc_base/third_party/sigslot/sigslot.h>

#include "pc/peer_connection.h"
#include "ice/port_allocator.h"

namespace xrtc {

class EventLoop;
class RtcStream;

enum class RtcStreamType {
    k_push,
    k_pull
};

class RtcStreamListener {
public:
    virtual void on_connection_state(RtcStream* stream, PeerConnectionState state) = 0;
    virtual void on_rtp_packet_received(RtcStream* stream, const char* data, size_t len) = 0;
    virtual void on_rtcp_packet_received(RtcStream* stream, const char* data, size_t len) = 0;
};


class RtcStream : public sigslot::has_slots<> {
public:
    RtcStream(EventLoop* el, PortAllocator* allocator, uint64_t uid,
        const std::string& stream_name, bool audio, bool video, uint32_t log_id);

    virtual ~RtcStream();

    void start(rtc::RTCCertificate* certificate);

    virtual std::string create_offer() = 0;
    virtual RtcStreamType stream_type() = 0;
    int set_remote_sdp(const std::string& sdp);
    void register_listener(RtcStreamListener* listener) { _listener = listener; }

    uint64_t get_uid() { return _uid; }
    std::string get_stream_name() { return _stream_name; }

    int send_rtp(const char* data, size_t len);

    std::string to_string();

private:
    void _on_connection_state(PeerConnection*, PeerConnectionState state);
    void _on_rtp_packet_received(PeerConnection*,
        rtc::CopyOnWriteBuffer* packet, int64_t /*ts*/);
    void _on_rtcp_packet_received(PeerConnection*,
        rtc::CopyOnWriteBuffer* packet, int64_t /*ts*/);

protected:
    EventLoop* _el;
    uint64_t _uid;
    std::string _stream_name;
    bool _audio;
    bool _video;
    uint32_t _log_id;

    PeerConnection* _pc;
    PeerConnectionState _state = PeerConnectionState::k_new;
    RtcStreamListener* _listener = nullptr;
};


} // namespace xrtc

#endif // __RTC_STREAM_H_