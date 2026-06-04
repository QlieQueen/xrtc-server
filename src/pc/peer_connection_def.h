#ifndef __PC_PEER_CONNECTION_H_
#define __PC_PEER_CONNECTION_H_

namespace xrtc {

enum class PeerConnectionState {
    k_new,
    k_connecting,
    k_connected,
    k_disconnected,
    k_failed,
    k_closed,
};

struct RTCOfferAnswerOptions {
    bool send_audio = true;
    bool send_video = true;
    bool recv_audio = true;
    bool recv_video = true;
    bool use_rtp_mux = true;    // 是否开启 BUNDLE
    bool use_rtcp_mux = true;   // 是否开启 RTP/RTCP 复用一个通道
    bool dtls_on = true;
};

} // namespace xrtc
#endif // __BASE_PEER_CONNECTION_H_