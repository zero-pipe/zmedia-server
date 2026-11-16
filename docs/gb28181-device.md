# GB28181 设备端（ZMS 作为 IPC）

> 目标：不依赖 Go/Python sidecar，ZMS 进程内完成 SIP 信令 + PS/RTP 推流，
> 注册到上级平台并提供目录、实时点播、回放、报警与录像查询。

## 1. 能力范围

已实现：

- **注册/保活**：Digest 挑战（MD5 / qop=auth）、定时续注册、注销、Keepalive。
- **查询**：Catalog、DeviceInfo、DeviceStatus、ConfigDownload、RecordInfo、PresetQuery（空）。
- **控制**：DeviceControl 应答（PTZCmd 记录、RecordCmd 手动录制/停止、GuardCmd/TeleBoot 等只应答）。
- **实时点播**：INVITE / ACK / BYE；UDP、TCP 主动（设备连平台）、TCP 被动（设备监听）。
- **回放**：`s=Playback` + `t=` 时间范围，按录像文件 seek 后以实时速度推 PS/RTP。
- **报警**：HTTP API 触发 MANSCDP Alarm 上报。
- **媒体**：gop_queue / VOD → MPEG-PS → RTP/PS（RFC2250）→ UDP/TCP；
  H.264/H.265 + AAC（自动补 ADTS）/G.711A/G.711U。
- **API**：`/index/api/gb28181/{status,channels,alarm}`。

未实现（按需再加）：

- SIP 服务端当前为单平台连接（首个平台连入后向其注册）；多平台并存需后续扩展。
- PTZ 实际转动（仅解析并应答；可后续接 ONVIF 客户端）。
- MANSRTSP 回放暂停/倍速/seek（INFO 只应答）。
- 回放时间范围跨多个录像文件时，当前只播放第一个相交文件。
- 下载（`s=Download`）按普通回放处理，忽略 `a=downloadspeed`。
- Catalog/告警 SUBSCRIBE-NOTIFY 订阅、移动位置、语音对讲。
- GB2312 转码（XML 声明 GB2312，但字符串按 UTF-8 输出；ASCII 名称无影响）。

本地端到端验证覆盖 **H.264 + AAC**；H.265、G.711A/U 代码路径已实现
（PS stream type 与 RTP 打包复用 libmpeg/librtp），但尚未用真机码流验证。

## 2. 代码位置

```
include/zms/device/gb28181/gb28181.h     公共接口（start/stop/alarm/channels）
src/device/gb28181/
  gb28181_internal.h                     内部共享结构
  gb_sip.c                               SIP 解析/构造 + Digest
  gb_manscdp.c                           MANSCDP XML
  gb_device.c                            生命周期/UDP 传输/注册保活/查询控制
  gb_invite.c                            INVITE/ACK/BYE + SDP 协商
  gb_media.c                             PS/RTP 发送线程（UDP/TCP）
  gb_playback.c                          VOD 回放（录像索引在 device_core）
src/device/core/record_index.c           通用录像索引（RecordInfo / 回放共用）
src/bridge/zm_ps_muxer.c                libmpeg ps_muxer 桥
src/bridge/zm_rtp_ps.c                  PS → RTP 分片（M 位置位）
src/ops/api/webapi/gb28181_api.c         /index/api/gb28181/*
examples/demo_gb28181_device.c           本地联调进程（HTTP+RTMP+GB28181）
tests/gb28181/mock_platform.py           本地模拟平台（UDP/TCP/回放）
```

## 3. 配置

```ini
[gb28181]
enable=1
platform_id=34020000002000000001
platform_domain=3402000000
platform_ip=192.168.1.100
platform_port=5060
device_id=34020000001320000001
device_name=ZMS-IPC
username=
password=12345678
local_ip=
local_port=5060
# udp（默认）/ tcp / tls
sip_transport=udp
# client（默认，设备连平台）/ server（平台连设备；仅 tcp/tls）
sip_mode=client
# TLS 客户端：CA 文件；tls_verify_peer=1 时校验平台证书
# TLS 服务端：sip_mode=server 时 tls_cert/tls_key 必填
tls_ca=
tls_cert=
tls_key=
tls_verify_peer=0
register_expires=3600
keepalive_interval=60
# <通道ID>:<vhost>/<app>/<stream>；vhost 可省略
channels=34020000001320000001:__defaultVhost__/live/cam1
# 空 = 使用 [record] root
record_root=
```

