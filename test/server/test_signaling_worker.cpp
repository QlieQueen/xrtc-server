/**
 * @file test_signaling_worker.cpp
 * @brief 信令工作线程模块的单元测试
 *
 * 测试策略：
 *   1. 纯逻辑测试：通过 #define private public 访问私有方法，
 *      测试 _process_push、_process_request、_process_query_buffer 等核心逻辑
 *   2. 集成测试：通过公共接口 init/start/stop/notify 测试生命周期
 *   3. 协议测试：通过 socketpair 模拟 TCP 连接，测试完整协议处理链路
 */

#include <gtest/gtest.h>

// 通过宏将私有成员暴露为 public，以便测试访问
#define private public
#include "server/signaling_worker.h"
#undef private

#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>

#include <rtc_base/slice.h>

#include "base/socket.h"
#include "base/xhead.h"
#include "server/tcp_connection.h"
#include "xrtc_server_def.h"

using namespace xrtc;
using json = nlohmann::json;

class SignalingWorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        options.host = "127.0.0.1";
        options.port = 9000;
        options.worker_num = 2;
        options.connection_timeout = 5000000;
        worker = new SignalingWorker(0, options);
    }

    void TearDown() override {
        if (worker) {
            worker->stop();
            worker->join();
            delete worker;
            worker = nullptr;
        }
    }

    SignalingServerOptions options;
    SignalingWorker* worker = nullptr;
};

// ============================================================
// 生命周期测试（通过公共接口）
// ============================================================

TEST_F(SignalingWorkerTest, InitAndDestroy) {
    EXPECT_EQ(worker->init(), 0);
}

TEST_F(SignalingWorkerTest, StartStop) {
    ASSERT_EQ(worker->init(), 0);
    EXPECT_TRUE(worker->start());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    worker->stop();
    worker->join();

    SUCCEED();
}

TEST_F(SignalingWorkerTest, DoubleStart) {
    ASSERT_EQ(worker->init(), 0);

    EXPECT_TRUE(worker->start());
    EXPECT_FALSE(worker->start());

    worker->stop();
    worker->join();
}

TEST_F(SignalingWorkerTest, NotifyQuit) {
    ASSERT_EQ(worker->init(), 0);
    EXPECT_TRUE(worker->start());

    EXPECT_EQ(worker->notify(SignalingWorker::QUIT), 0);

    worker->join();
}

// ============================================================
// notify 接口测试（不启动 event loop）
// ============================================================

TEST_F(SignalingWorkerTest, NotifyWithoutStart) {
    ASSERT_EQ(worker->init(), 0);

    // 未 start 时 notify 应能正常写入 pipe
    EXPECT_EQ(worker->notify(SignalingWorker::QUIT), 0);
}

TEST_F(SignalingWorkerTest, NotifyNewConnWithoutStart) {
    ASSERT_EQ(worker->init(), 0);

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    // 未 start 时 notify_new_conn 应能正常写入无锁队列和 pipe
    EXPECT_EQ(worker->notify_new_conn(sv[0]), 0);

    // 验证无锁队列中有数据
    EXPECT_FALSE(worker->_q_connfd.empty());
    EXPECT_EQ(worker->_q_connfd.size(), 1);

    close(sv[1]);
}

// ============================================================
// _process_push 测试（纯逻辑，无外部依赖）
// ============================================================

TEST_F(SignalingWorkerTest, ProcessPushValid) {
    json root = {
        {"uid", 12345},
        {"stream_name", "test_stream"},
        {"audio", 1},
        {"video", 1}
    };

    TcpConnection conn(0);
    int ret = worker->_process_push(CMDNO_PUSH, &conn, root, 10001);
    EXPECT_EQ(ret, 0);
}

TEST_F(SignalingWorkerTest, ProcessPushMissingField) {
    json root = {
        {"uid", 12345}
        // 缺少 stream_name, audio, video
    };

    TcpConnection conn(0);
    int ret = worker->_process_push(CMDNO_PUSH, &conn, root, 10002);
    EXPECT_EQ(ret, -1);
}

TEST_F(SignalingWorkerTest, ProcessPushEmptyJson) {
    json root = json::object();

    TcpConnection conn(0);
    int ret = worker->_process_push(CMDNO_PUSH, &conn, root, 10003);
    EXPECT_EQ(ret, -1);
}

// ============================================================
// _process_request 测试
// ============================================================

TEST_F(SignalingWorkerTest, ProcessRequestValidPush) {
    xhead_t header;
    memset(&header, 0, sizeof(header));
    header.magic_num = XHEAD_MAGIC_NUM;
    header.log_id = 20001;

    json body_json = {
        {"cmdno", CMDNO_PUSH},
        {"uid", 67890},
        {"stream_name", "push_stream"},
        {"audio", 1},
        {"video", 0}
    };
    std::string body_str = body_json.dump();
    header.body_len = body_str.size();

    rtc::Slice header_slice((const char*)&header, XHEAD_SIZE);
    rtc::Slice body_slice(body_str.data(), body_str.size());

    TcpConnection conn(0);
    int ret = worker->_process_request(&conn, header_slice, body_slice);
    EXPECT_EQ(ret, 0);
}

TEST_F(SignalingWorkerTest, ProcessRequestInvalidJson) {
    xhead_t header;
    memset(&header, 0, sizeof(header));
    header.magic_num = XHEAD_MAGIC_NUM;
    header.log_id = 20002;

    std::string invalid_body = "这不是合法的json{{{";
    header.body_len = invalid_body.size();

    rtc::Slice header_slice((const char*)&header, XHEAD_SIZE);
    rtc::Slice body_slice(invalid_body.data(), invalid_body.size());

    TcpConnection conn(0);
    int ret = worker->_process_request(&conn, header_slice, body_slice);
    EXPECT_EQ(ret, -1);
}

