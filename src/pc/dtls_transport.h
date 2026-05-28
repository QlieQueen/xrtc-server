#ifndef __DTLS_TRANSPORT_H_
#define __DTLS_TRANSPORT_H_

#include <memory>
#include <string>
#include <rtc_base/buffer.h>
#include <rtc_base/rtc_certificate.h>
#include <rtc_base/ssl_stream_adapter.h>
#include <rtc_base/third_party/sigslot/sigslot.h>

#include "ice/ice_transport_channel.h"

namespace xrtc {

enum class DtlsTransportState {
    k_new,
    k_connecting,
    k_connected,
    k_failed,
    k_num_values
};

// ============================================================================
// DtlsTransport — DTLS 传输层, 在 ICE 通道之上进行 DTLS 握手与数据加密
//
// 生命周期: set_local_description 时创建, 绑定到对应的 IceTransportChannel。
// 通过 signal_read_packet 订阅 ICE 层的非 STUN 数据, 后续 commit 加入
// OpenSSL 握手逻辑。
// ============================================================================
class DtlsTransport : public sigslot::has_slots<> {
public:
    DtlsTransport(IceTransportChannel* channel);
    ~DtlsTransport();

    const std::string& transport_name() { return _channel->transport_name(); }
    IceCandidateComponent component() { return _channel->component(); }

    std::string to_string();

private:
    void _on_read_packet(IceTransportChannel* channel, const char* buf, size_t size, int64_t ts);
    bool _setup_dtls();

private:
    IceTransportChannel* _channel = nullptr;
    DtlsTransportState _dtls_state = DtlsTransportState::k_new;
    bool _receiving = false;
    bool _writable = false;
    std::unique_ptr<rtc::SSLStreamAdapter> _dtls;
    rtc::Buffer _catched_client_hello;
    rtc::RTCCertificate* _local_certificate = nullptr;
}; 

} // namespace xrtc

#endif // __DTLS_TRANSPORT_H_