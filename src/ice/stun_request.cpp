#include "ice/stun_request.h"

#include <rtc_base/helpers.h>
#include <rtc_base/byte_buffer.h>
#include <rtc_base/logging.h>
#include <rtc_base/time_utils.h>

#include "ice/ice_connection.h"

namespace xrtc {

StunRequestManager::~StunRequestManager() {
    while (_requests.begin() != _requests.end()) {
        StunRequest* request = _requests.begin()->second;
        _requests.erase(_requests.begin());
        delete request;
    }
}

// ============================================================================
// StunRequestManager::send — 发送 STUN 请求
//
// 流程:
//   1. construct() → prepare()  填充消息字段
//   2. set_manager()             记录管理器指针 (请求通过它发射信号)
//   3. _requests[id] = request   存入 map，后续响应匹配用
//   4. request->send()           序列化消息 → 发射 signal_send_packet
// ============================================================================
void StunRequestManager::send(StunRequest* request) {
    request->construct();
    request->set_manager(this);
    _requests[request->id()] = request;
    request->send();
}

void StunRequestManager::remove(StunRequest* request) {
    auto iter = _requests.find(request->id());
    if (iter != _requests.end()) {
        _requests.erase(iter);
    }
}

// ============================================================================
// StunRequestManager::check_response — STUN 响应匹配与分发
//
// 流程:
//   1. 用 msg 的 transaction_id 在 _requests map 中查找对应的 request
//   2. 找到: 用 class 位掩码验证响应类型 (Success/Error) 与请求类型一致
//   3. 类型匹配 → 调用虚函数 on_response/on_error_response 分发给子类
//   4. 类型不匹配 → 仅打 warning 日志，不做分发
//
// 返回值: true=找到了对应的 request (无论类型是否匹配)
//         false=没有找到 (调用方自行处理 orphan response)
//
// class 位运算原理:
//   Request 的 class bits 全为 0 (STUN_CLASS_REQUEST = 0x000)
//   成功响应 class = 0x100, 错误响应 class = 0x110
//   get_stun_success_response: (req_type & ~0x0110) | 0x100  → 把 class 位替换为 Success
//   get_stun_error_response:  (req_type & ~0x0110) | 0x110  → 把 class 位替换为 Error
// ============================================================================
bool StunRequestManager::check_response(StunMessage* msg) {
    auto iter = _requests.find(msg->transaction_id());
    if (iter == _requests.end()) {
        return false;
    }

    StunRequest* request = iter->second;

    if (msg->type() == get_stun_success_response(request->type())) {
        request->on_request_response(msg);
    } else if (msg->type() == get_stun_error_response(request->type())) {
        request->on_request_error_response(msg);
    } else {
        RTC_LOG(LS_WARNING) << "Received Stun Binding response with wrong type="
            << msg->type() << ", id=" << msg->transaction_id();
        delete request;
        return false;
    }

    delete request;
    return true;
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
// TODO: 改为 Regular Nomination — 只对 selected connection 的 ping 带 USE-CANDIDATE，
//       探活其他 candidate pair 的 ping 不带。当前 Aggressive Nomination 在单网口
//       场景下无实际影响（仅 1 个 IceConnection），多网卡时会导致客户端被探活 ping
//       误导切到非 selected 的 pair。
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

// ============================================================================
// ConnectionRequest::on_response / on_error_response — 响应分发到 IceConnection
//
// StunRequestManager 匹配到响应后，通过虚函数回调此处。
// 不做业务处理，直接委托给 IceConnection 的方法 (后续 commit 实现 RTT 计算等)。
// ============================================================================
void ConnectionRequest::on_request_response(StunMessage* msg) {
    _connection->on_connection_request_response(this, msg);
}

void ConnectionRequest::on_request_error_response(StunMessage* msg) {
    _connection->on_connection_request_error_response(this, msg);
}

// 构造函数: 为消息生成随机的 transaction_id (96-bit = 12 bytes)
// 用于后续 STUN 响应匹配 (ping id == pong transaction id)
StunRequest::StunRequest(StunMessage* msg) :
    _msg(msg)
{
    _msg->set_transaction_id(rtc::CreateRandomString(k_stun_transaction_id_length));
}

StunRequest::~StunRequest() {
    if (_manager) {
        _manager->remove(this);
    }

    if (_msg) {
        delete _msg;
        _msg = nullptr;
    }
}

// construct() → prepare(): 模板方法，子类只需重写 prepare 填充消息字段
void StunRequest::construct() {
    prepare(_msg);
}

// ============================================================================
// StunRequest::send — 序列化消息并发射信号
//
// write() 将 StunMessage 转为网络字节序 → 通过 _manager 的 signal_send_packet
// 发射到上层 (IceConnection)，由 IceConnection 通过 UDPPort 发出。
// ============================================================================
void StunRequest::send() {
    _ts = rtc::TimeMillis();
    rtc::ByteBufferWriter buf;
    if (!_msg->write(&buf)) {
        return;
    }
    _manager->signal_send_packet(this, buf.Data(), buf.Length());
}


int StunRequest::elapsed() {
    return rtc::TimeMillis() - _ts;
}

void StunRequest::set_manager(StunRequestManager* manager) {
    _manager = manager;
}

} // namespace xrtc
