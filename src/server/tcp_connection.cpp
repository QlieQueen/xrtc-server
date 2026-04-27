#include "server/tcp_connection.h"

namespace xrtc {

TcpConnection::TcpConnection(int fd) :
    fd(fd), ip(""), port(0), querybuf(sdsempty())
{
}

TcpConnection::~TcpConnection() {

}

} // namespace xrtc
