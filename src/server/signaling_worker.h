#ifndef __SERVER_SIGNALING_WORKER_H_
#define __SERVER_SIGNALING_WORKER_H_

#include <vector>

#include <rtc_base/slice.h>

#include "base/json.hpp"
#include "base/lock_free_queue.h"
#include "server/signaling_server.h"

using json = nlohmann::json;

namespace xrtc {

class TcpConnection;

class SignalingWorker {
public:
    enum {
        QUIT = 0,
        NEW_CONN = 1
    };
    SignalingWorker(int worker_id, SignalingServerOptions options);
    ~SignalingWorker();

    int init();
    bool start();
    void stop();
    int notify(int msg);
    void join();
    int notify_new_conn(int fd);

    friend void signaling_worker_recv_nofify(EventLoop* el, IOWatcher* w, int fd, int events, void* data);
    friend void conn_io_cb(EventLoop* /*el*/, IOWatcher* /*w*/, int fd, int events, void* data);

private:
    void _process_notify(int msg);
    void _new_conn(int fd);
    void _close_conn(TcpConnection* c);
    void _remove_conn(TcpConnection* c);
    void _read_query(int fd);
    void _write_query(int fd);
    int _process_query_buffer(TcpConnection* c);
    int _process_request(TcpConnection* c,
        const rtc::Slice& header,
        const rtc::Slice& body);
    int _process_push(int cmdno, TcpConnection* c,
        const json& root, uint32_t log_id);

private:
    int _worker_id;
    SignalingServerOptions _options;
    EventLoop* _el;
    IOWatcher* _pipe_watcher = nullptr;
    int _notify_recv_fd = -1;
    int _notify_send_fd = -1;

    std::thread* _thread = nullptr;
    LockFreeQueue<int> _q_connfd;
    std::vector<TcpConnection*> _tcp_conns;
};

}


#endif // __SERVER_SIGNALING_WORKER_H_