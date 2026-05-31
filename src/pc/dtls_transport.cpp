#include "pc/dtls_transport.h"

#include <rtc_base/logging.h>
#include <api/crypto/crypto_options.h>
#include <absl/strings/string_view.h>

namespace xrtc {

// ============================================================================
// DTLS Record 头部常量
//
// k_dtls_record_header_len = 13 字节 (ContentType+Version+Epoch+SeqNum+Length)
// k_dtls_handshake_value_offset = 17 字节 (RecordHeader + HandshakeType + HandshakeLength)
// ============================================================================
const size_t k_dtls_record_header_len = 13;
const size_t k_dtls_handshake_value_offset = 17;

const size_t k_max_dtls_packet_len = 2048;
const size_t k_max_pending_packets = 2;

const size_t k_min_rtp_packet_len = 12;

// ============================================================================
// is_dtls_packet — 判断是否为合法 DTLS Record
//
// 条件: len >= 13 且 ContentType 在 20..63 (合法范围).
// buf[0] = 20 ChangeCipherSpec / 21 Alert / 22 Handshake / 23 ApplicationData.
// ============================================================================
bool is_dtls_packet(const char* buf, size_t len) {
    if (len < k_dtls_record_header_len) {
        return false;
    }

    const uint8_t* u = reinterpret_cast<const uint8_t*>(buf);
    return u[0] > 19 && u[0] < 64;
}

// ============================================================================
// is_dtls_client_hello_packet — 判断是否为 DTLS ClientHello
//
// 两层判断:
//   1. is_dtls_packet: ContentType 合法 + 长度 >=13
//   2. buf[0] == 22 (Handshake) + buf[13] == 1 (HandshakeType=ClientHello)
//      + len > 17 (至少包含 Handshake Type+Length 字段)
// ============================================================================
bool is_dtls_client_hello_packet(const char* buf, size_t len) {
    if (!is_dtls_packet(buf, len)) {
        return false;
    }

    const uint8_t* u = reinterpret_cast<const uint8_t*>(buf);
    return len > k_dtls_handshake_value_offset && (u[0] == 22 && u[13] == 1);
}

// is_rtp_packet — RTP 包识别: version bits == 2 (u[0] & 0xC0 == 0x80) 且长度 >= 12
bool is_rtp_packet(const char* buf, size_t len) {
    const uint8_t* u = reinterpret_cast<const uint8_t*>(buf);
    return len >= k_min_rtp_packet_len && ((u[0] & 0xC0) == 0x80);
}


StreamInterfaceChannel::StreamInterfaceChannel(IceTransportChannel* channel) :
    _channel(channel),
    _packets(k_max_pending_packets, k_max_dtls_packet_len)
{

}


// StreamInterfaceChannel::on_received_packet — 数据注入点
// 写入 BufferQueue 后 SignalEvent(SE_READ) 唤醒 OpenSSL 来 Read()
bool StreamInterfaceChannel::on_received_packet(const char* data, size_t size) {
    if (_packets.size() > 0) {
        RTC_LOG(LS_INFO) << ": Packet already in buffer queue";
    }
    
    if (!_packets.WriteBack(data, size, nullptr)) {
        RTC_LOG(LS_WARNING) << ": Failed to write packet to queue";
    }

    SignalEvent(this, rtc::SE_READ, 0);

    return true;
}

rtc::StreamState StreamInterfaceChannel::GetState() const {
    return _state;
}

rtc::StreamResult StreamInterfaceChannel::Read(void* buffer,
                size_t buffer_len,
                size_t* read,
                int* error)
{
    if (_state == rtc::SS_CLOSED) {
        return rtc::SR_EOS;
    }

    if (_state == rtc::SS_OPENING) {
        return rtc::SR_BLOCK;
    }

    if (!_packets.ReadFront(buffer, buffer_len, read)) {
        return rtc::SR_BLOCK;
    }

    return rtc::SR_SUCCESS;
}

// StreamInterfaceChannel::Write — OpenSSL 加密后的数据通过 ICE 发出
rtc::StreamResult StreamInterfaceChannel::Write(const void* data,
                size_t data_len,
                size_t* written,
                int* error)
{
    _channel->send_packet((const char*)data, data_len);
    if (written) {
        * written = data_len;
    }

    return rtc::SR_SUCCESS;
}

void StreamInterfaceChannel::Close() {
    _state = rtc::SS_CLOSED;
    _packets.Clear();
}


// DtlsTransport — 绑定 ICE channel, 订阅 signal_read_packet + signal_writable_state_change
DtlsTransport::DtlsTransport(IceTransportChannel* channel) :
    _channel(channel)
{
    _channel->signal_read_packet.connect(this, &DtlsTransport::_on_read_packet);
    _channel->signal_writable_state_change.connect(this, &DtlsTransport::_on_writable_state);

    webrtc::CryptoOptions crypto_options;
    _srtp_ciphers = crypto_options.GetSupportedDtlsSrtpCryptoSuites();
}

DtlsTransport::~DtlsTransport() {

}

void DtlsTransport::_on_writable_state(IceTransportChannel* channel) {
    RTC_LOG(LS_INFO) << to_string() << ": IceTransportChannel writable changed to "
        << channel->writable();

    if (!_dtls_active) {
        _set_writable_state(channel->writable());
        return;
    }

    switch (_dtls_state) {
        case DtlsTransportState::k_new:
            _maybe_start_dtls();
            break;
        case DtlsTransportState::k_connected:
            _set_writable_state(channel->writable());
            break;
        default:
            break;
    }

}

// DtlsTransport::_on_read_packet — ICE 层转发的非 STUN 数据
//
// k_new: 缓存 ClientHello, 等 DTLS 启动后重放
// k_connecting / k_connected: DTLS 包注入 OpenSSL, RTP/RTCP 经 is_rtp_packet 校验后向上层转发
void DtlsTransport::_on_read_packet(IceTransportChannel* /*channel*/,
        const char* buf, size_t len, int64_t ts)
{
    switch (_dtls_state) {
        case DtlsTransportState::k_new:
            if (_dtls) {
                RTC_LOG(LS_INFO) << to_string() << ": Received packet before DTLS started.";
            } else {
                RTC_LOG(LS_WARNING) << to_string() << ": Received packet before DTLS start";
            }

            if (is_dtls_client_hello_packet(buf, len)) {
                RTC_LOG(LS_INFO) << to_string() << ": Catching DTLS ClientHello packet until"
                    << " DTLS started";
                _catched_client_hello.SetData(buf, len);

                if (!_dtls && _local_certificate) {
                    _setup_dtls();
                }

            } else {
                RTC_LOG(LS_WARNING) << to_string() << " Not a DTLS ClientHello packet, "
                    << "dropping";
            }

            break;

        case DtlsTransportState::k_connecting:
        case DtlsTransportState::k_connected:
            if (is_dtls_packet(buf, len)) {  // Dtls包
                if (!_handle_dtls_packet(buf, len)) {
                    RTC_LOG(LS_WARNING) << to_string() << ": handle Dtls packet failed";
                    return;
                }
            } else {   // RTP/RTCP 包
                if (_dtls_state != DtlsTransportState::k_connected) {
                    RTC_LOG(LS_WARNING) << to_string() << ": Received non DTLS packet "
                        << "before DTLS complete";
                    return;
                }

                if (!is_rtp_packet(buf, len)) {
                    RTC_LOG(LS_WARNING) << to_string() << ": Received unexpected non "
                        << "RTP/RTCP packet";
                    return;
                }

                // RTP/RTCP — 经 is_rtp_packet 校验后向上层发射 signal_read_packet
                RTC_LOG(LS_INFO) << "======================rtp received: " << len;
                signal_read_packet(this, buf, len, ts);
            }

            break;

        default:
            break;
    }

}

// ============================================================================
// DtlsTransport::_setup_dtls — 初始化 OpenSSL DTLS 上下文
//
// 1. 创建 StreamInterfaceChannel 适配器, 使 OpenSSL 能通过 ICE 通道收发数据
// 2. 创建 SSLStreamAdapter (DTLS 模式 / DTLS 1.2 / 服务端角色)
// 3. 设置本地证书和对端指纹
// 4. 条件满足时启动 DTLS 握手 (_maybe_start_dtls)
// ============================================================================
bool DtlsTransport::_setup_dtls() {
    std::unique_ptr<StreamInterfaceChannel> downward = 
        std::make_unique<StreamInterfaceChannel>(_channel);

    _downward = downward.get();

    _dtls = rtc::SSLStreamAdapter::Create(std::move(downward));
    if (!_dtls) {
        RTC_LOG(LS_WARNING) << to_string() << ": Failed to create SSLStreamAdapter";
        return false;
    }

    _dtls->SetIdentity(_local_certificate->identity()->Clone());
    _dtls->SetMode(rtc::SSL_MODE_DTLS);
    _dtls->SetMaxProtocolVersion(rtc::SSL_PROTOCOL_DTLS_12);
    _dtls->SetServerRole(rtc::SSL_SERVER);
    // 订阅 _dtls 信号: SignalEvent 通知握手完成/解密数据就绪, SignalSSLHandshakeError 通知握手失败
    _dtls->SignalEvent.connect(this, &DtlsTransport::_on_dtls_event);
    _dtls->SignalSSLHandshakeError.connect(this,
            &DtlsTransport::_on_dtls_handshake_error);

    if (_remote_fingerprint_value.size() && !_dtls->SetPeerCertificateDigest(
                _remote_fingerprint_alg,
                _remote_fingerprint_value.data(),
                _remote_fingerprint_value.size()))
    {
        RTC_LOG(LS_WARNING) << to_string() << ": Failed to set remote fingerprint";
        return false;
    }

    if (!_srtp_ciphers.empty()) {
        if (!_dtls->SetDtlsSrtpCryptoSuites(_srtp_ciphers)) {
            RTC_LOG(LS_WARNING) << to_string() << ": Failed to set DTLS-SRTP crypto suites";
            return false;
        }
    } else {
        RTC_LOG(LS_WARNING) << to_string() << ": Not using DTLS-SRTP";
    }

    RTC_LOG(LS_INFO) << to_string() << ": Setup DTLS complete";

    _maybe_start_dtls();

    return true;
}

// _on_dtls_event — _dtls 的信号回调: SE_OPEN 握手完成 / SE_READ 解密数据就绪 / SE_CLOSE 连接关闭
void DtlsTransport::_on_dtls_event(rtc::StreamInterface* dtls, int sig, int error) {
    if (sig & rtc::SE_OPEN) {
        _set_writable_state(true);
        _set_dtls_state(DtlsTransportState::k_connected);
    }

    if (sig & rtc::SE_READ) {

        rtc::StreamResult ret;
        do {
            char buf[k_max_dtls_packet_len] = { 0 };
            size_t read;
            int read_error;
            ret = _dtls->Read(buf, sizeof(buf), &read, &read_error);
            if (ret == rtc::SR_SUCCESS) {

            } else if (ret == rtc::SR_EOS) {
                RTC_LOG(LS_INFO) << to_string() << ": DTLS transport closed by remote";
                _set_writable_state(false);
                _set_dtls_state(DtlsTransportState::k_closed);
                signal_closed(this);
            } else if (ret == rtc::SR_ERROR) {
                RTC_LOG(LS_WARNING) << to_string() << ": Closed DTLS transport by remote "
                    << "with error, code=" << read_error;
                _set_writable_state(false);
                _set_dtls_state(DtlsTransportState::k_closed);
                signal_closed(this);
            }
        } while (ret == rtc::SR_SUCCESS);
    }

    if (sig & rtc::SE_CLOSE) {
        if (!error) {
            RTC_LOG(LS_INFO) << to_string() << ": DTLS transport closed";
            _set_writable_state(false);
            _set_dtls_state(DtlsTransportState::k_closed);
        } else {
            RTC_LOG(LS_INFO) << to_string() << ": DTLS transport with error, "
                << "code=" << error;
            _set_writable_state(false);
            _set_dtls_state(DtlsTransportState::k_failed);
        }
    }
}

// _on_dtls_handshake_error — 握手失败回调
void DtlsTransport::_on_dtls_handshake_error(rtc::SSLHandshakeError error) {
    RTC_LOG(LS_INFO) << ": DTLS handshake error=" << (int)error;
}

// ============================================================================
// DtlsTransport::_maybe_start_dtls — 条件启动 DTLS 握手
//
// 启动条件: _dtls 已创建 + ICE 通道 writable。
// 1. StartSSL 失败 → k_failed
// 2. StartSSL 成功 → k_connecting, replay 缓存的 ClientHello (如有)
//    ClientHello replay 失败 → k_failed
// ============================================================================
void DtlsTransport::_maybe_start_dtls() {
    if (_dtls && _channel->writable()) {
        if (_dtls->StartSSL()) {
            RTC_LOG(LS_WARNING) << to_string() << ": Failed to StartSSL.";
            _set_dtls_state(DtlsTransportState::k_failed);
            return;
        }

        RTC_LOG(LS_INFO) << to_string() << ": Started DTLS.";
        _set_dtls_state(DtlsTransportState::k_connecting);


        if (_catched_client_hello.size()) {
            if (!_handle_dtls_packet(_catched_client_hello.data<char>(),
                        _catched_client_hello.size()))
            {
                RTC_LOG(LS_WARNING) << to_string() << ": Handling dtls packet failed";
                _set_dtls_state(DtlsTransportState::k_failed);
            }
            _catched_client_hello.Clear();
        }
    }
}

void DtlsTransport::_set_dtls_state(DtlsTransportState state) {
    if (_dtls_state == state) {
        return;
    }

    RTC_LOG(LS_INFO) << to_string() << ": Change dtls state from " << _dtls_state
        << " to " << state;
    _dtls_state = state;
    signal_dtls_state(this, state);
}

void DtlsTransport::_set_writable_state(bool writable) {
    if (_writable == writable) {
        return;
    }

    RTC_LOG(LS_INFO) << to_string() << ": Change dtls writable state from " << _writable
        << " to " << writable;
    _writable = writable;
    signal_writable_state(this);
}

// ============================================================================
// DtlsTransport::set_local_certificate — 设置 DTLS 本地证书
//
// _dtls_active 保证一次性设置:
//   - 未激活: 赋值并标记 active
//   - 已激活 + 相同证书: 忽略 (幂等)
//   - 已激活 + 不同证书: 拒绝 (不可更换)
// ============================================================================
bool DtlsTransport::set_local_certificate(rtc::RTCCertificate* certificate) {
    if (_dtls_active) {
        if (certificate == _local_certificate) {
            RTC_LOG(LS_INFO) << to_string() << ": Ingoring identical DTLS cert";
            return true;
        } else {
            RTC_LOG(LS_WARNING) << to_string() << ": Cannot change DTLS cert in this state";
            return false;
        }
    }

    if (certificate) {
        _local_certificate = certificate;
        _dtls_active = true;
    }

    return true;
}


// ========================================================================
// DtlsTransport::set_remote_fingerprint — 设置对端 DTLS 证书指纹
//
// 两种时序路径:
//
// 路径 A — answer SDP 先到 (正常):
//   指纹存入 _remote_fingerprint_* → _dtls 还不存在 → 跳过两个 if
//   → _setup_dtls() 内部 SetPeerCertificateDigest 一把配好
//
// 路径 B — ClientHello 先到 (UDP 不等 ICE):
//   ClientHello 触发 _setup_dtls(), 此时指纹空, SetPeerCertificateDigest 被跳过
//   → answer 后到, _dtls 已存在 && !fingerprint_change → 步骤 6 补调
//   SetPeerCertificateDigest 给已创建的 _dtls
//
// 步骤 7 (指纹变更): 收到不同的 answer SDP, 销毁旧 _dtls 全部重建。
// ========================================================================
bool DtlsTransport::set_remote_fingerprint(const std::string& digest_alg,
        const unsigned char* digest_data, size_t digest_len)
{
    rtc::Buffer remote_fingerprint_value(digest_data, digest_len);

    if (_dtls_active && _remote_fingerprint_value == remote_fingerprint_value) {
        RTC_LOG(LS_INFO) << to_string() << ": Ignoring identical remote fingerprint";
        return true;
    }

    if (digest_alg.empty()) {
        RTC_LOG(LS_WARNING) << to_string() << ": Other sides not support DTLS";
        _dtls_active = false;
        return false;
    }

    if (!_dtls_active) {
        RTC_LOG(LS_WARNING) << to_string() << ": Cannot set remote fingerprint before set local certificate";
        return false;
    }

    bool is_fingerprint_change = _remote_fingerprint_alg.size() > 0u;
    _remote_fingerprint_value = std::move(remote_fingerprint_value);
    _remote_fingerprint_alg = digest_alg;

    // 路径 B: ClientHello 先到已创建 _dtls, 但 _setup_dtls 里缺指纹跳过了
    // SetPeerCertificateDigest. 此处补设指纹到已有 _dtls, 无需重建.
    if (_dtls && !is_fingerprint_change) {
        rtc::SSLPeerCertificateDigestError err;
        if (!_dtls->SetPeerCertificateDigest(digest_alg, digest_data, digest_len, &err)) {
            RTC_LOG(LS_WARNING) << to_string() << ": Failed to set peer certificate digest";
            _set_dtls_state(DtlsTransportState::k_failed);
            return err == rtc::SSLPeerCertificateDigestError::VERIFICATION_FAILED;
        }
        return true;
    }

    // 步骤 7: 指纹变更 (不同 answer SDP) — 销毁旧 _dtls, 重建 DTLS 上下文
    if (_dtls && is_fingerprint_change) {
        _dtls.reset(nullptr);
        _set_dtls_state(DtlsTransportState::k_new);
        _set_writable_state(false);
    }

    // 走到这里是dtls还没setup
    if (!_setup_dtls()) {
        RTC_LOG(LS_WARNING) << to_string() << ": Failed to setup DTLS";
        _set_dtls_state(DtlsTransportState::k_failed);
        return false;
    }

    return true;
}


// ============================================================================
// DtlsTransport::_handle_dtls_packet — 验证 DTLS Record 完整性后注入 OpenSSL
//
// 1. 遍历 buffer 中所有 DTLS Record, 逐条校验 Record Header 中的 length 字段
//    (大端序: tmp_data[11]<<8 | tmp_data[12]), 确保不会读取越界
// 2. 全部 Record 校验通过后, 经 StreamInterfaceChannel 注入 SSLStreamAdapter
// ============================================================================
bool DtlsTransport::_handle_dtls_packet(const char* data, size_t size) {
    const uint8_t* tmp_data = reinterpret_cast<const uint8_t*>(data);
    size_t tmp_size = size;

    while (tmp_size > 0) {
        if (tmp_size < k_dtls_record_header_len) {
            return false;
        }

        size_t record_len = (tmp_data[11] << 8) | tmp_data[12];
        if (record_len + k_dtls_record_header_len > tmp_size) {
            return false;
        }

        tmp_data += k_dtls_record_header_len + record_len;
        tmp_size -= k_dtls_record_header_len + record_len;
    }

    return _downward->on_received_packet(data, size);
}

std::string DtlsTransport::to_string() {
    std::stringstream ss;
    absl::string_view RECEIVING[2] = {"-", "R"};
    absl::string_view WRITABLE[2] = {"-", "W"};

    ss << "DtlsTransport[" << transport_name() << "|"
        << (int)component() << "|"
        << RECEIVING[_receiving] << "|"
        << WRITABLE[_writable] << "]";
    return ss.str();
}


} // namespace xrtc
