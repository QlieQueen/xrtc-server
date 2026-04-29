#include <thread>
#include <memory>

#include <gtest/gtest.h>

#define private public
#include "server/rtc_server.h"
#include "server/rtc_worker.h"
#undef private

using namespace xrtc;

class RtcServerTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

// ============================================================
// init() 异常路径测试
// ============================================================

TEST_F(RtcServerTest, InitWithNullptr) {
    RtcServer server;
    EXPECT_EQ(server.init(nullptr), -1);
}

TEST_F(RtcServerTest, InitWithNonExistentFile) {
    RtcServer server;
    EXPECT_EQ(server.init("./conf/nonexistent.yaml"), -1);
}

TEST_F(RtcServerTest, InitWithInvalidYaml) {
    RtcServer server;
    EXPECT_EQ(server.init("./conf/rtc_server_invalid.yaml"), -1);
}

// ============================================================
// 启动/停止生命周期测试
// ============================================================

TEST_F(RtcServerTest, StartStop) {
    RtcServer server;
    ASSERT_EQ(server.init("./conf/rtc_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();

    server.join();

    SUCCEED();
}

TEST_F(RtcServerTest, NotifyQuit) {
    RtcServer server;
    ASSERT_EQ(server.init("./conf/rtc_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    EXPECT_EQ(server.notify(RtcServer::QUIT), 0);

    server.join();

    SUCCEED();
}

TEST_F(RtcServerTest, DoubleStart) {
    RtcServer server;
    ASSERT_EQ(server.init("./conf/rtc_server.yaml"), 0);

    EXPECT_TRUE(server.start());
    EXPECT_TRUE(server.start());

    server.stop();
    server.join();
}

TEST_F(RtcServerTest, NotifyQuitWithoutStart) {
    RtcServer server;
    ASSERT_EQ(server.init("./conf/rtc_server.yaml"), 0);

    // notify 只写入 pipe，不依赖 event loop
    EXPECT_EQ(server.notify(RtcServer::QUIT), 0);

    // 手动停止 worker 线程，否则析构时 crash
    for (auto worker : server._workers) {
        if (worker) {
            worker->stop();
            worker->join();
        }
    }

    SUCCEED();
}

TEST_F(RtcServerTest, NotifyUnknownMsg) {
    RtcServer server;
    ASSERT_EQ(server.init("./conf/rtc_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(server.notify(999), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    server.stop();
    server.join();
}

// ============================================================
// RTC 消息投递测试
// ============================================================

TEST_F(RtcServerTest, SendRtcMsg) {
    RtcServer server;
    ASSERT_EQ(server.init("./conf/rtc_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = CMDNO_PUSH;
    msg->uid = 12345;
    msg->stream_name = "test_stream";
    msg->audio = 1;
    msg->video = 1;
    msg->log_id = 10001;

    EXPECT_EQ(server.send_rtc_msg(msg), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    server.join();

    SUCCEED();
}

TEST_F(RtcServerTest, SendMultipleRtcMsg) {
    RtcServer server;
    ASSERT_EQ(server.init("./conf/rtc_server.yaml"), 0);

    EXPECT_TRUE(server.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int i = 0; i < 10; ++i) {
        auto msg = std::make_shared<RtcMsg>();
        msg->cmdno = CMDNO_PUSH;
        msg->uid = i;
        msg->stream_name = "stream_" + std::to_string(i);
        msg->audio = 1;
        msg->video = 1;
        msg->log_id = 20000 + i;

        EXPECT_EQ(server.send_rtc_msg(msg), 0);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    server.stop();
    server.join();

    SUCCEED();
}
