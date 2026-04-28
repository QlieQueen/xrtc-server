#ifndef __SERVER_RTC_WORKER_H_
#define __SERVER_RTC_WORKER_H_

#include "server/rtc_server.h"

namespace xrtc {

class RtcWorker {
public:
    enum {
        QUIT = 0,
        RTC_MSG = 1
    };

    RtcWorker(int worker_id, const RtcServerOptions& options) {}

    int init() {
        return 0;        
    }

    bool start() {
        return true;
    }

    void stop() {

    }

    void join() {
        
    }

};

}



#endif // __SERVER_RTC_WORKER_H_
