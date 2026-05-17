#ifndef __ICE_TRANSPORT_CHANNEL_H_
#define __ICE_TRANSPORT_CHANNEL_H_

#include <vector>
#include <rtc_base/third_party/sigslot/sigslot.h>

#include "base/event_loop.h"
#include "ice/ice_def.h"
#include "ice/port_allocator.h"
#include "ice/candidate.h"
#include "ice/ice_credentials.h"
#include "ice/udp_port.h"
#include "ice/ice_controller.h"


namespace xrtc {

class IceTransportChannel : public sigslot::has_slots<> {
public:
    IceTransportChannel(EventLoop* el, PortAllocator* alloctor,
            const std::string& transport_name,
            IceCandidateComponent component);
    virtual ~IceTransportChannel();

    void set_ice_params(const IceParameters& ice_params);
    void set_remote_ice_params(const IceParameters& remote_ice_params);
    IceParameters remote_ice_params() const { return _remote_ice_params; }
    const std::string& transport_name() { return _transport_name; }
    IceCandidateComponent component() { return _component; }
    void gathering_candidate();
    std::string to_string();

public:
    sigslot::signal2<IceTransportChannel*, const std::vector<Candidate>&>
        signal_candidate_allocate_done;

private:
    void _on_unknown_address(UDPPort* port,
            const rtc::SocketAddress& addr,
            StunMessage* stun_msg,
            const std::string& remote_ufrag);
    void _add_connection(IceConnection* conn);
    void _sort_connections_and_update_state();
    void _maybe_start_pinging();

private:
    EventLoop* _el;
    std::string _transport_name;
    IceCandidateComponent _component;
    PortAllocator* _alloctor;
    IceParameters _ice_params;
    IceParameters _remote_ice_params;
    std::vector<Candidate> _local_candidates;
    std::vector<UDPPort*> _ports;
    std::unique_ptr<IceController> _ice_controller;  // 连接选择器: 决定 ping 谁、选谁
    bool _start_pinging = false;                     // 连通性检查是否已启动 (只启动一次)
};


} // namespace xrtc

#endif // __ICE_TRANSPORT_CHANNEL_H_