#include "pc/dtls_transport.h"

#include <rtc_base/logging.h>
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


StreamInterfaceChannel::StreamInterfaceChannel(IceTransportChannel* channel) :
    _channel(channel)
{

}


rtc::StreamState StreamInterfaceChannel::GetState() const {
    return rtc::StreamState::SS_CLOSED;
}

rtc::StreamResult StreamInterfaceChannel::Read(void* buffer,
                size_t buffer_len,
                size_t* read,
                int* error)
{
    return rtc::StreamResult::SR_ERROR;
}

rtc::StreamResult StreamInterfaceChannel::Write(const void* data,
                size_t data_len,
                size_t* written,
                int* error)
{
    return rtc::StreamResult::SR_ERROR;
}

void StreamInterfaceChannel::Close() {

}


// ============================================================================
// DtlsTransport 构造 — 绑定 ICE channel, 订阅其 signal_read_packet
//
// ICE 层收到非 STUN 包时发射信号, 经 IceTransportChannel 转发到此。
// 当前仅打印包长度, 后续 commit 加入 DTLS 握手处理。
// ============================================================================
DtlsTransport::DtlsTransport(IceTransportChannel* channel) :
    _channel(channel)
{
    _channel->signal_read_packet.connect(this, &DtlsTransport::_on_read_packet);
}

DtlsTransport::~DtlsTransport() {

}

// ============================================================================
// DtlsTransport::_on_read_packet — 处理 ICE 层转发的非 STUN 数据
//
// k_new 状态: DTLS 尚未启动, 收到 ClientHello 则缓存到 _catched_client_hello。
// 缓存原因: 客户端拿到服务端地址后立即发 ClientHello, 不等待 ICE 连通。
// 此时服务端的 DTLS 启动条件 (证书/指纹/ICE writable) 可能还未满足,
// 若直接丢弃, 客户端 DTLS 重传超时是指数退避的, 握手迟迟无法开始。
// 缓存后在 _maybe_start_dtls 中重放给 OpenSSL, 握手立即启动。
// ============================================================================
void DtlsTransport::_on_read_packet(IceTransportChannel* /*channel*/,
        const char* buf, size_t len, int64_t /*ts*/)
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

    if (_remote_fingerprint_value.size() && !_dtls->SetPeerCertificateDigest(
                _remote_fingerprint_alg,
                _remote_fingerprint_value.data(),
                _remote_fingerprint_value.size()))
    {
        RTC_LOG(LS_WARNING) << to_string() << ": Failed to set remote fingerprint";
        return false;
    }

    RTC_LOG(LS_INFO) << to_string() << ": Setup DTLS complete";

    _maybe_start_dtls();

    return true;
}

// ============================================================================
// DtlsTransport::_maybe_start_dtls — 条件启动 DTLS 握手 (stub)
//
// 启动条件: 本地证书已设置 + 对端指纹已设置 + ICE 通道 writable。
// 当前仅返回 false, 后续 commit 实现完整条件检查与 StartSSL。
// ============================================================================
bool DtlsTransport::_maybe_start_dtls() {
    return false;
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

    if (!_setup_dtls()) {
        RTC_LOG(LS_WARNING) << to_string() << ": Failed to setup DTLS";
        _set_dtls_state(DtlsTransportState::k_failed);
        return false;
    }

    return true;
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
