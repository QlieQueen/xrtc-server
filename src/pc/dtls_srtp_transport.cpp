#include "pc/dtls_srtp_transport.h"

#include <rtc_base/logging.h>

#include "pc/dtls_transport.h"

namespace xrtc {

// RFC 5764: DTLS-SRTP 密钥导出标签, 用于 SSL_export_keying_material
static char k_dtls_srtp_exporter_label[] = "EXTRACTOR-dtls_srtp";

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

// _extract_params — 从 DTLS 密钥材料中导出 SRTP 密钥
//
// 1. 获取 DTLS 协商的 crypto suite → 确定 key_len/salt_len
// 2. 调用 SSL_export_keying_material (RFC 5705) 导出 keying material
// 3. 拆分: [client_write_key | server_write_key | client_write_salt | server_write_salt]
//    server → send_key (加密发出), client → recv_key (解密收到)
bool DtlsSrtpTransport::_extract_params(DtlsTransport* dtls_transport,
        int* selected_crypto_suite,
        rtc::ZeroOnFreeBuffer<unsigned char>* send_key,
        rtc::ZeroOnFreeBuffer<unsigned char>* recv_key)
{
    if (!dtls_transport || !dtls_transport->is_dtls_active()) {
        return false;
    }

    if (!dtls_transport->get_srtp_crypto_suite(selected_crypto_suite)) {
        RTC_LOG(LS_WARNING) << "No selected crypto suite";
        return false;
    }

    RTC_LOG(LS_INFO) << "Extract DTLS-SRTP key from transport " << _transport_name;

    int key_len;
    int salt_len;
    if (!rtc::GetSrtpKeyAndSaltLengths(*selected_crypto_suite, &key_len, &salt_len)) {
        RTC_LOG(LS_WARNING) << "Unknown DTLS-SRTP crypto suite " << *selected_crypto_suite;
        return false;
    }

    rtc::ZeroOnFreeBuffer<unsigned char> dtls_buffer(2 * (key_len + salt_len));
    if (!dtls_transport->export_keying_material(k_dtls_srtp_exporter_label,
                nullptr, 0, false, &dtls_buffer[0], dtls_buffer.size()))
    {
        RTC_LOG(LS_WARNING) << "Extracting DTLS-SRTP params failed";
        return false;
    }

    rtc::ZeroOnFreeBuffer<unsigned char> client_write_key(key_len + salt_len);
    rtc::ZeroOnFreeBuffer<unsigned char> server_write_key(key_len + salt_len);
    size_t offset = 0;
    memcpy(&client_write_key[0], &dtls_buffer[offset], key_len);
    offset += key_len;
    memcpy(&server_write_key[0], &dtls_buffer[offset], key_len);
    offset += key_len;
    memcpy(&client_write_key[key_len], &dtls_buffer[offset], salt_len);
    offset += salt_len;
    memcpy(&server_write_key[key_len], &dtls_buffer[offset], salt_len);

    *send_key = std::move(server_write_key);
    *recv_key = std::move(client_write_key);

    return true;
}

} // namespace xrtc