/**
 * @file test_event_loop.cpp
 * @brief 事件循环模块的单元测试
 */

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include "base/event_loop.h"

using namespace xrtc;

// ============================================================
// 测试 fixture：事件循环测试
// ============================================================
class EventLoopTest : public ::testing::Test {
protected:
    void SetUp() override {
        loop = new EventLoop(nullptr);
    }

    void TearDown() override {
        delete loop;
    }

    EventLoop* loop;
};

// ============================================================
// 测试用例
// ============================================================

/**
 * @brief 测试事件循环的创建和销毁
 */
TEST_F(EventLoopTest, CreateAndDestroy) {
    EventLoop el(nullptr);
    SUCCEED() << "事件循环创建成功";
}

/**
 * @brief 测试获取当前时间
 */
TEST_F(EventLoopTest, GetNowTime) {
    unsigned long now = loop->now();
    EXPECT_GT(now, 0) << "当前时间应该大于0";
}

/**
 * @brief 测试IO事件的创建和删除
 */
TEST_F(EventLoopTest, CreateAndDeleteIOEvent) {
    // 创建一个IO事件观察者
    IOWatcher* watcher = loop->create_io_event(nullptr, nullptr);
    EXPECT_NE(watcher, nullptr) << "IO事件观察者创建成功";

    // 删除IO事件观察者
    loop->delete_io_event(watcher);
    SUCCEED() << "IO事件观察者删除成功";
}

/**
 * @brief 测试定时器的创建和删除
 */
TEST_F(EventLoopTest, CreateAndDeleteTimer) {
    // 创建一个一次性定时器
    TimerWatcher* timer = loop->create_timer(nullptr, nullptr, false);
    EXPECT_NE(timer, nullptr) << "一次性定时器创建成功";

    // 删除定时器
    loop->delete_timer(timer);
    SUCCEED() << "定时器删除成功";
}

/**
 * @brief 测试重复定时器的创建和删除
 */
TEST_F(EventLoopTest, CreateAndDeleteRepeatTimer) {
    // 创建一个重复定时器
    TimerWatcher* timer = loop->create_timer(nullptr, nullptr, true);
    EXPECT_NE(timer, nullptr) << "重复定时器创建成功";

    // 删除定时器
    loop->delete_timer(timer);
    SUCCEED() << "重复定时器删除成功";
}

/**
 * @brief 测试定时器的启动和停止
 */
TEST_F(EventLoopTest, StartAndStopTimer) {
    // 创建一个定时器
    TimerWatcher* timer = loop->create_timer(nullptr, nullptr, false);
    ASSERT_NE(timer, nullptr);

    // 启动定时器（100ms后触发）
    loop->start_timer(timer, 100000);
    SUCCEED() << "定时器启动成功";

    // 停止定时器
    loop->stop_timer(timer);
    SUCCEED() << "定时器停止成功";

    // 清理
    loop->delete_timer(timer);
}

/**
 * @brief 测试定时器回调
 * 
 * 注意：要测试定时器回调是否被触发，必须在单独线程中启动事件循环，
 * 否则事件循环会阻塞当前线程，导致无法执行后续的断言和清理操作。
 */
TEST_F(EventLoopTest, TimerCallback) {
    bool timer_fired = false;

    // 定时器回调函数
    auto timer_cb = [](EventLoop* /*el*/, TimerWatcher* /*w*/, void* data) {
        bool* fired = static_cast<bool*>(data);
        *fired = true;
    };

    // 创建定时器
    TimerWatcher* timer = loop->create_timer(timer_cb, &timer_fired, false);
    ASSERT_NE(timer, nullptr);

    // 在单独线程中启动事件循环，这样事件循环才能处理定时器事件
    std::thread loop_thread([this]() {
        loop->start();
    });

    // 启动定时器（10ms后触发）
    loop->start_timer(timer, 10000);

    // 等待定时器触发
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 验证回调被执行
    EXPECT_TRUE(timer_fired) << "定时器回调应该被触发";

    // 停止并清理
    loop->stop_timer(timer);
    loop->delete_timer(timer);
    loop->stop();
    loop_thread.join();
}

/**
 * @brief 测试事件循环的启动和停止
 */
TEST_F(EventLoopTest, StartAndStopLoop) {
    // 在单独线程中启动事件循环
    std::thread loop_thread([this]() {
        loop->start();
    });

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 停止事件循环
    loop->stop();

    // 等待线程结束
    loop_thread.join();

    SUCCEED() << "事件循环启动和停止成功";
}
