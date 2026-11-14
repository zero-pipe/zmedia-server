# WebRTC 自闭环设计（设备端 / 平台端）

> 目标：在同一套 ZN/ZM 网络与媒体模型内，把 WHEP/WHIP 做成设备端和平台端都可开箱即用的能力。
> 不引入 KVS、Node 信令或第二套线程/内存模型；不做转码、GPU、AI 模型。

## 1. 范围

做：

- WHEP 播放（server / client）
- WHIP 推流（server / client）
- SDP、ICE、DTLS、SRTP、RTP/RTCP、弱网反馈
- 与现有 hub、GOP、ingest、MP4/HLS/RTSP/GB28181/ONVIF 路径复用
- 设备端与平台端同一会话核心，仅角色和装配不同

不做：

- KVS / libwebrtc / Pion 作为运行时桥接
- DataChannel、Simulcast、SVC、录制新容器
- H.265/AAC 转码；WebRTC 只做直通协商
- 自研完整 ICE/TURN 服务端（TURN 客户端做，TURN 服务可用 coturn）

## 2. 角色矩阵

| 角色 | 设备端 | 平台端 | 说明 |
|------|--------|--------|------|
| WHEP server | 是 | 是 | 浏览器/上级平台拉流 |
| WHIP server | 是 | 是 | 浏览器/设备推流 |
| WHEP client | 可选 | 是 | 平台主动拉设备 |
| WHIP client | 是 | 可选 | 设备主动推平台 |

四种角色共用：

```text
HTTP adapter (WHEP/WHIP)
        │
        ▼
zms_webrtc_session
  ├─ SDP offer/answer
  ├─ ICE (full / lite, STUN/TURN client)
  ├─ DTLS (OpenSSL)
  ├─ SRTP / SRTCP
  ├─ RTP/RTCP (payload, PLI/NACK, TWCC)
  └─ media binding
       ├─ play: hub GOP reader → RTP → SRTP → UDP
       └─ publish: UDP → SRTP → depacket → zms_live_ingest
```

设备端典型装配：

```ini
[webrtc]
enable=1
whep_server=1
whip_client=1
ice=stun
turn_url=turn:platform:3478?transport=udp
video=H264
audio=Opus,G711U
stun_server=
turn_server=turn:platform:3478?transport=udp
turn_user=
turn_pass=
# 可选：外部 WebSocket 信令（设备作为 master）
signaling_enable=1
signaling_url=ws://114.55.94.148:8089/ws
signaling_room=door-1
signaling_token=
signaling_app=live
signaling_stream=cam1
```

WebSocket 信令协议与 `ipc-mini/cloud/signaling` 对齐：
`join`（role=master）→ `peer-joined` → viewer `offer` → master `answer` + `candidate`；
断开后 2s 重连，viewer 重新发 offer。

平台端典型装配：

```ini
[webrtc]
enable=1
whep_server=1
whip_server=1
whep_client=1
ice=stun
turn_url=turn:platform:3478?transport=udp
```

## 3. 协议能力现状与目标

| 能力 | 当前 | 目标 | 优先级 |
|------|------|------|--------|
| WHEP/WHIP HTTP | 有 | 保持，补 PATCH/DELETE | P1 |
| SDP | offer/answer、BUNDLE、rtcp-mux | 浏览器/设备矩阵回归 | P0 |
| ICE | ice-lite；client 为 full controlling；STUN/TURN UDP；信令 ICE restart | TURN TCP/TLS | P2 |
| DTLS | OpenSSL、指纹 | 指纹校验、重传、角色冲突测试 | P0 |
| SRTP/SRTCP | 自研、OpenSSL 密钥导出 | RFC 3711/7714 测试向量 | P0 |
| H.264 RTP | 直通、PLI/NACK 触发 GOP 重发 | NACK cache + 按包重传 | P3 |
| TWCC | 扩展头、协商、反馈解析、pacer/AIMD、帧级丢弃 | 更精细的 congestion control | P3 |
| 音频 | Opus/G711 直通；PCMU/PCMA 对讲 | 双向音频矩阵回归 | P1 |
| DataChannel | 无 | 不做 | - |
| 转码 | 无 | 不做 | - |

