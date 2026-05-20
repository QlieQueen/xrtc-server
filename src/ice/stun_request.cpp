#include "ice/stun_request.h"

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

void ConnectionRequest::prepare(StunMessage* msg) {
    // todo: 后续 commit 填充 STUN Binding Request 各属性
}

StunRequest::StunRequest(StunMessage* msg) :
    _msg(msg) { }

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
