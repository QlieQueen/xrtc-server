/*
  IceAgent 是对 IceTransportChannel 的管理器，职责：

  ┌───────────────────────────────────────────────┬───────────────────────────────────────────────┐
  │                     方法                      │                     作用                      │
  ├───────────────────────────────────────────────┼───────────────────────────────────────────────┤
  │ create_channel(el, transport_name, component) │ new IceTransportChannel，存入 vector          │
  ├───────────────────────────────────────────────┼───────────────────────────────────────────────┤
  │ get_channel(transport_name, component)        │ 按 (name, component) 查找                     │
  ├───────────────────────────────────────────────┼───────────────────────────────────────────────┤
  │ set_ice_params(...)                           │ 转发到对应 channel                            │
  ├───────────────────────────────────────────────┼───────────────────────────────────────────────┤
  │ gathering_candidate()                         │ 遍历所有 channels，调用 gathering_candidate() │
  ├───────────────────────────────────────────────┼───────────────────────────────────────────────┤
  │ 信号转发                                       │ 接收 channel 的 signal，原样向上转发            │
  └───────────────────────────────────────────────┴───────────────────────────────────────────────┘

*/


#include "ice/ice_agent.h"

#include <algorithm>

namespace xrtc {

IceAgent::IceAgent(EventLoop* el, PortAllocator* allocator) :
    _el(el), _allocator(allocator)
{
}

IceAgent::~IceAgent() {
    for (auto channel : _channels) {
        delete channel;
    }

    _channels.clear();
}

bool IceAgent::create_channel(EventLoop* el, const std::string& transport_name,
    IceCandidateComponent component)
{
    if (get_channel(transport_name, component)) {
        return true;
    }

    auto channel = new IceTransportChannel(el, _allocator, transport_name, component);
    channel->signal_candidate_allocate_done.connect(this,
        &IceAgent::_on_candidate_allocate_done);
    
    _channels.push_back(channel);

    return true;
}

std::vector<IceTransportChannel*>::iterator IceAgent::_get_channel(
        const std::string& transport_name,
        IceCandidateComponent component)
{
    return std::find_if(_channels.begin(), _channels.end(),
        [transport_name, component](IceTransportChannel* channel) {
            return transport_name == channel->transport_name() && 
                component == channel->component();
        });
}

IceTransportChannel* IceAgent::get_channel(const std::string& transport_name,
    IceCandidateComponent component)
{
    auto iter = _get_channel(transport_name, component);
    return iter == _channels.end() ? nullptr : *iter;
}

void IceAgent::set_ice_params(const std::string& transport_name,
    IceCandidateComponent component,
    const IceParameters& ice_params)
{
    auto channel = get_channel(transport_name, component);
    if (channel) {
        channel->set_ice_params(ice_params);
    }
}

void IceAgent::set_remote_ice_params(const std::string& transport_name,
    IceCandidateComponent component,
    const IceParameters& remote_ice_params)
{
    auto channel = get_channel(transport_name, component);
    if (channel) {
        channel->set_remote_ice_params(remote_ice_params);
    }
}

void IceAgent::gathering_candidate() {
    for (auto channel : _channels) {
        channel->gathering_candidate();
    }
}

void IceAgent::_on_candidate_allocate_done(IceTransportChannel* channel,
    const std::vector<Candidate>& candidates)
{
    signal_candidate_allocate_done(this, channel->transport_name(),
        channel->component(), candidates);
}


} // namespace xrtc
