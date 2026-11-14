# WebRTC 待办清单

> 当前批次（双向音频、WHEP/WHIP client、ICE restart、TWCC pacer/AIMD、帧级门控）已完成并提交。
> 以下为后续可选增强，按优先级排列。

## P0 安全与正确性

- [ ] **DTLS 指纹校验**：client 角色当前完成 DTLS 握手但未校验对端证书指纹与 answer/offer
      中 `a=fingerprint` 是否一致。需要在 `dtls.c` 暴露对端证书 SHA-256，并在媒体启动前比对。
- [ ] **SRTP 重放窗口测试**：补充乱序/重放/ROC 跨越的测试向量。
- [ ] **SDP 回归**：m-line 顺序、BUNDLE、拒绝 m-line、TWCC extmap、PCMU/Opus 矩阵的断言测试。
- [ ] **会话销毁回归**：反复创建/销毁 WHEP/WHIP/client 会话，检查队列、reader、ingest、
      DTLS/SRTP 上下文无泄漏（ASan/Valgrind）。

## P1 网络与稳定性

- [ ] **TURN over TCP/TLS**：当前 zice 仅支持 UDP TURN；TCP/TLS URL 会告警并跳过。
      需要实现 TURN over TCP/TLS 客户端（Allocate/Permission/Channel/Send/Data 分帧与 TLS 连接）。
- [ ] **WHIP 接收队列额度**：入站 RTP 目前直接进 zm ingest；补按会话的接收字节/包预算与
      丢弃原因统计，避免慢 ingest 拖垮内存。
- [ ] **更精细拥塞控制**：当前为 TWCC 丢包 EWMA + 本地发送等待 EWMA + AIMD + 帧级丢弃；
      可继续做 TWCC 接收间隔趋势（delay-based）与更平滑的速率爬升。
- [ ] **ICE restart 扩展**：目前仅信令 viewer 重新 offer 的路径；可扩展到 WHEP/WHIP client
      在网络切换时主动重启 ICE。

## P2 验收与文档

- [ ] **浏览器矩阵**：Chrome / Edge / Firefox / Safari；`viewer.html` 画面、对讲、重连。
- [ ] **互操作矩阵**：OBS WHIP 推流、ffmpeg/浏览器 WHEP 拉流、设备↔平台双向。
- [ ] **压测与长稳**：`tests/webrtc_soak.py` 已支持并发 viewer；补 20% 丢包/100ms 抖动、
      网络切换、24h soak。
- [ ] **真机联调**：ipc-mini 采集 → H.264 baseline + G.711 → 云信令/TURN → 浏览器；
      检查首帧时间、延迟、CPU/内存、断网恢复。
- [ ] **运维面**：`getWebrtcClients/getWebrtcStats` 接入管理页面；补充设备端/平台端配置示例。

## 已知限制

- DataChannel/AI 数据通道不实现，浏览器 viewer 的 `detect` 通道会被拒绝（页面提示 AI 断开属预期）。
- 无转码：仅 H.264 视频直通，音频 Opus/PCMA/PCMU 直通；AAC 源在 WebRTC 播放时过滤为纯视频。
- TURN TCP/TLS 未实现；需要 UDP 可达的 TURN。
