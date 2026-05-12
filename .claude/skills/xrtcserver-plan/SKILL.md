---
name: "xrtcserver-plan"
description: "消息流程 + commit-by-commit 方式教学，手写实现 WebRTC 媒体中转服务"
---

# xrtcserver 实现路线

## 仓库路径

- **你的 xrtc-server**：`/home/ydqun/workspace/lession/xrtc-server`
- **参考项目 xrtcserver**：`/home/ydqun/workspace/lession/xrtcserver`
- **背景笔记**：`/home/ydqun/workspace/lession/xrtc-server/note/`
- **参考起始 commit**：`ca762a97`

## 教学方式

1. **消息流程驱动**：始终知道"这个消息现在到了哪一步"
2. **commit-by-commit**：按参考仓库的提交顺序，逐个 commit 指导
3. **Phase 开始前三问**（grill）：
   - 这个 Phase 的背景知识你读过了吗？（`note/` 下的对应文档）
   - 你想自己手写还是我逐步指导？
   - 本 Phase 你重点关注什么？

## 当前进度

| Phase | 内容 | 状态 | 参考 commits |
|-------|------|------|-------------|
| 1 | TCP xhead 解析 + JSON body | ✅ done | — |
| 2 | 跨线程路由 (CRC32 + LockFreeQueue) | ✅ done | — |
| 3 | SDP offer 生成 (codec) | ✅ done | — |
| 4 | UDPPort + Candidate 生成 | ✅ done | — |
| 5 | DTLS fingerprint 填入 SDP | ✅ done | — |
| 6 | PeerConnection + TransportController | ✅ done | — |
| 7 | ANSWER + set_remote_sdp | ✅ done | — |
| **8** | **STUN 消息编解码** | **WIP** | `7012b73` → `c18f33d` (10 commits) |
| 9 | ICE 连接状态机 + Controller | pending | `a0bdda8` → `9bb997d` |
| 10 | DTLS 握手 + SRTP | pending | `b01ce7f` → `092c650` |
| 11 | PC/ICE/Agent 状态聚合 | pending | `86a58c9` → `fc5ae77` |
| 12 | PULL 流 + STOP 命令 | pending | `5a0f518` → `84b1752` |
| 13 | RTP/RTCP 数据转发 | pending | `e43ac54` → `3372f65` |
| 14 | 异常处理 + 完整联调 | pending | `5a1e89b` → `8e8a514` |

## Phase 8 commit 清单

| # | commit | 内容 |
|---|--------|------|
| 1 | `7012b73` | StunMessage::read() — 解析消息头+属性循环 |
| 2 | `fb675f2` | StunByteStringAttribute — read/write/copy_bytes |
| 3 | `ba73fa3` | 解析并验证 USERNAME 属性 |
| 4 | `92bfad0` | 解析并验证 MESSAGE-INTEGRITY 属性 |
| 5 | `615e9ee` | STUN 绑定请求异常处理 |
| 6 | `fde76e2` | 创建 peer 反射地址 |
| 7 | `a70fdf5` | 创建 IceConnection |
| 8 | `98e68ce` | 构造 binding 响应 |
| 9 | `7a08e53` | 添加 MESSAGE-INTEGRITY 到响应 |
| 10 | `ef60ffe` | 添加 FINGERPRINT 到响应 |
| 11 | `c18f33d` | 发送 binding 响应 |

## 参考文件速查

每个 Phase 用到的参考代码：

| Phase | 参考文件（在 xrtcserver/src/ 下）|
|-------|-------------------------------|
| 8 | `ice/stun.h`, `ice/stun.cpp`, `ice/udp_port.h`, `ice/udp_port.cpp` |
| 9 | `ice/ice_connection.h/.cpp`, `ice/ice_controller.h/.cpp`, `ice/stun_request.h/.cpp`, `ice/ice_transport_channel.h/.cpp`, `ice/ice_agent.h/.cpp` |
| 10 | `pc/dtls_transport.h/.cpp`, `pc/dtls_srtp_transport.h/.cpp`, `pc/srtp_session.h/.cpp`, `pc/srtp_transport.h/.cpp` |
| 11 | `pc/peer_connection.h/.cpp`, `ice/ice_transport_channel.h/.cpp`, `ice/ice_agent.h/.cpp` |
| 12 | `stream/pull_stream.h/.cpp`, `stream/rtc_stream_manager.h/.cpp`, `server/rtc_worker.cpp` |
| 13 | `module/rtp_rtcp/rtp_utils.h/.cpp` |
| 14 | 端到端测试 |

## 每个 Phase 结束后的验证

1. 编译通过：`cd build && cmake .. && make -j$(nproc)`
2. 89 个已有测试全过：`./build/xrtc-server-test`
3. 新功能的端到端验证（如适用）

## 每 Phase 的标准流程

```
1. Grill（三问）
2. 查看参考仓库对应 commit 的 diff
3. 逐 commit 写代码
4. 编译 + 测试验证
5. git commit（你决定时机）
```

## 关键架构提醒

```
消息路径（PUSH）：
  TCP → SignalingWorker → RtcServer(CRC32) → RtcWorker
    → RtcStreamManager → PushStream → PeerConnection
      → TransportController → IceAgent → IceTransportChannel
        → UDPPort → AsyncUdpSocket

消息路径（ANSWER）：
  TCP → SignalingWorker → RtcServer → RtcWorker
    → RtcStreamManager → PushStream → PeerConnection::set_remote_sdp()
      → TransportController::set_remote_description()
```

## 背景知识文档

| 文档 | 内容 |
|------|------|
| `note/step1-sdp-background.md` | SDP 结构、各行含义 |
| `note/phase8-stun-background.md` | STUN 协议二进制格式、属性、验证流程 |
| `note/phase8-14-learning-plan.md` | Phase 8-14 完整学习计划 |

## 常见陷阱

1. candidate SDP 格式：`typ` 关键字必须写，否则客户端解析失败
2. STUN Message Type 位布局：C0=bit4, C1=bit8，class mask=0x0110
3. CRC32 路由确保同一 stream_name 到同一 worker
4. `LockFreeQueue` 只适用于 SPSC；多生产者用 `std::mutex`
5. 背景文档中的技术细节必须从参考代码推导，不能凭训练数据记忆
