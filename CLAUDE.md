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

## Collaboration Guidelines (CRITICAL)

These are agreed-upon rules for maximizing learning efficiency and code quality during commit-by-commit implementation.

### For Claude — 5-Rule Teaching Protocol (Phase 10+)

**Rule 1: Phase 启动 → 先给路标**

Before diving into commits, output:
- 最终目标（本 Phase 交付什么能力）
- 3~5 个里程碑（每个对应一个功能点）
- 与上一 Phase 的关系（从哪里接过来）
- 预计涉及的核心概念

**Rule 2: 进入 commit → 三段式**

Before the user reads each reference diff:
- **动机**：为什么要做？不做的后果？
- **变更摘要**：改哪些文件，新增/删除什么概念（类/函数/状态）
- **完成后状态**：现在代码"能做"什么了？
- （可选）下一个 commit 的入口猜想

**Rule 3: 不先抛抽象概念**

Default to explaining from the call chain / entry function. When asked "why this design?", answer with "what breaks if we don't do this" rather than giving definitions.

**Rule 4: "约定俗成"必须带验证提示**

When using phrases like "usually / generally / by convention":
- Proactively flag it as a knowledge point requiring verification
- Give verification method (grep pattern / reference code path / RFC section)

**Rule 5: Phase 结束 → 结构化反思清单**

After each Phase:
- 进度：完成了哪些里程碑
- 真实 bug / 理解错误
- 协作中"好"与"可改进"的点
- 下一个 Phase 的注意事项

### For the User

1. **Pre-read reference diffs before each Phase.** Read the reference repo's commits for the upcoming Phase (~15 min). Come with a fuzzy mental model and specific questions: "I get X, but Y doesn't make sense."

2. **Self-diagnose bugs 3 layers deep before asking for help.** When hitting unexpected behavior, write down "A caused B caused C" chain first, then verify with Claude. The 48ms dead-loop debugging was the gold standard.

3. **Challenge Claude.** If something sounds wrong or unclear, interrupt immediately. You have 7+ Phases of accumulated judgment — use it.

### Per-Phase Ritual

At the end of each Phase, spend 5 minutes verbalizing:
- What was the biggest pitfall?
- Which concept was most counter-intuitive?
- Does anything in this Phase connect to a pattern from a previous Phase?

This builds a cognitive map for faster pattern recognition in later Phases.

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

| Phase | 内容 | 状态 | 参考 commits |
|-------|------|------|-------------|
| 1 | TCP xhead 解析 + JSON body | ✅ | — |
| 2 | 跨线程路由 | ✅ | — |
| 3 | SDP offer 生成 | ✅ | — |
| 4 | UDPPort + Candidate | ✅ | — |
| 5 | DTLS fingerprint | ✅ | — |
| 6 | PeerConnection + TransportController | ✅ | — |
| 7 | ANSWER + set_remote_sdp | ✅ | — |
| 8 | STUN 消息编解码 | ✅ | `7012b73` → `c18f33d` (11 commits) |
| 9 | ICE 连接状态机 + Controller | ✅ | `a0bdda8` → `9bb997d` (21 commits) |
| **10** | **DTLS 握手 (DtlTransport)** | **✅ done** | `b01ce7f` → `f5719ec` (10 commits) |
| **11** | **PC/ICE/Agent 三态聚合** | **pending** | `86a58c9` → `fc5ae77` (5 commits) |
| 12 | PULL 流 + STOP 命令 + SSRC | pending | `d73cd98` → `29eb026` (10 commits) |
| 13 | DtlsSrtpTransport + SRTP 密钥 + RTP/RTCP 加解密 | pending | `092c650` → `3372f65` (12 commits) |
| 14 | STOP PULL + 异常处理 + 联调 | pending | `84b1752` → `8e8a514` (3 commits) |

### Phase 10 当前进度

