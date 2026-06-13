#include "server/rtc_server.h"

#include <unistd.h>
#include <errno.h>

#include <rtc_base/crc32.h>
#include <rtc_base/rtc_certificate_generator.h>
#include <rtc_base/logging.h>
#include <yaml-cpp/yaml.h>

#include "base/event_loop.h"
#include "server/rtc_worker.h"

namespace xrtc {

const uint64_t k_year_in_ms =  365 * 24 * 3600 * 1000UL;

//typedef void (*io_cb_t)(EventLoop* el, IOWatcher* w, int fd, int events, void*data);
void rtc_server_recv_notify(EventLoop* /*el*/, IOWatcher* /*w*/, int fd, int /*events*/, void* data)
{
    int msg;
    if (sizeof(int) != read(fd, (void*)&(msg), sizeof(msg))) {
        RTC_LOG(LS_WARNING) << "read from pipe failed, errno: " << errno
            << ", errmsg: " << strerror(errno);
    }

    RtcServer* server = (RtcServer*)data;
    server->_process_notify(msg);
}


RtcServer::RtcServer() : _el(new EventLoop(this))   {
}

RtcServer::~RtcServer(){
    if (_el) {
        delete _el;
        _el = nullptr;
    }

    if (_thread) {
        delete _thread;
        _thread = nullptr;
    }

    for (auto worker : _workers) {
        if (worker) {
            delete worker;
        }
    }

    _workers.clear();
}

int RtcServer::_generate_and_check_certificate() {
    if (!_certificate || _certificate->HasExpired(time(NULL) * 1000)) {
        rtc::KeyParams key_perams;
        RTC_LOG(LS_INFO) << "dtls enabled, key type: " << key_perams.type();
        _certificate = rtc::RTCCertificateGenerator::GenerateCertificate(key_perams,
             k_year_in_ms);
        if (_certificate) {
            rtc::RTCCertificatePEM pem = _certificate->ToPEM();
            RTC_LOG(INFO) << "rtc certificate: \n" << pem.certificate();
        }
    }

    if (!_certificate) {
        RTC_LOG(LS_WARNING) << "get certificate error";
        return -1;
    }

    return 0;
}

int RtcServer::init(const char* conf_file) {
    if (!conf_file) {
        RTC_LOG(LS_WARNING) << "[RtcServer::init] rtc server config file is nullptr";
        return -1;
    }

    try {
        YAML::Node config = YAML::LoadFile(conf_file);
        _options.worker_num = config["worker_num"].as<int>();
    } catch (YAML::Exception& e) {
        RTC_LOG(LS_WARNING) << "catch a YAML exception, line: " << e.mark.line
                            << ", column: " << e.mark.column
                            << ", errmsg: " << e.msg.c_str();
        return -1;
    }

    int pipefd[2];
    int ret = pipe(pipefd);
    if (ret == -1) {
        RTC_LOG(LS_WARNING) << "[RtcServer::init] pipe failed, errno: " << errno
                << ", errmsg: " << strerror(errno);
        return -1;
    }

    _notify_recv_fd = pipefd[0];
    _notify_send_fd = pipefd[1];

    _pipe_watcher = _el->create_io_event(rtc_server_recv_notify, this);
    _el->start_io_event(_pipe_watcher, _notify_recv_fd, EventLoop::READ);

    for (int i = 0; i < _options.worker_num; i++) {
        _create_worker(i);
    }

    return 0;
}

bool RtcServer::start() {
    if (_thread) {
        RTC_LOG(LS_WARNING) << "rtc server already running";
        return true;
    } 

    _thread = new std::thread([this]() {
        RTC_LOG(LS_INFO) << "rtc server event loop start";
        _el->start();
        RTC_LOG(LS_INFO) << "rtc server event loop stop";
    });

    return true;
}

void RtcServer::stop() {
    notify(RtcServer::QUIT);
}

int RtcServer::notify(int msg) {
    int written = write(_notify_send_fd, (void*)&(msg), sizeof(int));
    return written == sizeof(int) ? 0 : -1;
}

void RtcServer::join() {
    if (_thread && _thread->joinable()) {
        _thread->join();
    }
}

void RtcServer::push_msg(std::shared_ptr<RtcMsg> msg) {
    std::unique_lock<std::mutex> lock(_q_mutex);
    _q_msg.push(msg);
}

std::shared_ptr<RtcMsg> RtcServer::pop_msg() {
    std::unique_lock<std::mutex> lock(_q_mutex);
    if (_q_msg.empty()) {
        return nullptr;
    }

    std::shared_ptr<RtcMsg> msg = _q_msg.front();    
    _q_msg.pop();

    return msg;
}

int RtcServer::send_rtc_msg(std::shared_ptr<RtcMsg> msg) {
    push_msg(msg);
    return notify(RtcServer::RTC_MSG);
}

void RtcServer::_create_worker(int index) {
    RTC_LOG(LS_WARNING) << "rtc server create worker, worker_id: " << index;

    RtcWorker* worker = new RtcWorker(index, _options);

    if (worker->init() != 0) {
        RTC_LOG(LS_WARNING) << "[RtcServer::_create_worker] worker init failed";
        return;
    }

    if (!worker->start()) {
        RTC_LOG(LS_WARNING) << "[RtcServer::_create_worker] worker start failed";
        return;
    }

    _workers.push_back(worker);

    return;
}

void RtcServer::_process_notify(int msg) {
    switch (msg) {
        case RtcServer::QUIT:
            _stop();
            break;

        case RtcServer::RTC_MSG:
            _process_rtc_msg();
            break;
        default:
            RTC_LOG(LS_WARNING) << "unknown msg: " << msg;
            break;
    }    
}

void RtcServer::_process_rtc_msg() {
    RTC_LOG(LS_INFO) << "===================== process rtc msg";

    std::shared_ptr<RtcMsg> msg = pop_msg();

    //struct RtcMsg {
    //    int cmdno = -1;
    //    uint64_t uid = 0;
    //    std::string stream_name;
    //    std::string stream_type;
    //    int audio = 0;
    //    int video = 0;
    //    uint32_t log_id = 0;
    //    void* worker = nullptr;
    //    void* conn = nullptr;
    //    int fd = 0;
    //    std::string sdp;
    //    int err_no = 0;
    //    void* certificate = nullptr;  // rtc_worker初始化时生成(RtcServer::init)
    //};


    if (_generate_and_check_certificate() != 0) {
        return;
    }

    msg->certificate = _certificate.get();

    RTC_LOG(LS_INFO) << "cmdno: " << msg->cmdno
                     << ", uid: " << msg->uid
                     << ", stream_name: " << msg->stream_name
                     << ", stream_type: " << msg->stream_type
                     << ", audio: " << msg->audio
                     << ", video: " << msg->video
                     << ", log_id: " << msg->log_id
                     << ", woker: " << msg->worker
                     << ", conn: " << msg->conn
                     << ", fd: " << msg->fd
                     << ", sdp: " << msg->sdp
                     << ", err_no: " << msg->err_no;
    
    RtcWorker* worker = _get_worker(msg->stream_name);
    if (worker) {
        worker->send_rtc_msg(msg);
    }
}

void RtcServer::_stop() {
    if (!_thread) {
        RTC_LOG(LS_WARNING) << "[RtcServer::_stop] rtc server is not running";
        return;
    }

    _el->delete_io_event(_pipe_watcher);
    _el->stop();

    close(_notify_recv_fd);
    close(_notify_send_fd);

    for (auto worker : _workers) {
        if (worker) {
            worker->stop();
            worker->join();
        }
    }

    RTC_LOG(LS_INFO) << "rtc server stop";
}

RtcWorker* RtcServer::_get_worker(const std::string& stream_name) {
    if (_workers.size() == 0 || _workers.size() != (size_t)_options.worker_num) {
        return nullptr;
    }
    
    uint32_t num = rtc::ComputeCrc32(stream_name);
    size_t index = num % _options.worker_num;
    return _workers[index];
}


}

