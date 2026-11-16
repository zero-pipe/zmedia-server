# 设备协议架构：一套设备/平台双向开放套件

> 目标不是「把 ONVIF 和 GB28181 各写一遍」，而是同一套设备模型、能力模型、会话模型
> 和传输层，让 ZMS 同时具备四种角色：
>
> | 角色 | ONVIF | GB28181 |
> |------|-------|---------|
> | 设备导出（被平台接入） | `onvif_server`（Device/Media/Discovery UAS） | `gb28181` 设备端（REGISTER 到上级平台） |
> | 平台导入（接入设备） | `onvif_client`（发现/取流/PTZ） | `gb28181` 平台端（接受 REGISTER/MESSAGE/INVITE） |
>
> 现状：ONVIF 两种角色都有；GB28181 设备端已有，平台端信令仍在 Go 侧，
> 媒体面 `ps_server` 已在 C 进程内。后续按本文的分层把四块收敛到同一套内核。

## 1. 设计原则

1. **协议是适配器，不是内核**：ONVIF/GB28181 只负责协议规范、鉴权、消息序；
   设备、通道、能力、会话、录像、事件全部进 `device_core`。
2. **媒体路径不变**：仍走 `hub → gop_queue/VOD → PS/RTSP/RTP`；
   协议模块不维护流表（沿用 `docs/extensibility.md`）。
3. **传输层统一**：SIP/HTTP/WSD/媒体 RTP 的 UDP/TCP/TLS、分帧、重连、保活
   收敛到一层薄接口，底层优先复用 ZN；协议模块只面向「会话 + 帧」。
4. **消息表驱动**：每条命令是表项（名称/方向/能力/处理函数），
   而不是散落的 `if (strcmp(CmdType, ...))`。
5. **增量演进**：先抽接口和模型，行为保持不变；每步都能单独回归。

## 2. 目标分层

```text
┌──────────────────────────────────────────────────────────────┐
│ 协议适配器（wire only）                                      │
│  onvif_server  onvif_client  gb_device  gb_platform  future  │
├──────────────────────────────────────────────────────────────┤
│ device_core                                                  │
│  device/channel registry · capability registry · session mgr │
│  record index · event bus · command dispatch tables          │
├──────────────────────────────────────────────────────────────┤
│ transport facade（薄接口）                                   │
│  sip_transport(UDP/TCP/TLS) · http(TLS) · media_io(UDP/TCP)  │
│  wsd multicast · framing(sip-content-length/rtp-tcp/http)    │
├──────────────────────────────────────────────────────────────┤
│ ZN（poller/udp/tcp/tls/timer） + ZMS 媒体路径（hub/gop/VOD） │
└──────────────────────────────────────────────────────────────┘
```

### 2.1 device_core

- **Device/Channel**：设备与通道的唯一模型（ID、名称、厂商、在线、协议来源）。
  ONVIF Profiles 与 GB Catalog 都从这里生成；导入侧发现到的设备也写入这里。
- **Capability**：能力注册表（live / playback / ptz / snapshot / events /
  record_query / config）。导出侧用它宣告能力（ONVIF GetCapabilities、GB Catalog
  的 PTZType 等）；导入侧用它描述对端能力。
- **Session**：统一的点播/回放会话（协议 dialog/call_id、SSRC、传输方式、
  reader、PS/RTSP 输出、pacing、统计）。ONVIF 的 RTSP 播放与 GB 的 PS/RTP
  只是两种 egress 实现。
- **Record index**：录像目录/时间范围查询（GB RecordInfo、ONVIF Recording 共用）。
- **Event bus**：设备在线、流状态、报警、移动侦测；映射到 GB Alarm / ONVIF Events。

### 2.2 适配器职责

| 模块 | 只做 | 不碰 |
|------|------|------|
| `onvif_server` | WS-Discovery、SOAP 解析、鉴权、动作分发 | 通道从哪来、流怎么发 |
| `onvif_client` | 发现/GetProfiles/GetStreamUri/PTZ 的线协议 | 设备模型归属、拉流实现 |
| `gb_device` | SIP UAC/UAS、MANSCDP、SDP | 媒体发送细节 |
| `gb_platform`（待建） | 接受 REGISTER/MESSAGE/INVITE，发起 INVITE/BYE | 媒体接收（复用 `ps_server`） |

