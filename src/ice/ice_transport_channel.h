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

public:
    sigslot::signal2<IceTransportChannel*, const std::vector<Candidate>&>
        signal_candidate_allocate_done;
private:
    EventLoop* _el;
    std::string _transport_name;
    IceCandidateComponent _component;
    PortAllocator* _alloctor;
    IceParameters _ice_params;
    IceParameters _remote_ice_params;
    std::vector<Candidate> _local_candidates;
    std::vector<UDPPort*> _ports;
};


} // namespace xrtc

#endif // __ICE_TRANSPORT_CHANNEL_H_