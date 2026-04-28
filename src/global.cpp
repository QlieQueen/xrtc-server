#include "base/log.h"
#include "base/conf.h"
#include "server/rtc_server.h"
#include "server/signaling_server.h"

xrtc::GeneralConf* g_conf = nullptr;
xrtc::XrtcLog* g_log = nullptr;
xrtc::SignalingServer* g_signaling_server = nullptr;
xrtc::RtcServer* g_rtc_server = nullptr;
