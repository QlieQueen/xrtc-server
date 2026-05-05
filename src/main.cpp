#include <iostream>
#include <thread>

#include <signal.h>
#include <execinfo.h>
#include <stdio.h>

#include "base/log.h"

void crash_handler(int sig) {
    fprintf(stderr, "\n=== CRASH: signal %d ===\n", sig);
    void* bt[100];
    int n = backtrace(bt, 100);
    backtrace_symbols_fd(bt, n, 2 /*stderr*/);
    _exit(1);
}
#include "base/conf.h"
#include "server/rtc_server.h"
#include "server/signaling_server.h"

extern xrtc::GeneralConf* g_conf;
extern xrtc::XrtcLog* g_log;
extern xrtc::SignalingServer* g_signaling_server;
extern xrtc::RtcServer* g_rtc_server;

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

int init_signaling_server() {
    g_signaling_server = new xrtc::SignalingServer();

    if (-1 == g_signaling_server->init("./conf/signaling_server.yaml")) {
        RTC_LOG(LS_WARNING) << "[init_signaling_server] signaling server init failed";
        return -1;
    }

    g_signaling_server->start();    

    return 0;
}

int init_rtc_server() {
    g_rtc_server = new xrtc::RtcServer();

    if (-1 == g_rtc_server->init("./conf/rtc_server.yaml")) {
        RTC_LOG(LS_WARNING) << "[init_rtc_server] rtc server init failed";
        return -1;
    }

    g_rtc_server->start();


    return 0;
}

void signal_process(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            if (g_signaling_server) {
                g_signaling_server->stop();
            }
            
            if (g_rtc_server) {
                g_rtc_server->stop();
            }
            break;

        default:
            break;
    }
}

int main() {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);

    if (init_general_conf("./conf/general.yaml") != 0) {
        return -1;
    }

    if (init_log(g_conf->log_dir, g_conf->log_name, g_conf->log_level) != 0) {
        return -1;
    }

    if (init_signaling_server() != 0) {
        return -1;
    }

    if (init_rtc_server() != 0) {
        return -1;
    }

    signal(SIGINT, signal_process);

    g_signaling_server->join();

    g_rtc_server->join();

    // 停止日志系统
    if (g_log) {
        g_log->stop();
    }

    return 0;
}
