# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Teaching Method: Entry-Point Code Derivation (CRITICAL)

When guiding the user through implementing a new feature or commit, **always start from the entry point** (the call site) and derive code outward, implementing only what the current call chain demands. Never dump all code for a commit at once.

**The rule**: Begin at the function where execution enters (e.g., `get_stun_message`). As you need a method/constant/type that doesn't exist yet, pause and implement it — then resume the calling code. This mirrors how a developer naturally writes code: from the call site downward.

**Example (bad)**: "This commit adds validate_message_integrity, IntegrityStatus enum, _validate_message_integrity_of_type, HMAC computation, _buffer saving in read(), k_stun_message_integrity_size constant, and the udp_port.cpp call." — dumping everything at once.

**Example (good)**:
1. Start at `get_stun_message` — "you need to call a method on stun_msg to validate MI, what should it take?"
2. When user figures out `validate_message_integrity(password)`, ask "what should it return? just bool? or are there multiple states?"
3. Only then introduce the `IntegrityStatus` enum
4. Then go into `validate_message_integrity` implementation, discovering what it needs step by step

**Key principles**:
- Constants, enums, helper methods are introduced **only when the calling code demands them**
- Each step is a question: "what does this need?" not "here's what to add"
- The user writes code at each step, not at the end

## Background Documentation Rules (CRITICAL)

When writing background/technical documentation (e.g., `note/phase*-background.md`), **every technical detail must be derived from and verified against the reference project's actual source code** (`/home/ydqun/workspace/webrtc/xrtcserver/src/`), never from training data "memory" or assumptions.

Concrete example of what went wrong:
- **STUN Message Type bit layout**: I drew `|0 0|M|M M|M|M M|C|M|M M|C|C|C|` from training data memory — completely fabricated.
- **Correct layout** (derived from `stun.h` constants `k_stun_class_mask = 0x0110`, `k_stun_method_mask = 0x3EEF`):
  ```
  |0 0|M11|M10|M9|M8|M7|C1|M6|M5|M4|C0|M3|M2|M1|M0|
  ```
  C0 is at bit 4 (`0x010`), C1 is at bit 8 (`0x100`). Class mask `0x0110` = bits 4+8.

Rules:
1. **Bit layouts, protocol fields, constants, enum values** — must be traced back to a specific line in the reference code. If you cannot point to the line, do not write the claim.
2. **"It looks right" is not a valid reason.** If you have not read it from the reference code in the current session, verify it before writing.
3. **When in doubt, read the reference code first**, write the document second.

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

## Phase Progress

| Phase | 内容 | 状态 | 最新 commit |
|-------|------|------|-------------|
| 1 | TCP xhead 解析 + JSON body | ✅ | — |
| 2 | 跨线程路由 | ✅ | — |
| 3 | SDP offer 生成 | ✅ | — |
| 4 | UDPPort + Candidate | ✅ | — |
| 5 | DTLS fingerprint | ✅ | — |
| 6 | PeerConnection + TransportController | ✅ | — |
| 7 | ANSWER + set_remote_sdp | ✅ | — |
| 8 | STUN 消息编解码 | ✅ | `d3ca1b3` |
| **9** | **ICE 连接状态机 + Controller** | **WIP** | `9c3f92a` |
| 10 | DTLS 握手 + SRTP | pending | — |
| 11 | PC/ICE/Agent 状态聚合 | pending | — |
| 12 | PULL 流 + STOP 命令 | pending | — |
| 13 | RTP/RTCP 数据转发 | pending | — |
| 14 | 异常处理 + 完整联调 | pending | — |

### Phase 9 当前进度

- **参考项目**: `/home/ydqun/workspace/webrtc/xrtcserver`, commits `a0bdda8` → `9bb997d` (21 个)
- **已完成**: 18/21 commits
  - ✅ commit 1: UDP 高性能发送 (用户提前实现)
  - ✅ commit 2: ICE 连接保活 (用户提前实现)
  - ✅ commit 3: StunErrorCodeAttribute + send_binding_error_response (`1.5.52`)
  - ✅ commit 4: IceController + WriteState + 连通性检查首次启动 (`1.5.53`)
  - ✅ commit 5: ICE 传输通道 ping 周期 — 定时器 + ping 间隔常量 + 凭据补填 (`1.5.53`/`1.5.54`)
  - ✅ commit 6: ICE 连接 ping 优先级选择 — 两层限速 + 自适应退避 (`1.5.54`)
  - ✅ commit 7: 选择一个连接执行 ping — round-robin 公平选择 (`1.5.55`)
  - ✅ commit 8: 构造 STUN 绑定请求 — ConnectionRequest::prepare (`1.5.56`)
  - ✅ commit 17: STUN 错误响应处理 — on_connection_request_error_response + 内存泄漏修复 + mark_connection_pinged (`1.5.65`)
  - ✅ commit 18: 设置 Candidate pair 状态 — IceCandidatePairState 枚举 + 状态流转 + 销毁链路 (`1.5.66`)  - ✅ commit 9: ICE 普通提名和积极提名 — USE-CANDIDATE (`1.5.57`)
  - ✅ commit 10: 发送 STUN ping 请求 — signal/slot 发送链路 (`1.5.58`)
  - ✅ commit 11: 处理 STUN 响应 — check_response 匹配分发 + MI 校验 (`1.5.59`)
  - ✅ commit 12: RTT 计算与 ping 日志 — elapsed() + print_pings_since_last_response (`1.5.60`)
  - ✅ commit 13: ICE 连接读写状态更新 — received_ping_response + update_receiving + set_write_state + signal_state_change (`1.5.61`)
  - ✅ commit 14: 选中连接切换策略 — sort_and_switch_connection + _compare_connections 4级优先级 + signal_state_change 链路 (`1.5.62`)
  - ✅ commit 15: RTT 指数平滑 + RFC 5245 pair priority + _compare_connections 第5级 + RTT fallback (`1.5.63`)
  - ✅ commit 16: 开始切换 selected 连接 — _ready_to_send + _maybe_switch_selected_connection (`1.5.64`)
  - ✅ commit 17: STUN 错误响应处理 — on_connection_request_error_response + 内存泄漏修复 + mark_connection_pinged (`1.5.65`)
  - ✅ commit 18: 设置 Candidate pair 状态 — IceCandidatePairState 枚举 + 状态流转 + 销毁链路 (`1.5.66`)
- **下一步**: commit 19 `63b5f45` — 处理 ICE ping 周期问题
- **知识文档**: `note/phase9-background.md`, `note/phase9-connectivity-check-concepts.md`
