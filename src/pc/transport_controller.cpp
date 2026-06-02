#include "pc/transport_controller.h"

#include "pc/dtls_transport.h"

namespace xrtc {

TransportController::TransportController(EventLoop* el, PortAllocator* alloctor) :
    _el(el),
    _ice_agent(new IceAgent(el, alloctor))
{
    _ice_agent->signal_candidate_allocate_done.connect(this,
        &TransportController::_on_candidate_allocator_done);
}

// 析构函数 -- delete IceAgent
TransportController::~TransportController() {
    if (_ice_agent) {
        delete _ice_agent;
        _ice_agent = nullptr;
    }
}

// 为每个media创建ICE channel并触发候选收集
int TransportController::set_local_description(SessionDescription* desc) {
    if (!desc) {
        return -1;
    }

    for (auto content : desc->contents()) {
        std::string mid = content->mid();

        // 开启BUNDLE 时只有一个 mid 需要创建channel，其他的复用该channel
        if (desc->is_bundle(mid) && mid != desc->get_first_bundle_mid()) {
            continue;        
        }

        _ice_agent->create_channel(_el, mid, IceCandidateComponent::RTP);
        auto td = desc->get_transport_info(mid);
        if (td) {
            _ice_agent->set_ice_params(mid, IceCandidateComponent::RTP,
                IceParameters(td->ice_ufrag, td->ice_pwd));
        }

        DtlsTransport* dtls_transport = new DtlsTransport(
            _ice_agent->get_channel(mid, IceCandidateComponent::RTP));
        dtls_transport->set_local_certificate(_local_certificate);
        // 订阅 DtlsTransport 三个状态信号, 用于计算 PC 整体状态
        dtls_transport->signal_receiving_state.connect(this,
                &TransportController::_on_dtls_receiving_state);
        dtls_transport->signal_writable_state.connect(this,
                &TransportController::_on_dtls_writable_state);
        dtls_transport->signal_dtls_state.connect(this,
                &TransportController::_on_dtls_state);
        _add_dtls_transport(dtls_transport);
    }
    
    _ice_agent->gathering_candidate();

    return 0;
}

void TransportController::_on_dtls_receiving_state(DtlsTransport*) {
    _update_state();
}

void TransportController::_on_dtls_writable_state(DtlsTransport*) {
    _update_state();
}

void TransportController::_on_dtls_state(DtlsTransport*, DtlsTransportState) {
    _update_state();
}

// ========================================================================
// _update_state — 聚合所有 DtlsTransport 状态计算 PeerConnectionState
//
// 规则:
//   有 any failed           → k_failed
//   全部 new + closed       → k_new
//   有 connecting + closed  → k_connecting
//   全部 connected + closed → k_connected
//
// k_closed 归入 k_connected: closed 的 transport 曾经建立过, 不是握手失败.
// ========================================================================
void TransportController::_update_state() {
    PeerConnectionState pc_state = PeerConnectionState::k_new;

    std::map<DtlsTransportState, int> dtls_state_counts;
    auto iter = _dtls_transport_by_name.begin();
    for (; iter != _dtls_transport_by_name.end(); iter++) {
        dtls_state_counts[iter->second->dtls_state()]++;
    }

    int total_connected = dtls_state_counts[DtlsTransportState::k_connected];
    int total_connecting = dtls_state_counts[DtlsTransportState::k_connecting];
    int total_failed = dtls_state_counts[DtlsTransportState::k_failed];
    int total_closed = dtls_state_counts[DtlsTransportState::k_closed];
    int total_new = dtls_state_counts[DtlsTransportState::k_new];
    int total_transports = _dtls_transport_by_name.size();

    if (total_failed > 0) {
        _pc_state = PeerConnectionState::k_failed;
    }/* else if (IceTransportState::k_disconnected == _ice_agent->ice_state()) {
        _pc_state = PeerConnectionState::k_disconnected;
    }*/ else if (total_new + total_closed == total_transports) {
        _pc_state = PeerConnectionState::k_new;
    } else if (total_connecting + total_closed > 0) {
        _pc_state = PeerConnectionState::k_connecting;
    } else if (total_connected + total_closed == total_transports) {
        _pc_state = PeerConnectionState::k_connected;
    }

    if (_pc_state != pc_state) {
        _pc_state = pc_state;
        signal_connection_state(this, pc_state);
    }

}

// ============================================================================
// TransportController::_add_dtls_transport — 注册/替换 DTLS 传输对象
//
// 按 transport_name 索引, 已存在则先销毁旧对象再替换。
// ============================================================================
void TransportController::_add_dtls_transport(DtlsTransport* dtls_transport) {
    std::string name = dtls_transport->transport_name();
    auto iter = _dtls_transport_by_name.find(name);
    if (iter != _dtls_transport_by_name.end()) {
        delete iter->second;
        iter->second = dtls_transport;
    } else {
        _dtls_transport_by_name[name] = dtls_transport;
    }
}

DtlsTransport* TransportController::_get_dtls_transport(const std::string& transport_name)
{
    auto iter = _dtls_transport_by_name.find(transport_name);
    if (iter != _dtls_transport_by_name.end()) {
        return iter->second;
    }

    return nullptr;
}


int TransportController::set_remote_description(SessionDescription* remote_desc) {
    if (!remote_desc) {
        return -1;
    }

    for (auto content : remote_desc->contents()) {
        std::string mid = content->mid();

        if (remote_desc->is_bundle(mid) && mid != remote_desc->get_first_bundle_mid()) {
            continue;
        }

        auto td = remote_desc->get_transport_info(mid);
        if (td) {
            _ice_agent->set_remote_ice_params(mid, IceCandidateComponent::RTP,
                IceParameters(td->ice_ufrag, td->ice_pwd));
            DtlsTransport* dtls = _get_dtls_transport(mid);
            if (dtls) {
                dtls->set_remote_fingerprint(td->identity_fingerprint->algorithm,
                    td->identity_fingerprint->digest.data(),
                    td->identity_fingerprint->digest.size());
            }
        }
    }

    return 0;
}


void TransportController::set_local_certificate(rtc::RTCCertificate* cert) {
    _local_certificate = cert;
}

// 继续向上转发
void TransportController::_on_candidate_allocator_done(
        IceAgent* ice_agent,
        const std::string& transport_name,
        IceCandidateComponent component,
        const std::vector<Candidate>& candidates)
{
    signal_candidate_allocate_done(this, transport_name, component, candidates);
}


} // namespace xrtc
