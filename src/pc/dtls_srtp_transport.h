#ifndef __DTLS_SRTP_TRANSPORT_H_
#define __DTLS_SRTP_TRANSPORT_H_

#include <string>
#include <rtc_base/buffer.h>

#include "pc/srtp_transport.h"
#include "pc/dtls_transport.h"

namespace xrtc {

// DtlsSrtpTransport — DTLS 握手 + SRTP 加解密的组合传输层
// 不继承 DtlsTransport，而是持有指针并订阅其 signal_read_packet / signal_dtls_state
// 数据面: 收包 → SRTP 解密 → signal_rtp_packet_received
//         发包 → SRTP 加密 → _rtp_dtls_transport->send_packet()
class DtlsSrtpTransport : public SrtpTransport {
public:
    DtlsSrtpTransport(const std::string& transport_name, bool rtcp_mux_enable);
    ~DtlsSrtpTransport() = default;

    void set_dtls_transport(DtlsTransport* rtp_dtls_transport,
            DtlsTransport* rtcp_dtls_transport);

private:
    bool _extract_params(DtlsTransport* dtls_transport, 
            int* selected_crypto_suite,
            rtc::ZeroOnFreeBuffer<unsigned char>* send_key,
            rtc::ZeroOnFreeBuffer<unsigned char>* recv_key);

private:
    std::string _transport_name;
    DtlsTransport* _rtp_dtls_transport = nullptr;
    DtlsTransport* _rtcp_dtls_transport = nullptr;
};

} // namespace xrtc


#endif // __DTLS_SRTP_TRANSPORT_H_