#include "base/socket.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

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

int tcp_connect(const char* addr, int port) {
    // 1. 创建socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        RTC_LOG(LS_WARNING) << "create socket error, errno: " << errno 
        << ", errmsg: " << strerror(errno);
        return -1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    inet_aton(addr, &dst.sin_addr);
    dst.sin_port = htons(port);
    if (-1 == connect(sock, (const sockaddr*)&dst, sizeof(dst))) {
        RTC_LOG(LS_WARNING) << "connect to: " << addr
                            << ", port: " << port
                            << " failed. errmsg: " << strerror(errno);
        return -1;
    }

    return 0;
}

int sock_setnonblock(int sock) {
    int flags = fcntl(sock, F_GETFL);
    if (flags == -1) {
        RTC_LOG(LS_WARNING) << "fcntl(F_GETFL) failed, errno: " << errno
            << ", errmsg: " << strerror(errno) << ", fd: " << sock;
        return -1;
    }
    
    if (-1 == fcntl(sock, F_SETFL, flags | O_NONBLOCK)) {
        RTC_LOG(LS_WARNING) << "fcntl(F_SETFL) failed, errno: " << errno
            << ", errmsg: " << strerror(errno) << ", fd: " << sock;
        return -1;
    }

    return 0;
}

int sock_setnodely(int sock) {
    int yes = -1;
    if (-1 == setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes))) {
        RTC_LOG(LS_WARNING) << "setsockopt TCP_NODELAY failed, errno: " << errno
            << ", errmsg: " << strerror(errno) << ", fd: " << sock;
        return -1;
    }
    
    return 0;
}


int sock_peer_to_str(int sock, char*ip, uint16_t* port) {
    if (!ip || !port) {
        return -1;
    }

    struct sockaddr_in sa;
    socklen_t salen;

    int ret = getpeername(sock, (struct sockaddr*)&sa, &salen);
    if (ret == -1) {
        ip[0] = '?';
        ip[1] = '\n';

        *port = 0;
    }

    strcpy(ip, inet_ntoa(sa.sin_addr));
    *port = sa.sin_port;

    return 0;
}

int sock_read_data(int sock, char* buf, size_t len) {
    int nread = read(sock, buf, len);
    if (nread < 0) {
        
        RTC_LOG(LS_WARNING) << "[sock_read_data] read fd: " << sock
            << " failed, errmsg: " << strerror(errno);
        return -1;
    } 
}

} // namespace xrtc