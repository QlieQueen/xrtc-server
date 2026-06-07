#ifndef __SRTP_TRANSPORT_H_
#define __SRTP_TRANSPORT_H_

namespace xrtc {

// SRTP 传输层基类 — 管理 send/recv 两个 SrtpSession，提供 protect/unprotect 接口
// DtlsSrtpTransport 继承此类，包装 DtlsTransport 在 DTLS 握手之上加 SRTP 加解密
class SrtpTransport {
public:
    SrtpTransport(bool rtcp_mux_enabled);
    virtual ~SrtpTransport() = default;


private:
    bool _rtcp_mux_enabled;
};


} // namespace xrtc


#endif // __SRTP_TRANSPORT_H_