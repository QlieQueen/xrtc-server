#ifndef __SRTP_SESSION_H_
#define __SRTP_SESSION_H_

#include <vector>
#include <string>

// libsrtp2 核心头文件 — 提供 srtp_ctx_t, srtp_init, srtp_err_status_ok 等
#include <srtp2/srtp.h>

namespace xrtc {

// SRTP 会话 — 封装 libsrtp 的 srtp_ctx_t，管理一个方向的加解密
// 每个 SrtpTransport 持有两个 SrtpSession：send（加密数据包）+ recv（解密数据包）
class SrtpSession {
public:
    SrtpSession();
    ~SrtpSession();

    // 设置发送方向密钥（加密） — _set_key(ssrc_any_outbound, ...)
    bool set_send(int cs, const uint8_t* key, size_t key_len,
            const std::vector<int>& extension_ids);
    bool update_send(int cs, const uint8_t* key, size_t key_len,
            const std::vector<int>& extension_ids);
    bool set_recv(int cs, const uint8_t* key, size_t key_len,
            const std::vector<int>& extension_ids);
    bool update_recv(int cs, const uint8_t* key, size_t key_len,
            const std::vector<int>& extension_ids);

    bool unprotect_rtp(void* p, int in_len, int* out_len);
    bool unprotect_rtcp(void* p, int in_len, int* out_len);

private:
    // 核心：为 type 方向（ssrc_any_outbound / ssrc_any_inbound）设置密码套件+密钥
    bool _set_key(int type, int cs, const uint8_t* key, size_t key_len,
        const std::vector<int>& extension_ids);
    bool _update_key(int type, int cs, const uint8_t* key, size_t key_len,
        const std::vector<int>& extension_ids);
    bool _do_set_key(int type, int cs, const uint8_t* key,
        size_t key_len, const std::vector<int>& /*extension_ids*/);
    // 引用计数 + 懒初始化 libsrtp 全局状态（srtp_init 只能调用一次）
    static bool _increment_libsrtp_usage_count_and_maybe_init();
    // C 回调 thunk — srtp_install_event_handler 注册，从 ev->session 取 user_data 回 this
    static void _event_handle_thunk(srtp_event_data_t* ev);
    // 处理 libsrtp 运行时事件（SSRC 冲突、密钥限制、包序号溢出）
    void _handle_event(srtp_event_data_t* ev); 

private:
    srtp_ctx_t* _session = nullptr;  // libsrtp 会话句柄，srtp_create 后赋值
    bool _inited = false;             // 本会话是否已通过 _set_key 初始化
    int _rtp_auth_tag_len = 0;
    int _rtcp_auth_tag_len = 0;
};


} // namespace xrtc


#endif // __SRTP_SESSION_H_