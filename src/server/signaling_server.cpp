#include "server/signaling_server.h"

#include <errno.h>
#include <unistd.h>

#include <yaml-cpp/yaml.h>
#include <rtc_base/logging.h>

#include "base/socket.h"
#include "server/signaling_worker.h"

namespace xrtc {


void signaling_server_recv_nofify(EventLoop* /*el*/, IOWatcher* /*w*/, int fd, int /*events*/, void* data)
{
    int msg;
    if (sizeof(int) != read(fd, (void*)&msg, sizeof(int))) {
        RTC_LOG(LS_WARNING) << "read from pipe error: " << errno
            << ", errmsg: " << strerror(errno);
        return;
    }

    SignalingServer* server = (SignalingServer*)data;
    server->_process_notify(msg);
}

void accept_new_connect(EventLoop* /*el*/, IOWatcher* /*w*/,
    int fd, int /*events*/, void* data)
{
    int conn_fd;
    char conn_ip[128];
    int conn_port;

    conn_fd = tcp_accept(conn_fd, conn_ip, &conn_port);
    if (-1 == conn_fd) {
        return;
    }

    RTC_LOG(LS_INFO) << "accept new conn, listen fd: " << fd << ", ip: " << conn_fd
        << ", port: " << conn_port << ", client fd: " << conn_fd;

    SignalingServer* server = (SignalingServer*)data;
    server->_dispatch_new_conn(conn_fd);

}


SignalingServer::SignalingServer() : _el(new EventLoop(this)) {
}

SignalingServer::~SignalingServer() {
    if (_el) {
        delete _el;
        _el = nullptr;
    }

    if (_thread) {
        delete _thread;
        _thread = nullptr;
    }

    // TODO: delete workers
    for (auto worker : _workers) {
        if (worker) {
            delete worker;
        }
    }

    _workers.clear();
}

int SignalingServer::init(const char* conf_file) {
    if (!conf_file) {
        RTC_LOG(LS_WARNING) << "conf file is nullptr";
        return -1;
    }

    try {
        YAML::Node config = YAML::LoadFile(conf_file);
        
        _options.host = config["host"].as<std::string>();
        _options.port = config["port"].as<int>();
        _options.worker_num = config["worker_num"].as<int>();
        _options.connection_timeout = config["connection_timeout"].as<int>();
    } catch (YAML::Exception& e) {
        RTC_LOG(LS_WARNING) << "catch a YMAL::Exception, line: " << e.mark.line
                            << ", colum: " << e.mark.column
                            << ", errmsg: " << e.msg.c_str();
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd)) {
        RTC_LOG(LS_WARNING) << "pipe failed, errmsg: " << strerror(errno);
        return -1;
    }

    _notify_recv_fd = pipefd[0];
    _notify_send_fd = pipefd[1];

    _pipe_watcher = _el->create_io_event(signaling_server_recv_nofify, this);
    _el->start_io_event(_pipe_watcher, _notify_recv_fd, EventLoop::READ);

    _listen_fd = create_tcp_server(_options.host.c_str(), _options.port);   
    if (-1 == _listen_fd) {
        return -1;
    }

    _io_watcher = _el->create_io_event(accept_new_connect, this);
    _el->start_io_event(_io_watcher, _listen_fd, EventLoop::READ);

    // TODO: 创建worker
    for (int i = 0; i < _options.worker_num; i++) {
        _create_worker(i);
    }

    return 0;
}

bool SignalingServer::start() {
    if (_thread) {
        RTC_LOG(LS_WARNING) << "signaling thread already start.";
        return false;
    }

    _thread = new std::thread([=]() {
        RTC_LOG(LS_INFO) << "[SignalingServer::start] signaling server event loop start";
        _el->start();
        RTC_LOG(LS_INFO) << "[SignalingServer::start] signaling server event loop stop";
    });

    return true;
}

void SignalingServer::stop() {
    notify(SignalingServer::QUIT);
}

int SignalingServer::notify(int msg) {
    int written = write(_notify_send_fd, &msg, sizeof(int));
    return written == sizeof(int) ? 0 : -1;
}

void SignalingServer::join() {
    if (_thread && _thread->joinable()) {
        _thread->join();
    }
}

void SignalingServer::_process_notify(int msg) {
    switch (msg) {
        case QUIT:
            _stop();
            break;
        default:
            RTC_LOG(LS_WARNING) << "unknown msg: " << msg;
            break;
    }
}

void SignalingServer::_stop() {
    if (!_thread) {
        RTC_LOG(LS_WARNING) << "signaling server is not running";
        return;
    }

    _el->delete_io_event(_pipe_watcher);
    _el->delete_io_event(_io_watcher);
    _el->stop();

    close(_notify_recv_fd);
    close(_notify_send_fd);
    close(_listen_fd);

    RTC_LOG(LS_INFO) << "signaling server stop";
}

void SignalingServer::_dispatch_new_conn(int fd) {

}

int SignalingServer::_create_worker(int worker_id) {
    RTC_LOG(LS_INFO) << "signaling server create worker, worker_id: " << worker_id;

    SignalingWorker* worker = new SignalingWorker(worker_id, _options);

    if (worker->init() != 0) {
        RTC_LOG(LS_WARNING) << "[SignalingServer::_create_worker] worker init failed, worker_id: " << worker_id;
        return -1;
    }

    if (!worker->start()) {
        RTC_LOG(LS_WARNING) << "[SignalingServer::_create_worker] worker start failed, worker_id: " << worker_id;
        return -1;
    }

    _workers.push_back(worker);

    return 0;
}


} // namespace xrtc
