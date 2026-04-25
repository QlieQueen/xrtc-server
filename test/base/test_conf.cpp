/**
 * @file test_conf.cpp
 * @brief 配置文件模块的单元测试
 */

#include <gtest/gtest.h>
#include "base/conf.h"

using namespace xrtc;

// ============================================================
// 测试 fixture：配置测试
// ============================================================
class ConfTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个测试用例执行前的初始化
    }

    void TearDown() override {
        // 每个测试用例执行后的清理
    }
};

// ============================================================
// 测试用例
// ============================================================

/**
 * @brief 测试加载不存在的配置文件
 */
TEST_F(ConfTest, LoadNonExistentFile) {
    GeneralConf conf;
    int ret = load_general_conf("/path/to/nonexistent.yaml", &conf);
    EXPECT_NE(ret, 0) << "加载不存在的配置文件应该返回错误";
}

/**
 * @brief 测试传入空指针
 */
TEST_F(ConfTest, NullPointer) {
    int ret = load_general_conf(nullptr, nullptr);
    EXPECT_NE(ret, 0) << "传入空指针应该返回错误";
}

/**
 * @brief 测试加载有效的配置文件
 * 
 * 注意：测试需要在项目根目录下运行，或者使用绝对路径。
 * 如果从build目录运行，需要指定相对于项目根目录的路径。
 */
TEST_F(ConfTest, LoadValidConfig) {
    GeneralConf conf;
    // 尝试多个可能的路径
    const char* paths[] = {
        "./conf/general.yaml",           // 从项目根目录运行
        "../conf/general.yaml",          // 从build目录运行
        "/home/ydqun/workspace/lession/xrtc-server/conf/general.yaml"  // 绝对路径
    };

    int ret = -1;
    for (const char* path : paths) {
        ret = load_general_conf(path, &conf);
        if (ret == 0) break;
    }

    EXPECT_EQ(ret, 0) << "加载有效的配置文件应该成功";

    // 验证配置值（与 conf/general.yaml 中的实际值保持一致）
    EXPECT_EQ(conf.log_dir, "./log");
    EXPECT_EQ(conf.log_name, "xrtcserver");
    EXPECT_EQ(conf.log_level, "verbose");
    EXPECT_TRUE(conf.log_to_stderr);
    EXPECT_EQ(conf.ice_min_port, 10025);
    EXPECT_EQ(conf.ice_max_port, 65535);
}

/**
 * @brief 测试GeneralConf默认值
 */
TEST_F(ConfTest, DefaultValues) {
    GeneralConf conf;
    EXPECT_EQ(conf.ice_min_port, 0);
    EXPECT_EQ(conf.ice_max_port, 0);
    EXPECT_TRUE(conf.log_dir.empty());
    EXPECT_TRUE(conf.log_name.empty());
    EXPECT_TRUE(conf.log_level.empty());
}