- `local_ip` 留空时按 `platform_ip` 探测本机出口地址；NAT/多网卡场景建议显式填写。
- 通道的 `<app>/<stream>` 指向 ZMS 中已在线的直播源（RTMP/RTSP/SRT/RTP-PS 均可）。
- 回放/录像查询使用 `{record_root}/{stream}/{YYYY-MM-DD}/{YYYYMMDDHHMMSS}.mp4`
  （与 `egress_mp4_recorder` 的落盘规则一致）。

## 4. API

```
GET /index/api/gb28181/status
GET /index/api/gb28181/channels
GET /index/api/gb28181/alarm?channel=<id>&priority=1&method=5&description=xxx
GET /index/api/device/capabilities   # 协议无关能力并集（live/playback/ptz/...）
```

`status.data`：`running / registered / sessions`。
`channels.data[]`：通道 ID、映射、是否在线。

## 5. 本地验证（无需真机）

```bash
# 1) 启动联调进程（RTMP 19350 + HTTP 8080 + GB28181 SIP 15061）
build-mingw/examples/demo_gb28181_device.exe --config tests/gb28181/config.ini --seconds 60

# 2) 推一路测试流（建议 GOP 1s，便于快速起播）
ffmpeg -re -stream_loop -1 -i test_gop1.mp4 -c copy -f flv rtmp://127.0.0.1:19350/live/cam1

# 回放前置：造一份录像（目录/文件名即录像时间）
mkdir -p www/gb28181-record/cam1/2026-11-09
cp test_gop1.mp4 www/gb28181-record/cam1/2026-11-09/20261109100000.mp4

# 3) 运行模拟平台
python tests/gb28181/mock_platform.py --out live.ps
# SIP over TCP / TLS（设备作为客户端连接平台）
python tests/gb28181/mock_platform.py --sip-tcp --start-delay 1.5 --out sip_tcp.ps
# SIP 服务端（平台主动连设备；sip_mode=server）
python tests/gb28181/mock_platform.py --sip-connect --device-port 15061     --start-delay 1.5 --out sip_tcp_server.ps
# TLS 需先生成自签证书：
#   openssl req -x509 -newkey rsa:2048 -nodes -keyout tls_key.pem -out tls_cert.pem #     -days 2 -subj "//CN=127.0.0.1"
python tests/gb28181/mock_platform.py --sip-tls --tls-cert tls_cert.pem --tls-key tls_key.pem     --start-delay 1.5 --out sip_tls.ps
# TLS 服务端：（设备用 config_tls_server.ini，平台连入）
python tests/gb28181/mock_platform.py --sip-tls-connect --device-port 15061     --start-delay 1.5 --out sip_tls_server.ps
python tests/gb28181/mock_platform.py --media-tcp listen  --out tcp_listen.ps
python tests/gb28181/mock_platform.py --media-tcp connect --out tcp_connect.ps
python tests/gb28181/mock_platform.py --playback --out playback.ps

# 4) 校验 PS（应输出 h264 + aac）
ffprobe -v error -show_entries stream=codec_name,width,height,sample_rate,channels \
  -of default=nw=1 live.ps
```

模拟平台覆盖：401 Digest 挑战、Catalog/DeviceInfo/DeviceStatus/RecordInfo、
INVITE/ACK/BYE、SIP over UDP/TCP/TLS、媒体 UDP 与 TCP 主被动；收到 RTP 后去掉
12 字节头并按出现顺序重组为 PS 文件。

## 6. 与平台侧（Go 版）的兼容点

- INVITE `Subject: <channel>:<ssrc>,<platform>:0`：SSRC 从 Subject 解析；
  缺省回退 SDP `y=`。
- SDP 应答：`m=video <port> RTP/AVP 96`（或 `TCP/RTP/AVP`）、`a=sendonly`、
  `a=rtpmap:96 PS/90000`、`y=<ssrc>`；`a=setup:passive/active` 与平台互补。
- TCP-RTP 承载：每个 RTP 包前加 2 字节大端长度（GB28181 附录）。
- 起播：等待 IDR 后才发首帧，IDR 前自动补 SPS/PPS（H.265 为 VPS/SPS/PPS）；
  每个 IDR 重发 PSM。
