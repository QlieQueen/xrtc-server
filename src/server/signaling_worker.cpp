#include "server/signaling_worker.h"

#include <errno.h>
#include <unistd.h>

#include <rtc_base/logging.h>

#include "base/event_loop.h"

namespace xrtc {

//typedef void (*io_cb_t)(EventLoop* el, IOWatcher* w, int fd, int events, void*data);
static void signaling_worker_recv_nofify(EventLoop* el, IOWatcher* w, int fd, int events, void* data)
{
    int msg;
    if (sizeof(int) != read(fd, (void*)&msg, sizeof(int))) {
        RTC_LOG(LS_WARNING) << "read from pipe error: " << errno
            << ", errmsg: " << strerror(errno);
        return;
    }
}

SignalingWorker::SignalingWorker(int worker_id, SignalingServerOptions options) :
    _worker_id(worker_id),
    _options(options),
    _el(new EventLoop(this))
{

}

SignalingWorker::~SignalingWorker() {

}

int SignalingWorker::init() {
    int pipefd[2];
    if (pipe(pipefd)) {
        RTC_LOG(LS_WARNING) << "pipe failed, errmsg: " << strerror(errno);
        return -1;
    }

    _notify_recv_fd = pipefd[0];
    _notify_send_fd = pipefd[1];

    _pipe_watcher = _el->create_io_event(signaling_worker_recv_nofify, this);
    _el->start_io_event(_pipe_watcher, _notify_recv_fd, EventLoop::READ);

    return 0;
}

bool SignalingWorker::start() {
    if (_thread) {
        RTC_LOG(LS_WARNING) << "signaling worker thread already start, worker_id: " << _worker_id;
        return false;
    }

    _thread = new std::thread([=]() {
        RTC_LOG(LS_INFO) << "[SignalingWorker::start] signaling worker event loop start, worker_id: " << _worker_id;
        _el->start();
        RTC_LOG(LS_INFO) << "[SignalingWorker::start] signaling worker event loop stop, worker_id: " << _worker_id;
    });

    return true;
}

void SignalingWorker::stop() {
    notify(SignalingWorker::QUIT);
}

int SignalingWorker::notify(int msg) {
    int written = write(_notify_send_fd, (void*)&msg, sizeof(int));
    return written == sizeof(int) ? 0 : -1;
}

void SignalingWorker::join() {
    if (_thread && _thread->joinable()) {
        _thread->join();
    }
}

int SignalingWorker::notify_new_conn(int fd) {

    return 0;
}



} // namespace xrtc