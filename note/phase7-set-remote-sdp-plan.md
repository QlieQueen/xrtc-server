# Phase 7 Step 4: `set_remote_sdp()` 实现方案

## 总体结构

```
PeerConnection::set_remote_sdp(const std::string& sdp)
  │
  ├─ 1. 按行分割 SDP 文本
  │
  ├─ 2. 创建 _remote_desc = SessionDescription(SdpType::k_answer)
  │
  ├─ 3. 逐行解析，逐步填充 _remote_desc 的各层数据
  │     ├─ 先填充 contents（m= 行 → MediaContentDescription）
  │     ├─ 再填充 transport_infos（a=ice-ufrag/pwd/fingerprint/setup）
  │     ├─ 再填充 groups（a=group:BUNDLE）
  │     └─ 再填充 streams（a=ssrc:/a=ssrc-group:）
  │
  └─ 4. 调 _transport_controller->set_remote_description(_remote_desc.get())
```

## 逐行解析决策表

每行的解析目标：

| SDP 行 | 作用 | 对应 _remote_desc 的哪部分 |
|--------|------|--------------------------|
| `m=audio/video...` | 标记新的媒体段开始 | `add_content(MediaContentDescription)` |
| `a=group:BUNDLE a v` | BUNDLE 分组 | `add_group(ContentGroup("BUNDLE"))` |
| `a=mid:xxx` | 媒体标识 | 验证当前 content 的 mid（Audio 默认 "audio"） |
| `a=ice-ufrag:xxx` | 远端 ICE ufrag | TransportDescription.ice_ufrag |
| `a=ice-pwd:xxx` | 远端 ICE pwd | TransportDescription.ice_pwd |
| `a=fingerprint:sha-256 XXX` | 远端 DTLS 指纹 | TransportDescription.identity_fingerprint |
| `a=setup:active/passive` | 远端 DTLS 角色 | TransportDescription.connection_role |
| `a=sendonly/recvonly` | 媒体方向 | MediaContentDescription.direction |
| `a=rtcp-mux` | RTP/RTCP 复用 | MediaContentDescription.rtcp_mux |
| `a=ssrc:N attr:value` | 媒体源标识 | StreamParams (聚合后 add_stream) |
| `a=ssrc-group:SEM ssrcs` | SSRC 分组 | SsrcGroup (关联到对应 StreamParams) |

以下行忽略：`v=`, `o=`, `s=`, `t=`, `c=`, `a=rtcp:`, `a=rtpmap:`, `a=fmtp:`, `a=candidate:`, `a=msid-semantic:`

---

## 分步实现计划

### Step 4a — 骨架 + contents 填充

**目标**：解析出 `m=` 行，创建对应的 MediaContentDescription，得到有 contents 的 `_remote_desc`。

**实现内容**：
- 字符串分割工具函数 `split(s, delim)`
- `connection_role` 字符串转枚举 `string_to_connection_role(role)`
- 主循环框架：逐行遍历，只处理 `m=` 和 `a=group:BUNDLE`
- `m=audio` → `new AudioContentDescription()` → `add_content()`
- `m=video` → `new VideoContentDescription()` → `add_content()`
- 维护 `current_content` 指针

**验证**：解析后 `_remote_desc->contents().size()` > 0

### Step 4b — transport_infos 填充

**目标**：解析 ICE/DTLS 属性，创建 TransportDescription。

**实现内容**：
- 辅助函数：`_get_or_create_td(mid)` — 惰性创建 TransportDescription
- 处理 `a=ice-ufrag:`, `a=ice-pwd:` → 填入 td
- 处理 `a=fingerprint:sha-256 XXX` → `rtc::SSLFingerprint::CreateFromRfc4572(alg, digest)`
- 处理 `a=setup:` → `string_to_connection_role()` → td.connection_role
- 用 `add_transport_info(td)` 存入 `_remote_desc`

**验证**：`get_transport_info(mid)->ice_ufrag` 非空

### Step 4c — groups + direction + rtcp-mux 填充

**目标**：解析 BUNDLE 组、方向、rtcp-mux。

**实现内容**：
- 处理 `a=group:BUNDLE` → ContentGroup → add_group
- 处理 `a=sendonly/recvonly` → set_direction
- 处理 `a=rtcp-mux` → set_rtcp_mux(true)

**验证**：`get_group_by_name("BUNDLE")` 非空，content 的 direction 正确

### Step 4d — SSRC + StreamParams 填充

**目标**：解析 `a=ssrc:` 和 `a=ssrc-group:` 行，构建 StreamParams 并分配。

**实现内容**：
- 维护 `map<uint32_t, StreamParams> pending_ssrcs` 和 `vector<SsrcGroup> pending_ssrc_groups`
- 解析 `a=ssrc:N cname:X` → pending_ssrcs[N].cname = X; pending_ssrcs[N].ssrcs = {N}
- 解析 `a=ssrc:N msid:S T` → pending_ssrcs[N].stream_id = S; pending_ssrcs[N].id = T
- 解析 `a=ssrc-group:SEM ssrc1 ssrc2` → pending_ssrc_groups.emplace_back(SEM, {ssrc1, ssrc2})
- 遍历结束后，将 pending_ssrcs 中的 StreamParams 分配到对应 content
- 将 pending_ssrc_groups 关联到包含相关 SSRC 的 StreamParams

**验证**：content 的 `streams()` 非空，SSRC group 正确关联

### Step 4e — 调 set_remote_description()

**目标**：解析完成后调用 `_transport_controller->set_remote_description(_remote_desc.get())`。

一行代码。目的：把远端 ICE 参数下发给 IceAgent → IceTransportChannel。

---

## 关键辅助函数

```cpp
// 按分隔符分割字符串
static std::vector<std::string> split(const std::string& s, const std::string& delim);

// connection_role 字符串转枚举
static ConnectionRole string_to_connection_role(const std::string& role);

// 惰性获取或创建 TransportDescription
std::shared_ptr<TransportDescription> _get_or_create_remote_td(const std::string& mid);
```

## 不需要修改的头文件

`peer_connection.h` 中 `set_remote_sdp()` 声明已存在，不需要改动。

---

## 和后续 Steps 的衔接

Step 4 完成后，`_remote_desc` 构造完毕。Step 5 (RtcStream::set_remote_sdp) 和 Step 6 (RtcStreamManager::set_answer) 只是转发调用，Step 7 (RtcWorker::_process_answer) 填空。

全部完成后，两端 ICE 参数就绪，进入 Phase 8 (STUN)。
