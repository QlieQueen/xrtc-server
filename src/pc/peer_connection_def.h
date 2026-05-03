#ifndef __PC_PEER_CONNECTION_H_
#define __PC_PEER_CONNECTION_H_

namespace xrtc {

enum class PeerConnectionState {
    k_new,
    k_connecting,
    k_connected,
    k_disconnected,
    k_failed,
    k_close,
};

} // namespace xrtc
#endif // __BASE_PEER_CONNECTION_H_