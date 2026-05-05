#ifndef __ICE_ICE_CREDENTIALS_H_
#define __ICE_ICE_CREDENTIALS_H_

#include <string>

namespace xrtc {

// IceParamters 结构体包含 ICE 的 ufrag（用户名片段）和 pwd（密码），在 SDP 中以 a=ice-ufrag: / a=ice-pwd: 行呈现。
struct IceParameters {
    IceParameters() = default;
    IceParameters(const std::string& ufrag, const std::string& pwd) :
        ice_ufrag(ufrag), ice_pwd(pwd) { }

    std::string ice_ufrag;
    std::string ice_pwd;
};

class IceCredentials {
public:
    static IceParameters create_random_ice_credentials();
};

} // namespace xrtc

#endif // __ICE_ICE_CREDENTIALS_H_