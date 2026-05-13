#include "ice/ice_transport_channel.h"

#include <sstream>
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

void IceTransportChannel::set_remote_ice_params(const IceParameters& remote_ice_params) {
    _remote_ice_params = remote_ice_params;
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
        port->signal_unknown_address.connect(this, &IceTransportChannel::_on_unknown_address);
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

// ============================================================================
// IceTransportChannel::_on_unknown_address — 创建 peer reflexive candidate
//
// 当 UDPPort 收到一个未知对端地址的合法 STUN Binding Request 时触发。
// ICE RFC 5245 将此称为 "peer reflexive candidate" (prflx):
//   服务端从 STUN 消息中提取对端地址和 PRIORITY，构造一个 Candidate，
//   它代表"通过 STUN 发现的、对端可通的实际传输地址"。
//
// 与 local host candidate 的区别:
//   - host candidate: 服务端自己的 IP:Port (local)
//   - prflx candidate: 对端的 IP:Port (remote)，从 STUN 消息中发现的
//
// prflx candidate 和 host candidate 的配对形成 ICE candidate pair，
// 后续 ICE 连接状态机会通过发送/接收 STUN 请求完成连通性检查。
// ============================================================================
void IceTransportChannel::_on_unknown_address(UDPPort* port,
        const rtc::SocketAddress& addr,
        StunMessage* stun_msg,
        const std::string& remote_ufrag)
{
    // 1. 提取 PRIORITY 属性 (RFC 5245 §7.1.2.4): 对端计算的候选地址优先级
    const StunUint32Attribute* priority_attr;
    priority_attr = stun_msg->get_uint32_t(STUN_ATTR_PRIORITY);
    if (!priority_attr) {
        RTC_LOG(LS_WARNING) << to_string() << ": priority not found in the"
             << " binding request message, remote_addr: " << addr.ToString();
        port->send_binding_error_response(stun_msg, addr, STUN_ERROR_BAD_REQUEST,
            STUN_ERROR_REASON_BAD_REQUEST);
        return;
    }

    // 2. 用远程地址信息构造 prflx candidate
    uint32_t remote_priority = priority_attr->value();
    Candidate remote_candidate;
    remote_candidate.component = _component;
    remote_candidate.protocol = "udp";
    remote_candidate.priority = remote_priority;
    remote_candidate.address = addr;
    remote_candidate.username = remote_ufrag;  // 对端的 ICE ufrag
    remote_candidate.password = _remote_ice_params.ice_pwd;  // 对端的 ICE pwd
    remote_candidate.type = PRFLX_PORT_TYPE;  // "prflx"

    RTC_LOG(LS_INFO) << to_string() << "create peer reflexive candidate: "
        << remote_candidate.to_string();

    IceConnection* conn = port->create_connection(remote_candidate);
    if (!conn) {
        RTC_LOG(LS_WARNING) << to_string() << ": create connection from "
            << "peer reflexive candidate error, remote_addr: " << addr.ToString();
        port->send_binding_error_response(stun_msg, addr, STUN_ERROR_SERVER_ERROR,
            STUN_ERROR_REASON_SERVER_ERROR);
    }

    RTC_LOG(LS_INFO) << to_string() << ": create connection from "
        << "peer reflexive candidate success, remote_addr: " << addr.ToString();
}

std::string IceTransportChannel::to_string() {
    std::stringstream ss;
    ss << "Channel[" << this << ":" << _transport_name << ":" << _component
        << "]";
    return ss.str();
}

} // namespace xrtc

