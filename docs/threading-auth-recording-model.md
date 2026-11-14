# 线程模型 / 鉴权模型 / 录制模型 / 内存模型

> 本文是**强制约定**，不是建议。新增代码与本轮之后的存量改造都必须符合本文件。
> 设计对齐 ZLMediaKit 已验证的做法：底层通信只有「poller 可读 / 可写」两种事件，
> 鉴权与录制都不许另起线程漂移。

## 0. 一句话原则

**poller 线程只做非阻塞的事；任何会阻塞的操作（DNS、阻塞 connect、磁盘 IO、sleep、
等待别的锁）都不许出现在 poller 线程上；线程种类越少越好。**

---

## 1. 线程模型

### 1.1 允许的线程种类（只有三种）

| 线程 | 数量 | 职责 | 禁止 |
|---|---|---|---|
| poller 线程 | `poller_threads`（默认 2） | 所有 socket 可读/可写事件、定时器、协议状态机、鉴权验签、webhook 通知、录制入队/同步写 | 阻塞调用、DNS、sleep、跨线程等锁 |
| 后台 IO 线程池 | 固定少量（VOD 文件、录制「关文件」） | 不可避免的慢磁盘操作 | 访问会话状态、访问 poller 独占数据 |
| 专用协议线程 | 仅在协议库强制时存在（如 SRT） | 该协议收发 | 回调中直接改 zms 会话状态；必须投递回 poller |

**除此之外不允许新建线程或线程池。** webhook 通知、鉴权都已在 poller 上完成，
不存在「鉴权线程池」「通知线程池」这类形态（见第 2 节）。

### 1.2 poller 线程规则

- 所有网络收发都通过 `zn_tcp_client` / `zn_tcp_server` / `zn_udp_server`，
  由 poller 以「可读 / 可写」事件驱动，不轮询、不阻塞。
- 所有延时逻辑用 `zn_poller_do_delay` / `zn_timer_*`，禁止 `zb_sleep_ms`。
- poller 线程内访问会话数据是**独占**的，不加锁；跨线程访问必须投递
  `zn_poller_async` 回到所属 poller 后再访问。
- 一次回调内处理的条数必须设上限（如 `ZN_TIMER_FLUSH_MAX`、
  `ZMS_MP4_REC_TICK_FRAMES`），避免单会话饿死其他会话。

### 1.3 跨线程回投

工作线程完成任务后，**只能**通过 `zn_poller_async(io, fn, ctx, 0)` 把结果送回会话
poller 线程，然后在那里访问会话。投递失败必须释放 ctx 并计数放行，不得在 worker
线程直接执行 `on_io`（`on_io` 只会访问 poller 独占状态）。

---

## 2. 鉴权模型（token 验签，不回调 hook）

### 2.1 边界

| 来源 | 是否鉴权 | 说明 |
|---|---|---|
| 外部主动推流：RTMP publish、RTSP RECORD、WHIP、SRT 推流、外部 RTP-PS 直推 | **是** | 客户端 URL/streamid 上必须带 token |
| 信令协商推流：GB28181、ONVIF | **否** | 由上级平台配置与协商保证安全 |
| 播放：HTTP-FLV/TS/MP4、HLS、RTSP PLAY、RTMP play、SRT play、WHEP | **是** | 与推流同一套 token 验签 |

- 业务鉴权（谁能不能推/拉这条流）**不属于流媒体**。推流 app 与播放 app 先到 web
  后台完成鉴权，后台签发带时效的 token，客户端把 token 放进协议 URL 的参数
  （HTTP/RTSP/WHEP 用 query，RTMP 用 app/stream 的 query，SRT 用 streamid 的 query，
  RTP-PS 用 openRtpServer 的 query）。
- 流媒体**只做本地验签**：解参数、验签名与时效，得出「允许/拒绝 + 是否录制」。
  不查库、不回调 hook、不发任何网络请求。
- 验签实现是预留挂点：`zms_auth_set_verifier()` 注册；`[auth] enable=0`（默认）时
  一律放行，`enable=1` 且未注册实现时 **fail-closed**（全部拒绝并告警）。
- 信令协商打开的 RTP-PS 槽位以 `signaling_authorized` 标记，开槽时跳过验签，
  录制默认开启。

### 2.2 执行方式：同步、本地、非阻塞

- 入口只有一个：`zms_auth_check(const zms_auth_request *, zms_auth_decision *)`。
- 必须在**调用方线程**同步返回，纯本地计算；poller 线程可直接调用。
- **禁止**在鉴权路径上做 HTTP（无论同步还是异步）、DNS、磁盘 IO、sleep、等锁。
- 拒绝时按协议回错误并断开/不建流：RTMP `NetStream.Publish.BadAuth`、
  RTSP `403 Forbidden`、HTTP `403`、SRT 直接关闭、RTP-PS `openRtpServer` 返回
  `ZN_ERR_DENIED`（webapi 映射 403）。

