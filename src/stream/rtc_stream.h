#ifndef __RTC_STREAM_H_
#define __RTC_STREAM_H_

#include <string>
#include <stdint.h>

#include <rtc_base/rtc_certificate.h>

#include "pc/peer_connection.h"
#include "ice/port_allocator.h"

namespace xrtc {

class EventLoop;

class RtcStream {
public:
    RtcStream(EventLoop* el, PortAllocator* allocator, uint64_t uid,
        const std::string& stream_name, bool audio, bool video, uint32_t log_id);

    virtual ~RtcStream();

    void start(rtc::RTCCertificate* certificate);

    virtual std::string create_offer() = 0;
    int set_remote_sdp(const std::string& sdp);
    uint64_t get_uid() { return _uid; }
    std::string get_stream_name() { return _stream_name; }

protected:
    EventLoop* _el;
    uint64_t _uid;
    std::string _stream_name;
    bool _audio;
    bool _video;
    uint32_t _log_id;
    PeerConnection* _pc;
};


} // namespace xrtc

#endif // __RTC_STREAM_H_