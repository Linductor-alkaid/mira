# 维护计划：Host ABI 第一轮上游反馈（2026-09）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M4 后维护轮，依据 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)）
> 前置：M2、M4（Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-09-06

## 1. 目标

落地 miracle（DEC-011 独立验证载体）第一轮真机验证反馈的 7 个 GitHub issues（#7–#13）：
修复 lease 统计缺陷、补齐 adapter 契约能力（artifact 注入、输入时长、UI 树聚合）、
扩展构建矩阵（x86_64）、登记方向性缺口（ToolProposals）并回填 ABI 文档证据与契约澄清。

## 2. 范围与非目标

### 2.1 范围

- `AndroidHostAdapter`：artifact store 注入/容量（#10）、`duration_ms` 映射与上限校验（#11）、
  UI 树聚合与能力门控（#7）。
- `HostDispatcherBridge`：`leases_released` 统计口径修复（#12，BUG-20260906-001）。
- 公开契约：`InputEvent.duration_ms`、`AndroidHostAdapterOptions`、`mira.host.tree.v1`
  线格式（[DEC-012](../decisions/DEC-012-host-adapter-feedback-round1.md)）。
- 构建矩阵：`android-x86_64` 工具链/预设/CI（#9）。
- 文档：android-host-abi.md 证据回填与 3 条契约澄清（#13）、M7 方向登记交叉引用（#8）。

### 2.2 非目标

- AgentLoop 内执行 ToolProposals（#8 仅登记方向，触发条件 POST-01 证据到位后重定义）。
- 真机/模拟器 instrumented 冒烟（属 miracle 侧 POST-02 与本仓库后续补跑项）。
- 前台应用与设备状态组件、连续控制、Tool 模组等 M5/M6/M7 原范围内容。

## 3. 设计与决策依据

- [DEC-012](../decisions/DEC-012-host-adapter-feedback-round1.md)：本轮全部契约修订
- [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)：Host 边界
- [android-host-abi.md](../compatibility/android-host-abi.md)：ABI 冻结范围与线格式
- [DEC-009](../decisions/DEC-009-tool-module-boundary.md)、M7 Blocked 状态（#8 的既有载体）

## 4. 工作项

- [x] `MNT-202609-01`（#12 / BUG-20260906-001）修复 `leases_released` 口径：统计覆盖
  bridge 内部释放与消费方 guard 释放（含 observe 尾部），guard 生命周期超过 bridge 时
  计数安全；契约测试覆盖成功、聚合、错误结算三条路径。
- [x] `MNT-202609-02`（#10）`AndroidHostAdapter::create` 支持 `AndroidHostAdapterOptions`
  注入 `IArtifactStore` 或声明内存容量；默认容量 8→64 MiB（DEC-012）；容量耗尽为结构化
  `ResourceExhausted`；契约测试覆盖容量边界与注入路径。
- [x] `MNT-202609-03`（#11）`InputEvent` 增加 `duration_ms`（默认 0 向后兼容）；adapter
  填充 ABI 字段并对照 `max_gesture_duration_ms` fail closed；契约测试覆盖显式时长传递、
  默认 0 与超限拒绝。
- [x] `MNT-202609-04`（#7）定义 `mira.host.tree.v1` JSON 线格式；`capabilities().ui_tree`
  如实映射 `accessibility_completeness >= 1`；`observe()` 聚合 structure（含
  structure-only、聚合观察的 BoundedSkew 声明）；`= 0` fail closed；垃圾负载拒绝且无
  lease 泄漏；契约测试覆盖聚合、epoch 失效、能力降级、fail closed 路径。
- [x] `MNT-202609-05`（#9）新增 `cmake/toolchains/android-x86_64.cmake`、
  `android-x86_64-base`/`android-x86_64-release` 预设与 CI android matrix 扩展。
- [x] `MNT-202609-06`（#8）在 M7 计划中交叉引用 GitHub #8 作为方向登记；不在本轮实现，
  维持 M7 Blocked 与 DEC-009 现状。
- [x] `MNT-202609-07`（#13）android-host-abi.md：回填 miracle 真机/模拟器互操作证据行、
  3 条调用约定澄清（out 参数零初始化、`out_operation` 可空、`deadline_ns` 绝对时刻）、
  `mira.host.tree.v1` 规范；头文件注释同步成文。

## 5. 风险与阻塞

- 真机 UI 树互操作（miracle 侧序列化未实现）与 x86_64 模拟器 instrumented 冒烟未执行；
  属补跑项，见 §6 与 android-host-abi.md §3。
- CI 证据已回填（§6、§7）；x86_64 构建证据等级见
  [platform-matrix.md](../compatibility/platform-matrix.md)。

## 6. 测试与退出条件

- [x] 新增/更新的 fake host 契约测试全部通过（本地 Linux x86_64, GCC 13, Debug）。
- [x] 既有 m2/contract/m3/m4 测试回归通过（本地全量 ctest，43/43）。
- [x] `platform-boundary-check`、公共头独立包含测试通过（ABI 头仅注释变更）。
- [x] CI 全绿（含 android arm64 + x86_64 matrix、sanitizers、quality）：PR #16 push run
  [`34014586674`](https://github.com/Linductor-alkaid/mira/actions/runs/34014586674)
  （commit `6d49cc7`）24/24 检查通过——Linux GCC/Clang Debug+Release、Windows Debug+Release、
  Android arm64+x86_64（NDK 26.3，API 24）、ASAN/UBSAN/TSAN、quality（clang-tidy、
  clang-format 18.1.8、docs、SBOM、平台边界）。期间修复根 `CMakeLists.txt` 的 Android ABI
  守卫（原仅允许 arm64-v8a，首次 run `34014171269` 的 x86_64 configure 失败）。
- [ ] miracle 侧按 `mira.host.tree.v1` 消费并回传真机 structure 证据（外部依赖，
  登记于 android-host-abi.md §3）。

## 7. 验证记录

2026-09-06：本地验证（Ubuntu 24.04 x86_64，GCC 13.3.0，CMake 3.28.3，`debug` preset，
commit 见 PR #16）。

- 构建：`cmake --preset debug && cmake --build --preset debug -j 4` 全目标通过。
- 测试：`ctest --test-dir build/debug --output-on-failure` 43/43 通过（含新增
  `check_leases_released_counts_every_release_path`、`check_structure_observation_aggregation`、
  `check_structure_epoch_and_capability_degradation`、
  `check_artifact_store_capacity_and_injection`、`check_input_duration_semantics`）。
- `docs-check`、`platform-boundary-check`、`format-check` 目标通过。

2026-09-06：CI 验证（PR #16，commit `6d49cc7`）。

- push run [`34014586674`](https://github.com/Linductor-alkaid/mira/actions/runs/34014586674)
  与 pull_request run [`34014588478`](https://github.com/Linductor-alkaid/mira/actions/runs/34014588478)
  全部 24 项检查通过；Android x86_64 构建证据已回填
  [platform-matrix.md](../compatibility/platform-matrix.md)（`Build verified`）。
- 首次 run [`34014171269`](https://github.com/Linductor-alkaid/mira/actions/runs/34014171269)
  的 `android (android-x86_64-release)` configure 失败：根 `CMakeLists.txt` 的 ANDROID 守卫
  硬编码 arm64-v8a。修复为 `^(arm64-v8a|x86_64)$` 后复验通过；失败与修复记录保留于此。
- 本机限制（无 clang/clang-tidy/sudo、无 Android NDK）已由上述 CI run 覆盖补跑。