### 2.3 不再有「等待期」

本地验签没有挂起状态，因此**不存在**等待期间缓存媒体、pending 标记、
延迟销毁、`destroy_token` 兜底这些机制。新增协议时不要再引入它们。

### 2.4 参数传递

- 各协议在自己的 URL 解析点把原始查询串放进 `zms_auth_request.params`，
  鉴权模块只做键值提取（`zms_auth_param_get`），不理解业务字段。
- `zms_auth_split_query()` 用于把 `name?query` 拆成名字与查询串。

## 3. 录制模型（对齐 ZLMediaKit）

### 3.1 写盘在 poller 上同步进行

- 录制由数据驱动：在推流数据处理线程（poller）上把帧直接交给封装器同步写
  `mp4_writer_write`。依赖 `FILE*` 缓冲吸收，绝大多数写入只落内存缓冲。
- 不加队列、不加 worker、不做跨线程拷贝。这是 ZLMediaKit 已验证的做法。

### 3.2 只有「关文件」异步

- 切片与停推时：**同步摘下**当前 `mov`/`fp` 与相关引用，立刻开新文件；
  把「关闭旧文件」打包投递到后台 IO 线程池。
- 后台任务完成：`mov_writer_destroy`、`fflush`/`fclose`、统计大小、`rename` 临时名
  为正式名、触发 `on_record_mp4` 通知。过小文件（<1KB）删除。
- 关闭任务必须在 poller 池销毁之前跑完（与 webhook 线程池同样的顺序要求）。

### 3.3 切片规则

- 视频：相对时长超过 `mp4_max_second` 且当前帧是视频关键帧才切。
- 纯音频：超过时长即切。
- 文件先写 `.<name>.mp4` 临时名，关闭后 `rename`，避免半成品被访问。

### 3.4 poller 侧禁止项（录制相关）

- 禁止在 poller 上 `fflush` / `fclose` / `rename` / 算文件大小。
- 单次 tick 处理帧数设上限，超出的留到下次 tick。

---

## 4. 内存模型

### 4.1 所有权

- 入站帧进入 `zms_gop_queue` 后由队列持有引用；`zms_gop_reader_read*` 返回的
  `slot.data` 指针只保证**到该 reader 下一次读取之前有效**（reader 持有 `held_ref`）。
- 因此：同线程「读一帧 → 立即消费/写盘」是安全的；**把 `slot.data` 交给别的线程
  或长期保存必须先拷贝或增引用**。
- 跨线程传递数据一律「拷贝」或「带引用计数」，禁止传裸指针跨越 poller 边界。

### 4.2 生命周期

- 会话对象的释放只能在所属 poller 线程执行。
- 存在未完成的异步回调（鉴权、异步关闭、异步发送）时，会话不得释放；
  用 `destroy_token` / 引用计数（`tcp session` 已有 `ref`）保证存活。
- 延迟销毁必须有唯一归属：谁最后完成异步任务，谁负责最终释放，且只释放一次。

### 4.3 缓冲区

- 复用缓冲池（`zms_buf_pool`）的缓冲区归分配它的线程所有，跨线程释放必须回投。
- 每个会话的临时缓冲（响应缓冲、装配缓冲）只在该会话 poller 线程使用。

---

## 5. 迁移清单

1. ✅ **鉴权改为本地 token 验签**：`zms_auth_check()` + `zms_auth_set_verifier()`；
   推流五点（RTMP/RTSP/SRT/WHIP/RTP-PS）与播放七点（HTTP-FLV/TS/MP4/HLS、
   RTSP/RTMP/SRT play、WHEP）全部切换。
2. ✅ **删除 hook 鉴权**：`on_publish` / `on_play` 从 `[hook]` 移除，等鉴权的
   tag 缓存、`*_auth_pending`、延迟销毁全部删除。
3. ✅ **webhook 只剩通知**：`on_stream_changed` / `on_none_reader` / `on_play_stop` /
   `on_server_started` / `on_record_mp4`，全部走主 poller 上的非阻塞 HTTP 客户端，
   删除阻塞 `http_post_sync` 与通知线程池。
4. ✅ **录制关文件改异步**：同步摘引用 + 后台关闭 + `.tmp` 改名。
5. ⏳ 栈/结构体瘦身：SIP 64KB 栈帧、ONVIF 132KB 栈帧、`zms_http_parser` 274KB/连接等。

每步完成后：构建 + `ctest` 全绿 + 相关用例 ASan/UBSan 通过，再提交推送。
