# 代码地图与命名约定

> 给第一次打开仓库的人：先看这张图，再决定把代码加到哪。

## 1. 分层与依赖方向

```text
core / media ──> bridge ──> egress ──> publish / play / vod / pull
                                   │
                              session（各协议会话/业务）
                                   │
                               protocol（线协议编解码、无业务）
                                   ▲
ops（组合根：server / config / webapi / webhook）位于最顶层，可依赖所有层
device（GB28181 / ONVIF 设备与平台角色）依赖 core/media/protocol/session
```

- **只允许向下依赖**：`protocol` 通过 port 头（`*_port.h`、
  `play/dispatch.h`、`play/play_binding.h`、`publish/ingest/common/*`）回调角色，
  不直接 include 角色实现。
- **protocol 与 session 分离**：`protocol/<p>/` 只做字节流 → 完整报文的拆分/解析/
  编解码，不认识会话状态；`session/<p>/` 放会话状态机、业务分发与发送节拍。
  WebRTC 的 ICE 通过 `protocol/webrtc/ice_host.h` 与会话交互，不反向包含会话头。
- **ZM/ZN 边界**：`media/` 与 `bridge/` 是 ZM 适配层；`egress/`、`play/`
  只允许通过 `zm/*` 门面（packager / format / transport）使用 ZM。
  裸 ZM C 头目前只允许出现在 `bridge/`；`protocol/`、`session/`、`publish/`、`vod/`、
  `pull/`、`core/`、`ops/` 不得直接依赖 ZM。存量越界按 P1 逐层收敛。
- 依赖方向由 CMake 对象库与 port 头约束，新增越界应在评审中拦截。

## 2. 目录职责

```text
src/
  core/        进程内基础对象：frame / track / clock / event / GOP 环形队列 / 流注册表
  media/       codec ES 位流、容器读写、wire 编解码（薄封装 ZM）
  bridge/      ZM ↔ ZMS 适配（zm_mem/track/codec 映射）
  protocol/    线协议编解码：rtmp / rtsp / http(+websocket) / srt / webrtc / rtp 的
               拆分器、解析器、握手、SDP/STUN/DTLS 原语（不含业务）
  session/     协议会话与业务：http / rtmp / rtsp / webrtc 的会话状态机、路由、
               分发、发送节拍与生命周期
  message/     跨协议的报文外壳与文档视图：zms_msg（键值视图）、xml 助手
  publish/     推流接入编排：ingest 管线、RTMP/RTP 适配
  play/        播放分发与直播输出：dispatch、play_binding、hls / dash / http_flv
  vod/         点播：文件读取（io/）与 play lane（play/）
  pull/        拉流代理与远端播放器
  egress/      出站泵：pacing/（clock、pacing、live_policy）、source/、recorder/、pipeline/、rtp/、mpegts/
  device/      设备协议角色：core（通道/能力/PTZ 注册）、gb28181（设备端+平台端）、
               onvif（服务端 + WS-Discovery + PTZ 客户端）
  ops/         进程组合根：server、config、webapi、webhook、module registry
  util/        thread_pool / buf_pool / poller_drain / crypto / dns_cache
```

`include/zms/` 与 `src/` 一一对应（单一头文件树，无 `advanced/`）。

- 服务端会话门面（`*_server.h`、`rtmp/session.h`、`rtsp/session_auth.h`、
  `webrtc/whep.h`）位于 `include/zms/session/`，对应 `src/session/<proto>/`。
  对应的 `include/zms/protocol/<proto>/<同名>` 仅保留一行兼容 shim，新代码请用
  `zms/session/...`。`zms/session.h` 是会话层伞头，`zms/protocol.h` 只留线协议编解码。
- `src/message/`（原 `src/proto/`）承载跨协议报文外壳与 XML 助手；避免与
  `src/protocol/` 近名混淆。

## 3. 加代码时放哪

| 我要加… | 放这里 |
|---------|--------|
| 一个新的线协议（会话/握手/命令） | `protocol/<name>/`（编解码）+ `session/<name>/`（业务） |
| 协议客户端的拉流实现 | `protocol/<name>/client.c` 或 `pull/` |
| 一个新的输出封装/切片 | 优先 ZM（`libhls`/`libdash`/`libmov`…）；接 ZMS 的泵放 `egress/` |
| 一个协议无关的业务角色 | `publish/` / `play/` / `vod/` / `pull/` |
| 跨层回调接口 | 放在拥有方，命名 `<area>_port.h` |
| codec 位流/容器细节 | `media/codec/`、`media/container/`；能独立于会话就下沉 ZM |
| 设备/平台协议角色 | `device/<name>/` |
| 进程启动/配置/HTTP API | `ops/` |

## 4. 命名约定

- **文件**：`<area>_<part>.c`，例如 `session_play.c`、`rtp_payload_muxer.c`、
  `handshake_complex.c`。避免缩写（`hs_`、`rtp_over_rtsp` 这类已清理）。
- **协议目录内**：`protocol/<p>/` 放 `<p>_session*.c` 会话状态机；`client.c` 客户端；
  `*_server.c` 对外门面；`parser.c`/`dispatch.c` 输入解析与分发。
  `session/<p>/` 与 `protocol/<p>/` 同名成对，前者业务、后者编解码。
- **内部头**：`*_internal.h` 只给同目录/同层使用；port 头才是跨层契约。
  port 头按拥有方命名，允许同名（如 `play/vod_play_port.h` 与
  `egress/vod_play_port.h`）。
- **include 写法**：公共头一律 `#include "zms/..."`；同目录私有头用相对名
  （`"poller.h"`）；跨层复用的私有头应提升到 `include/zms/`，不要靠
  `src/` 作为 include root 绕行（P1 待收敛）。
- **历史命名**：`zms_http_hls_*` / `zms_http_dash_*` 是 HTTP 播出侧的旧命名，
  因 ABI 保留；新增内部符号不要再带 `http_`。
- **ZM 侧**：`zm_*`，只暴露 `zm_reader/writer/sink/io`、`zm_rtp_*`、`zm_sdp`。

## 5. 关键阅读顺序

1. `src/core/stream/hub.c` — 流注册表：谁在推、谁在播。
2. `src/session/rtmp/session*.c` + `session_transport.c` — 一条推流/播放会话怎么走。
3. `src/play/dispatch.c`、`src/play/http_port.h` — 播放如何分发到 HLS/DASH/FLV/RTP。
4. `src/egress/pipeline/egress_pipeline.c`、`src/egress/rtp/rtp_muxer.c` — 出站怎么打包。
5. `src/vod/play/play_lane.c` — 点播 seek/追帧。
6. `src/session/webrtc/play_session.c`、`src/protocol/webrtc/ice.c` — WebRTC 收发与 ICE。