## 3. 传输层：复用 ZN 的边界

ZN 已经提供 poller、timer、`zn_udp_server`、`zn_tcp_server`/`zn_tcp_client`、
`zn_tls_client`、`zn_udp_client`（含组播）。复用策略：

- **SIP UDP**：已用 `zn_udp_server`，保持。
- **SIP TCP**：`zn_tcp_server` + Content-Length 分帧；外发用 `zn_tcp_client`；
  `Contact/Via` 按 transport 生成。事务重传、CSeq、Dialog 仍由 SIP 层自己做
  （ZN 不提供 SIP 语义）。
- **SIP TLS**：`zn_tcp_server` 的 `ssl_ctx` + 证书配置（`[ssl]`）；
  设备端注册到平台很少用 TLS，作为可选能力，优先级低于 UDP/TCP。
- **媒体 RTP**：UDP 用 `zn_udp_client`（可先 bind 拿本地端口，满足 SDP answer
  需要先知道端口）；TCP 用 `zn_tcp_client`（设备主动连）/`zn_tcp_server`（设备监听），
  分帧为 2 字节大端长度。若 poller 归属使实现变复杂，允许保留当前裸 socket，
  但必须收在 `media_io` 端口后备，便于替换。
- **ONVIF WSD**：组播 + 单播都用 `zn_udp_client_set_multicast`，替换现有裸 socket。
- **HTTP/HTTPS**：统一走 `zms_http_exec`，HTTPS 接 `zn_tls_client`
  （当前 ONVIF HTTPS 是缺口）。

注意：当前本机 MinGW 的 OpenSSL 1.0.2 不满足 ZN 的 1.1 API；正式构建要求
OpenSSL 1.1+（MSVC 环境通常满足）。TLS 相关能力上线前要纳入构建矩阵。

## 4. 可扩展的「指令」模型

每条协议命令用表项描述，适配器只做查表 + 参数/结果映射：

```c
typedef struct zms_proto_cmd {
    const char *ns;         /* gb28181:MANSCDP / onvif:Device / onvif:Media */
    const char *name;       /* Catalog / GetStreamUri / Listen / ... */
    int direction;          /* QUERY / CONTROL / NOTIFY / RESPONSE */
    unsigned capability;    /* LIVE / PLAYBACK / PTZ / SNAPSHOT / EVENT */
    int (*exec)(zms_device_ctx *ctx, const zms_cmd_params *in, zms_cmd_result *out);
} zms_proto_cmd;
```

好处：
- 新增一条命令 = 一个表项 + 一个 handler；
- 能力未实现时统一返回「不支持」，而不是各协议各写一套；
- ONVIF 与 GB 的命令可以在同一张能力矩阵里对齐（例如都映射到 `ptz`）。

消息体解析共用一个小 XML 层（SOAP 信封 + 平铺 XML），仍然**不做** XSD 校验，
只做长度/实体/嵌套边界检查，避免引入 gSOAP 这类重依赖。

## 5. 设备/平台双向的统一入口

```c
typedef struct zms_device_provider_ops {
    int (*channels)(ctx, zms_channel_info *out, size_t cap, size_t *count);
    int (*capabilities)(ctx, const char *channel, zms_capability_set *out);
    int (*open_live)(ctx, const char *channel, const zms_session_opts *o,
                     zms_device_session **out);
    int (*open_playback)(ctx, const char *channel, zms_time_range range,
                         const zms_session_opts *o, zms_device_session **out);
    int (*ptz)(ctx, const char *channel, zms_ptz_cmd cmd);
    int (*snapshot)(ctx, const char *channel, zms_snapshot *out);
    int (*record_query)(ctx, const char *channel, zms_time_range range,
                        zms_record_list *out);
    int (*event_subscribe)(ctx, zms_event_filter f, zms_event_sink sink);
} zms_device_provider_ops;
```

