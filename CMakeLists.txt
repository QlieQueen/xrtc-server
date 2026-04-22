cmake_minimum_required(VERSION 3.22)

project(xrtc-server)

add_definitions("-g -pipe -W -Wall -fPIC -std=gnu++14 -DWEBRTC_POSIX -DWEBRTC_LINUX")

include_directories(
    "./src"
    "./third_party/include"
    "../rtcbase/src"
)

link_directories(
    "./third_party/lib"
    "../rtcbase/out"
)

file (GLOB all_src
    "./src/*.cpp"
    "./src/base/*.cpp"
)

add_executable(xrtc-server ${all_src})

target_link_libraries(xrtc-server
    libyaml-cpp.a
    librtcbase.a

    libabsl_strings.a
    libabsl_throw_delegate.a
    libabsl_base.a

    -lpthread
    -no-pie
)

