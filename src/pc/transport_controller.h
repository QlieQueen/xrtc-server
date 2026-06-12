#ifndef __TRANSPORT_CONTROLLER_H_
#define __TRANSPORT_CONTROLLER_H_

#include <map>
#include <rtc_base/third_party/sigslot/sigslot.h>
#include <rtc_base/copy_on_write_buffer.h>

#include "base/event_loop.h"
#include "ice/ice_agent.h"
#include "ice/candidate.h"
#include "ice/port_allocator.h"
#include "pc/session_description.h"
#include "pc/peer_connection_def.h"

namespace xrtc {

class DtlsTransport;
enum class DtlsTransportState;
class DtlsSrtpTransport;

class TransportController : public sigslot::has_slots<> {
public:
    TransportController(EventLoop* el, PortAllocator* allocator);
    ~TransportController();

    int set_local_description(SessionDescription* desc);
    int set_remote_description(SessionDescription* remote_desc);

    void set_local_certificate(rtc::RTCCertificate* cert); // 先存着，DTLS握手时使用

    int send_rtp(const std::string& transport_name, const char* data, size_t len);

public:
    // 信号转发： IceAgent -> TransportController -> PeerConnection
    sigslot::signal4<TransportController*, const std::string&, IceCandidateComponent,
        const std::vector<Candidate>&> signal_candidate_allocate_done;
    // PC 整体状态变化 → PeerConnection
    sigslot::signal2<TransportController*, PeerConnectionState> signal_connection_state;

    sigslot::signal3<TransportController*, rtc::CopyOnWriteBuffer*, int64_t>
        signal_rtp_packet_received;
    sigslot::signal3<TransportController*, rtc::CopyOnWriteBuffer*, int64_t>
        signal_rtcp_packet_received;

private:
    void _on_candidate_allocator_done(IceAgent* agent,
            const std::string& transport_name,
            IceCandidateComponent component,
            const std::vector<Candidate>& candidates);
    void _on_ice_state(IceAgent*, IceTransportState);
    void _on_dtls_receiving_state(DtlsTransport*);
    void _on_dtls_writable_state(DtlsTransport*);
    void _on_dtls_state(DtlsTransport*, DtlsTransportState);
    void _on_rtp_packet_received(DtlsSrtpTransport*,
            rtc::CopyOnWriteBuffer* packet, int64_t ts);
    void _on_rtcp_packet_received(DtlsSrtpTransport*,
            rtc::CopyOnWriteBuffer* packet, int64_t ts);
    void _update_state();

    void _add_dtls_transport(DtlsTransport* dtls_transport);
    DtlsTransport* _get_dtls_transport(const std::string& transport_name);
    void _add_dtls_srtp_transport(DtlsSrtpTransport* dtls_srtp_transport);
    DtlsSrtpTransport* _get_dtls_srtp_transport(const std::string& transport_name);

private:
    EventLoop* _el;
    IceAgent* _ice_agent;
    rtc::RTCCertificate* _local_certificate = nullptr;
    std::map<std::string, DtlsTransport*> _dtls_transport_by_name;
    std::map<std::string, DtlsSrtpTransport*> _dtls_srtp_transport_by_name;
    // _pc_state — 聚合所有 DtlsTransport 状态计算出的 PC 整体状态
    PeerConnectionState _pc_state = PeerConnectionState::k_new;
};



} // namespace xrtc

#endif // __TRANSPORT_CONTROLLER_H_
