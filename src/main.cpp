#include <iostream>
#include <thread>

#include "base/conf.h"
#include "base/log.h"

xrtc::GeneralConf* g_conf = nullptr;
xrtc::XrtcLog* g_log = nullptr;

int init_general_conf(const char* filename) {
    if (!filename) {
        fprintf(stderr, "filename is nullptr\n");
        return -1;
    }

    g_conf = new xrtc::GeneralConf();

    int ret = load_general_conf(filename, g_conf);
    if (ret != 0) {
        fprintf(stderr, "load general conf: %s failed", filename);
        return -1;
    }
    
    return 0;
}

int init_log(const std::string& log_dir,
            const std::string& log_name,
            const std::string& log_level)
{
    g_log = new xrtc::XrtcLog(log_dir, log_name, log_level);

    int ret = g_log->init();
    if (ret != 0) {
        fprintf(stderr, "init log failed\n");
        return -1;
    }

    g_log->start();

    return 0;
}


int main() {

    if (init_general_conf("./conf/general.yaml") != 0) {
        return -1;
    }

    if (init_log(g_conf->log_dir, g_conf->log_name, g_conf->log_level) != 0) {
        return -1;
    }

    RTC_LOG(LS_INFO) << "hello world!";
    RTC_LOG(LS_WARNING) << "hello world!";

    // 等待日志写入
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 停止日志系统
    if (g_log) {
        g_log->stop();
    }

    return 0;
}
