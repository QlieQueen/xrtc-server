/**
 * @file test_log.cpp
 * @brief 日志系统模块的单元测试
 */

#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

#include "base/log.h"

using namespace xrtc;

// ============================================================
// 测试 fixture：日志测试
// ============================================================
class LogTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试用例使用独立的日志目录
        test_log_dir = "./test_log_" + std::to_string(::getpid());
        test_log_name = "test_log";
    }

    void TearDown() override {
        // 清理测试日志文件
        std::string log_file = test_log_dir + "/" + test_log_name + ".log";
        std::string log_file_wf = test_log_dir + "/" + test_log_name + ".log.wf";
        std::remove(log_file.c_str());
        std::remove(log_file_wf.c_str());
        std::remove(test_log_dir.c_str());
    }

    std::string test_log_dir;
    std::string test_log_name;
};

// ============================================================
// 测试用例
// ============================================================

/**
 * @brief 测试日志系统的创建和初始化
 */
TEST_F(LogTest, CreateAndInit) {
    XrtcLog log(test_log_dir, test_log_name, "verbose");
    int ret = log.init();
    EXPECT_EQ(ret, 0) << "日志系统初始化应该成功";
}

/**
 * @brief 测试日志系统的启动和停止
 */
TEST_F(LogTest, StartAndStop) {
    XrtcLog log(test_log_dir, test_log_name, "verbose");
    ASSERT_EQ(log.init(), 0);

    bool started = log.start();
    EXPECT_TRUE(started) << "日志系统启动应该成功";

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 停止日志系统
    log.stop();
    SUCCEED() << "日志系统停止成功";
}

/**
 * @brief 测试日志写入文件
 */
TEST_F(LogTest, WriteLogToFile) {
    XrtcLog log(test_log_dir, test_log_name, "verbose");
    ASSERT_EQ(log.init(), 0);
    ASSERT_TRUE(log.start());

    // 记录一些日志
    RTC_LOG(LS_INFO) << "Test info message";
    RTC_LOG(LS_WARNING) << "Test warning message";
    RTC_LOG(LS_ERROR) << "Test error message";

    // 等待日志写入
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 停止日志系统
    log.stop();

    // 检查日志文件是否存在
    std::string log_file = test_log_dir + "/" + test_log_name + ".log";
    std::string log_file_wf = test_log_dir + "/" + test_log_name + ".log.wf";

    // 检查普通日志文件
    std::ifstream infile(log_file);
    EXPECT_TRUE(infile.good()) << "普通日志文件应该存在";

    // 检查错误日志文件
    std::ifstream infile_wf(log_file_wf);
    EXPECT_TRUE(infile_wf.good()) << "错误日志文件应该存在";
}

/**
 * @brief 测试日志级别过滤
 */
TEST_F(LogTest, LogLevelFilter) {
    // 使用"error"级别，只记录错误日志
    XrtcLog log(test_log_dir, test_log_name, "error");
    ASSERT_EQ(log.init(), 0);
    ASSERT_TRUE(log.start());

    // 记录不同级别的日志
    RTC_LOG(LS_VERBOSE) << "Verbose message (should be filtered)";
    RTC_LOG(LS_INFO) << "Info message (should be filtered)";
    RTC_LOG(LS_WARNING) << "Warning message (should be filtered)";
    RTC_LOG(LS_ERROR) << "Error message (should be written)";

    // 等待日志写入
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 停止日志系统
    log.stop();

    // 检查错误日志文件
    std::string log_file_wf = test_log_dir + "/" + test_log_name + ".log.wf";
    std::ifstream infile_wf(log_file_wf);
    EXPECT_TRUE(infile_wf.good()) << "错误日志文件应该存在";
}

/**
 * @brief 测试日志系统的多次启动
 */
TEST_F(LogTest, MultipleStart) {
    XrtcLog log(test_log_dir, test_log_name, "verbose");
    ASSERT_EQ(log.init(), 0);

    // 第一次启动
    EXPECT_TRUE(log.start());

    // 第二次启动应该返回false
    EXPECT_FALSE(log.start()) << "重复启动应该返回false";

    // 停止
    log.stop();
    SUCCEED() << "多次启动测试完成";
}

/**
 * @brief 测试日志系统的析构函数
 */
TEST_F(LogTest, Destructor) {
    {
        XrtcLog log(test_log_dir, test_log_name, "verbose");
        ASSERT_EQ(log.init(), 0);
        ASSERT_TRUE(log.start());

        RTC_LOG(LS_INFO) << "Message before destruction";
    }
    // 析构函数应该自动停止日志系统
    SUCCEED() << "日志系统析构成功";
}
