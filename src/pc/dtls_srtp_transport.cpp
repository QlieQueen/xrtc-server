#include "pc/dtls_srtp_transport.h"
#include "pc/dtls_transport.h"

namespace xrtc {

DtlsSrtpTransport::DtlsSrtpTransport(const std::string& transport_name,
        bool rtcp_mux_enable) :
    SrtpTransport(rtcp_mux_enable), _transport_name(transport_name)
{

}

// set_dtls_transport — 绑定底层 DTLS 传输通道，后续 _maybe_setup_dtls_srtp 会订阅 dtls 信号
void DtlsSrtpTransport::set_dtls_transport(DtlsTransport* rtp_dtls_transport,
        DtlsTransport* rtcp_dtls_transport)
{
    _rtp_dtls_transport = rtp_dtls_transport;
    _rtcp_dtls_transport = rtcp_dtls_transport;
}

} // namespace xrtc