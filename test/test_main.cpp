/**
 * @file test_main.cpp
 * @brief Google Test 主入口文件
 * 
 * 该文件包含Google Test的主函数，所有测试用例的入口点。
 * 注意：如果链接了gtest_main库，则不需要此文件。
 * 这里保留作为备用，方便自定义测试初始化。
 */

#include <gtest/gtest.h>

int main(int argc, char **argv) {
    // 初始化Google Test
    ::testing::InitGoogleTest(&argc, argv);

    // 运行所有测试用例
    return RUN_ALL_TESTS();
}
