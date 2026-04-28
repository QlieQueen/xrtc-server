#ifndef __SERVER_RTC_SERVER_H_
#define __SERVER_RTC_SERVER_H_

#include <vector>
#include <thread>
#include <queue>
#include <memory>
#include <mutex>

#include "xrtc_server_def.h"
#include "base/event_loop.h"

namespace xrtc {

class RtcWorker;

struct RtcServerOptions {
    int  worker_num;
};

class RtcServer {
public:
    enum {
        QUIT = 0,
        RTC_MSG = 1
    };
public:
    RtcServer();
    ~RtcServer();

    int init(const char* conf_file);
    bool start();
    void stop();
    int notify(int msg);
    void join();
    void push_msg(std::shared_ptr<RtcMsg> msg);
    std::shared_ptr<RtcMsg> pop_msg();
    int send_rtc_msg(std::shared_ptr<RtcMsg> msg);

    friend void rtc_server_recv_notify(EventLoop* el, IOWatcher* w,
        int fd, int events, void* data);
    
private:
    void _create_worker(int index); 
    void _process_notify(int msg);
    void _stop();
    void _process_rtc_msg();

private:
    EventLoop* _el;
    RtcServerOptions _options;
    std::thread* _thread = nullptr;

    IOWatcher* _pipe_watcher = nullptr;
    int _notify_recv_fd = -1;
    int _notify_send_fd = -1;
    
    std::queue<std::shared_ptr<RtcMsg>> _q_msg;
    std::mutex _q_mutex;

    std::vector<RtcWorker*> _workers;
    size_t _next_worker_index = 0;
};



} // namespace xrtc

#endif // __SERVER_RTC_SERVER_H_