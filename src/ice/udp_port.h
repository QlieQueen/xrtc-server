#ifndef __UDP_PORT_H_
#define __UDP_PORT_H_

#include <vector>
#include <memory>
#include <string>

#include <rtc_base/socket_address.h>
#include <rtc_base/third_party/sigslot/sigslot.h>

#include "base/event_loop.h"
#include "base/network.h"
#include "base/async_udp_socket.h"
#include "ice/ice_credentials.h"
#include "ice/candidate.h"
#include "ice/stun.h"


namespace xrtc {

class UDPPort : public sigslot::has_slots<> {
public:
    UDPPort(EventLoop* el, const std::string& transport_name,
            IceCandidateComponent component, const IceParameters& ice_params);
    ~UDPPort();

    int create_ice_candidate(Network* network, int min_port, int max_port, Candidate& c);
    bool get_stun_message(const char* buf, size_t len,
            std::unique_ptr<StunMessage>* out_msg);

    int send_to(const char* buf, size_t len, const rtc::SocketAddress& addr);

    const std::string& transport_name() const { return _transport_name; }
    IceCandidateComponent component() const { return _component; }
    const rtc::SocketAddress& local_addr() const { return _local_addr; }
    const std::vector<Candidate>& condidates() const { return _candidates; }
    std::string to_string();

private:
    void _on_read_packet(AsyncUdpSocket* socket, char* buf, size_t size,
                        const rtc::SocketAddress& addr, int64_t ts);
private:
    EventLoop* _el;
    std::string _transport_name;
    IceCandidateComponent _component;
    IceParameters _ice_params;
    int _socket = -1;
    std::unique_ptr<AsyncUdpSocket> _async_socket;
    rtc::SocketAddress _local_addr;
    std::vector<Candidate> _candidates;
};

} // namespace xrtc

#endif // __UDP_PORT_H_