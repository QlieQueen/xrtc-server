/**
 * @file test_signaling_server.cpp
 * @brief 信令服务消息监听模块的单元测试
 */

#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "base/socket.h"
#include "server/signaling_server.h"

using namespace xrtc;

class SignalingServerTest : public::testing::Test {
protected:
    void SetUp() override {

    }

    void TearDown() override {

    }
};

TEST_F(SignalingServerTest, StartStop) {
    SignalingServer server;
    ASSERT_EQ(server.init("./conf/signaling_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();

    server.join();

    SUCCEED();
}

TEST_F(SignalingServerTest, NotifyQuit) {
    SignalingServer server;

    ASSERT_EQ(server.init("./conf/signaling_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    EXPECT_EQ(server.notify(SignalingServer::QUIT), 0);

    server.join();

    SUCCEED();
}

TEST_F(SignalingServerTest, AcceptNewConn) {
    SignalingServer server;

    ASSERT_EQ(server.init("./conf/signaling_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    // 等待服务器启动完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::thread c_thread([]() {
        EXPECT_EQ(tcp_connect("127.0.0.1", 9000), 0);
        std::this_thread::sleep_for(std::chrono::seconds(3));
    });

    // 等待客户端连接完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();

    server.join();

    c_thread.join();

    SUCCEED();
}

// ============================================================
// init() 异常路径测试
// ============================================================

TEST_F(SignalingServerTest, InitWithNullptr) {
    SignalingServer server;

    // 传入空指针，init 应返回 -1
    EXPECT_EQ(server.init(nullptr), -1);
}

TEST_F(SignalingServerTest, InitWithNonExistentFile) {
    SignalingServer server;

    // 传入不存在的配置文件，init 应返回 -1
    EXPECT_EQ(server.init("./conf/nonexistent.yaml"), -1);
}

TEST_F(SignalingServerTest, InitWithInvalidYaml) {
    SignalingServer server;

    // 传入格式错误的 YAML 文件（connection_timeout 不是整数），init 应返回 -1
    EXPECT_EQ(server.init("./conf/signaling_server_invalid.yaml"), -1);
}

// ============================================================
// start() 重复调用测试
// ============================================================

TEST_F(SignalingServerTest, DoubleStart) {
    SignalingServer server;

    ASSERT_EQ(server.init("./conf/signaling_server.yaml"), 0);

    // 第一次 start 应返回 true
    EXPECT_TRUE(server.start());

    // 第二次 start 应返回 false（线程已启动）
    EXPECT_FALSE(server.start());

    server.stop();
    server.join();
}

// ============================================================
// 未知消息通知测试
// ============================================================

TEST_F(SignalingServerTest, NotifyUnknownMsg) {
    SignalingServer server;

    ASSERT_EQ(server.init("./conf/signaling_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 发送一个未定义的消息，不应崩溃
    EXPECT_EQ(server.notify(999), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 正常停止
    server.stop();
    server.join();
}

// ============================================================
// 多客户端并发连接测试
// ============================================================

TEST_F(SignalingServerTest, MultipleConcurrentConnections) {
    SignalingServer server;

    ASSERT_EQ(server.init("./conf/signaling_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    // 等待服务器启动完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int client_count = 5;
    std::vector<std::thread> clients;

    for (int i = 0; i < client_count; ++i) {
        clients.emplace_back([]() {
            EXPECT_EQ(tcp_connect("127.0.0.1", 9000), 0);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        });
    }

    // 等待所有客户端连接
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    server.stop();
    server.join();

    for (auto& t : clients) {
        t.join();
    }

    SUCCEED();
}

// ============================================================
// 不调用 start() 直接 stop/join 测试
// ============================================================

TEST_F(SignalingServerTest, StopWithoutStart) {
    SignalingServer server;

    ASSERT_EQ(server.init("./conf/signaling_server.yaml"), 0);

    // 未 start 就 stop，不应崩溃
    server.stop();
    server.join();

    SUCCEED();
}

// ============================================================
// 不调用 init() 直接 start 测试
// ============================================================

TEST_F(SignalingServerTest, StartWithoutInit) {
    SignalingServer server;

    // 未 init 就 start，此时 _thread 为 nullptr，start 应返回 true
    // 但 event loop 没有初始化，运行后会出问题
    // 这里只验证不崩溃
    EXPECT_TRUE(server.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    server.stop();
    server.join();

    SUCCEED();
}
