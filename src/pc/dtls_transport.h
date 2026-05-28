#ifndef __DTLS_TRANSPORT_H_
#define __DTLS_TRANSPORT_H_

#include <memory>
#include <string>
#include <rtc_base/stream.h>
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
// StreamInterfaceChannel — ICE 与 OpenSSL 之间的适配器
//
// OpenSSL 通过 rtc::StreamInterface 抽象读写数据, 不认识 IceConnection。
// 此类把 Read → 从 BufferQueue 取包, Write → ice_channel->send_packet()。
// ============================================================================
class StreamInterfaceChannel : public rtc::StreamInterface {
public:
    StreamInterfaceChannel(IceTransportChannel* channel);
    ~StreamInterfaceChannel() override = default;

    rtc::StreamState GetState() const override;

    rtc::StreamResult Read(void* buffer,
                        size_t buffer_len,
                        size_t* read,
                        int* error) override;
    rtc::StreamResult Write(const void* data,
                        size_t data_len,
                        size_t* written,
                        int* error) override;
    void Close() override;
private:
    IceTransportChannel* _channel = nullptr;
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
    bool _maybe_start_dtls();

private:
    IceTransportChannel* _channel = nullptr;
    DtlsTransportState _dtls_state = DtlsTransportState::k_new;
    bool _receiving = false;
    bool _writable = false;
    std::unique_ptr<rtc::SSLStreamAdapter> _dtls;
    rtc::Buffer _catched_client_hello;
    rtc::RTCCertificate* _local_certificate = nullptr;
    StreamInterfaceChannel* _downward = nullptr;
    rtc::Buffer _remote_fingerprint_value;
    std::string _remote_fingerprint_alg;
}; 

} // namespace xrtc

#endif // __DTLS_TRANSPORT_H_