- **参考项目**: `/home/ydqun/workspace/lession/xrtcserver`, commits `b01ce7f` → `f5719ec` (10 commits)
- **已完成**: 10/10 commits ✅
  - ✅ commit 1: DtlsTransport 封装 + signal_read_packet 链路 (`1.5.70`)
  - ✅ commit 2: ClientHello 缓存 (`1.5.71`)
  - ✅ commit 3: 安装 DTLS — StreamInterfaceChannel + _setup_dtls (`1.5.72`)
  - ✅ commit 4: 设置本地证书 + _dtls_active (`1.5.73`)
  - ✅ commit 5: 设置远程指纹 — 两条时序路径 (`1.5.74`)
  - ✅ commit 6: 启动 DTLS — _maybe_start_dtls + ClientHello replay + _handle_dtls_packet (`1.5.75`)
  - ✅ commit 7: DTLS 数据读取 — BufferQueue + _on_writable_state (`1.5.76`)
  - ✅ commit 8: DTLS 数据写入 — Write() + ICE send_packet 链路 (`1.5.77`)
  - ✅ commit 9: SRTP 密码套件 + is_rtp_packet + signal_read_packet (`1.5.78`)
  - ✅ commit 10: DTLS 传输状态 — _on_dtls_event + k_connected/k_closed (`1.5.79`)
  - ✅ commit 11: DTLS 接收状态 — _on_receiving_state mirror ICE receiving (`1.5.80`)
- **注意**: `092c650` (DtlsSrtpTransport) 在参考仓库中位于 Phase 11 之后，不属于 Phase 10
- **下一步**: Phase 11 `86a58c9` — 计算 PC 状态
- **知识文档**: `note/phase10-background.md`, `note/phase10-summary.md`

### Phase 11 commit 清单

| # | commit | 内容 |
|---|--------|------|
| 1 | `86a58c9` | 计算 PC 的状态 |
| 2 | `9047639` | 计算 Ice 传输通道的状态 |
| 3 | `c98be95` | 计算 IceAgent 的状态 |
| 4 | `d77f345` | 重新计算 PC 状态 |
| 5 | `fc5ae77` | PC 失败状态下的资源清理 |

### Phase 12 commit 清单

| # | commit | 内容 |
|---|--------|------|
| 1 | `d73cd98` | 分发服务支持停止推流 |
| 2 | `3bb6a82` | 修复编译警告 |
| 3 | `306556c` | 处理 PULL 命令 |
| 4 | `5a0f518` | 音视频转发方案设计 |
| 5 | `f494372` | 解析 SDP 的 SSRC 信息 |
| 6 | `a0bb515` | 解析 SSRC 组信息 |
| 7 | `a391d98` | 创建音视频 Track |
| 8 | `8025d5e` | 获取音视频源 |
| 9 | `e925bf0` | 设置音视频源 |
| 10 | `29eb026` | SDP 中增加 SSRC 描述 |

### Phase 13 commit 清单

| # | commit | 内容 |
|---|--------|------|
| 1 | `092c650` | 创建 DtlsSrtpTransport 加密传输通道 |
| 2 | `d52a949` | 从 DTLS 中导出密钥 |
| 3 | `c442539` | 创建 SRTP 会话并设置参数 |
| 4 | `7f79ec1` | 引入 libsrtp 库 |
| 5 | `ad8bbde` | 初始化 libsrtp 库 |
| 6 | `79057c3` | 创建 SRTP 上下文结构 |
| 7 | `c68cac3` | 完成 SRTP 的设置和更新方法 |
| 8 | `a177c95` | 开始安装 DTLS-SRTP |
| 9 | `e43ac54` | 解复用 RTP 和 RTCP 包 |
| 10 | `0cc8f93` | RTP 包判断方法 |
| 11 | `8764293` | RTP 数据包解密 |
| 12 | `b0ad147` | RTCP 数据包解密 + 获取 RTP/RTCP 数据包 |

### Phase 14 commit 清单

| # | commit | 内容 |
|---|--------|------|
| 1 | `01880eb` | 转发 RTP 数据 |
| 2 | `1cddb36` | 加密 RTP 发送 |
| 3 | `3372f65` | 加密 RTP + 发送加密 RTCP |
| 4 | `84b1752` | 处理停止拉流命令 |
| 5 | `5a1e89b` | 异常处理、代码完善 |
| 6 | `8e8a514` | 联调通过 |
