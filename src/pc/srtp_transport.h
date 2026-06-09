#ifndef __SRTP_TRANSPORT_H_
#define __SRTP_TRANSPORT_H_

#include <memory>
#include <vector>

#include "pc/srtp_session.h"

namespace xrtc {

// SRTP 传输层基类 — 管理 send/recv 两个 SrtpSession，提供 protect/unprotect 接口
// DtlsSrtpTransport 继承此类，包装 DtlsTransport 在 DTLS 握手之上加 SRTP 加解密
class SrtpTransport {
public:
    SrtpTransport(bool rtcp_mux_enabled);
    virtual ~SrtpTransport() = default;

    bool set_rtp_params(int send_cs,
            const uint8_t* send_key,
            size_t send_key_len,
            const std::vector<int>& send_extension_ids,
            int recv_cs,
            const uint8_t* recv_key,
            size_t recv_key_len,
            const std::vector<int>& recv_extension_ids);

    void reset_params();
private:
    void _create_srtp_session();

private:
    bool _rtcp_mux_enabled;
    std::unique_ptr<SrtpSession> _send_session;
    std::unique_ptr<SrtpSession> _recv_session;
};


} // namespace xrtc


#endif // __SRTP_TRANSPORT_H_