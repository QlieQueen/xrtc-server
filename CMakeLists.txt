cmake_minimum_required(VERSION 3.22)

project(xrtc-server)

# ============================================================
# 编译选项
# ============================================================
add_definitions("-g -pipe -W -Wall -fPIC -rdynamic -std=gnu++14 -DWEBRTC_POSIX -DWEBRTC_LINUX")

# ============================================================
# 包含目录
# ============================================================
include_directories(
    "./src"
    "./third_party/include"
    "../rtcbase/src"
)

# ============================================================
# 链接目录
# ============================================================
link_directories(
    "./third_party/lib"
    "../rtcbase/out"
)

# ============================================================
# 主程序源文件
# ============================================================
file (GLOB all_src
    "./src/*.cpp"
    "./src/base/*.cpp"
    "./src/server/*.cpp"
    "./src/stream/*.cpp"
    "./src/pc/*.cpp"
    "./src/ice/*.cpp"
    "./src/module/rtp_rtcp/*.cpp"
)

# ============================================================
# 主程序目标
# ============================================================
add_executable(xrtc-server ${all_src})

target_link_libraries(xrtc-server
    libyaml-cpp.a
    librtcbase.a
    libssl.a
    libcrypto.a
    libabsl_strings.a
    libabsl_throw_delegate.a
    libabsl_bad_optional_access.a
    libev.a
    libsrtp2.a
    -lpthread

    -no-pie
)

# ============================================================
# Google Test 集成
# ============================================================
# 启用测试
enable_testing()

# 使用FetchContent自动下载Google Test
include(FetchContent)
# FetchContent_Declare(
#     googletest
#     URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
#     DOWNLOAD_EXTRACT_TIMESTAMP TRUE
# )

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# 设置Google Test的编译选项（不编译不必要的部分）
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# 如果本地已经有googletest源码，可以优先使用本地路径
# 取消注释下面这行并注释上面的FetchContent_Declare，使用本地源码
# FetchContent_Declare(googletest SOURCE_DIR /path/to/local/googletest)

FetchContent_MakeAvailable(googletest)

# ============================================================
# 测试源文件（包含基础模块源码，以便测试目标可以链接）
# ============================================================
file (GLOB test_src
    "./src/global.cpp"
    "./test/*.cpp"
    "./test/base/*.cpp"
    "./src/base/*.cpp"
    "./test/server/*.cpp"
    "./src/server/*.cpp"
    "./src/stream/*.cpp"
    "./src/pc/*.cpp"
    "./src/ice/*.cpp"
    "./src/module/rtp_rtcp/*.cpp"
)

# ============================================================
# 测试目标
# ============================================================
add_executable(xrtc-server-test ${test_src})

target_link_libraries(xrtc-server-test
    gtest_main
    gtest

    libyaml-cpp.a
    librtcbase.a
    libssl.a
    libcrypto.a
    libev.a

    libabsl_strings.a
    libabsl_throw_delegate.a
    libabsl_bad_optional_access.a
    libabsl_base.a
    libsrtp2.a

    -lpthread
    -no-pie
)

# 注册测试（设置工作目录为源码根目录，确保测试可找到配置文件）
add_test(NAME xrtc-server-test COMMAND xrtc-server-test)
set_tests_properties(xrtc-server-test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
