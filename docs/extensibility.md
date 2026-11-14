# 商业化边界与扩展约定（vhost / TLS / GB28181 / ONVIF）

> 目标：在进入第三批（商业化能力）前，把「媒体面 / 控制面 / 传输面」的边界定清楚。
> 后续 GB28181、ONVIF、集群、更多设备协议都按这里的约定接入，避免再改核心。

## 1. 媒体面的唯一标识：schema × vhost × app × stream

- `vhost` 从第三批开始成为一级维度，默认 `__defaultVhost__`。
- `[general] enableVhost=0`（默认）时所有输入 host 归一化为默认 vhost，行为与现在完全一致。
- `enableVhost=1` 时按协议解析：
  - RTMP：`tcUrl` 的主机名（或 connect app 里的 `?vhost=`）
  - RTSP：请求 URL 的主机名
  - HTTP / WS / WHIP / WHEP：`Host` 头（或 `?vhost=` 查询参数）
  - SRT：`streamid` 中的 `vhost=` 字段
- 注册表（`zms_media_source_*_ex`）、API 过滤、hook 载荷、media JSON、HLS 落盘目录都使用同一个 vhost 值。
- **不允许**任何协议自己维护一套流表；所有媒体源统一进 `core/stream/hub`。

## 2. 入站（ingest）边界

```
协议会话 (RTMP/RTSP/SRT/WebRTC/GB28181/ONVIF)
        │  只做：握手、鉴权、分帧、RTP/PS/TS 解包
        ▼
zms_live_ingest  (publish/ingest/common)
        │  统一：时间线、codec 配置、GOP 写入、事件/hook
        ▼
zms_media_source (core/stream/hub)  ──► gop_queue ──► 所有出站协议
```

- 新协议只允许通过 `zms_live_ingest_create_*_vhost()` + `zms_live_ingest_input_*()` 接入，
  不允许直接写 `zms_gop_queue`。
- GB28181：复用 `rtp_receiver` + `mpegps_demuxer` + `live_ingest`；
  SIP 信令、设备/通道管理放独立模块，不进入媒体路径。
- ONVIF：本质是 RTSP 拉流 + 设备发现。设备发现/PTZ 走独立控制面模块；
  媒体仍走 RTSP 客户端 / pull 代理，不新增媒体路径。

## 3. 控制面与 API 边界

- `/index/api/*` 保持 ZLM 兼容语义，不放设备协议私有字段。
- 新设备能力（GB28181 设备列表、ONVIF 发现、PTZ、抓图）使用独立命名空间
  （例如 `/index/api/gb28181/*`、`/index/api/onvif/*`），通过 `ops/server/module_registry`
  注册，不修改既有端点行为。
- 设备模块只依赖「端口头」：`zms_pull/player.h`、`zms/play/http_port.h`、
  `core/stream/*`，不直接依赖具体协议实现。
- 抓图/录像等需要帧访问的能力，通过 `play` 层的 reader/port 获取，不走全局状态。

## 4. 传输面（TLS）

- 目标：HTTP/HTTPS、WS/WSS、RTSP/RTSPS、RTMP/RTMPS 各有一组 `sslport`，与明文端口并存。
- ZN 目前只有 `tls_client`。第三批新增服务端 TLS 抽象：
  `zn_tls_server`（监听 + 接受 + 非阻塞握手 + 加解密 send/recv），
  协议服务只感知「安全会话」和「明文会话」两种 transport，不直接调用 OpenSSL。
- 证书配置统一放在 `[ssl]`（cert/key/ca），按协议 `[http]/[rtsp]/[rtmp] sslport=0` 控制是否监听。
- WebRTC 的 DTLS-SRTP 已独立实现，不与该 TLS 层混用。

## 5. 按需（demand）边界

- `hls_demand` 已实现：只在首个 HLS 请求时创建切片器。
- 后续 demand 模式统一在「sidecar 创建」处判断（`play/*/sidecar.c`），
  不允许在协议路由里散落创建逻辑。
- `directProxy`（RTSP RTP 透传）作为独立出站模式，放在 `protocol/rtsp/`，
  不改变 gop_queue 默认路径。

## 6. 第三批实施顺序

1. **3A 多 vhost**：core 注册表 + 各协议解析 + API/hook/JSON + 隔离测试。
2. **3B TLS**：zn_tls_server + HTTP/HTTPS/WSS，再扩展 RTSP/RTSPS、RTMP/RTMPS。
3. **3C RTSP**：lowLatency、multicast、directProxy。
4. **3D 设备扩展**：GB28181 / ONVIF 模块骨架（信令/发现独立，媒体复用 ingest）。
