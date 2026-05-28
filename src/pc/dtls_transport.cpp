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
                RTC_LOG(LS_WARNING) << to_string() << " Not a DTLS ClientHello packet.";
            }

        break;

    }

}

// ============================================================================
// DtlsTransport::_setup_dtls — 初始化 OpenSSL DTLS 上下文 (stub)
//
// 当前返回 false, 后续 commit 实现:
//   - 创建 StreamInterfaceChannel (ICE 适配器)
//   - 创建 SSLStreamAdapter
//   - 设置证书、DTLS 模式、服务端角色、对端指纹
// ============================================================================
bool DtlsTransport::_setup_dtls() {
    return false;
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