- AAC：ZMS 内部为裸 AU，PS 封装前补 ADTS 头；G.711 直接封装。

## 7. 真机验证建议

1. 平台侧配置设备 ID/密码，确认 REGISTER 挑战与续注册；
2. Catalog 正常返回通道；点播（海康/大华等平台）确认首帧时间与音画；
3. 若有 TCP 模式要求，分别验证平台连入与设备连出；
4. 录像：确认 `[record]` 产出的 MP4 能被 RecordInfo 列出并回放；
5. 抓包对比：REGISTER/Keepalive 间隔、INVITE SDP、RTP 时间戳与 SSRC。

## 8. 平台端（M4a，最小信令集）

同一进程可同时作为平台端接受下级设备注册，配置独立：

```ini
[gb28181_platform]
enable=1
id=34020000002000000001
domain=3402000000
password=12345678      # 空=不校验（仅测试）
advertise_ip=          # 空=自动探测
listen_port=5060
transport=udp          # udp / tcp / tls（tcp/tls 服务端，设备连入）
tls_cert=
tls_key=
keepalive_timeout_sec=180
media_vhost=
media_app=gb28181
```

能力：
- REGISTER + Digest 挑战/校验、注销、Keepalive 保活与超时离线；
- 注册后自动查询 Catalog / DeviceInfo；解析设备信息、通道目录、DeviceStatus；
- API：
  ```
  GET /index/api/gb28181/platform/devices
  GET /index/api/gb28181/platform/channels?device=<设备ID>
  GET /index/api/gb28181/platform/query?device=<设备ID>&cmd=Catalog|DeviceInfo|DeviceStatus
  GET /index/api/gb28181/platform/invite?device=<设备ID>&channel=<通道ID>&app=gb28181&stream=<流名>&tcp=0|1|2
  GET /index/api/gb28181/platform/sessions
  GET /index/api/gb28181/platform/bye?call_id=<call_id>
  GET /index/api/gb28181/platform/ptz?device=&channel=&pan=&tilt=&zoom=&stop=1
  GET /index/api/gb28181/platform/record_query?device=&channel=&start=2026-11-09T10:00:00&end=2026-11-09T11:00:00
  GET /index/api/gb28181/platform/records?device=&channel=
  GET /index/api/gb28181/platform/alarms
  GET /index/api/device/events
  GET /index/api/device/ptz?channel=&pan=&tilt=&zoom=&stop=1
  ```
- 取流：`tcp=0` UDP、`1` TCP 被动（平台 listen）、`2` TCP 主动（平台 connect）；
  媒体经 `ps_server` 进入 hub，`app/stream` 默认 `gb28181/<通道ID>`，
  可直接用 `/gb28181/<流名>.live.flv` 播放。
- 录像/报警：平台可查询设备录像并列表展示；设备 Alarm 上报进入平台报警列表与事件总线。
- PTZ：平台 `platform/ptz` 生成 PTZCmd 下发；设备端解析为归一化命令，
  通过 `zms_device_ptz_set_handler()` 可接真实云台/ONVIF 客户端（默认仅记录事件）。

本地自测（设备端与平台端同进程）：

```bash
build-mingw/examples/demo_gb28181_device.exe --config tests/gb28181/config_selftest.ini --seconds 20
curl "http://127.0.0.1:8080/index/api/gb28181/platform/devices"
curl "http://127.0.0.1:8080/index/api/gb28181/platform/channels?device=34020000001320000001"
# 取流（需先推一路 rtmp://127.0.0.1:19350/live/cam1）
curl "http://127.0.0.1:8080/index/api/gb28181/platform/invite?device=34020000001320000001&channel=34020000001320000001"
ffprobe -v error -show_entries stream=codec_name -of default=nw=1 http://127.0.0.1:8080/gb28181/34020000001320000001.live.flv
curl "http://127.0.0.1:8080/index/api/gb28181/platform/sessions"
```

## 9. 相关设计

- 设备/平台双向、能力模型与传输收敛的总体方案见
  [`device-protocol-architecture.md`](device-protocol-architecture.md)。
- 本节实现是该架构的「纵向最小切片」；后续重构按架构文档的 M1–M5 增量推进，
  保持现有行为与 API 不变。
