#ifndef __STREAM_RTC_STREAM_MANAGER_H_
#define __STREAM_RTC_STREAM_MANAGER_H_

#include <stdint.h>
#include <string>
#include <unordered_map>

#include <rtc_base/rtc_certificate.h>

#include "ice/port_allocator.h"

namespace xrtc {

class EventLoop;
class PushStream;

class RtcStreamManager {
public:
    RtcStreamManager(EventLoop* el);
    ~RtcStreamManager();

    int create_push_stream(uint64_t uid, const std::string& stream_name, 
        bool audio, bool video, uint32_t log_id,
        rtc::RTCCertificate* certificate,
        std::string& offer);

    int create_pull_stream(uint64_t uid, const std::string& stream_name, 
        bool audio, bool video, uint32_t log_id,
        rtc::RTCCertificate* certificate,
        std::string& offer);

private:
    EventLoop* _el = nullptr;
    std::unordered_map<std::string, PushStream*> _push_streams;
    std::unique_ptr<PortAllocator> _allocator;
};

} // namespace xrtc

#endif // __STREAM_RTC_STREAM_MANAGER_H_