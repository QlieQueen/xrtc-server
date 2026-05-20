#include "ice/ice_connection.h"

#include <rtc_base/logging.h>

#include "ice/stun_request.h"

namespace xrtc {

IceConnection::IceConnection(EventLoop* el, UDPPort* port, const Candidate& remote_candidate) :
    _el(el), _port(port), _remote_candidate(remote_candidate) { }

IceConnection::~IceConnection() { }

void IceConnection::handle_stun_binding_request(StunMessage* stun_msg) {
    // role的冲突问题，ice角色： controlling control（为什么xrtc-server不存在角色冲突问题）

    send_stun_binding_response(stun_msg);
}

void IceConnection::send_stun_binding_response(StunMessage* stun_msg) {
    const StunByteStringAttribute* username_attr = stun_msg->get_byte_string(
            STUN_ATTR_USERNAME);
    if (!username_attr) {
        RTC_LOG(LS_WARNING) << "send stun binding response error: no username";
        return;
    }

    StunMessage response;
    response.set_type(STUN_BINDING_RESPONSE);
    response.set_transaction_id(stun_msg->transaction_id());
    response.add_attribute(std::make_unique<StunXorAddressAttribute>(
        STUN_ATTR_XOR_MAPPED_ADDRESS,
        remote_candidate().address));
    response.add_message_integrity(_port->ice_pwd());
    response.add_fingerprint();

    send_response_message(response);
}

void IceConnection::send_response_message(const StunMessage& response) {
    const rtc::SocketAddress& addr = _remote_candidate.address;

    rtc::ByteBufferWriter buf;
    if (!response.write(&buf)) {
        return;
    }

    int ret = _port->send_to(buf.Data(), buf.Length(), addr);
    if (ret < 0) {
        RTC_LOG(LS_WARNING) << to_string() << ": send "
            << stun_method_to_string(response.type())
            << " error, to " << addr.ToString()
            << ", id=" << rtc::hex_encode(response.transaction_id());
        return;
    }

    RTC_LOG(LS_INFO) << to_string() << ": sent "
        << stun_method_to_string(response.type())
        << " to " << addr.ToString()
        << ", id=" << rtc::hex_encode(response.transaction_id());
}

void IceConnection::on_read_packet(const char* buf, size_t len, int64_t ts) {
    std::unique_ptr<StunMessage> stun_msg;
    std::string remote_ufrag;
    const Candidate& remote = _remote_candidate;
    if (!_port->get_stun_message(buf, len, remote.address, &stun_msg, &remote_ufrag)) {
        // 这个不是stun包，可能是其他的，比如dtls或者rtp包
    } else if (!stun_msg) {

    } else { // stun_msg
        switch (stun_msg->type()) {
            case STUN_BINDING_REQUEST: {
                if (remote_ufrag != remote.username) {
                    RTC_LOG(LS_WARNING) << to_string() << ": Received "
                        << stun_method_to_string(stun_msg->type())
                        << " with bad username=" << remote_ufrag
                        << " id=" << rtc::hex_encode(stun_msg->transaction_id());
                    _port->send_binding_error_response(stun_msg.get(),
                            remote.address, STUN_ERROR_UNAUTHORIZED, 
                            STUN_ERROR_REASON_UNAUTHORIZED);
                } else {
                    RTC_LOG(LS_INFO) << to_string() << ": Received "
                        << stun_method_to_string(stun_msg->type())
                        << " id=" << rtc::hex_encode(stun_msg->transaction_id());
                    handle_stun_binding_request(stun_msg.get());
                }
                break;
            }
            default:
                break;
        }
    }
}

// ============================================================================
// IceConnection::maybe_set_remote_ice_params — 补填远程 ICE 密码
//
// 场景: STUN binding request 到达时，从 USERNAME 提取了客户端的 ufrag，
//       remote_candidate.username 已知，但 password 还是空的。
//       ANSWER SDP 到达后，set_remote_ice_params 被调用，
//       根据已匹配的 ufrag 把 password 补填进去。
//       此后该连接满足"远程凭据完整"条件，可以被 ping。
// ============================================================================
void IceConnection::maybe_set_remote_ice_params(const IceParameters& ice_params) {
    if (_remote_candidate.username == ice_params.ice_ufrag &&
            _remote_candidate.password.empty())
    {
        _remote_candidate.password = ice_params.ice_pwd;
    }
}

bool IceConnection::stable(int64_t now) const {
    // todo
    return false;
}

void IceConnection::ping(int64_t now) {
    ConnectionRequest* request = new ConnectionRequest(this);    // 记得在收到对应的response的时候进行delete回收
    _pings_since_last_responses.push_back(SentPing(request->id(), now));
}

std::string IceConnection::to_string() {
    std::stringstream ss;
    ss << "Conn[" << this << ":" << _port->transport_name()
        << ":" << _port->component()
        << ":" << _port->local_addr().ToString()
        << ":" << _remote_candidate.address.ToString();
    return ss.str();
}


} // namespace xrtc
