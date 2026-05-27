#include "pc/dtls_transport.h"

#include <rtc_base/logging.h>

namespace xrtc {

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

void DtlsTransport::_on_read_packet(IceTransportChannel* /*channel*/,
        const char* buf, size_t len, int64_t ts)
{
    RTC_LOG(LS_INFO) << "===============================len: " << len;
}


DtlsTransport::~DtlsTransport() {

}

} // namespace xrtc
