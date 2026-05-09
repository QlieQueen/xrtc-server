#include "pc/transport_controller.h"


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

    }
    
    _ice_agent->gathering_candidate();

    return 0;
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
