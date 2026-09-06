# 维护计划：传输导出与图像 wire 媒体类型（2026-09 第二轮）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M4 后维护轮，依据 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)）
> 前置：M2、M3、M4（Completed）；第一轮维护（[maintenance-2026-09-host-abi-feedback.md](maintenance-2026-09-host-abi-feedback.md)）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-09-06

## 1. 目标

落地 miracle 第二轮反馈的 2 个 GitHub issues（#14、#15）：安装包导出官方网络传输
头文件（消除"有库无头"），并以"宿主负责编码"语义修复模型图像 wire 媒体类型链路
（载荷元数据契约 + fail-closed 方言门 + 工件读取 API）。

## 2. 范围与非目标

### 2.1 范围

- `adapters/net/*.hpp` → `include/mira/adapters/net/` 迁移与安装导出（#14）。
- `ScreenFrameDescriptor` 载荷元数据字段与校验；android/simulator adapter 填充（#15）。
- `AgentLoop::build_request` 从工件记录取媒体类型/字节数（#15）。
- 方言层 `image/*` fail-closed 门（#15）。
- `AndroidHostAdapter::artifact_store()` 读取句柄（#15）。
- 公共头自包含检查、安装消费者测试覆盖新头与官方传输构造。

### 2.2 非目标

- Core 内置 PNG/JPEG 编码器（DEC-013 否决；转码是宿主职责）。
- png/jpeg 白名单（保留宿主选择 `image/webp` 等的自由；具体端点接受范围由 profile
  与端点决定）。
- miracle 侧真实端到端验收（P3，外部仓库）。

## 3. 设计与决策依据

- [DEC-013](../decisions/DEC-013-transport-export-and-image-media.md)
- [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)（传输依赖）
- [DEC-012](../decisions/DEC-012-host-adapter-feedback-round1.md)（store 注入，本轮回读配套）
- [API 手册：模型层](../api/model-agent-loop.md)、[环境与 Observation](../api/environment-observation.md)

## 4. 工作项

- [x] `MNT-202609-08`（#14）`socket_transport.hpp`/`mbedtls_tls.hpp`/`openssl_tls.hpp`
  迁移至 `include/mira/adapters/net/`，三个 transport 目标移除 PRIVATE 头路径，
  `install(DIRECTORY include/)` 自然导出；公共头自包含检查覆盖（socket 头由 header
  check 提供 executor include）；安装消费者测试以 `find_package(Mira)` 构造
  `SocketHttpTransport`（start/shutdown 生命周期）与 `MbedTlsChannelFactory`
  （缺 CA 文件必须 fail closed）。
- [x] `MNT-202609-09`（#15）`ScreenFrameDescriptor` 新增 `payload_media_type`/
  `payload_byte_size`/`payload_digest`（校验强制）；双 adapter 从 commit 记录填充；
  `build_request` 的 `ArtifactRef` 改用工件记录元数据（内联门槛随之按实际载荷字节数
  判定）；方言层两个 ImagePart encode 站点对非 `image/*` 媒体类型在 fetch 前
  fail closed；`AndroidHostAdapter::artifact_store()` 读取句柄。

## 5. 风险与阻塞

- 未转码的 android 原始帧（`image/x-host-frame`）通过 `image/*` 门但真实端点可能仍拒：
  (a) 语义既定边界，宿主转码后消除；已在 DEC-013 与 API 手册写明。
- 真实端点端到端（PNG wire + 200 + decision）与 miracle P3 三类任务取证为外部补跑项。
- CI 证据待 PR pipeline 回填本文件。

## 6. 测试与退出条件

- [x] 新增/更新测试本地通过：`check_frame_payload_metadata_and_store_access`（m2）、
  `non_image_media_type_fails_closed`（m3 dialect，双方言）、loop wire 断言
  `data:image/x-rgba8888;base64,`（m3）、帧描述符元数据负向校验（m2 观察契约）、
  公共头自包含（含三个 net 头）、安装消费者构造官方传输栈。
- [x] 本地全量 `ctest` 43/43 通过（Ubuntu 24.04 x86_64，GCC 13.3，debug preset）。
- [ ] CI 全平台矩阵全绿；回填 run 链接。
- [ ] miracle 按 (a) 语义注入转码 store 并回传真机端到端证据（外部依赖）。

## 7. 验证记录

2026-09-06：本地验证（Ubuntu 24.04 x86_64，GCC 13.3.0，CMake 3.28.3，`debug` preset，
分支 `fix/transport-export-and-image-media`，基于第一轮分支）。

- 构建：全目标通过；`docs-check`、`platform-boundary-check` 通过（迁移后的 net 头
  位于 `include/mira`，经边界扫描确认无平台 SDK 头）。
- 测试：`ctest` 43/43 通过，含上述新增检查；安装消费者测试经
  `mira_installed_consumer_test` 验证 `find_package(Mira)` 构造官方传输栈。
- 本机限制（无 clang/clang-tidy/sudo、无 Android NDK）同第一轮记录；由 PR pipeline
  补跑后回填。