## 4. 内存模型

### 4.1 目标

- 一个进程只保留一个缓冲池域：`zms_buf_pool`。
- 热路径零拷贝或单次拷贝；所有队列有字节上限。
- 每会话有固定额度，慢消费者只丢弃自己的队列，不影响 hub 和其他会话。
- 会话销毁后所有缓冲区可归还；无跨会话悬垂引用。

### 4.2 缓冲分类

| 类别 | 典型大小 | 来源 | 生命周期 |
|------|----------|------|----------|
| RTP 发送包 | ≤ 2 KiB | poller 本地池 | 入队 → 发送 → 释放 |
| RTP 接收包 | ≤ 2 KiB | poller 本地池 | UDP → SRTP → depacket → ingest |
| SRTP 暂存 | 4 KiB | poller 本地池 | 会话级 |
| 控制消息（SDP/JSON） | ≤ 32 KiB | 全局池 | 请求作用域 |
| GOP/媒体帧 | 共享引用 | hub ring | 引用计数 |
| ICE/DTLS/SRTP 上下文 | 固定 | 模块分配 | 会话生命周期 |

### 4.3 每会话额度（建议默认值）

```text
rtp_send_queue  512 KiB   （按字节 + 包数双上限）
rtp_recv_queue  256 KiB
crypt_scratch     4 KiB
control          32 KiB
```

`64` 个会话的理论上限约 `50 MiB`（不含共享 GOP）。额度可配置，超过时：

- 优先丢弃非关键帧/最旧包；
- 请求关键帧，而不是无限增长；
- 记录 drop reason 和高水位。

### 4.4 所有权规则

1. `hub` 中的编码帧是共享只读引用；WebRTC play 只持有 reader 引用，不复制整帧。
2. RTP payloader 把包写入池化 buffer；SRTP 原地加密；UDP 发送后立即释放。
3. WHIP 接收的 RTP buffer 在 SRTP 解密、depacket 后交给 ingest；由 ingest 接管或复制一次，明确所有权。
4. 会话销毁时必须清空 RTP 队列、释放 SRTP/DTLS/ICE 上下文、归还 io buffer。
5. 禁止在会话回调里 malloc 大块；控制面除外。

### 4.5 可观测性

- 每会话：`rtpq_bytes`、`rtpq_bytes_max`、`rtpq_pkts`、`rtpq_dropped`、`srtp_bytes`。
- 全局：会话数、总内存、池命中/越档、每类分配失败次数。
- API：`/index/api/getWebrtcStats`（P0 先落内部计数，P1 接更多指标）。
- 测试：ASan/Valgrind、64 会话、20% 丢包、24h soak。

## 5. 实施阶段

### P0 内存与正确性（先做）

- 会话级 RTP 队列字节计数与上限；
- SRTP/DTLS 缓冲区统一走 `zms_buf_pool`；
- 会话销毁/重连的泄漏与悬垂检查；
- SDP/DTLS/SRTP 基础测试向量；
- 统计字段和日志。

验收：64 会话压测无增长；ASan 无泄漏；RTP 队列有高水位和丢弃计数。

### P1 角色闭环

- WHEP/WHIP server 已有；补 `PATCH`/`DELETE`；
- 增加 WHEP/WHIP client（offerer）；
- 设备端/平台端配置模板与 API；
- 直通 codec 矩阵：H264 + Opus/G711U，其他明确拒绝。

验收：设备 WHIP→平台 WHIP server；平台 WHEP client→设备 WHEP server；浏览器 WHEP/WHIP。

### P2 NAT 与 ICE

- full ICE 角色、候选对优先级、consent freshness；
- STUN srflx；
- TURN client：UDP/TCP/TLS、allocate/refresh/permission/channel；
- trickle ICE、ICE restart。

