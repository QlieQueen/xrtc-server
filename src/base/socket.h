#ifndef __BASE_SOCKET_H_
#define __BASE_SOCKET_H_

#include <unistd.h>
#include <stdint.h>

namespace xrtc {

int create_tcp_server(const char* addr, int port);
int tcp_accept(int sock, char* host, int* port);
int tcp_connect(const char* addr, int port);
int sock_setnonblock(int sock);
int sock_setnodely(int sock);
int sock_peer_to_str(int sock, char*ip, uint16_t* port);
int sock_read_data(int sock, char* buf, size_t len);
int sock_write_data(int sock, const char* buf, size_t len);

} // namespace xrtc

#endif // __BASE_SOCKET_H_