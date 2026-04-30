---
description: "Guide for learning the xrtcserver reference project and implementing xrtc-server from scratch"
---

# xrtcserver Learning Guide

You are helping a user learn from the reference project `/home/ydqun/workspace/lession/xrtcserver` and implement their own version at `/home/ydqun/workspace/lession/xrtc-server`.

## Project Understanding

xrtcserver is a WebRTC signaling and media server with this architecture:

### Two-Process Architecture
- **SignalingServer** (control plane): TCP server that accepts client connections, parses signaling commands (PUSH/PULL/ANSWER/STOPPUSH/STOPPULL), distributes connections to SignalingWorkers
- **RtcServer** (media plane): Manages RTC workers, handles stream creation, generates DTLS certificates, routes messages via CRC32

### Threading Model
- Each component has its own EventLoop (libev wrapper) and dedicated thread
- Cross-thread communication via pipe-based notification + lock-free queue or mutex-protected queue
- `RtcServer` main thread → notifies `RtcWorker` threads
- `SignalingServer` main thread → notifies `SignalingWorker` threads via round-robin dispatch

### Signaling Flow (CMDNO_PUSH example)
1. TCP client → binary protocol (36-byte XHead header + JSON body)
2. `SignalingWorker::_read_query` → `_process_query_buffer` → `_process_request` → `_process_push`
3. `_process_push` creates `RtcMsg` → sends to `RtcServer` via `g_rtc_server->send_rtc_msg()`
4. `RtcServer::_process_rtc_msg` → `_get_worker(stream_name)` via CRC32 routing → `RtcWorker::send_rtc_msg()`
5. `RtcWorker::_process_push` → `RtcStreamManager::create_push_stream()` → generates offer SDP
6. Response flows back: `worker->send_rtc_msg(msg)` → `SignalingWorker::_response_server_offer`
7. `_response_server_offer` constructs JSON response → `_add_reply` → `_write_query` → TCP client socket

## Key Design Patterns

### Binary Protocol (XHead)
- 36-byte header (`xhead_t`): magic_num, body_len, log_id, etc.
- Short connection model: `bytes_processed = 65536` after processing one request
- State machine: `STATE_HEAD` → `STATE_BODY` per TcpConnection

### EventLoop
- Wraps libev (`struct ev_loop`)
- I/O watchers (create/start/stop/delete) for fd events (READ/WRITE)
- Timer watchers (create/start/stop/delete) for periodic callbacks
- Pipe-based wakeup for cross-thread notification

### Message Queues
- `LockFreeQueue` (SPSC lock-free queue) for RtcWorker messages
- `std::queue + std::mutex` for RtcServer and SignalingWorker messages
- Pipe write for wakeup: `notify(msg)` → write int to pipe → event loop reads in callback

### RtcMsg (cross-component message)
```cpp
struct RtcMsg {
    int cmdno;           // CMDNO_PUSH(1), CMDNO_PULL(2), CMDNO_ANSWER(3), etc.
    uint64_t uid;
    std::string stream_name;
    std::string stream_type;
    int audio, video;
    uint32_t log_id;
    void* worker;        // back-pointer to SignalingWorker for response routing
    void* conn;          // back-pointer to TcpConnection
    int fd;
    std::string sdp;
    int err_no;
    void* certificate;   // DTLS certificate from RtcServer
};
```

## Project Structure

```
xrtc-server/
├── src/
│   ├── main.cpp                  # Entry point
│   ├── global.cpp                # Global pointer definitions (g_rtc_server, etc.)
│   ├── xrtc_server_def.h         # RtcMsg struct + cmdno constants
│   ├── base/
│   │   ├── event_loop.h/cpp      # libev event loop wrapper
│   │   ├── socket.h/cpp          # TCP socket I/O utilities
│   │   ├── lock_free_queue.h     # SPSC lock-free queue (header-only)
│   │   ├── log.h/cpp             # Async logging (thread + queue)
│   │   ├── conf.h/cpp            # YAML config loader
│   │   └── xhead.h               # Binary protocol header definition
│   ├── server/
│   │   ├── signaling_server.h/cpp    # TCP listener + worker dispatch
│   │   ├── signaling_worker.h/cpp    # Per-connection signaling handler
│   │   ├── tcp_connection.h/cpp      # Connection state abstraction
│   │   ├── rtc_server.h/cpp          # RTC server (media plane)
│   │   └── rtc_worker.h/cpp          # RTC worker (stream processing)
│   └── stream/
│       └── rtc_stream_manager.h/cpp  # Stream lifecycle management
├── test/
│   ├── test_main.cpp
│   ├── base/                    # Tests for base modules
│   └── server/                  # Tests for server modules
├── conf/                        # YAML config files
└── CMakeLists.txt               # Single build file
```

## Test Patterns

### Google Test
- `#define private public` before including headers to test private methods
- Test fixtures with SetUp/TearDown for resource management
- Tests must be run from project root (config files are relative to `./conf/`)

### Common Pre-existing Pitfalls (documented from experience)
1. **`pop_msg()` condition inversion**: `if (!_q_msg.empty()) return nullptr;` should be `if (_q_msg.empty())`
2. **`_stop()` inverted condition**: `if (_thread)` should be `if (!_thread)` in stop methods
3. **`notify()` return inverted**: `written == sizeof(int) ? -1 : 0;` should be `? 0 : -1`
4. **`send_rtc_msg` missing return**: must return the result of `notify()`
5. **`init()` missing `return 0;`**: GCC inserts `ud2` → SIGILL at runtime
6. **Globals in wrong namespace**: `g_rtc_server` etc. must be in global namespace, not `namespace xrtc`
7. **Link order**: `librtcbase.a` must come BEFORE `libssl.a`/`libcrypto.a`
8. **WRITE event never stopped**: after `_write_query` finishes, must `stop_io_event(w, fd, WRITE)` or CPU 100%
9. **EOF not handled**: `sock_read_data` returning 0 (peer closed) must trigger `_close_conn()`
10. **Thread cleanup**: Destructors must `notify(QUIT)` + `join()` before deleting thread objects
11. **Log sink dangling pointer**: `AddLogToStream` requires `RemoveLogToStream` in destructor
12. **Server stop must clean workers**: `SignalingServer::_stop()` must stop workers even when server thread isn't running

## Implementation Order (Recommended)
1. Base modules: EventLoop, Socket, Log, Conf, LockFreeQueue ✓
2. SignalingServer + SignalingWorker + TcpConnection ✓
3. RtcServer + RtcWorker ✓
4. RtcStreamManager + PushStream/PullStream (in progress)
5. Full end-to-end integration
6. Additional protocol commands and edge cases

## Build & Test
```bash
cd build && cmake .. && make -j$(nproc)
# Run tests from project root:
cd /home/ydqun/workspace/lession/xrtc-server && ./build/xrtc-server-test
# Or via ctest:
cd build && ctest
```

## Reference Project
The reference project at `/home/ydqun/workspace/lession/xrtcserver` is the original implementation. Compare implementation approaches when stuck, but write code from scratch to ensure understanding.
