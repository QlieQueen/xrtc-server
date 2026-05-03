#ifndef __RTC_STREAM_H_
#define __RTC_STREAM_H_

#include <string>
#include <stdint.h>

namespace xrtc {

class EventLoop;

class RtcStream {
public:
    RtcStream(EventLoop* el, uint64_t uid,
        const std::string& stream_name, bool audio, bool video, uint32_t log_id);
    virtual ~RtcStream();
    virtual std::string create_offer() = 0;
    uint64_t get_uid() { return _uid; }
    std::string get_stream_name() { return _stream_name; }

protected:
    EventLoop* _el;
    uint64_t _uid;
    std::string _stream_name;
    bool _audio;
    bool _video;
    uint32_t _log_id;
};


} // namespace xrtc

#endif // __RTC_STREAM_H_