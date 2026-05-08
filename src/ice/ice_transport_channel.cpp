#include "ice/ice_transport_channel.h"

#include <rtc_base/logging.h>

namespace xrtc {

IceTransportChannel::IceTransportChannel(EventLoop* el, PortAllocator* alloctor,
        const std::string& transport_name,
        IceCandidateComponent component) :
    _el(el),
    _transport_name(transport_name),
    _alloctor(alloctor),
    _component(component)
{
    RTC_LOG(LS_INFO) << "ice transport channel created, transport_name: " << _transport_name
        << ", component: " << component;
}

IceTransportChannel::~IceTransportChannel() {

    for (auto port : _ports) {
        delete port;
    }

    _ports.clear();

}

void IceTransportChannel::set_ice_params(const IceParameters& ice_params) {
    _ice_params = ice_params;
}

void IceTransportChannel::gathering_candidate() {
    // 1.检查_ice_params的ufrag/pwd非空
    if (_ice_params.ice_ufrag.empty() || _ice_params.ice_pwd.empty()) {
        RTC_LOG(LS_WARNING) << "cannot gathering candidate, because ICE param is empty."
            << ", transport_name: " << _transport_name
            << ", component: " << _component
            << ", ufrag: " << _ice_params.ice_ufrag
            << ", pwd: " << _ice_params.ice_pwd;
        return;
    }

    auto network_list = _alloctor->get_networks();
    if (network_list.empty()) {
        RTC_LOG(LS_WARNING) << "cannot gathering candidate. because network list is empty."
            << ", transport_name: " << _transport_name
            << ", component: " << _component;
        return;
    }

    for (auto network : network_list) {
        UDPPort* port = new UDPPort(_el, _transport_name, _component, _ice_params);
        _ports.push_back(port);

        Candidate c;
        int ret = port->create_ice_candidate(network, _alloctor->min_port(),
            _alloctor->max_port(), c);
        if (ret != 0) {
            continue;
        }

        _local_candidates.push_back(c);

    }
    signal_candidate_allocate_done(this, _local_candidates);
}

} // namespace xrtc

