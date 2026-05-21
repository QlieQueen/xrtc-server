#include "ice/stun_request.h"

#include <rtc_base/helpers.h>

#include "ice/ice_connection.h"

namespace xrtc {

// ============================================================================
// StunRequestManager::send — 发送 STUN 请求
//
// 当前直接调用 construct()，后续 commit 加入定时重传和响应匹配。
// ============================================================================
void StunRequestManager::send(StunRequest* request) {
    request->construct();
}

// ============================================================================
// ConnectionRequest — ICE 连通性检查请求
//
// 构造函数: 创建空白 StunMessage，关联到对应 IceConnection。
// prepare(): 填充 Binding Request 属性 (后续 commit 实现)。
// ============================================================================
ConnectionRequest::ConnectionRequest(IceConnection* connection) :
    StunRequest(new StunMessage()), _connection(connection) {}

// ============================================================================
// ConnectionRequest::prepare — 填充 STUN Binding Request 属性
//
// 构造一个完整的连通性检查请求，采用积极提名策略 (每包都带 USE-CANDIDATE):
//   1. TYPE: STUN_BINDING_REQUEST (0x0001)
//   2. USERNAME: remote_ufrag:local_ufrag (对端在前)
//   3. ICE_CONTROLLING: tiebreaker = 0 (xrtc-server 始终 controlling，无角色冲突)
//   4. USE_CANDIDATE: 空属性 (0 字节)，标记积极提名
//   5. PRIORITY: 对端 prflx 优先级 = type_pref(高8) | local_priority(低24)
//   6. MESSAGE-INTEGRITY: HMAC-SHA1(remote_ice_pwd, message)
//   7. FINGERPRINT: CRC32(message) ^ 0x5354554E
// ============================================================================
void ConnectionRequest::prepare(StunMessage* msg) {
    msg->set_type(STUN_BINDING_REQUEST);

    // USERNAME: remote_ufrag:local_ufrag
    std::string username;
    _connection->port()->create_stun_username(
            _connection->remote_candidate().username, &username);
    msg->add_attribute(std::make_unique<StunByteStringAttribute>(
                STUN_ATTR_USERNAME, username));

    // ICE_CONTROLLING: 64位 tiebreaker，server 始终 controlling，填 0
    msg->add_attribute(std::make_unique<StunUint64Attribute>(
                STUN_ATTR_ICE_CONTROLLING, 0));

    // USE_CANDIDATE: 积极提名标志，告诉对端用这个 pair 通信
    msg->add_attribute(std::make_unique<StunByteStringAttribute>(
                STUN_ATTR_USE_CANDIDATE, 0));

    // PRIORITY: prflx = type_pref << 24 | (local_priority & 0x00FFFFFF)
    int type_pref = ICE_TYPE_PREFERENCE_PRFLX;
    uint32_t prflx_priority = (type_pref << 24) |
        (_connection->local_candidate().priority & 0x00FFFFFF);
    msg->add_attribute(std::make_unique<StunUint32Attribute>(
            STUN_ATTR_PRIORITY, prflx_priority));

    // 消息完整性: HMAC-SHA1(remote_ice_pwd, message)
    msg->add_message_integrity(_connection->remote_candidate().password);
    // 指纹: CRC32
    msg->add_fingerprint();
}

// 构造函数: 为消息生成随机的 transaction_id (96-bit = 12 bytes)
// 用于后续 STUN 响应匹配 (ping id == pong transaction id)
StunRequest::StunRequest(StunMessage* msg) :
    _msg(msg)
{
    _msg->set_transaction_id(rtc::CreateRandomString(k_stun_transaction_id_length));
}

StunRequest::~StunRequest() {
    if (_msg) {
        delete _msg;
        _msg = nullptr;
    }
}

// construct() → prepare(): 模板方法，子类只需重写 prepare 填充消息字段
void StunRequest::construct() {
    prepare(_msg);
}


} // namespace xrtc
