#include "base/socket.h"

#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
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
        ip[1] = '\0';

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
    return nread;
}

int sock_write_data(int sock, const char* buf, size_t len) {
    int nwritten = write(sock, buf, len);
    if (nwritten == -1) {
        // tcp写缓冲区满，返回0等待下一次写事件触发
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            nwritten = 0;
        } else { // 真正写出错的情况，返回-1
            RTC_LOG(LS_WARNING) << "sock write failed, error: " << errno
                << ", errmsg: " << strerror(errno) << ", fd: " << sock;
            return -1;
        }
    }
    // write返回大于等于0的情况直接返回（这里如果对端关闭，此时write会返回0）
    return nwritten;
}

int create_udp_socket(int family) {
    int sock = socket(family, SOCK_DGRAM, 0);
    if (sock == -1) {
        RTC_LOG(LS_WARNING) << "create udp socket error: " << strerror(errno)
            << ", errno: " << errno;
        return -1;
    }

    return sock;
}

int sock_bind(int sock, struct sockaddr* addr, socklen_t len, int min_port, int max_port) {
    int ret = -1;
    if (0 == min_port && 0 == max_port) {
        // 让操作系统自动选择一个port
        ret = bind(sock, addr, len);
    } else {
        struct sockaddr_in* addr_in = (struct sockaddr_in*)addr;
        for (int port = min_port; port <= max_port && ret !=0; port++) {
            addr_in->sin_port = htons(port);
            ret = bind(sock, addr, len);
        }
    }

    if (ret != 0) {
        RTC_LOG(LS_WARNING) << "bind error: " << strerror(errno)
            << ", errno: " << errno;
    }

    return ret;
}

int sock_get_address(int sock, char* ip, int* port) {
    struct sockaddr_in addr_in;
    socklen_t len = sizeof(struct sockaddr);
    int ret = getsockname(sock, (struct sockaddr*)&addr_in, &len);
    if (ret != 0) {
        RTC_LOG(LS_WARNING) << "getsockname error: " << strerror(errno)
            << ", errno: " << errno;
        return -1;
    }

    if (ip) {
        memcpy(ip, inet_ntoa(addr_in.sin_addr), sizeof(addr_in.sin_addr));
    }

    if (port) {
        *port = ntohs(addr_in.sin_port);
    }

    return 0;
}

int sock_recv_from(int sock, char* buf, size_t size, struct sockaddr* addr, socklen_t addr_len) {
    int received = recvfrom(sock, buf, size, 0, addr, &addr_len);
    if (received < 0) {
        if (errno == EAGAIN) {
            received = 0;
        } else {
            RTC_LOG(LS_WARNING) << "recv from error: " << strerror(errno)
                << ", errno: " << errno;
            return -1;
        }
    } else if (0 == received) {
        RTC_LOG(LS_WARNING) << "recv from error: " << strerror(errno)
            << ", errno: " << errno;
        return -1;
    }

    return received;
}

int sock_send_to(int sock, const char* buf, size_t len, int flag, 
    struct sockaddr* addr, socklen_t addr_len)
{
    int sent = sendto(sock, buf, len, flag, addr, addr_len);
    if (sent < 0) {
        if (EAGAIN == errno || EWOULDBLOCK == errno) {
            sent = 0;
        } else {
            RTC_LOG(LS_WARNING) << "sendto error: " << strerror(errno)
                << ", errno: " << errno;
            return -1;
        }
    } else if (sent == 0) {
        RTC_LOG(LS_WARNING) << "sendto error: " << strerror(errno)
            << ", errno: " << errno;
        return -1;
    }

    return sent;
}

// 返回微秒
int64_t sock_get_recv_timestamp(int sock) {
    struct timeval time;
    int ret = ioctl(sock, SIOCGSTAMP_OLD, &time);
    if (ret != 0) {
        return -1;
    }

    return time.tv_sec * 1000000 + time.tv_usec;
}

} // namespace xrtc