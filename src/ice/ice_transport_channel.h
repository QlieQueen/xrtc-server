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
    sigslot::signal<IceTransportChannel*> signal_receiving_state_change;
    sigslot::signal<IceTransportChannel*> signal_writable_state_change;

private:
    void _on_unknown_address(UDPPort* port,
            const rtc::SocketAddress& addr,
            StunMessage* stun_msg,
            const std::string& remote_ufrag);
    void _add_connection(IceConnection* conn);
    void _sort_connections_and_update_state();
    void _maybe_start_pinging();
    void _on_check_and_ping();                      // 定时器回调: 周期性连通性检查
    void _on_connection_state_change(IceConnection* conn);
    void _on_connection_destroyed(IceConnection* conn);    // 连接销毁回调: 清理引用 + selected 重选
    void _ping_connection(IceConnection* conn);
    void _maybe_switch_selected_connection(IceConnection* conn); // 非空包装 → _switch_selected_connection
    void _switch_selected_connection(IceConnection* conn); // 实际切换逻辑, conn 可为 nullptr
    void _update_connection_states();
    void _update_state();
    void _set_receiving(bool receiving);
    void _set_writable(bool writable);

    // libev 定时器回调函数，声明为 friend 以访问私有成员 _on_check_and_ping
    friend void ice_ping_cb(EventLoop* /*el*/, TimerWatcher* /*w*/, void* data);
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
    TimerWatcher* _ping_watcher = nullptr;           // 周期性 ping 定时器 (libev repeating timer)
    int _cur_ping_interval = WEAK_PING_INTERVAL;
    int64_t _last_ping_sent_ms = 0;
    IceConnection* _selected_connection = nullptr;
    bool _receiving = false;
    bool _writable = false;
};


} // namespace xrtc

#endif // __ICE_TRANSPORT_CHANNEL_H_