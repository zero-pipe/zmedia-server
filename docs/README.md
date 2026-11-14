# 文档索引

ZMS（zero-media-server）的文档按用途分成三类：**规范（强制）**、**设计（当前实现）**、
**参考（查找用）**。新增文档请归入其中一类并在此登记。

## 规范（写代码前必读，评审依据）

| 文档 | 内容 |
|------|------|
| [threading-auth-recording-model.md](threading-auth-recording-model.md) | 线程模型 / 鉴权模型 / 录制模型 / 内存模型。**强制约定**，新增代码必须符合 |
| [code-review-checklist.md](code-review-checklist.md) | 逐条评审清单，配合上面的模型使用 |

## 设计（当前架构说明）

| 文档 | 内容 |
|------|------|
| [structure.md](structure.md) | 代码地图：分层、依赖方向、目录职责、命名约定、阅读顺序 |
| [api-tiers.md](api-tiers.md) | 头文件与 API 分层：稳定符号由 `ZMS_API` 决定 |
| [device-protocol-architecture.md](device-protocol-architecture.md) | 设备/平台双向套件：设备模型、能力模型、会话模型、传输层 |
| [extensibility.md](extensibility.md) | 商业化边界与扩展约定（vhost / TLS / GB28181 / ONVIF） |
| [webrtc-self-contained.md](webrtc-self-contained.md) | WebRTC 自闭环设计（WHEP/WHIP、ICE/DTLS/SRTP、拥塞控制） |

## 参考（特性与待办）

| 文档 | 内容 |
|------|------|
| [gb28181-device.md](gb28181-device.md) | GB28181 设备端：SIP 信令 + PS/RTP 推流的实现说明 |
| [webrtc-todo.md](webrtc-todo.md) | WebRTC 后续可选增强（DTLS 指纹校验、TURN TCP/TLS、压测矩阵） |

## 质量门禁（CI / 测试 / fuzz）

- `.github/workflows/ci.yml`：Linux 构建 + ctest（WebRTC 开关两矩阵）、
  ASan+UBSan、libFuzzer 冒烟。
- `tests/`：单元/回归用例，`ctest` 运行。
- `fuzz/`：libFuzzer 靶（HTTP/RTSP/SIP/XML/RTMP 解析），
  `-DZMS_BUILD_FUZZ=ON` + clang 构建，用法见 `fuzz/CMakeLists.txt`。

## 已归档（历史计划，已完成，仅留档）

已完成的重构计划与巡检记录已随对应提交清理，不再单列。需要追溯时翻 git 历史：

- 模型符合性巡检、协议分层重构、运营加固等计划，见对应提交信息与 diff。