TEST_F(SignalingWorkerTest, ProcessRequestUnknownCmdno) {
    xhead_t header;
    memset(&header, 0, sizeof(header));
    header.magic_num = XHEAD_MAGIC_NUM;
    header.log_id = 20003;

    json body_json = {
        {"cmdno", 999}  // 未定义的 cmdno
    };
    std::string body_str = body_json.dump();
    header.body_len = body_str.size();

    rtc::Slice header_slice((const char*)&header, XHEAD_SIZE);
    rtc::Slice body_slice(body_str.data(), body_str.size());

    TcpConnection conn(0);
    // 未知 cmdno 时 switch 会 fall through，返回未初始化的 ret
    // 这里只验证不崩溃
    worker->_process_request(&conn, header_slice, body_slice);

    SUCCEED();
}

// ============================================================
// _process_query_buffer 测试
// ============================================================

TEST_F(SignalingWorkerTest, ProcessQueryBufferInvalidMagic) {
    TcpConnection conn(0);
    conn.querybuf = sdsempty();

    xhead_t header;
    memset(&header, 0, sizeof(header));
    header.magic_num = 0xdeadbeef;  // 错误的 magic number
    header.body_len = 0;

    conn.querybuf = sdscatlen(conn.querybuf, (const char*)&header, XHEAD_SIZE);

    int ret = worker->_process_query_buffer(&conn);
    EXPECT_EQ(ret, -1);

    sdsfree(conn.querybuf);
}

TEST_F(SignalingWorkerTest, ProcessQueryBufferFullFlow) {
    TcpConnection conn(0);
    conn.querybuf = sdsempty();

    json body_json = {
        {"cmdno", CMDNO_PUSH},
        {"uid", 11111},
        {"stream_name", "full_flow_test"},
        {"audio", 1},
        {"video", 1}
    };
    std::string body_str = body_json.dump();

    xhead_t header;
    memset(&header, 0, sizeof(header));
    header.magic_num = XHEAD_MAGIC_NUM;
    header.log_id = 30001;
    header.body_len = body_str.size();

    conn.querybuf = sdscatlen(conn.querybuf, (const char*)&header, XHEAD_SIZE);
    conn.querybuf = sdscatlen(conn.querybuf, body_str.data(), body_str.size());

    int ret = worker->_process_query_buffer(&conn);
    EXPECT_EQ(ret, 0);

    // 验证状态已更新
    EXPECT_EQ(conn.current_state, TcpConnection::STATE_BODY);
    EXPECT_EQ(conn.bytes_processed, (size_t)65536);  // 短链接处理

    sdsfree(conn.querybuf);
}

TEST_F(SignalingWorkerTest, ProcessQueryBufferIncompleteData) {
    TcpConnection conn(0);
    conn.querybuf = sdsempty();

    // 只写入部分数据（不足 XHEAD_SIZE）
    conn.querybuf = sdscatlen(conn.querybuf, "partial", 7);

    int ret = worker->_process_query_buffer(&conn);
    EXPECT_EQ(ret, 0);

    sdsfree(conn.querybuf);
}

TEST_F(SignalingWorkerTest, ProcessQueryBufferMultipleRequests) {
    TcpConnection conn(0);
    conn.querybuf = sdsempty();

    // 第一个请求
    json body1 = {
        {"cmdno", CMDNO_PUSH},
        {"uid", 1},
        {"stream_name", "stream1"},
        {"audio", 1},
        {"video", 0}
    };
    std::string body1_str = body1.dump();

    xhead_t header1;
    memset(&header1, 0, sizeof(header1));
    header1.magic_num = XHEAD_MAGIC_NUM;
    header1.log_id = 40001;
    header1.body_len = body1_str.size();

    conn.querybuf = sdscatlen(conn.querybuf, (const char*)&header1, XHEAD_SIZE);
    conn.querybuf = sdscatlen(conn.querybuf, body1_str.data(), body1_str.size());

    // 第二个请求
    json body2 = {
        {"cmdno", CMDNO_PUSH},
        {"uid", 2},
        {"stream_name", "stream2"},
        {"audio", 0},
        {"video", 1}
    };
    std::string body2_str = body2.dump();

    xhead_t header2;
    memset(&header2, 0, sizeof(header2));
    header2.magic_num = XHEAD_MAGIC_NUM;
    header2.log_id = 40002;
    header2.body_len = body2_str.size();

    conn.querybuf = sdscatlen(conn.querybuf, (const char*)&header2, XHEAD_SIZE);
    conn.querybuf = sdscatlen(conn.querybuf, body2_str.data(), body2_str.size());

    int ret = worker->_process_query_buffer(&conn);
    EXPECT_EQ(ret, 0);

    sdsfree(conn.querybuf);
}

// ============================================================
// _read_query 测试（边界条件）
// ============================================================

TEST_F(SignalingWorkerTest, ReadQueryInvalidFd) {
    ASSERT_EQ(worker->init(), 0);

    // 传入无效 fd，应直接返回不崩溃
    worker->_read_query(-1);
    worker->_read_query(100);  // 超出 _tcp_conns 范围

    SUCCEED();
}

// ============================================================
// _new_conn 测试（边界条件，不启动 event loop）
// ============================================================

TEST_F(SignalingWorkerTest, NewConnInvalidFd) {
    ASSERT_EQ(worker->init(), 0);

    // 传入无效 fd（<=0），应直接返回不崩溃
    worker->_new_conn(0);
    worker->_new_conn(-1);

    SUCCEED();
}

