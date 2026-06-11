# Phase 13 总结：RTP/RTCP 加解密

## 进度

12 commits，全部完成 ✅

| # | commit | 内容 |
|---|--------|------|
| 1 | `1.5.92` | 创建 DtlsSrtpTransport 加密传输通道骨架 |
| 2 | `1.5.93` | 从 DTLS 中导出 SRTP 密钥 — _extract_params 拆分 client/server key+salt |
| 3 | `1.5.94` | 创建 SRTP 会话并设置参数 — SrtpSession 骨架 + set_rtp_params |
| 4 | `1.5.95` | 引入 libsrtp 库 — CMake 链接 libsrtp2.a |
| 5 | `1.5.96` | 初始化 libsrtp — 全局引用计数 + srtp_init + event_handler thunk |
| 6 | `1.5.97` | 创建 SRTP 上下文结构 — _do_set_key + srtp_policy_t 填充 + auth_tag_len |
| 7 | `1.5.98` | 完成 SRTP 的设置和更新 — set_recv + update_send/recv + 激活 set_rtp_params |
| 8 | `1.5.99` | 开始安装 DTLS-SRTP — _on_dtls_state + _maybe_setup_dtls_srtp + _setup_dtls_srtp |
| 9 | `1.5.100` | 解复用 RTP 和 RTCP — infer_rtp_packet_type + _on_read_packet 接入信号 |
| 10 | `1.5.101` | 实现 RTP 判断方法 — is_rtp_packet + is_rtcp_packet 基于 RFC 5761 |
| 11 | `1.5.102` | RTP 数据包解密 — unprotect_rtp + _on_rtp_packet_received + 解析工具 |
| 12 | `1.5.103-104` | RTCP 解密 + 信号全链路转发到 RtcStreamManager |

## 新增文件

| 文件 | 内容 |
|------|------|
| `src/pc/srtp_session.h/.cpp` | SrtpSession — libsrtp 封装 (srtp_create/update/unprotect) |
| `src/pc/srtp_transport.h/.cpp` | SrtpTransport 基类 — 管理 send/recv 两个 session |
| `src/pc/dtls_srtp_transport.h/.cpp` | DtlsSrtpTransport — 密钥导出 + 加解密编排 + 信号发射 |
| `src/module/rtp_rtcp/rtp_utils.h/.cpp` | RTP/RTCP 解复用 + 包头解析工具 |

## 修改文件

| 文件 | 改动 |
|------|------|
| `CMakeLists.txt` | 添加 module/rtp_rtcp 目录 |
| `src/pc/dtls_transport.h` | 新增 writable() 访问器 |
| `src/pc/transport_controller.h/.cpp` | 创建 DtlsSrtpTransport + dtslcC 信号转发 |
| `src/pc/peer_connection.h/.cpp` | 订阅 + 转发 RTP/RTCP 信号 |
| `src/stream/rtc_stream.h/.cpp` | 转发 RTP/RTCP 到 listener |
| `src/stream/rtc_stream_manager.h/.cpp` | 实现 listener 空桩 |

## 核心类关系

```
SrtpSession  (libsrtp srtp_ctx_t 封装，单方向加解密)
     ↑
SrtpTransport  (管理 _send_session + _recv_session，对外提供 protect/unprotect)
     ↑
DtlsSrtpTransport  (从 DtlsTransport 导出密钥，串联 DTLS + SRTP)
     ↑
TransportController  (创建 + 管理 DtlsSrtpTransport，信号向上转发)
```

## 数据流

```
收到加密包: UDP → IceTransportChannel → DtlsTransport(signal_read_packet)
              → DtlsSrtpTransport::_on_read_packet
                → infer_rtp_packet_type() 解复用
                → _on_rtp/rtcp_packet_received
                  → srtp_unprotect() 解密
                  → signal_rtp/rtcp_packet_received
                    → TransportController → PeerConnection
                      → RtcStream → RtcStreamManager (空实现)
```

## 真实 bug / 理解错误

1. **is_rtcp_packet 缺 version 检查** — 我们最初实现没有检查 `has_correct_rtp_version`，参考代码有。虽然实际影响不大（PT 在 64-95 且 version!=2 的包极少），但为健壮性应加上。
2. **get_rtcp_type 误用 & 0x7F 掩码** — 一开始混淆了 RTP 和 RTCP 包头，把判定用的低 7 位掩码用到了获取实际类型上。RTCP 的 PT 是完整 8 bit（SR=200, RR=201 等），不应掩码。
3. **_setup_dtls_srtp 两个 if 独立导致未初始化变量** — 最初写成两个独立 if 块，`_extract_params` 失败后仍会执行 `set_rtp_params`。应该用 `||` 串联或分别 return。

## 协作中"好"的点

1. 从入口点 `set_dtls_transport` 开始推导，逐层展开依赖，避免了"一次给出所有代码"的问题
2. `srtp_policy_t` 逐字段拆解，先理解再写代码
3. RTP/RTCP 包头格式先画出来再写判断逻辑

## 下一个 Phase 注意事项

Phase 14: 异常处理 + 完整联调

- RtcStreamManager 的空实现需要填转发逻辑（PushStream → PullStream）
- 加密发送方向（srtp_protect）还差实现
- 停止拉流命令处理
- 端到端测试验证完整链路
