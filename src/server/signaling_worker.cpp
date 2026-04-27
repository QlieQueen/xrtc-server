#include "server/signaling_worker.h"

#include <errno.h>
#include <unistd.h>

#include <rtc_base/slice.h>
#include <rtc_base/logging.h>

#include "xrtc_server_def.h"
#include "base/socket.h"
#include "base/event_loop.h"
#include "server/tcp_connection.h"

using json = nlohmann::json;

namespace xrtc {

//typedef void (*io_cb_t)(EventLoop* el, IOWatcher* w, int fd, int events, void*data);
void signaling_worker_recv_nofify(EventLoop* /*el*/, IOWatcher* /*w*/, int fd, int /*events*/, void* data)
{
    int msg;
    if (sizeof(int) != read(fd, (void*)&msg, sizeof(int))) {
        RTC_LOG(LS_WARNING) << "read from pipe error: " << errno
            << ", errmsg: " << strerror(errno);
        return;
    }

    SignalingWorker* worker = (SignalingWorker*)data;
    worker->_process_notify(msg);
}

// typedef void (*io_cb_t)(EventLoop* el, IOWatcher* w, int fd, int events, void*data);
void conn_io_cb(EventLoop* /*el*/, IOWatcher* /*w*/, int fd, int events, void* data) {
    SignalingWorker* worker = (SignalingWorker*)data;

    if (events && EventLoop::READ) {
        worker->_read_query(fd);
    }

    if (events && EventLoop::WRITE) {
        worker->_write_query(fd);
    }

}

// typedef void (*timer_cb_t)(EventLoop* el, TimerWatcher* w, void* data);
void conn_timer_cb(EventLoop* /*el*/, TimerWatcher* /*w*/, void* data) {
    SignalingWorker* worker = (SignalingWorker*)data;
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
    _q_connfd.produce(fd);
    return notify(SignalingWorker::NEW_CONN);
}

void SignalingWorker::_process_notify(int msg) {
    switch (msg) {
        case SignalingWorker::QUIT:
            _el->stop();
            break;

        case SignalingWorker::NEW_CONN:
            int fd = 0;
            if (!_q_connfd.consume(&fd)) {
                RTC_LOG(LS_WARNING) << "[SignalingWorker::_process_notify] connfd queue consume failed.";
            }
            _new_conn(fd);
            break;
    }
}

void SignalingWorker::_new_conn(int fd) {
    RTC_LOG(LS_INFO) << "[SignalingWorker::_new_conn] signaling worker: " << _worker_id
                     << ", receive fd: " << fd;
    
    if (fd <= 0) {
        RTC_LOG(LS_INFO) << "[SignalingWorker::_new_conn] worker: " << _worker_id
                         << "invalid fd: " << fd;
        return;
    }

    TcpConnection* c = new TcpConnection(fd);
    sock_peer_to_str(c->fd, c->ip, &(c->port));
    c->io_watcher = _el->create_io_event(conn_io_cb, this);
    _el->start_io_event(c->io_watcher, fd, EventLoop::READ);

    c->timer_watcher = _el->create_timer(conn_timer_cb, c, true);
    _el->start_timer(c->timer_watcher, 100000); // 100ms

    if ((size_t)fd >= _tcp_conns.size()) {
        size_t new_size = _tcp_conns.size() == 0 ? fd + 1 : 2 * _tcp_conns.size();
        if (new_size <= (size_t)fd) {
            new_size = fd + 1;
        }
        _tcp_conns.resize(new_size);
    }
    _tcp_conns[fd] = c; 
}

void SignalingWorker::_close_conn(TcpConnection* c) {
    RTC_LOG(LS_INFO) << "close connection, fd: " << c->fd;
    close(c->fd);
}

void SignalingWorker::_remove_conn(TcpConnection* c) {
    _el->delete_io_event(c->io_watcher);
    _el->delete_timer(c->timer_watcher);
    _tcp_conns[c->fd] = nullptr;
    delete c;
}

void SignalingWorker::_read_query(int fd) {
    RTC_LOG(LS_INFO) << "signaling worker " << _worker_id
        << " receive read event, fd: " << fd;

    if (fd < 0 || (size_t)fd >= _tcp_conns.size()) {
        RTC_LOG(LS_WARNING) << "[SignalingWorker::_read_query] invalid fd: " << fd;
        return;
    }

    TcpConnection* c = _tcp_conns[fd];
    int nread = 0;
    int read_len = c->bytes_expected;
    int qb_len = sdslen(c->querybuf);
    c->querybuf = sdsMakeRoomFor(c->querybuf, read_len);
    nread = sock_read_data(fd, c->querybuf + qb_len, read_len);

    c->list_interaction = _el->now();
    RTC_LOG(LS_INFO) << "sock read data, len: " << nread;

    if (-1 == nread) {
        _close_conn(c);
    } else {
        sdsIncrLen(c->querybuf, nread);
    }

    int ret = _process_query_buffer(c);
    if (ret != 0) {
        _close_conn(c);
        return;
    }

}

void SignalingWorker::_write_query(int fd) {


}

int SignalingWorker::_process_query_buffer(TcpConnection* c) {
    while (sdslen(c->querybuf) >= c->bytes_expected + c->bytes_processed) {
        xhead_t* head = (xhead_t*)(c->querybuf);
        if (TcpConnection::STATE_HEAD == c->current_state) {
            if (XHEAD_MAGIC_NUM != head->magic_num) {
                RTC_LOG(LS_WARNING) << "[SignalingWorker::_process_query_buffer] invalid data, fd: " << c->fd;
                return -1;
            }
            c->current_state = TcpConnection::STATE_BODY;
            c->bytes_processed = XHEAD_SIZE;
            c->bytes_expected = head->body_len;
        } else {
            rtc::Slice header(c->querybuf, XHEAD_SIZE);
            rtc::Slice body(c->querybuf + XHEAD_SIZE, head->body_len);

            int ret = _process_request(c, header, body);
            if (ret != 0) {
                return -1;
            }

            // 短链接处理
            c->bytes_processed = 65536;
        }
    }


    return 0;
}

int SignalingWorker::_process_request(TcpConnection* c,
    const rtc::Slice& header,
    const rtc::Slice& body)
{
    RTC_LOG(LS_INFO) << "receive body: " << body.data();

    xhead_t* xh = (xhead_t*)(header.data());

    int ret = 0;
    int cmdno;
    json root;
    try {
        root = json::parse(body.data());
        cmdno = root["cmdno"];

        RTC_LOG(LS_INFO) << "cmdno: " << cmdno;
    } catch (json::parse_error& e) {
        RTC_LOG(LS_WARNING) << "parse error: " << e.what()
            << ", pos: " << e.byte
            << ", fd: " << c->fd
            << ", log_id: " << xh->log_id;
        return -1;
    }

    switch(cmdno) {
        case CMDNO_PUSH:
            return _process_push(cmdno, c, root, xh->log_id);
            break;

        default:
            RTC_LOG(LS_WARNING) << "unknown cmdno: " << cmdno
                << ", fd: " << c->fd
                << ", log_id: " << xh->log_id;
            return -1;
            break;
    }

    return -1;
}

int SignalingWorker::_process_push(int cmdno, TcpConnection* c,
    const json& root, uint32_t log_id)
{
    uint64_t uid;
    std::string stream_name;
    int audio;
    int video;

    try {
        uid = root.at("uid");
        stream_name = root.at("stream_name");
        audio = root.at("audio");
        video = root.at("video");

    } catch (const json::out_of_range& e) {
        RTC_LOG(LS_WARNING) << "parse error: " << e.what()
            << ", fd: " << c->fd
            << ", log_id: " << log_id;
        return -1;
    }

    RTC_LOG(LS_INFO) << "cmdno[" << cmdno
        << "] uid[" << uid
        << "] stream_name[" << stream_name
        << "] audio[" << audio
        << "] video[" << video
        << "] signaling server push request";

    std::shared_ptr<RtcMsg> msg = std::make_shared<RtcMsg>();
    msg->cmdno = cmdno;
    msg->uid = uid;
    msg->stream_name = stream_name;
    msg->audio = audio;
    msg->video = video;
    msg->log_id = log_id;
    msg->worker = this;
    msg->conn = c;
    msg->fd = c->fd;

    return 0;
}



} // namespace xrtc