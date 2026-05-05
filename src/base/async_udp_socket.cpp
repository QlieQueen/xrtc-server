#include "base/async_udp_socket.h"

#include <sys/socket.h>
#include <rtc_base/logging.h>

#include "base/socket.h"

namespace xrtc {

const size_t MAX_BUF_SIZE = 1500;

//typedef void (*io_cb_t)(EventLoop* el, IOWatcher* w, int fd, int events, void*data);
void async_udp_socket_io_cb(EventLoop* el, IOWatcher* w, int fd, int events, void* data) {
    AsyncUdpSocket* udp_socket = (AsyncUdpSocket*)data;

    if (events & EventLoop::READ) {
        udp_socket->recv_data();
    }

    if (events & EventLoop::WRITE) {
        udp_socket->send_data();
    }
}


AsyncUdpSocket::AsyncUdpSocket(EventLoop* el, int socket) :
    _el(el),
    _socket(socket),
    _buf(new char[MAX_BUF_SIZE]),
    _size(MAX_BUF_SIZE)
{
    _socket_watcher = _el->create_io_event(async_udp_socket_io_cb, this);
    _el->start_io_event(_socket_watcher, _socket, EventLoop::READ);
}

AsyncUdpSocket::~AsyncUdpSocket() {

    if (_socket_watcher) {
        _el->delete_io_event(_socket_watcher);
        _socket_watcher = nullptr;
    }

    if (_buf) {
        delete []_buf;
        _buf = nullptr;
    }
}

void AsyncUdpSocket::recv_data() {
    while (true) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);

        int len = sock_recv_from(_socket, _buf, _size, (struct sockaddr*)&addr, addr_len);
        if (len <= 0) {
            return;
        }

        int64_t ts = sock_get_recv_timestamp(_socket);
        int port = ntohs(addr.sin_port);
        char ip[64] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        rtc::SocketAddress remote_addr(ip, port);

        signal_read_packet(this, _buf, len, remote_addr, ts);
    }
}

void AsyncUdpSocket::_send_data_from_list() {
    size_t len = 0;
    int sent = 0;

    while (!_udp_packet_list.empty()) {
        // 发送udp packet节点
        UdpPacketData* packet = _udp_packet_list.front();
        sockaddr_storage saddr;
        len = packet->addr().ToSockAddrStorage(&saddr);
        sent = sock_send_to(_socket, packet->data(), packet->size(),
                MSG_NOSIGNAL, (struct sockaddr*)&saddr, len);
        if (sent < 0) {
            RTC_LOG(LS_WARNING) << "send udp packet error, remote_addr: " <<
                packet->addr().ToString();
            delete packet;
            _udp_packet_list.pop_front();
            return;
        } else if (0 == sent) {
            RTC_LOG(LS_WARNING) << "send 0 bytes, try again, remote_addr: "
                << packet->addr().ToString();
            return;
        } else {
            delete packet;
            _udp_packet_list.pop_front();
        }
    }
}

void AsyncUdpSocket::send_data() {
    _send_data_from_list();

    // 若数据发送完，必须停止写监听，防止写时间一直触发
    if (_udp_packet_list.empty()) {
        _el->stop_io_event(_socket_watcher, _socket, EventLoop::WRITE);
    }
}

int AsyncUdpSocket::send_to(const char* data, size_t size,
    const rtc::SocketAddress& addr)
{
    return _add_udp_packet(data, size, addr);
}

int AsyncUdpSocket::_add_udp_packet(const char* data, size_t size,
    const rtc::SocketAddress& addr)
{
    size_t len = 0;
    int sent = 0;
    // 1.若list里还有数据未发送，先发送list里的数据
    _send_data_from_list();

        // 2、发送当前数据
    sockaddr_storage saddr;
    len = addr.ToSockAddrStorage(&saddr);
    sent = sock_send_to(_socket, data, size,
            MSG_NOSIGNAL, (struct sockaddr*)&saddr, len);
    if (sent < 0) {
        RTC_LOG(LS_WARNING) << "send udp packet error, remote_addr: " << addr.ToString();
        return -1;
    } else if (0 == sent) {
        RTC_LOG(LS_WARNING) << "send o bytes, try again, remote_addr: " <<
            addr.ToString();
        goto SEND_AGAIN;
    }

    return size;

SEND_AGAIN:
    // 3、无法发送出去时，才开启写事件监控
    UdpPacketData* packet_data = new UdpPacketData(data, size, addr);
    _udp_packet_list.push_back(packet_data);
    _el->start_io_event(_socket_watcher, _socket, EventLoop::WRITE);

    return size;

}






} // namespace xrtc
