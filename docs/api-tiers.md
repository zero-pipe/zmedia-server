# 头文件与 API 分层

> 头文件树与 `src/` 分层同构，只有一棵：`include/zms/<layer>/...`。
> 稳定与否由 `ZMS_API` 标注的**符号**决定，而不是靠目录名。

## 1. 唯一的头文件树

```text
include/zms/
  sdk.h           嵌入 / 插件宿主的唯一推荐入口（Stable 伞头）
  server.h        服务器门面
  version.h       版本宏
  zms_export.h    ZMS_API / ZMS_LOCAL 导出宏
  live.h          领域伞头：直播入站、代理拉流、通用 codec/容器适配
  vod.h           领域伞头：VOD reader、play lane、HTTP-FLV
  player.h        领域伞头：远端拉流播放器
  protocol.h      领域伞头：RTSP / RTMP / HTTP / SRT 协议栈
  zms.h           全量伞头（demo / 内部方便用）
  core/ media/ bridge/ protocol/ publish/ play/ vod/ pull/ egress/ ops/ util/   与 src/ 一一对应
```

`cmake --install` 安装 `include/zms` 下的全部头。内部头也可以被嵌入方使用，但
不保证 ABI；破坏性改动只对 `ZMS_API` 符号负责。

## 2. 稳定性契约

- **稳定**：由 `ZMS_API` 导出的符号；删除或改签名属于破坏性变更，
  需要在发布说明中明确标注（只允许新增）。
- **内部**：`src/` 内跨编译单元使用、用 `ZMS_LOCAL`（hidden visibility）标注的
  符号，不进共享库 ABI。
- 头文件路径本身不承诺稳定；`sdk.h` / `server.h` 及其直接依赖最稳定。

## 3. 领域伞头

按使用场景聚合，而不是按实现目录聚合：

| 伞头 | 典型使用者 | 内容 |
|------|-----------|------|
| `zms/sdk.h` | 嵌入式宿主 | server、version、export |
| `zms/live.h` | 直播接入/分发 | ingest、proxy、codec 适配器 |
| `zms/vod.h` | 点播 | reader、play lane、HTTP-FLV |
| `zms/player.h` | 拉流播放器 | pull player |
| `zms/protocol.h` | 自定义协议接入 | RTSP/RTMP/HTTP/SRT 栈 |
| `zms/zms.h` | demo / 内部 | 全量聚合 |

## 4. 守护

- `ZMS_API`：对外稳定符号，破坏性变更需在发布说明中标注。
- `ZMS_LOCAL`：内部 hidden 符号，不进入共享库 ABI。
- 分层依赖与 ZM/ZN 边界由 CMake 对象库和代码评审约束。
