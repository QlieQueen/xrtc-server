---
name: "xrtcserver-plan"
description: "消息流程 + commit-by-commit 方式教学，手写实现 WebRTC 媒体中转服务"
---

# xrtcserver 实现路线

## 仓库路径

- **你的 xrtc-server**：`/home/ydqun/workspace/webrtc/xrtc-server`
- **参考项目 xrtcserver**：`/home/ydqun/workspace/webrtc/xrtcserver`
- **背景笔记**：`/home/ydqun/workspace/webrtc/xrtc-server/note/`
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
| 8 | STUN 消息编解码 | ✅ done | `7012b73` → `c18f33d` (11 commits) |
| 9 | ICE 连接状态机 + Controller | ✅ done | `a0bdda8` → `9bb997d` (21 commits) |
| 10 | DTLS 握手 + SRTP | ✅ done | `b01ce7f` → `f5719ec` (10 commits) |
| 11 | PC/ICE/Agent 三态聚合 | ✅ done | `86a58c9` → `fc5ae77` (5 commits) |
| 12 | PULL 流 + STOP 命令 + SSRC | ✅ done | `d73cd98` → `29eb026` (10 commits) |
| **13** | **RTP/RTCP 加解密 + 转发** | **✅ done** | `092c650` → `3372f65` (12 commits) |
| 14 | 异常处理 + 完整联调 | ✅ done | `84b1752` → `8e8a514` (6 commits) |

### 全 Phase 完成

所有 14 个 Phase 已全部完成。端到端推拉流链路可正常工作。

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

## Phase 9 commit 清单

| # | commit | 内容 |
|---|--------|------|
| 1 | `a0bdda8` | UDP 包高性能发送（你的 `_send_data_from_list` 已做） |
| 2 | `af25839` | ICE 连接保活 — 定时检查连接是否超时 |
| 3 | `da9e320` | 发送 STUN 错误响应消息 |
| 4 | `89b5985` | 服务侧连通性检查 — `_on_check_and_ping()` |
| 5 | `97afa2c` | ICE 传输通道 ping 周期 — timestamp 追踪 |
| 6 | `d95f7ea` | ICE 连接 ping 优先级选择 |
| 7 | `c365708` | 选择一个连接执行 ping 请求 |
| 8 | `410644e` | 构造 STUN 绑定请求（ConnectionRequest::prepare） |
| 9 | `6225d4f` | ICE 普通提名和积极提名 |
| 10 | `183ca73` | 发送 STUN ping 请求 |
| 11 | `c90a868` | 处理 STUN 响应（RTT 计算） |
| 12 | `3710be1` | 输出 RTT 和 ping id |
| 13 | `d26dce4` | 更新 Ice 连接读写状态 |
| 14 | `f017815` | 选中连接切换策略 |
| 15 | `cb60f45` | 切换策略考虑连接优先级 |
| 16 | `722f876` | 开始切换 selected 连接 |
| 17 | `a60d5ee` | STUN 请求错误响应处理 |
| 18 | `b808117` | 设置 Candidate pair 状态 |
| 19 | `63b5f45` | 处理 ICE ping 周期问题 |
| 20 | `9bc4471` | ICE 连接探活机制 |
| 21 | `9bb997d` | 更新 ICE 传输通道状态 |

## Phase 10 commit 清单

| # | commit | 内容 |
|---|--------|------|
| 1 | `b01ce7f` | DtlsTransport 骨架 + OpenSSL BIO 对接 |
| 2~10 | — | DTLS 握手完成 (详见 CLAUDE.md) |

## Phase 11 commit 清单

| # | commit | 内容 |
|---|--------|------|
| 1 | `86a58c9` | 计算 PC 的状态 |
| 2 | `9047639` | 计算 Ice 传输通道的状态 |
| 3 | `c98be95` | 计算 IceAgent 的状态 |
| 4 | `d77f345` | 重新计算 PC 状态 |
| 5 | `fc5ae77` | PC 失败状态下的资源清理 |

## Phase 12 commit 清单

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

## Phase 13 commit 清单

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

## Phase 14 commit 清单

| # | 参考 commit | 本地 commit | 内容 |
|---|-----------|------------|------|
| 1 | `01880eb` | `1.5.105` | 转发 RTP 数据骨架 |
| 2 | `1cddb36` | `1.5.106` | 加密 RTP 发送 |
| 3 | `3372f65` | `1.5.107-108` | 加密 RTP + RTCP + 双向转发 |
| 4 | `84b1752` | `1.5.109` | 停止拉流命令 |
| 5 | `5a1e89b` | `1.5.110` | 异常处理 + 资源释放 |
| 6 | `8e8a514` | `1.5.111` | 联调修复 — on_stream_exception |

## 参考文件速查

每个 Phase 用到的参考代码：

| Phase | 参考文件（在 xrtcserver/src/ 下）|
|-------|-------------------------------|
| 8 | `ice/stun.h`, `ice/stun.cpp`, `ice/udp_port.h`, `ice/udp_port.cpp` |
| 9 | `ice/ice_connection.h/.cpp`, `ice/ice_controller.h/.cpp`, `ice/ice_transport_channel.h/.cpp`, `ice/ice_agent.h/.cpp`, `ice/stun_request.h/.cpp` |
| 10 | `pc/dtls_transport.h/.cpp`, `pc/dtls_srtp_transport.h/.cpp`, `pc/srtp_session.h/.cpp`, `pc/srtp_transport.h/.cpp` |
| 11 | `pc/peer_connection.h/.cpp`, `ice/ice_transport_channel.h/.cpp`, `ice/ice_agent.h/.cpp` |
| 12 | `stream/pull_stream.h/.cpp`, `stream/rtc_stream_manager.h/.cpp`, `server/rtc_worker.cpp` |
| 13 | `module/rtp_rtcp/rtp_utils.h/.cpp`, `pc/dtls_srtp_transport.h/.cpp`, `pc/srtp_session.h/.cpp` |
| 14 | 端到端测试 + 联调 |

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
