# zmedia-server (ZMS) · AI 指引

> C99 流媒体服务/可嵌入库。跨工程背景先读上级 [`../Claude.md`](../Claude.md)。
> **改代码前先读 [`docs/stream-pipeline.md`](docs/stream-pipeline.md)**——它是 ZMS 唯一总文档（能力矩阵、URL 规则、数据通路、改码规矩）。

## 1. 核心心智模型

> 协议可以多，**媒体路径只有一条**：推流进 → 理顺时间线与帧形态 → 放进 GOP 环形缓存 → 各协议从同一条环上按需重封出口。
>
> 动协议层之前先问：这一步在"共用媒体路径"里有没有对应物？没有才新增，有就复用。

## 2. 目录职责

| 目录 | 职责 |
|---|---|
| `include/zms/` | 对外头文件：`sdk.h`（嵌入）、`server.h`、`live.h`、`player.h`、`session.h`、`vod.h` |
| `src/core/` | 媒体内核：GOP 环、stream、codec_filter（**编解码白名单卡死处**） |
| `src/publish/` | 推流入（rtmp / rtp / ingest） |
| `src/session/` | 会话按协议分（rtmp / rtsp / http / webrtc） |
| `src/protocol/` | 协议实现（http / rtmp / rtp / rtsp / srt / webrtc） |
| `src/egress/` | 出口封装（rtp / mpegts / recorder / pacing / pipeline / source） |
| `src/play/` | HLS / DASH / HTTP-FLV 切片 |
| `src/vod/` | 本地点播 |
| `src/pull/` | 拉流代理 |
| `src/device/` | 设备侧：`gb28181/`、`onvif/` |
| `src/ops/` | 运维：`api/webapi/`（平台对接的 HTTP API）、`config/`、`server/`、`webhook/` |
| `examples/` | 可执行 demo：`demo_media_server.c` 是主入口样例 |
| `conf/config.ini` | 配置（每项都有注释；未知 key 会告警） |

## 3. 构建

```bash
# Linux/macOS：默认 RTMP/RTSP/HTTP
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Windows（最小）
cmake -S . -B build -DZN_ENABLE_WEPOLL=ON
cmake --build build --config Release
```

### 3.1 打开 SRT + WebRTC（本机依赖已就位）

本机已安装（2026-11 核实；README「打开 SRT / WebRTC」有完整说明）：

| 依赖 | 本机路径 | 目录要求 |
|---|---|---|
| libsrt | `D:\srt` | `include\srt\srt.h`、`lib\srt.lib`、`bin\srt.dll` |
| OpenSSL | `C:\Program Files\OpenSSL-Win64` | `include\openssl\ssl.h` |
| zice（WebRTC 协议栈） | 仓库自带 `3rdpart/zice/` | — |

```powershell
cmake -S . -B build `
  -DZN_ENABLE_WEPOLL=ON `
  -DZMS_ENABLE_SRT=ON `
  -DZMS_ENABLE_WEBRTC=ON `
  -DZMS_WEBRTC_USE_ZICE=ON `
  -DZMS_SRT_ROOT="D:/srt" `
  -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"
cmake --build build --config Release
Copy-Item "D:\srt\bin\srt.dll" "build\examples\Release\"   # 运行前必须带上 srt.dll
```

配置成功看 CMake 输出：`SRT transport enabled (libsrt found)`、`WebRTC DTLS-SRTP enabled (OpenSSL)`。
缺库只打 WARNING、对应功能不编入——**功能"开了却没生效"先看这三行**。
换机器时把路径写进 `cmake/build.local.cmake`（模板 `cmake/build.local.cmake.example`，已 gitignore）。
Linux：`apt install libsrt-openssl-dev libssl-dev`；macOS：`brew install srt openssl@3`。

> 运行时 DLL 一致性：`D:\srt\bin` 里同时带着 `srt.dll` 和 `libssl-4/ libcrypto-4` 系依赖，
> 而 `OpenSSL-Win64\bin` 是另一套 OpenSSL。**运行目录里只保留链接时用的那一套**，
> 混放会出现"能启动、一连 SRT/WebRTC 就崩或握手失败"。

### 3.2 用 ffmpeg 验证推流/播放

工具：`E:\13_zero\tools\ffmpeg\bin\`（含 `ffmpeg.exe`/`ffprobe.exe`；**该构建已 `--enable-libsrt`**，可直接测 SRT）。
测试素材就在同目录：`input.mp4`、`input265.flv`、`input_1080p.mp4` 等。

```powershell
# RTMP 推流（端口 1935，app/stream 自定）
ffmpeg -re -stream_loop -1 -i input.mp4 -c copy -f flv rtmp://127.0.0.1:1935/live/test
# 播放（HTTP-FLV 8080；也可以 rtsp://127.0.0.1:8554/live/test、.m3u8、WHEP）
ffplay http://127.0.0.1:8080/live/test.flv
ffprobe -v error -show_streams http://127.0.0.1:8080/live/test.flv

# RTSP 推流（ZMS 支持 RTSP RECORD）
ffmpeg -re -i input.mp4 -c copy -f rtsp rtsp://127.0.0.1:8554/live/test

