#ifndef __SERVER_SIGNALING_SERVER_H_
#define __SERVER_SIGNALING_SERVER_H_

#include <vector>
#include <thread>
#include <string>

#include "base/event_loop.h"

namespace xrtc {

class SignalingWorker;

struct SignalingServerOptions {
    std::string    host = "127.0.0.1";
    int            port = 9000;
    int            worker_num = 2;
    int            connection_timeout = 5000000;
};

class SignalingServer {
public:
    enum {
        QUIT = 0
    };
    SignalingServer();
    ~SignalingServer();

    int init(const char* conf_file);
    bool start();
    void stop();
    int notify(int msg);
    void join();

    friend void signaling_server_recv_nofify(EventLoop* el, IOWatcher*w,
        int fd, int events, void* data);
    
    friend void accept_new_connect(EventLoop* el, IOWatcher* w,
        int fd, int events, void* data);

private:
    void _process_notify(int msg);
    void _stop();
    void _dispatch_new_conn(int fd);
    int _create_worker(int index);

private:
    SignalingServerOptions _options;
    EventLoop* _el;
    IOWatcher* _io_watcher = nullptr;
    IOWatcher* _pipe_watcher = nullptr;
    int _notify_recv_fd = -1;
    int _notify_send_fd = -1;
    std::thread* _thread = nullptr;

    int _listen_fd = -1;
    std::vector<SignalingWorker*> _workers;
    size_t _next_worker_index = 0;
};

} // namespace xrtc

#endif // __SERVER_SIGNALING_SERVER_H_
