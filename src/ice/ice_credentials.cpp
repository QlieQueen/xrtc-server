#include "ice/ice_credentials.h"
#include "ice/ice_def.h"

#include <rtc_base/helpers.h>

namespace xrtc {

IceParameters IceCredentials::create_random_ice_credentials() {
    return {rtc::CreateRandomString(ICE_UFRAG_LENGTH),
            rtc::CreateRandomString(ICE_PWD_LENGTH)};
}

} // namespace xrtc