# SRT 推流（需 ZMS 编译时打开 SRT、config `[protocol] enable_srt=1`；PowerShell 用单引号防止 #! 被解析）
ffmpeg -re -i input.mp4 -c copy -f mpegts 'srt://127.0.0.1:9000?streamid=#!::r=live/test,m=publish'
```

排查顺序：`ffprobe` 直接播 URL 能通 → ZMS 侧正常，问题在客户端/平台；不通 → 看 ZMS 日志 `zms_media_server.log`（临时 `log_level=debug`）与 `[protocol]` 开关。

## 4. 运行与配置要点

- 起 `demo_media_server` + `conf/config.ini`。最小改动只需 `[general] externIP`。
- 端口：RTMP 1935 / RTSP 8554 / HTTP+API 8080 / SRT 9000 / WebRTC UDP 50000-60000。
- `externIP` 填错 → WebRTC ICE、SDP `c=`、WebAPI 播放地址全错，播放器连不上。
- 排查问题先开 `log_level=debug`（量大，别长期），日志 `zms_media_server.log`。
- 部署参考 `deploy/`（Dockerfile / systemd / logrotate）。

## 5. 与平台对接：接口、密钥、认证

### 5.1 接口面

- 平台通过 `src/ops/api/webapi/` 调：设备（device_api）、国标（gb28181_api）、ONVIF（onvif_api）、RTP 收流（webapi_rtp）、录像（webapi_record）、WebRTC（webapi_webrtc）、统计（webapi_stats）。
- 事件/状态经 `src/ops/api/webhook/webhook_client.c` 回调平台。
- **改这些接口要同步平台 `internal/mediakit/client.go`。**

### 5.2 API 密钥（ZMS ↔ 平台）

- ZMS 侧：`conf/config.ini`
  ```ini
  [api]
  secret=zwszms2026      # 空 = 只接受本机访问；跨机/公网必须设置
  ```
  平台每次调用 `/index/api/*` 都带 `?secret=`。
- 平台侧：在「媒体节点」页面登记节点时填写 **SECRET**，落库 `zws_media_server.secret`；`internal/mediakit/client.go` 用它与 IP/HTTP 端口拼请求。
- **两端必须一致**：页面 secret ≠ config.ini secret → 平台侧节点"不可达/鉴权失败"，ZMS 侧日志能看到 401/拒绝。改密码要两边同时改。

### 5.3 Webhook（ZMS → 平台，单向、不鉴权）

```ini
[hook]
enable=1
on_stream_changed=http://127.0.0.1:18080/index/hook/on_stream_changed
on_stream_none_reader=http://127.0.0.1:18080/index/hook/on_stream_none_reader
on_play_stop=http://127.0.0.1:18080/index/hook/on_play_stop
on_server_started=http://127.0.0.1:18080/index/hook/on_server_started
on_server_keepalive=http://127.0.0.1:18080/index/hook/on_server_keepalive
on_record_mp4=http://127.0.0.1:18080/index/hook/on_record_mp4
```
- 平台入口：`/index/hook/*`（`internal/interfaces/hook/`，router.go 注册）。
- webhook 不参与鉴权，**只应指向可信内网平台地址**；平台换端口/域名时这 6 个地址会一起失效（表现为流状态不更新、录像不登记）。

### 5.4 推拉流 token 鉴权（可选，公网必须）

```ini
[auth]
enable=0            # 1 = 推/拉流 URL 必须带 ?token=
token_param=token
secret=             # 由注入的 verifier 决定；
```
- ZMS 只提供参数名与钩子，验签由业务侧签发、`zms_auth_set_verifier()` 注入。未注入验签实现时是 fail-closed（全部拒绝）。
- **内置白名单**（当前主用）：平台把启用中的接入 token 推到 `/index/api/auth/tokens?tokens=a,b,c`，`zms_auth_set_tokens()` 写入本地白名单并运行期启用验签；未注入自定义 verifier 时用内置比对。白名单为空 → 关闭验签、全放行（默认安全）。
- 内置验签**仅对“推流”强制 token**，播放保持放行（避免影响平台自身观看链路）。
- 平台侧签发/配置逻辑在 `zero-web-server/internal/application/accesstoken/`（页面「接入鉴权」）。
- 内网联调先保持无启用 token，开公网前再在页面添加并启用，否则会出现"平台能取到地址、播放器 401/403"。

### 5.4.1 WebAPI 路径分组（平台已统一）

- 平台改调分组路径 `/index/api/<域>/<动作>`（system/media/proxy/rtp/record/webrtc/onvif/gb28181/device）。
- ZMS `webapi.c` 的 `normalize_api_path()` 把新路径归一到旧扁平名，**新旧路径同时可用**；新增接口按域分组，旧名保留别名。

### 5.5 GB28181 / ONVIF 相关

- 平台 SIP 侧（注册、级联）密码在平台 `configs/config.yaml` 的 `sip.password` 与「国标配置」页面；ZMS 的设备侧（`src/device/gb28181/`）是另一种用法（ZMS 主动接入设备/上级），别把两边的域、密码混用。
- ONVIF 设备侧账号密码随设备配置，ZMS `[onvif]` 只控制服务能力。

## 6. 常见坑

- 编解码组合不匹配直接被 `codec_filter` 拒掉（这是设计，不是 bug）：先对照 `docs/stream-pipeline.md` 1.2 节的出口矩阵。
- 同一路流多个播放入口共享 GOP 环；切片/录像/代理不要各自缓存另一份。
- 目录级重构还有两处未落地（SRT 在 `protocol/srt/`、`play/` 未并入 `egress/`），文档里已标注，别以为读到了旧结构。
- Windows 构建必须带 `-DZN_ENABLE_WEPOLL=ON`（否则 poller 不可用）。
