# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Commands

```bash
# Build everything
./auto_build.sh

# Manual build
cd build && cmake .. && make -j$(nproc)

# Run all tests
./build/xrtc-server-test

# Run a single test case
./build/xrtc-server-test --gtest_filter="RtcServerTest.StartStop"

# Run tests matching a pattern
./build/xrtc-server-test --gtest_filter="*Worker*"
```

Tests run from the project root directory (configured in CMakeLists.txt).

## Architecture Overview

xrtc-server is a WebRTC signaling and media server in C++14. Thread-per-core event-driven architecture built on **libev**.

### Thread Model

- **Main thread**: init → blocks on signaling/rtc server join
- **SignalingServer thread**: 1 event loop, accepts TCP connections
- **SignalingWorker threads** (default 2): protocol parsing, per-connection I/O
- **RtcServer thread**: 1 event loop, routes messages to workers via CRC32(stream_name) % N
- **RtcWorker threads** (default 2): WebRTC stream lifecycle (create_push_stream, etc.)

### Message Flow

```
Client TCP → SignalingServer → SignalingWorker → RtcServer → RtcWorker → RtcStreamManager
                 ↓                                                         ↓
            JSON parsing                                              WebRTC SDP
                 ↓                                                         ↓
            RtcServer queue ← ← ← ← ← ← ← ← ← ← RtcWorker → SignalingWorker → Client
```

### Global State

Four global singletons declared in `src/global.cpp` and accessed via `extern`:

```cpp
xrtc::GeneralConf* g_conf;        // Config (log dir/level, etc.)
xrtc::XrtcLog* g_log;             // Log system (separate thread)
xrtc::SignalingServer* g_signaling_server;
xrtc::RtcServer* g_rtc_server;
```

### Custom Protocol

- **36-byte binary header** (`xhead_t` in `src/base/xhead.h`): id, version, log_id, provider[16], magic_num=0xfb202202, reserved, body_len
- **JSON body**: variable length, parsed via nlohmann/json
- Commands (from `CMDNO_*` defines): PUSH(1), PULL(2), ANSWER(3), STOP_PUSH(4), STOP_PULL(5)
- Short-lived connections — one request/response per TCP connection

## Key Data Structures

### `RtcMsg` (src/xrtc_server_def.h)

Central inter-thread message struct. Fields: `cmdno`, `uid`, `stream_name`, `stream_type`, `audio`, `video`, `log_id`, `worker` (SignalingWorker\*), `conn` (TcpConnection\*), `fd`, `sdp`, `err_no`, `certificate`.

Processing flow per cmdno:
- **PUSH/PULL**: SignalingWorker → RtcServer routes → RtcWorker creates offer → RtcServer → SignalingWorker sends JSON response with SDP offer
- **ANSWER**: SignalingWorker → RtcServer routes → RtcWorker sets remote answer
- **STOP_PUSH/STOP_PULL**: SignalingWorker → RtcServer routes → RtcWorker stops stream

### `TcpConnection` (src/server/tcp_connection.h)

Per-connection state machine using an sds buffer:

| State | bytes_expected | Description |
|-------|---------------|-------------|
| STATE_HEAD | `XHEAD_SIZE` (36) | Reading binary header, validate magic_num |
| STATE_BODY | `head->body_len` | Reading JSON body, then process request |

After one request is processed, `bytes_processed` is set to 65536 to prevent further reads (short-lived connection model).

## Queue Strategy

| Component | Queue Type | Use |
|-----------|-----------|-----|
| SignalingWorker | `LockFreeQueue<int>` (SPSC) | Incoming connection fds from SignalingServer |
| SignalingWorker | `std::queue` + `std::mutex` | RtcMsg from RtcServer (multiple producers) |
| RtcServer | `std::queue` + `std::mutex` | RtcMsg from SignalingWorkers (multiple producers) |
| RtcWorker | `LockFreeQueue<shared_ptr<RtcMsg>>` (SPSC) | RtcMsg from RtcServer |

All inter-thread notification uses pipe fd + write(1 int) to wake the event loop.

## EventLoop Pattern (src/base/event_loop.h)

Thin C++ wrapper around libev (`struct ev_loop`). Each thread owns one `EventLoop`. Callbacks are C-style function pointers, registered as `friend` functions in the owning class:

- `create_io_event(io_cb_t cb, void* data)` — register I/O watcher with callback
- `start_io_event(watcher, fd, EventLoop::READ|WRITE)` — arm watcher on fd
- I/O callback signature: `void cb(EventLoop*, IOWatcher*, int fd, int events, void* data)`
- `events` is bitmask: check with `events & EventLoop::READ` / `events & EventLoop::WRITE`

```
// Pattern used in I/O callbacks:
if (events & EventLoop::READ)  { read_handler(fd); }
if (events & EventLoop::WRITE) { write_handler(fd); }
```

The `data` pointer caries the owning object (server/worker), cast back in the callback.

## Config Files

| File | Purpose |
|------|---------|
| `conf/general.yaml` | Log directory, log name, log level |
| `conf/signaling_server.yaml` | Host, port, worker_num, connection_timeout |
| `conf/rtc_server.yaml` | worker_num |

Parsed via `yaml-cpp` into `GeneralConf` / `SignalingServerOptions` / `RtcServerOptions`.

### Source Layout

| Path | Purpose |
|------|---------|
| `src/main.cpp` | Entry point, init sequence (conf→log→signaling→rtc) |
| `src/global.cpp` | Global extern variables (g_conf, g_log, g_signaling_server, g_rtc_server) |
| `src/xrtc_server_def.h` | RtcMsg struct and CMDNO_* defines |
| `src/base/` | EventLoop (libev wrapper), Socket utils, LockFreeQueue, Conf, Log, xhead protocol, nlohmann/json |
| `src/server/signaling_server.*` | TCP listener, connection dispatch (round-robin to workers) |
| `src/server/signaling_worker.*` | Protocol parsing, client I/O, forwards requests to RtcServer |
| `src/server/rtc_server.*` | Message router, DTLS certificate management, CRC32-based worker selection |
| `src/server/rtc_worker.*` | WebRTC stream processing (push/pull/answer), owns RtcStreamManager |
| `src/server/tcp_connection.*` | Per-connection state (SDS buffer, STATE_HEAD/STATE_BODY state machine) |
| `src/stream/rtc_stream_manager.*` | WebRTC push/pull stream creation (calls rtcbase) |
| `test/` | Google Test unit tests (private member access via `#define private public`) |
| `conf/` | YAML config files |
| `third_party/` | Pre-built static libs (libev, yaml-cpp, OpenSSL, abseil, rtcbase) |

### Dependencies

- **rtcbase** — sibling project at `../rtcbase/`, provides WebRTC infrastructure (SDS, Slice, logging, DTLS certificate, CRC32)
- **libev** — event loop backend
- **yaml-cpp** — config file parsing
- **OpenSSL** — DTLS certificate generation
- **Google Test** — unit tests (fetched at build time from local `../googletest-1.14.0/`)

### Test Conventions

- Tests use `#define private public` before including headers to access internals
- Tests run from project root (CMake sets WORKING_DIRECTORY)
- Both pure-logic tests (no event loop) and integration tests (with thread start/stop)
