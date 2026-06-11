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

// 更新发送密钥 (re-keying) — _update_key 要求 _session 已存在
bool SrtpSession::update_send(int cs, const uint8_t* key, size_t key_len,
        const std::vector<int>& extension_ids)
{
    return _update_key(ssrc_any_outbound, cs, key, key_len, extension_ids);
}

// 设置接收方向密钥（解密） — _set_key(ssrc_any_inbound, ...) 创建 recv session
bool SrtpSession::set_recv(int cs, const uint8_t* key, size_t key_len,
        const std::vector<int>& extension_ids)
{
    return _set_key(ssrc_any_inbound, cs, key, key_len, extension_ids);
}

// 更新接收密钥 (re-keying) — _update_key 要求 _session 已存在
bool SrtpSession::update_recv(int cs, const uint8_t* key, size_t key_len,
        const std::vector<int>& extension_ids)
{
    return _update_key(ssrc_any_inbound, cs, key, key_len, extension_ids);
}

// unprotect_rtp — 原地解密 RTP 包 (libsrtp: srtp_unprotect)
// *out_len 返回解密后的实际长度 (= in_len - auth_tag_len)
bool SrtpSession::unprotect_rtp(void* p, int in_len, int* out_len) {
    if (!_session) {
        return false;
    }
    *out_len = in_len;
    return srtp_err_status_ok == srtp_unprotect(_session, p, out_len);
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

// _update_key — update_send/update_recv 的共同实现
// 与 _set_key 不同: 不检查 _inited/lib-srtp-init, 但要求 _session 已存在
// _do_set_key 看到 _session 不为空 → 走 srtp_update 分支
bool SrtpSession::_update_key(int type, int cs, const uint8_t* key, size_t key_len,
        const std::vector<int>& extension_ids)
{
    if (!_session) {
        RTC_LOG(LS_WARNING) << "Failed to update on non-exsiting SRTP session";
        return false;
    }

    return _do_set_key(type, cs, key, key_len, extension_ids);
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

    return _do_set_key(type, cs, key, key_len, extension_ids);
}

// _do_set_key — 构造 srtp_policy_t 并创建/更新 libsrtp 会话上下文
//
// 填充流程:
//   1. 从 crypto suite 编号自动填充 policy.rtp / policy.rtcp (密码器+认证函数+密钥长度)
//   2. 校验 key 长度匹配密码套件要求的 cipher_key_len
//   3. 设置 SSRC 方向 (ssrc_any_outbound 加密 / ssrc_any_inbound 解密)
//   4. 首次调用 srtp_create 创建 _session, 后续调用 srtp_update 更新密钥
//   5. 记录 auth_tag_len — 后续解密时需要确定 RTP/RTCP 包尾部的认证标签长度
bool SrtpSession::_do_set_key(int type, int cs, const uint8_t* key,
        size_t key_len, const std::vector<int>& /*extension_ids*/)
{
    srtp_policy_t policy;
    memset(&policy, 0, sizeof(policy));

    bool rtp_ret = srtp_crypto_policy_set_from_profile_for_rtp(
            &policy.rtp, (srtp_profile_t)cs);
    bool rtcp_ret = srtp_crypto_policy_set_from_profile_for_rtcp(
            &policy.rtcp, (srtp_profile_t)cs);

    if (rtp_ret != srtp_err_status_ok || rtcp_ret != srtp_err_status_ok) {
        RTC_LOG(LS_WARNING) << "SRTP session " << (_session ? "create" : "update")
            << " failed: unsupported crypto suite " << cs;
        return false;
    }

    if (!key || key_len != (size_t)policy.rtp.cipher_key_len) {
        RTC_LOG(LS_WARNING) << "SRTP session " << (_session ? "create" : "update")
            << " failed: invalid key";
        return false;
    }

    policy.ssrc.type = (srtp_ssrc_type_t)type;
    policy.ssrc.value = 0;

    policy.key = (uint8_t*)key;

    policy.window_size = 1024;
    policy.allow_repeat_tx = 1;
    policy.next = nullptr;

    if (!_session) {
        int err = srtp_create(&_session, &policy);
        if (err != srtp_err_status_ok) {
            RTC_LOG(LS_WARNING) << "Failed to create srtp, err: " << err;
            _session = nullptr;
            return false;
        }
        srtp_set_user_data(_session, this);
    } else {
        int err = srtp_update(_session, &policy);
        if (err != srtp_err_status_ok) {
            RTC_LOG(LS_WARNING) << "Failed to update srtp, err: " << err;
            return false;
        }
    }

    _rtp_auth_tag_len = policy.rtp.auth_tag_len;
    _rtcp_auth_tag_len = policy.rtcp.auth_tag_len;

    return true;
}


} // namespace xrtc

