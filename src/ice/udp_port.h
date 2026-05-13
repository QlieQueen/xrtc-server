#ifndef __UDP_PORT_H_
#define __UDP_PORT_H_

#include <map>
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

class IceConnection;

// 按对端地址索引的 IceConnection map
// key=对端 SocketAddress, value=对应的 IceConnection
// 用于去重: 同一对端地址重复收到 STUN 包时复用已有连接
typedef std::map<rtc::SocketAddress, IceConnection*> AddressMap;

class UDPPort : public sigslot::has_slots<> {
public:
    UDPPort(EventLoop* el, const std::string& transport_name,
            IceCandidateComponent component, const IceParameters& ice_params);
    ~UDPPort();

    int create_ice_candidate(Network* network, int min_port, int max_port, Candidate& c);
    bool get_stun_message(const char* buf, size_t len,
            std::unique_ptr<StunMessage>* out_msg,
            const rtc::SocketAddress& addr,
            std::string* out_username);

    int send_to(const char* buf, size_t len, const rtc::SocketAddress& addr);

    const std::string& transport_name() const { return _transport_name; }
    IceCandidateComponent component() const { return _component; }
    const rtc::SocketAddress& local_addr() const { return _local_addr; }
    const std::vector<Candidate>& condidates() const { return _candidates; }
    std::string to_string();
    void send_binding_error_response(StunMessage* stun_msg,
            const rtc::SocketAddress& addr,
            int err_code,
            const std::string& reason);
    IceConnection* create_connection(const Candidate& remote_candidate);

    // STUN binding request 收到且校验通过后，发射此信号通知上层 (IceTransportChannel)
    // 参数: UDPPort自己, 对端地址, 解析好的stun_msg, 对端的remote_ufrag
    // 上层收到后创建 peer reflexive candidate
    sigslot::signal4<UDPPort*, const rtc::SocketAddress&, StunMessage*, const std::string&>
            signal_unknown_address;

private:
    void _on_read_packet(AsyncUdpSocket* socket, char* buf, size_t size,
                        const rtc::SocketAddress& addr, int64_t ts);
    bool _parse_stun_username(StunMessage* stun_msg, std::string* local_ufrag,
                        std::string* remote_ufrag);
private:
    EventLoop* _el;
    std::string _transport_name;
    IceCandidateComponent _component;
    IceParameters _ice_params;
    int _socket = -1;
    std::unique_ptr<AsyncUdpSocket> _async_socket;
    rtc::SocketAddress _local_addr;
    std::vector<Candidate> _candidates;
    AddressMap _connections;    // 按对端地址索引的 IceConnection (去重)
};

} // namespace xrtc

#endif // __UDP_PORT_H_