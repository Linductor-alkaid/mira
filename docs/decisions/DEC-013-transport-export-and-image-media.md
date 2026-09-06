# DEC-013：传输头文件导出与模型图像 wire 媒体类型

> 状态：Accepted
> 日期：2026-09-06
> 负责人：Mira Maintainers
> 冻结里程碑：M4 后维护轮（[维护计划第二轮](../plans/maintenance-2026-09-transport-and-image-media.md)）
> 替代/被替代：无（补充 [DEC-010](DEC-010-cross-platform-tls-proxy-upload.md) 的分发面与
> [DEC-005](DEC-005-observation-coordinate-host-boundary.md) 的载荷元数据）

## 背景与问题

miracle 第二轮真机反馈（GitHub #14、#15）暴露两个系统性缺口：

1. **安装包"有库无头"（#14）**：`mira_net_transport` / `mira_mbedtls_transport` /
   `mira_openssl_transport` 的库随包安装，但 `socket_transport.hpp`、`mbedtls_tls.hpp`、
   `openssl_tls.hpp` 位于 `adapters/net/`（PRIVATE include），不随
   `install(DIRECTORY include/)` 导出。包外消费者无法合法声明
   `SocketHttpTransport`/`MbedTlsChannelFactory`，被迫自建传输（miracle P3 重复实现了
   Kotlin HTTPS 传输）或手写类声明（ABI 脆弱）。
2. **图像 wire 媒体类型系统性错误（#15，P1）**：`AgentLoop::build_request` 把截图工件
   硬编码为 `application/octet-stream` + `w*h*4` 字节，方言层据 `reference.media_type`
   生成 `data:application/octet-stream;base64,<原始 RGBA>`——真实 OpenAI 兼容端点
   （`image_url`）只接受 `image/png` 等 image 媒体类型，闭环被全量拒绝；且 8 MiB 内联
   门槛按原始 RGBA 字节数判定，高分辨率设备在到达端点前即被拒。同时 adapter 自有
   artifact store 无读取 API，宿主无法转码。

## 决策

1. **传输头文件导出（#14）**：`adapters/net/*.hpp` 迁移到公共头文件树
   `include/mira/adapters/net/`，随安装包导出；`Mira::net_transport` 等目标移除
   PRIVATE 头路径，公共头自包含检查与安装消费者测试覆盖三个新头（mbedtls 目标按
   构建条件存在于导出集）。`socket_transport.hpp` 公共接口引用 Executor 类型，
   消费者需同时 `find_package(executor)`。
2. **载荷元数据契约（#15，采用 issue 语义 (a)——宿主负责编码）**：
   - `ScreenFrameDescriptor` 新增 `payload_media_type` / `payload_byte_size` /
     `payload_digest`，由 adapter 在工件 commit 时从 store 记录填充（android 原始帧
     如实标 `image/x-host-frame`，simulator 标 `image/x-rgba8888`），校验强制三者
     齐备。这三个字段与 `payload_artifact` 一起构成"已发布载荷的完整 store 记录引用"。
   - `build_request` 的截图 `ArtifactRef`（media type + byte size）取自上述字段，
     不再假设原始帧布局；内联门槛按实际载荷字节数判定。
   - 宿主转码路径：注入自有 `IArtifactStore`（DEC-012）在 commit 时转码 RGBA→
     PNG/JPEG，返回的描述符即决定 wire 媒体类型与大小；或经
     `AndroidHostAdapter::artifact_store()` 读取句柄转码后另存。
   - 方言层对 `ImagePart` 的非 `image/*` 媒体类型在 payload fetch 前 fail closed
     （`CapabilityMismatch` -> `UnsupportedCapability`），把"端点 opaque 400"变成
     本地可诊断错误。
3. **边界**：mira 不在 Core 内引入 PNG/JPEG 编码器（不新增图像编解码依赖）。转码是
   宿主职责；mira 保证元数据如实流动且 fail closed。`image/*` 前缀之外不做 png/jpeg
   白名单（保留 webp 等宿主选择与 simulator 测试兼容性）；具体端点接受范围由
   profile 能力与端点行为决定。

## 备选方案

- **mira 内置编码器（issue 语义 (b)）**：自足但引入 PNG/JPEG 编码依赖（stb/miniz 或
  libpng），供应链、SBOM、跨平台验证成本高；且宿主仍需降采样策略控制（token 成本），
  编码边界放在宿主更贴合"平台能力经宿主注入"的分层。否决（真机证据表明必要可再评）。
- **媒体类型经 `IArtifactSource::describe()` 查询而非帧描述符携带**：需要扩展
  `IArtifactSource` 接口并破坏外部实现者；元数据在发布点（adapter commit）即确定，
  随帧携带更直接。否决。
- **对 `image/x-host-frame` 这类非标准 image 类型也拒绝**：会把"诚实标注"的原始帧
  与 simulator 测试一并拒掉，且端点差异（部分网关接受任意 image/*）不该由 mira 硬编码。
  否决；文档写明真实端点通常需要 png/jpeg，宿主转码后自然满足。

## 影响与风险

- 公开 API 变更：`ScreenFrameDescriptor` 追加三个带校验的字段（破坏性：旧代码构造的
  未带元数据描述符将被 `validate_frame_descriptor` 拒绝——仓库内全部构造点已同步）；
  `AndroidHostAdapter::artifact_store()` 追加；net 三个头成为公共 API。
- wire 行为变更：经方言层的图像 data URL 媒体类型随工件记录流动；未转码的 android
  原始帧（`image/x-host-frame`）通过 `image/*` gate 但真实端点可能仍拒——这是 (a)
  语义的既定边界，宿主转码后消除；此前 octet-stream 场景则从端点 opaque 拒绝变为
  本地 `UnsupportedCapability` 快速失败。
- 安装包新增公共头使 `Mira::net_transport` 的 usage requirement 依赖 executor 头文件
  （该头本来就链接 executor）；header check 与消费者测试覆盖此路径。

## 验证方式

本地：全量构建 + `ctest`（43/43，含新增
`check_frame_payload_metadata_and_store_access`、`non_image_media_type_fails_closed`、
loop wire 前缀断言 `data:image/x-rgba8888;base64,`、公共头自包含、安装消费者构造
`SocketHttpTransport` + `MbedTlsChannelFactory`）；CI 全平台矩阵。真机端到端
（PNG wire + 真实端点 200）属 miracle P3 补跑项，登记于维护计划。

## 关联文档和工作项

- GitHub issues #14、#15；miracle 台账 MIR-20260906-006/007
- [维护计划第二轮 maintenance-2026-09-transport-and-image-media.md](../plans/maintenance-2026-09-transport-and-image-media.md)
- [DEC-010](DEC-010-cross-platform-tls-proxy-upload.md)（传输依赖）、
  [DEC-012](DEC-012-host-adapter-feedback-round1.md)（store 注入）、
  [DEC-005](DEC-005-observation-coordinate-host-boundary.md)
- [API 手册：模型层](../api/model-agent-loop.md)、[环境与 Observation](../api/environment-observation.md)
