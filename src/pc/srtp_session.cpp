#include "pc/srtp_session.h"

#include <rtc_base/logging.h>

namespace xrtc {

SrtpSession::SrtpSession() {

}

SrtpSession::~SrtpSession() {

}

// 设置发送密钥 — ssrc_any_outbound 匹配任意出口 SSRC
bool SrtpSession::set_send(int cs, const uint8_t* key, size_t key_len,
        const std::vector<int>& extension_ids)
{
    return _set_key(ssrc_any_outbound, cs, key, key_len, extension_ids);
}

// 引用计数 + 懒初始化 libsrtp：只有第一个使用者触发 srtp_init()
// 当前为 stub — 真正的 srtp_init() 在下个 commit 完成
bool SrtpSession::_increment_libsrtp_usage_count_and_maybe_init() {
    return true;
}

// 核心密钥设置 — 检查 session 是否已存在，然后触发 libsrtp 初始化
bool SrtpSession::_set_key(int type, int cs, const uint8_t* key, size_t key_len,
        const std::vector<int>& extension_ids)
{
    if (_session) {
        RTC_LOG(LS_WARNING) << "Failed to create session: "
            << "SRTP session already created";
        return false;
    }

    if (_increment_libsrtp_usage_count_and_maybe_init()) {
        _inited = true;
    } else {
        return false;
    }

    return true;
}    

} // namespace xrtc

