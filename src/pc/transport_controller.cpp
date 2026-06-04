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
    for (auto dtls : _dtls_transport_by_name) {
        delete dtls.second;
    }
    _dtls_transport_by_name.clear();

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
        _ice_agent->signal_ice_state.connect(this,
                &TransportController::_on_ice_state);
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

// _on_ice_state — IceAgent 聚合后的 ICE 状态变化 → 重新计算 PC 状态
void TransportController::_on_ice_state(IceAgent*, IceTransportState) {
    _update_state();
}

// ========================================================================
// _update_state — 聚合 DtlsTransport + IceTransportChannel 状态计算 PC 状态
//
// 两种 transport 合并计数, 每个 DtlsTransport 算 dtls + ice 两份。
// total_transports = N * 2.
//
// 规则 (优先级从高到低):
//   有 any failed                    → k_failed
//   有 ice disconnected              → k_disconnected
//   全部 new + closed                → k_new
//   有 ice checking 或 dtls connecting → k_connecting
//   全部 connected/completed/closed  → k_connected
//
// k_closed 归入 k_connected: closed 的 transport 曾经建立过, 不是握手失败.
// ========================================================================
void TransportController::_update_state() {
    PeerConnectionState pc_state = PeerConnectionState::k_new;

    std::map<DtlsTransportState, int> dtls_state_counts;
    std::map<IceTransportState, int> ice_state_counts;
    auto iter = _dtls_transport_by_name.begin();
    for (; iter != _dtls_transport_by_name.end(); iter++) {
        dtls_state_counts[iter->second->dtls_state()]++;
        ice_state_counts[iter->second->ice_channel()->state()]++;
    }

    int total_connected = ice_state_counts[IceTransportState::k_connected] + 
        dtls_state_counts[DtlsTransportState::k_connected];
    int total_dtls_connecting = dtls_state_counts[DtlsTransportState::k_connecting];
    int total_failed = ice_state_counts[IceTransportState::k_failed] + 
        dtls_state_counts[DtlsTransportState::k_failed];
    int total_closed = ice_state_counts[IceTransportState::k_closed] + 
        dtls_state_counts[DtlsTransportState::k_closed];
    int total_new = ice_state_counts[IceTransportState::k_new] + 
        dtls_state_counts[DtlsTransportState::k_new];
    int total_ice_checking = ice_state_counts[IceTransportState::k_checking];
    int total_ice_disconnected = ice_state_counts[IceTransportState::k_disconnected];
    int total_ice_complete = ice_state_counts[IceTransportState::k_completed];

    int total_transports = _dtls_transport_by_name.size() * 2;

    if (total_failed > 0) {
        pc_state = PeerConnectionState::k_failed;
    } else if (total_ice_disconnected > 0) {
        pc_state = PeerConnectionState::k_disconnected;
    } else if (total_new + total_closed == total_transports) {
        pc_state = PeerConnectionState::k_new;
    } else if (total_ice_checking + total_dtls_connecting + total_new > 0) {
        pc_state = PeerConnectionState::k_connecting;
    } else if (total_connected + total_ice_complete + total_closed == total_transports) {
        pc_state = PeerConnectionState::k_connected;
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
