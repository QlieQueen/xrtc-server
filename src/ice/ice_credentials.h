#ifndef __ICE_ICE_CREDENTIALS_H_
#define __ICE_ICE_CREDENTIALS_H_

#include <string>

namespace xrtc {

// IceParamters 结构体包含 ICE 的 ufrag（用户名片段）和 pwd（密码），在 SDP 中以 a=ice-ufrag: / a=ice-pwd: 行呈现。
struct IceParameters {
    std::string ice_ufrag;
    std::string ice_pwd;
};

} // namespace xrtc

#endif // __ICE_ICE_CREDENTIALS_H_