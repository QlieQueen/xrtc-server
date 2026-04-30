This is a WebRTC signaling server project (xrtc-server). It uses C++14, libev event loop, WebRTC stack via rtcbase.

## Available Skills
- `/xrtcserver-learn` — Guide for learning xrtcserver reference project and implementing xrtc-server from scratch. Covers architecture, threading model, signaling flow, test patterns, and common pitfalls.

## Build Commands
```bash
cd build && cmake .. && make -j$(nproc)
```
Run tests from project root:
```bash
./build/xrtc-server-test
```
