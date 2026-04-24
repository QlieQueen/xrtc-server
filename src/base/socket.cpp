#include "base/socket.h"

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <rtc_base/logging.h>

namespace xrtc {

int create_tcp_server(const char* addr, int port) {
    // 1. 创建socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        RTC_LOG(LS_WARNING) << "create socket error, errno: " << errno 
        << ", errmsg: " << strerror(errno);
        return -1;
    }

    // 2. 设置SO_REUSEADDR
    int on = 1;
    int ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (ret == -1) {
        RTC_LOG(LS_WARNING) << "setsockopt SO_REUSEADDR error, errno: " << errno
            << ", errmsg: " << strerror(errno);
        close(sock);
        return -1;
    }

    // 3. 创建addr
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (addr && inet_aton(addr, &sa.sin_addr) == 0) {
        RTC_LOG(LS_WARNING) << "invalid address";
        close(sock);
        return -1;
    }

    // 4. bind
    ret = bind(sock, (const sockaddr*)&(sa), sizeof(sa));
    if (ret == -1) {
        RTC_LOG(LS_WARNING) << "bind error, errno: " << errno
            << ", errmsg: " << strerror(errno);
        close(sock);
        return -1;
    }

    // 5. listen
    ret = listen(sock, 4095);
    if (ret == -1) {
        RTC_LOG(LS_WARNING) << "listen error, errno: " << errno
            << ", errmsg: " << strerror(errno);
        close(sock);
        return -1;    
    }

    return sock;
}

static int generic_accept(int sock, struct sockaddr* sa, socklen_t* len) {
    int fd = -1;

    while (1) {
        fd = accept(sock, sa, len);
        if (-1 == fd) {
            if (errno == EINTR) {
                continue;
            } else {
                RTC_LOG(LS_WARNING) << "tcp accept error: " << strerror(errno)
                    << ", errno: " << errno;
                return -1;
            }
        }
        break;
    }
    return fd;
}

int tcp_accept(int sock, char* host, int* port) {
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    int fd = generic_accept(sock, (struct sockaddr*)&sa, &salen);
    if (-1 == fd) {
        return -1;
    }

    if (host) {
        strncpy(host, inet_ntoa(sa.sin_addr), sizeof(sa.sin_addr));
    }

    if (port) {
        *port = ntohs(sa.sin_port);
    }

    return fd;
}



} // namespace xrtc