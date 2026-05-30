#ifndef __DTLS_TRANSPORT_H_
#define __DTLS_TRANSPORT_H_

#include <memory>
#include <string>
#include <rtc_base/stream.h>
#include <rtc_base/buffer.h>
#include <rtc_base/buffer_queue.h>
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

    bool on_received_packet(const char* data, size_t size);

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
    rtc::BufferQueue _packets;
    rtc::StreamState _state = rtc::SS_OPEN;
};

// DtlsTransport — ICE 通道之上的 DTLS 握手与数据加密
class DtlsTransport : public sigslot::has_slots<> {
public:
    DtlsTransport(IceTransportChannel* channel);
    ~DtlsTransport();

    const std::string& transport_name() { return _channel->transport_name(); }
    IceCandidateComponent component() { return _channel->component(); }

    // ========================================================================
    // set_local_certificate — 设置 DTLS 本地证书
    //
    // _dtls_active 保证证书设置后不可更改: 相同证书返回 true, 不同证书返回 false。
    // ========================================================================
    bool set_local_certificate(rtc::RTCCertificate* certificate);
    bool set_remote_fingerprint(const std::string& digest_alg,
        const unsigned char* digest_data, size_t digest_len);

    std::string to_string();

public:
    sigslot::signal2<DtlsTransport*, DtlsTransportState> signal_dtls_state;
    sigslot::signal1<DtlsTransport*> signal_writable_state;

private:
    void _on_read_packet(IceTransportChannel* channel, const char* buf, size_t size, int64_t ts);
    bool _setup_dtls();
    void _maybe_start_dtls();
    void _set_dtls_state(DtlsTransportState state);
    void _set_writable_state(bool writable);
    bool _handle_dtls_packet(const char* data, size_t size);
    void _on_writable_state(IceTransportChannel* channel);

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
    // _dtls_active 标记证书已设置, 防止 DTLS 启动后更换证书
    bool _dtls_active = false;
}; 

} // namespace xrtc

#endif // __DTLS_TRANSPORT_H_