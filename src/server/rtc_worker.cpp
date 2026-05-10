#include "server/rtc_worker.h"

#include <unistd.h>
#include <rtc_base/logging.h>

#include "server/signaling_worker.h"

namespace xrtc {

//typedef void (*io_cb_t)(EventLoop* el, IOWatcher* w, int fd, int events, void*data);
void rtc_worker_recv_notify(EventLoop* el, IOWatcher* w, int fd, int events, void* data)
{
    int msg;
    if (sizeof(int) != read(fd, (void*)&(msg), sizeof(int))) {
        RTC_LOG(LS_WARNING) << "read from pipe failed, errno: " << errno
            << ", errmsg: " << strerror(errno);
    }

    RtcWorker* worker = (RtcWorker*)data;

    worker->_process_notify(msg);
}


RtcWorker::RtcWorker(int worker_id, const RtcServerOptions& options) :
    _worker_id(worker_id),
    _options(options),
    _el(new EventLoop(this)),
    _rtc_stream_mgr(new RtcStreamManager(_el))
{
}

RtcWorker::~RtcWorker(){
    if (_el) {
        delete _el;
        _el = nullptr;
    }

    if (_thread) {
        delete _thread;
        _thread = nullptr;
    }
}

int RtcWorker::init() {
    int pipefd[2];
    int ret = pipe(pipefd);
    if (ret == -1) {
        RTC_LOG(LS_WARNING) << "[RtcWorker::init] pipe failed, errno: " << errno
                << ", errmsg: " << strerror(errno);
        return -1;
    }

    _notify_recv_fd = pipefd[0];
    _notify_send_fd = pipefd[1];

    _pipe_watcher = _el->create_io_event(rtc_worker_recv_notify, this);
    _el->start_io_event(_pipe_watcher, _notify_recv_fd, EventLoop::READ);

    return 0;
}

bool RtcWorker::start() {
    if (_thread) {
        RTC_LOG(LS_WARNING) << "rtc worker is running, worker_id: " << _worker_id;
        return true;
    }

    _thread = new std::thread([this]() {
        RTC_LOG(LS_INFO) << "rtc worker event loop start, worker_id: " << _worker_id;
        _el->start();
        RTC_LOG(LS_INFO) << "rtc worker event loop stop, worker_id: " << _worker_id;
    });

    return true;
}

void RtcWorker::stop() {
    notify(RtcWorker::QUIT);
}

int RtcWorker::notify(int msg) {
    int written = write(_notify_send_fd, (void*)&msg, sizeof(int));
    return written == sizeof(int) ? 0 : -1;
}

void RtcWorker::join() {
    if (_thread && _thread->joinable()) {
        _thread->join();
    }
}

void RtcWorker::push_msg(std::shared_ptr<RtcMsg> msg) {
    _q_msg.produce(msg);
}

bool RtcWorker::pop_msg(std::shared_ptr<RtcMsg>* msg) {
    return _q_msg.consume(msg);
}

int RtcWorker::send_rtc_msg(std::shared_ptr<RtcMsg> msg) {
    push_msg(msg);
    return notify(RtcWorker::RTC_MSG);
}

void RtcWorker::_stop() {
    if (!_thread) {
        RTC_LOG(LS_WARNING) << "rtc worker not running, worker_id: " << _worker_id;
        return;
    }

    _el->delete_io_event(_pipe_watcher);
    _el->stop();

    close(_notify_recv_fd);
    close(_notify_send_fd);
}

void RtcWorker::_process_rtc_msg() {
    std::shared_ptr<RtcMsg> msg;
    if (!pop_msg(&msg)) {
        return;
    }

    switch (msg->cmdno) {
        case CMDNO_PUSH:
            _process_push(msg);
            break;
        case CMDNO_PULL:
            _process_pull(msg);
            break;
        case CMDNO_STOPPUSH:
            _process_stop_push(msg);
            break;
        case CMDNO_STOPPULL:
            _process_stop_pull(msg);
            break;
        case CMDNO_ANSWER:
            _process_answer(msg);
            break;
        default:
            RTC_LOG(LS_WARNING) << "[RtcWorker::_process_rtc_msg] invalid cmdno";
            break;
    }
}

void RtcWorker::_process_push(std::shared_ptr<RtcMsg>& msg) {
    RTC_LOG(LS_INFO) << "rtc worker process push request, worker_id: " << _worker_id;
    std::string offer;
    int ret = _rtc_stream_mgr->create_push_stream(msg->uid, msg->stream_name,
                                        msg->audio, msg->video,
                                        msg->log_id, (rtc::RTCCertificate*)(msg->certificate), offer);
    if (ret != 0) {
        msg->err_no = -1;
    }
    
    RTC_LOG(INFO) << "offer: " << offer;
    
    msg->sdp = offer;
    
    SignalingWorker* worker = (SignalingWorker*)msg->worker;
    if (worker) {
        worker->send_rtc_msg(msg);
    }

}

void RtcWorker::_process_pull(std::shared_ptr<RtcMsg>& msg) {
    RTC_LOG(LS_INFO) << "rtc worker process pull request, worker_id: " << _worker_id;
}

void RtcWorker::_process_stop_push(std::shared_ptr<RtcMsg>& msg) {
    RTC_LOG(LS_INFO) << "rtc worker process stop push request, worker_id: " << _worker_id;
}

void RtcWorker::_process_stop_pull(std::shared_ptr<RtcMsg>& msg) {
    RTC_LOG(LS_INFO) << "rtc worker process stop pull request, worker_id: " << _worker_id;
}

void RtcWorker::_process_answer(std::shared_ptr<RtcMsg>& msg) {
    RTC_LOG(LS_INFO) << "rtc worker process answer request, worker_id: " << _worker_id;
    int ret = _rtc_stream_mgr->set_answer(msg->uid, msg->stream_name,
        msg->sdp, msg->stream_type, msg->log_id);

    RTC_LOG(LS_INFO) << "rtc worker process answer, uid: " << msg->uid
        << ", stream_name: " << msg->stream_name
        << ", worker_id: " << _worker_id
        << ", log_id: " << msg->log_id
        << ", ret: " << ret;
}

void RtcWorker::_process_notify(int msg) {
    switch (msg) {
        case RtcWorker::QUIT:
            _stop();
            break;

        case RtcWorker::RTC_MSG:
            _process_rtc_msg();
            break;

        default:
            break;
    }
}


} // namespace xrtc