验收：对称 NAT 下经 coturn 播放/推流；ICE 重启恢复。

### P3 弱网算法

- NACK cache + 按包重传；
- PLI/FIR 节流；
- TWCC 反馈解析、发送 pacer、简单 AIMD；
- 关键帧请求与丢弃策略。

验收：20% 丢包、100ms 抖动下可播放；CPU/内存不随丢包增长。

### P4 验收与文档

- 浏览器矩阵：Chrome/Edge/Firefox/Safari；
- OBS WHIP、ffmpeg/浏览器 WHEP；
- 设备/平台双向矩阵；
- 24h soak；
- 运维 API 与示例。

## 6. 直通 codec 规则

| 源 | WebRTC | 处理 |
|----|--------|------|
| H.264 | H.264 | 直通 |
| H.265 | 无 | SDP 拒绝视频/明确错误 |
| Opus | Opus | 直通 |
| G.711U | PCMU | 直通（补协商） |
| AAC | 无 | SDP 不带音轨，不转码 |
| VP8/VP9 | VP8/VP9 | 仅在源本身为 VP8/VP9 时直通 |

设备端编码器应输出 H.264 baseline（profile-level-id 42e01f），否则浏览器兼容性不保证。

## 7. 与现有模块的关系

- 网络：只用 `zn_poller` / `zn_udp_server` / `zn_udp_client` / `zn_timer`；不新增线程模型。
- 媒体：WHEP 从 hub/GOP reader 读；WHIP 写入 `zms_live_ingest`；复用 ZM RTP/SDP/Bridge。
- 文件：不新增容器；录像仍走 MP4/fMP4/HLS。
- 设备/平台：同一 `zms_webrtc_session`，角色由路由/配置决定。
- 现有 WHEP/WHIP HTTP API 保持不变，作为唯一对外信令面。

## 8. 明确不做

- 不把 KVS 作为运行时桥接：会引入第二套线程、内存、TLS（mbedTLS）和 SCTP。
- 不做 DataChannel / AI 推送；AI 作为后续独立后端分析模块。
- 不做转码；没有匹配的直通 codec 就拒绝，而不是偷偷转码。
- 不做完整 GCC/Simulcast/SVC；先完成 ICE/TURN/NACK/pacer。

## 8.1 尚未完成（不阻塞当前设备/平台验证）

后续可选增强（含 DTLS 指纹校验、TURN TCP/TLS、压测矩阵）见 [webrtc-todo.md](webrtc-todo.md)。

## 9. 当前落地（P0 第一刀）

- WHEP 播放 RTP 队列增加字节上限 `ZMS_WEBRTC_PLAY_RTPQ_BYTES`，与包数上限共同约束；
- 增加每会话 `rtpqBytes / rtpqBytesMax / rtpqDroppedBytes / rtpqDroppedPkts / rtpqAllocFail` 计数；
- 视频发送增加 NACK 重传缓存（256 包 / 256 KiB），RTCP NACK 命中即重传，未命中回退关键帧请求；
- 解析 RTCP RR，统计丢包率、jitter、RTT，并纳入 `/index/api/getWebrtcStats`；
- WHIP 入站增加接收包/字节、丢弃、SRTP 失败计数，便于定位推流侧问题；
- 新增 `[webrtc] stun_server / turn_server / turn_user / turn_pass`，收集 srflx/relay 候选并写入 SDP answer；
- 选中 relay 候选时媒体改走 TURN channel/data 发送，避免直发 peer 绕过中继；
- WHEP/WHIP 支持 CORS 预检 `OPTIONS`，并暴露 `Location` 响应头，浏览器可直接调用；
- 新增外部 WebSocket 信令客户端（master）：join / offer / answer / candidate / 重连；
  支持最多 3 个 viewer，已与 `ws://114.55.94.148:8089/ws` 的 viewer 完成 offer→answer 联调；
