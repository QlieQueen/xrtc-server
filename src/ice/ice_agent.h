#ifndef __ICE_AGENT_H_
#define __ICE_AGENT_H_

#include <vector>
#include <string>
#include <rtc_base/third_party/sigslot/sigslot.h>

#include "base/event_loop.h"
#include "ice/ice_def.h"
#include "ice/port_allocator.h"
#include "ice/ice_transport_channel.h"
#include "ice/ice_credentials.h"

namespace xrtc {

class IceAgent : public sigslot::has_slots<> {
public:
    IceAgent(EventLoop* el, PortAllocator* allocator);
    ~IceAgent();

    bool create_channel(EventLoop* el, const std::string& transport_name,
            IceCandidateComponent component);
    IceTransportChannel* get_channel(const std::string& transport_name,
            IceCandidateComponent component);

    void set_ice_params(const std::string& transport_name,
            IceCandidateComponent component,
            const IceParameters& ice_params);
    void set_remote_ice_params(const std::string& transport_name,
            IceCandidateComponent component,
            const IceParameters& remote_ice_params);
    void gathering_candidate();
    IceTransportState ice_state() { return _ice_state; }

public:
    sigslot::signal4<IceAgent*, const std::string&, IceCandidateComponent,
        const std::vector<Candidate>&> signal_candidate_allocate_done;
    sigslot::signal2<IceAgent*, IceTransportState> signal_ice_state;

private:
    // 接收 IceTransportChannel 的信号
    void _on_candidate_allocate_done(IceTransportChannel* channel,
            const std::vector<Candidate>& candidates);
    std::vector<IceTransportChannel*>::iterator _get_channel(
            const std::string& transport_name,
            IceCandidateComponent component);
    void _on_receiving_state_change(IceTransportChannel* /*channel*/);
    void _on_writable_state_change(IceTransportChannel* /*channel*/);
    void _on_ice_state_change(IceTransportChannel* /*channel*/);
    void _update_state();

private:
    EventLoop* _el;
    std::vector<IceTransportChannel*> _channels;
    PortAllocator* _allocator;
    IceTransportState _ice_state = IceTransportState::k_new;
};

} // namespace xrtc 


#endif // __ICE_AGENT_H_