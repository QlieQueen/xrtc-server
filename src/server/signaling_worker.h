#ifndef __SERVER_SIGNALING_WORKER_H_
#define __SERVER_SIGNALING_WORKER_H_

#include "server/signaling_server.h"

namespace xrtc {

class SignalingWorker {
public:
    enum {
        QUIT = 0
    };
    SignalingWorker(int worker_id, SignalingServerOptions options);
    ~SignalingWorker();

    int init();
    bool start();
    void stop();
    int notify(int msg);
    void join();
    int notify_new_conn(int fd);

private:
    int _worker_id;
    SignalingServerOptions _options;
    EventLoop* _el;
    IOWatcher* _pipe_watcher = nullptr;
    int _notify_recv_fd = -1;
    int _notify_send_fd = -1;

    std::thread* _thread = nullptr;
};

}


#endif // __SERVER_SIGNALING_WORKER_H_