- WHEP 播放已用 aiortc 打通媒体面：ICE connected、DTLS-SRTP、连续收到 640x360 视频帧；
  修复了播放 pump 定时器在 timer 回调内重入死锁、会话 poller 过早销毁、ICE 连接后过早停 agent 三个问题；
- WHEP/WHIP 增加 `PATCH` trickle ICE 远端候选追加；
- 会话记录创建/最近收包时间，新会话创建时回收握手超时（30s）或空闲超时（120s）的旧会话；
- 新增 `GET /index/api/getWebrtcCapabilities`（直通 codec、WHEP/WHIP、trickle、NACK 能力）；
- 新增只读 API `GET /index/api/getWebrtcStats`（含全局 totals）；
- offer 里出现 `transport-cc` 扩展时，answer 回写同一 extmap id 与 `a=rtcp-fb:<pt> transport-cc`，
  发送 RTP 时按协商 id 写入 TWCC 扩展；解析 RTPFB FMT=15 状态块（run-length / 1bit / 2bit），
  统计 `twccFbPkts / twccReceived / twccLost / twccLossPermille`；
- PLI/NACK miss 触发关键帧请求做 300ms 节流，统计 `pliCount / pliSuppressed`；
- TWCC 反馈驱动令牌桶 pacer（50ms 突发）与 AIMD：丢包 EWMA ≥10% 降速 15%，≤2% 回升 4%，
  统计 `sendBps / twccLossEwma / paceWaits`；未协商 TWCC 时 pacer 不启用；
- 播放方向增加接收 SRTP/SRTCP 上下文（客户端写密钥）：浏览器 RR/NACK/PLI/TWCC 反馈可解密；
  修正 SRTP 接收暂存与报文同缓冲时 tag 被 ROC 覆盖、SRTCP 使用 label 3/4/5 独立密钥；
- 音频直通扩展为 Opus/PCMA/PCMU；offer 的 audio m-line 选择 PCMU/PCMA/Opus 作为对讲下行，
  播放会话可 `recvonly/sendrecv` 接收浏览器对讲，按 `zms_webrtc_server_set_talk_audio_cb`
  投递；无消费者时 G711 回环给同一 viewer 便于联调，统计 `talkRxPkts/talkRxBytes`；
- 新增 WebRTC client 角色：full ICE controlling + DTLS client + 主动 HTTP(S) POST；
  `kind=whep` 拉流写入本地 `app/stream`，`kind=whip` 把本地源推给远端 WHIP；
  提供 `startWebrtcClient / getWebrtcClients / stopWebrtcClient` 与
  `[webrtc] whep_client_* / whip_client_*` 启动项；已在本机 WHEP/WHIP 服务端闭环验证；
- 信令会话支持 ICE restart：viewer 重新 offer 且 ICE ufrag 变化时，保留 DTLS/SRTP/GOP reader，
  只重建 ICE agent 并回新 answer；TURN TCP/TLS URL 明确报告不支持，仅使用 UDP TURN；
- TWCC 目标码率低于源码率时做帧级门控：1s 窗口实际码率超目标 90% 丢整帧，
  非关键帧丢弃后等待下一个关键帧并请求 GOP 关键帧重发；统计
  `adaptBps / adaptDropping / adaptDroppedFrames / adaptDroppedBytes`；
- 音频低延迟优先：音频 RTP 绕过视频 RTP 队列/pacer/丢帧策略直接 SRTP 发送，避免
  关键帧突发造成音频头阻塞；pacer/AIMD 仅在收到首个 TWCC 反馈后启用，避免启动阶段
  按源码率估计误降速；对讲仅在 offer 音频方向允许发送时启用；
- 新增 `tests/webrtc_soak.py` 并发 viewer 拉流压测（帧率/重连/异常统计）；
- 下一步：更多 SDP/DTLS/SRTP 回归向量、WHIP 接收队列额度与浏览器矩阵。
