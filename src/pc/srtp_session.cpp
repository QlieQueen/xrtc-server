#include "pc/srtp_session.h"

#include <rtc_base/logging.h>
#include <rtc_base/synchronization/mutex.h>
#include <absl/base/attributes.h>

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

// 全局引用计数 + 互斥锁 — 保证 srtp_init() 只被调用一次
ABSL_CONST_INIT int g_libsrtp_usage_count = 0;
ABSL_CONST_INIT webrtc::GlobalMutex g_libsrtp_lock(absl::kConstInit);

// C 回调 thunk — srtp_install_event_handler 注册的全局回调
// libsrtp 触发事件时调用，从 ev->session 取回 srtp_set_user_data 保存的 this
void SrtpSession::_event_handle_thunk(srtp_event_data_t* ev) {
    SrtpSession* session = (SrtpSession*)(srtp_get_user_data(ev->session));
    if (session) {
        session->_handle_event(ev);
    }
}

// 处理 libsrtp 运行时事件 — SSRC 冲突 / 密钥软硬限制 / 包序号溢出
void SrtpSession::_handle_event(srtp_event_data_t* ev) {
    switch (ev->event) {
        case event_ssrc_collision:
            RTC_LOG(LS_INFO) << "SRTP event: ssrc collision";
            break;
        case event_key_soft_limit:
            RTC_LOG(LS_INFO) << "SRTP event: reached key soft limit";
            break;
        case event_key_hard_limit:
            RTC_LOG(LS_INFO) << "SRTP event: reached key hard limit";
            break;
        case event_packet_index_limit:
            RTC_LOG(LS_INFO) << "SRTP event: packet index limit";
            break;
        default:
            RTC_LOG(LS_WARNING) << "SRTP unknown event: " << ev->event;
            break;
    }
}


// 引用计数 + 懒初始化 libsrtp：只有第一个使用者触发 srtp_init()
bool SrtpSession::_increment_libsrtp_usage_count_and_maybe_init() {
    webrtc::GlobalMutexLock ls(&g_libsrtp_lock);

    if (0 == g_libsrtp_usage_count) {
        int err = srtp_init();
        if (err != srtp_err_status_ok) {
            RTC_LOG(LS_WARNING) << "Failed to init srtp, err: " << err;
            return false;
        }

        err = srtp_install_event_handler(&SrtpSession::_event_handle_thunk);
        if (err != srtp_err_status_ok) {
            RTC_LOG(LS_WARNING) << "Failed to install srtp event, err: " << err;
            return false;
        }
    }

    g_libsrtp_usage_count++;
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

