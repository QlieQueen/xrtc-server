#include <thread>
#include <memory>

#include <gtest/gtest.h>

#define private public
#include "server/rtc_worker.h"
#undef private

using namespace xrtc;

class RtcWorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        options.worker_num = 2;
        worker = new RtcWorker(0, options);
    }

    void TearDown() override {
        if (worker) {
            worker->stop();
            worker->join();
            delete worker;
            worker = nullptr;
        }
    }

    RtcServerOptions options;
    RtcWorker* worker = nullptr;
};

// ============================================================
// 生命周期测试
// ============================================================

TEST_F(RtcWorkerTest, InitAndDestroy) {
    EXPECT_EQ(worker->init(), 0);
}

TEST_F(RtcWorkerTest, StartStop) {
    ASSERT_EQ(worker->init(), 0);
    EXPECT_TRUE(worker->start());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    worker->stop();
    worker->join();

    SUCCEED();
}

TEST_F(RtcWorkerTest, DoubleStart) {
    ASSERT_EQ(worker->init(), 0);

    EXPECT_TRUE(worker->start());
    EXPECT_TRUE(worker->start());

    worker->stop();
    worker->join();
}

TEST_F(RtcWorkerTest, NotifyQuit) {
    ASSERT_EQ(worker->init(), 0);
    EXPECT_TRUE(worker->start());

    EXPECT_EQ(worker->notify(RtcWorker::QUIT), 0);

    worker->join();
}

// ============================================================
// 消息投递测试
// ============================================================

TEST_F(RtcWorkerTest, SendRtcMsg) {
    ASSERT_EQ(worker->init(), 0);

    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = CMDNO_PUSH;
    msg->uid = 12345;
    msg->stream_name = "test_stream";
    msg->audio = 1;
    msg->video = 1;
    msg->log_id = 10001;

    EXPECT_EQ(worker->send_rtc_msg(msg), 0);

    // 验证消息已进入无锁队列
    EXPECT_FALSE(worker->_q_msg.empty());
    EXPECT_EQ(worker->_q_msg.size(), 1);
}

TEST_F(RtcWorkerTest, SendRtcMsgAndStart) {
    ASSERT_EQ(worker->init(), 0);

    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = CMDNO_PUSH;
    msg->uid = 12345;
    msg->stream_name = "test_stream";
    msg->audio = 1;
    msg->video = 1;
    msg->log_id = 10001;

    EXPECT_EQ(worker->send_rtc_msg(msg), 0);

    EXPECT_TRUE(worker->start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    worker->stop();
    worker->join();

    SUCCEED();
}

TEST_F(RtcWorkerTest, SendMultipleRtcMsg) {
    ASSERT_EQ(worker->init(), 0);

    for (int i = 0; i < 10; ++i) {
        auto msg = std::make_shared<RtcMsg>();
        msg->cmdno = CMDNO_PUSH;
        msg->uid = i;
        msg->stream_name = "stream_" + std::to_string(i);
        msg->audio = 1;
        msg->video = 1;
        msg->log_id = 20000 + i;

        EXPECT_EQ(worker->send_rtc_msg(msg), 0);
    }

    EXPECT_EQ(worker->_q_msg.size(), 10);

    // 消费所有消息
    for (int i = 0; i < 10; ++i) {
        std::shared_ptr<RtcMsg> popped;
        EXPECT_TRUE(worker->pop_msg(&popped));
        EXPECT_EQ(popped->uid, (uint64_t)i);
    }

    EXPECT_TRUE(worker->_q_msg.empty());
}

TEST_F(RtcWorkerTest, SendAllCmdnoTypes) {
    ASSERT_EQ(worker->init(), 0);

    int cmdnos[] = {CMDNO_PUSH, CMDNO_PULL, CMDNO_ANSWER, CMDNO_STOPPUSH, CMDNO_STOPPULL};
    for (int cmdno : cmdnos) {
        auto msg = std::make_shared<RtcMsg>();
        msg->cmdno = cmdno;
        msg->uid = 1;
        msg->stream_name = "s";
        msg->log_id = 30000 + cmdno;

        EXPECT_EQ(worker->send_rtc_msg(msg), 0);
    }

    EXPECT_EQ(worker->_q_msg.size(), 5);

    // 启动 worker 处理消息（process_xxx 目前是桩，只验证不崩溃）
    EXPECT_TRUE(worker->start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    worker->stop();
    worker->join();

    SUCCEED();
}

// ============================================================
// notify 接口测试（不启动 event loop）
// ============================================================

TEST_F(RtcWorkerTest, NotifyWithoutStart) {
    ASSERT_EQ(worker->init(), 0);

    EXPECT_EQ(worker->notify(RtcWorker::QUIT), 0);
}

TEST_F(RtcWorkerTest, NotifyRtcMsgWithoutStart) {
    ASSERT_EQ(worker->init(), 0);

    EXPECT_EQ(worker->notify(RtcWorker::RTC_MSG), 0);
}

// ============================================================
// 消息处理函数测试（纯逻辑，不启动 event loop）
// ============================================================

TEST_F(RtcWorkerTest, ProcessPush) {
    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = CMDNO_PUSH;
    msg->uid = 12345;
    msg->stream_name = "push_stream";

    // 目前是桩函数，只验证不崩溃
    worker->_process_push(msg);

    SUCCEED();
}

TEST_F(RtcWorkerTest, ProcessPull) {
    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = CMDNO_PULL;
    msg->uid = 67890;
    msg->stream_name = "pull_stream";

    worker->_process_pull(msg);

    SUCCEED();
}

TEST_F(RtcWorkerTest, ProcessAnswer) {
    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = CMDNO_ANSWER;
    msg->uid = 11111;
    msg->stream_name = "answer_stream";
    msg->sdp = "dummy sdp";

    worker->_process_answer(msg);

    SUCCEED();
}

TEST_F(RtcWorkerTest, ProcessStopPush) {
    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = CMDNO_STOPPUSH;
    msg->uid = 22222;
    msg->stream_name = "stop_push_stream";

    worker->_process_stop_push(msg);

    SUCCEED();
}

TEST_F(RtcWorkerTest, ProcessStopPull) {
    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = CMDNO_STOPPULL;
    msg->uid = 33333;
    msg->stream_name = "stop_pull_stream";

    worker->_process_stop_pull(msg);

    SUCCEED();
}

// ============================================================
// _process_rtc_msg 完整流程测试
// ============================================================

TEST_F(RtcWorkerTest, ProcessRtcMsgFullFlow) {
    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = CMDNO_PUSH;
    msg->uid = 99999;
    msg->stream_name = "full_flow";
    msg->audio = 1;
    msg->video = 1;
    msg->log_id = 40001;

    worker->push_msg(msg);
    EXPECT_EQ(worker->_q_msg.size(), 1);

    worker->_process_rtc_msg();

    EXPECT_TRUE(worker->_q_msg.empty());
}

TEST_F(RtcWorkerTest, ProcessRtcMsgEmptyQueue) {
    // 空队列调用 _process_rtc_msg 不崩溃
    worker->_process_rtc_msg();

    SUCCEED();
}

TEST_F(RtcWorkerTest, ProcessRtcMsgUnknownCmdno) {
    auto msg = std::make_shared<RtcMsg>();
    msg->cmdno = 999;
    msg->stream_name = "unknown";

    worker->push_msg(msg);
    worker->_process_rtc_msg();

    SUCCEED();
}