- **导出适配器**（ONVIF server / GB device）：把 wire 命令映射到 provider。
- **导入适配器**（ONVIF client / GB platform）：用远端信息填充 device/channel/capability。
- **跨协议桥接**：同一个 provider 同时被导入和导出使用，就能实现
  「ONVIF 摄像头 → GB28181 平台」或「GB28181 设备 → ONVIF 平台」。

## 6. 分阶段落地

| 阶段 | 内容 | 行为变化 | 估时 |
|------|------|----------|------|
| M0 | 本文档 + 接口草案 + 一致性夹具（消息/能力矩阵） | 无 | 0.5–1 周 |
| M1 | 抽 `device_core`（device/channel/capability/session/record index），ONVIF server 与 GB 设备端改为 adapter | 无（可回归对拍） | 1.5–2.5 周 |
| M2 | transport facade：SIP 增加 TCP/TLS 选项；媒体 socket、WSD、HTTP 收敛到接口 | 可选能力新增 | 1–2 周 |
| M3 | 消息表驱动 + 共享 XML 层 + 能力统一宣告（ONVIF GetCapabilities / GB Catalog） | 无 | 1 周 |
| M4 | `gb_platform`：进程内接受设备 REGISTER/MESSAGE/INVITE，复用 `ps_server`；与 Go 信令并行/替换 | 新角色 | 2–3 周 |
| M5 | 事件/PTZ/录像统一：ONVIF Events/PTZ ↔ GB Alarm/PTZCmd ↔ 本地 provider | 能力增强 | 1–2 周 |
| M6 | ONVIF 设备端补齐 PTZ 服务与 Events PullPoint，能力按 `device_core` 宣告 | 能力增强 | 0.5–1 周 |

M1 是关键分水岭：完成后「新增协议 = 新 adapter」，
「新增能力 = provider + 能力表项」，M2 之后的传输/消息收敛都是低风险替换。

## 7. 明确不做

- 不做完整 RFC3261 SIP 协议栈（事务/对话只做到 GB28181 需要的程度）。
- 不做 WSDL 驱动/代码生成的通用 ONVIF（固定消息集 + 表驱动足够）。
- 不做 XSD 校验、SOAP Header 全语义、ONVIF 全 Profile。
- 不改变媒体主路径，不把 RTP/PS/RTSP 再包一层抽象。

## 8. 当前状态

已落地：

- `device_core`：设备/通道/能力/录像索引统一模型；
- `sip_transport`：SIP UDP/TCP/TLS 客户端与服务端；
- `device_xml` / `device_command`：共享 XML 与表驱动命令；
- `device_event` / `device_ptz`：事件总线与归一化 PTZ；
- GB28181 设备端与平台端：注册、目录、心跳、INVITE 取流、PTZ、录像、报警；
- ONVIF 设备端：Device / Media / PTZ / Events（PullPoint）。

待做：

- GB 平台多设备并发、设备离线自动断流；
- ONVIF 客户端 PTZ 接入 `device_ptz` 后端；
- ONVIF Events Push 订阅、Recording/Replay；
- WSD、媒体 socket、HTTP 传输收敛与 TLS 证书策略完善。

## 9. 验证与回归

- 现有 `tests/gb28181/mock_platform.py` 扩展为四角色夹具：
  GB 平台端（已有）、GB 设备端（mock 上级平台）、ONVIF 设备端（已有 mock）、
  ONVIF 平台端（现有 ONVIF 设备端可复用）。
- 每次重构保持 `api + live` 回归、PS/RTSP 出流 `ffprobe` 通过；
  新增能力矩阵用例（命令 → 能力 → 结果码）。
- ABI：新增 `ZMS_API` 只增不删；协议内部结构不进入稳定头。

## 10. 验收标准（方向性）

1. ONVIF 与 GB28181 的通道/能力来自同一份 device_core 数据；
2. 同一路流可同时被 ONVIF 平台和 GB28181 平台点播，会话统计统一；
3. 新增一条协议命令只改表项与 handler，不改其他协议；
4. transport 可在 UDP/TCP/TLS 之间切换，协议模块无感知；
5. 作为平台可接受设备注册（GB 信令进程内），作为设备可注册到上级平台，
   两条链路共用同一套会话与录像索引。
