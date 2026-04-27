#ifndef __SERVER_TCP_CONNECTION_H_
#define __SERVER_TCP_CONNECTION_H_

#include <stdint.h>

#include <rtc_base/sds.h>

#include "base/xhead.h"

namespace xrtc {

class IOWatcher;
class TimerWatcher;

class TcpConnection {
public:
    enum {
        STATE_HEAD = 0,
        STATE_BODY = 1
    };

    TcpConnection(int fd);
    ~TcpConnection();

public:
    int fd;
    char ip[128];
    uint16_t port;
    IOWatcher* io_watcher = nullptr;
    TimerWatcher* timer_watcher = nullptr;
    sds querybuf; // 存储读取的数据 redis
    size_t bytes_expected = XHEAD_SIZE; // 第一次期待读取的大小为头部大小
    size_t bytes_processed = 0; // 标记当前处理了buf里多少个字节的数据
    int current_state = STATE_HEAD;
    unsigned long list_interaction = 0;
};

} // namespace xrtc

#endif // __SERVER_TCP_CONNECTION_H_