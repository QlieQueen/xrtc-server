#ifndef __STUN_REQUEST_H_
#define __STUN_REQUEST_H_

#include <map>
#include <rtc_base/third_party/sigslot/sigslot.h> 

#include "ice/stun.h"

namespace xrtc {

class StunRequest;

// ============================================================================
// StunRequestManager — STUN 请求发送管理器
//
// 统一的请求发送入口，后续 commit 会加入定时重传、响应匹配等逻辑。
// ============================================================================
class StunRequestManager {
public:
    StunRequestManager() = default;
    ~StunRequestManager() = default;

    void send(StunRequest* request);

    // STUN 响应匹配: 用 transaction_id 查找对应 request，验证响应类型后分发
    // 返回 true = 找到了对应的 request (无论类型匹配与否)
    bool check_response(StunMessage* msg);

public:
    sigslot::signal3<StunRequest*, const char*, size_t>
            signal_send_packet;

private:
    typedef std::map<std::string, StunRequest*> requestMap;
    requestMap _requests;
};

class IceConnection;

// ============================================================================
// StunRequest — STUN 请求基类
//
// 持有一个 StunMessage，通过 construct() → prepare() 的模板方法模式，
// 让子类只关注消息内容的填充。
//
// 生命周期:
//   1. 子类构造函数中 new StunMessage() 传入基类
//   2. 外部调用 construct() → prepare(msg) 填充字段
//   3. 发送后等待响应，匹配成功时 delete
// ============================================================================
class StunRequest {
public:
    StunRequest(StunMessage* msg);
    ~StunRequest();

    std::string id() { return _msg->transaction_id(); }
    // request type — 用于 check_response 中验证响应类型是否与请求匹配
    uint16_t type() { return _msg->type(); }

    void construct();

    void send();

    int elapsed();

    void set_manager(StunRequestManager* manager);

protected:
    virtual void prepare(StunMessage* msg) { }

    // 响应回调 — 由 StunRequestManager::check_response 根据响应类型分发
    // 子类重写以处理业务逻辑 (如 ConnectionRequest 委托给 IceConnection)
    virtual void on_request_response(StunMessage* msg) { }
    virtual void on_request_error_response(StunMessage* msg) { }

    friend class StunRequestManager;  // 允许 Manager 调用 set_manager()
private:
    StunMessage* _msg;
    StunRequestManager* _manager = nullptr;
    int64_t _ts = 0;
};

// ============================================================================
// ConnectionRequest — ICE 连通性检查请求 (STUN Binding Request)
//
// 每个 ConnectionRequest 对应一次 STUN ping。
// prepare() 负责填充 Binding Request 的各个属性 (后续 commit)。
// ============================================================================
class ConnectionRequest : public StunRequest {
public:
    ConnectionRequest(IceConnection* connection);
    ~ConnectionRequest() = default;

    void prepare(StunMessage* msg) override;
    void on_request_response(StunMessage* msg);
    void on_request_error_response(StunMessage* msg);

private:
    IceConnection* _connection;
};

} // namespace xrtc

#endif // __STUN_REQUEST_H_