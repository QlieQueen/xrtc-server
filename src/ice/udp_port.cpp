#include "ice/udp_port.h"

#include <netinet/in.h>
#include <sstream>
#include <memory>

#include <rtc_base/logging.h>
#include <rtc_base/crc32.h>

#include "base/socket.h"

namespace xrtc {

UDPPort::UDPPort(EventLoop* el, const std::string& transport_name,
        IceCandidateComponent component, const IceParameters& ice_params) :
    _el(el),
    _transport_name(transport_name),
    _component(component),
    _ice_params(ice_params)
{
}

UDPPort::~UDPPort() {
}

static std::string compute_foundation(const std::string& type,
    const std::string& protocol,
    const rtc::SocketAddress& base)
{
    std::stringstream ss;
    ss << type << base.HostAsURIString() << protocol;
    return std::to_string(rtc::ComputeCrc32(ss.str()));
}

int UDPPort::create_ice_candidate(Network* network, int min_port, int max_port,
    Candidate& c)
{
    _socket = create_udp_socket(network->ip().family());
    if (_socket < 0) {
        return -1;
    }

    if (sock_setnonblock(_socket) != 0) {
        close(_socket);
        _socket = -1;
        return -1;
    }

    struct sockaddr_in addr_in;
    memset(&addr_in, 0, sizeof(addr_in));
    addr_in.sin_family = network->ip().family();
    addr_in.sin_addr.s_addr = INADDR_ANY;

    if (sock_bind(_socket, (struct sockaddr*)&addr_in, sizeof(addr_in),
            min_port, max_port) != 0)
    {
        close(_socket);
        _socket = -1;
        return -1;
    }

    int port = 0;
    if (sock_get_address(_socket, nullptr, &port) != 0) {
        close(_socket);
        _socket = -1;
        return -1;
    }

    _local_addr.SetIP(network->ip());
    _local_addr.SetPort(port);

    _async_socket = std::make_unique<AsyncUdpSocket>(_el, _socket);
    _async_socket->signal_read_packet.connect(this,
            &UDPPort::_on_read_packet);
        
    RTC_LOG(LS_INFO) << "prepared socket address: " << _local_addr.ToString();

    c.component = _component;
    c.protocol = "udp";
    c.address = _local_addr;
    c.port = port;
    c.priority = c.get_priority(ICE_TYPE_PREFERENCE_HOST, 0, 0);
    c.username = _ice_params.ice_ufrag;
    c.password = _ice_params.ice_pwd;
    c.type = LOCAL_PORT_TYPE;
    c.foundation = compute_foundation(c.type, c.protocol, c.address);

    _candidates.push_back(c);

    return 0;
}

int UDPPort::send_to(const char* buf, size_t len, const rtc::SocketAddress& addr) {
    if (!_async_socket) {
        return -1;
    }

    return _async_socket->send_to(buf, len, addr);
}

void UDPPort::_on_read_packet(AsyncUdpSocket* socket, char* buf, size_t size,
                const rtc::SocketAddress& addr, int64_t ts)
{

}

std::string UDPPort::to_string() {
    std::stringstream ss;
    ss << "Port[" << this << ":" << _transport_name << ":" << (int)_component
       << ":" << _ice_params.ice_ufrag << ":" << _ice_params.ice_pwd 
       << ":" << _local_addr.ToString() << "]";
    return ss.str();
}

} // namespace xrtc