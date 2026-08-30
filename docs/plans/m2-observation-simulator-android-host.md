# M2：Observation、坐标、Simulator 与 Android Host ABI

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M1
> 建议发布点：Environment alpha
> 更新日期：2026-08-30

## 1. 目标

交付诚实、可校验的环境感知与动作坐标基础：平台无关 Observation 聚合契约、显式坐标空间与
变换引擎、升级后的 Simulator 参考环境，以及 Android Host 与 Native Adapter 之间的版本化 C
ABI。M2 完成后，M3 的视觉 Agent 闭环可以运行在带 capture span、environment epoch 和坐标
provenance 的 Observation 上，而不是裸字符串快照。

## 2. 范围与非目标

### 2.1 范围

- `CaptureSpan`、`ObservationComponent`、`Observation` 聚合、`ObservationRequest` 与质量语义。
- `ScreenFrameDescriptor`、`PlaneLayout`、`FrameCoverage`、`DisplayTopology` 及其校验。
- 坐标空间、`Matrix3x3`、`CoordinateTransform`、变换链解析、误差 bound 与失效规则。
- `UiTreeSnapshot`、`UiNode`、`ElementRef` 契约与截断/增量语义。
- `IEnvironment` 契约升级（capabilities、request 驱动 observe、OperationContext）及迁移。
- Simulator 场景升级：旋转、density、letterbox、inset、多 display、非原子组件与 epoch 失效注入。
- Observation Pipeline 聚合器：组件编排、deadline、partial 结算与质量计算。
- Android Host ABI 头文件、Host dispatcher 桥与 Native Adapter 骨架（以 fake host 验证）。

### 2.2 非目标

- 本地 OCR/检测/ONNX 感知（M5）；连续控制与实时轨迹（M6）。
- 真实 Android 设备上的运行验证；M2 只交付 ABI 与 Adapter 骨架及 fake host 契约测试。
- 外部 LLM Provider、Tool 隔离与长期 Memory。
- 零拷贝 AHardwareBuffer 扩展；首期 buffer lease 以复制语义交付。

## 3. 设计与决策依据

- [Observation、坐标与 Android Host ABI 设计](../design/observation_coordinate_android_host.md)
- [Mira Runtime 设计](../design/mira_runtime_design.md)
- [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)
- [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md)、
  [DEC-002](../decisions/DEC-002-public-contract-versioning.md)、
  [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)

## 4. 工作项

### 4.1 平台无关契约（本批）

- [x] `M2-01` 实现坐标空间、`Matrix3x3`、`CoordinateTransform`、变换链解析与逐段校验；
  奇异矩阵、缺失链、错误 epoch、越界点和 NaN 输入返回稳定错误，不产生猜测坐标。
- [x] `M2-02` 实现 `CaptureSpan`、`ObservationComponent`、`Observation` 聚合、
  `ObservationRequest`、原子性分级与请求满足度评估；skew 计算不把同批返回伪装成原子。
- [x] `M2-03` 实现 `ScreenFrameDescriptor`、`PlaneLayout`、`FrameCoverage`、mask 语义、
  `DisplayTopology` 与拓扑 hash；格式/平面/stride 越界校验 fail closed。
- [x] `M2-04` 实现 `UiNode`、`UiTreeSnapshot`、`ElementRef` 契约；截断/增量/可见性声明、
  password 节点不明文和 parent 完整性由校验函数强制。

### 4.2 环境契约与 Simulator

- [ ] `M2-05` 升级 `IEnvironment`：`EnvironmentCapabilities`、request 驱动的
  `observe/execute/interrupt` 与 `OperationContext`；迁移 Runtime、Simulator、示例和既有测试。
- [ ] `M2-06` 升级 Simulator 参考环境：旋转、density、letterbox、system inset、多 display、
  非原子组件与 epoch 失效注入，并提供契约测试夹具。
- [ ] `M2-07` 实现 Observation Pipeline 聚合器：组件按 deadline 结算、partial 降级、
  skew/质量计算与 Observation 发布事件。

### 4.3 Android Host 边界

- [ ] `M2-08` 冻结 Android Host ABI 头文件：版本化 struct、稳定 status code、callback
  exactly-once 约定、`MiraHostBufferLeaseV1` 与 capability snapshot。
- [ ] `M2-09` 实现 Host dispatcher 桥与 Native Android Adapter 骨架：Executor 受管提交、
  callback 结算、lease exactly-once release、epoch 递增，使用 fake host 契约测试验证。

### 4.4 验证与文档

- [ ] `M2-10` 整合 M2 契约测试矩阵（坐标、ABI 生命周期、输入安全），同步设计、兼容性、
  决策与本计划验证记录。

## 5. Executor 路由与关闭

| 工作 | Executor 能力 | Owner | 结算证据 |
| --- | --- | --- | --- |
| 组件采集（截图、tree、设备状态） | `submit_auto()` 或 blocking worker | Observation Pipeline | 已消费 future/result |
| 变换链解析、契约校验、hash | 同步纯函数，调用方任务内 | 调用方 Task | Result/事件 |
| Host dispatcher 请求 | 外部事件循环扩展边界 | Android Adapter | operation terminal 回执 |
| epoch/能力事件处理 | Runtime 串行控制面 | TaskCoordinator | 事件序列 |

Android Host 无法取消的原子平台调用返回后只能标记 stale，不得改变旧 Task 状态；Host callback
只做有界复制和 completion 提交。若 Host dispatcher 桥无法用 Executor 公开能力表达，先登记
`docs/executor_feedback/ledger.md`。

## 6. 风险与阻塞

- `RISK-2026-014`：坐标语义（尤其旋转与 letterbox）容易在不同层各自实现。缓解：变换只允许
  显式 `CoordinateTransform` 链，Simulator 契约测试固定 0/90/180/270、density、inset、多
  display 的期望矩阵；任何“猜 scale”路径 fail closed。
- `RISK-2026-015`：Android ABI 细节（线程亲和、callback 线程模型）依赖尚未验证的 Host 实现。
  Owner：M2 Host owner。缓解：先冻结语义与 fake host 契约测试，真实 Host 集成保持未勾选并
  记录补跑条件。
- `RISK-2026-016`：`IEnvironment` 升级会破坏 M1 示例与测试。缓解：一次迁移内同步 Runtime、
  Simulator、consumer 与文档，公共契约变更按 `DEC-002` 以兼容扩展优先。

当前没有确认的 Executor 能力缺口，不新增反馈台账记录。

## 7. 测试矩阵

| 维度 | 必测场景 |
| --- | --- |
| 变换 | 0/90/180/270、density、letterbox、inset、多 display、往返误差 bound |
| 变换失效 | 缺链、错 epoch、奇异矩阵、NaN/Inf、valid region 越界、低质量链 |
| Frame | packed/planar 平面数、stride/offset 越界、mask 区域、格式解析 |
| Observation | required 缺失、skew 分级、max_age 过期、partial 质量 |
| UI Tree | 截断/增量/可见性、parent 悬空、password 明文拒绝 |
| Simulator（M2-06） | 旋转前后 epoch 失效、坐标链重解析、非原子组件 |
| ABI（M2-08/09） | struct_size/abi_version、callback exactly-once、lease 释放、stop 幂等 |

## 8. 退出条件

- [ ] `M2-01` 至 `M2-10` 全部完成并有可复现验证记录。
- [ ] 坐标与 Observation 契约测试矩阵全部通过，负向路径（缺链、错 epoch、奇异、越界、
  NaN）均返回稳定错误且不产生坐标猜测。
- [ ] Simulator 契约测试覆盖旋转、letterbox、多 display、非原子组件和 epoch 失效。
- [ ] Android Host ABI 以 fake host 契约测试验证 callback exactly-once 与 lease 释放；
  真实设备验证保持未勾选并记录补跑条件。
- [ ] ASAN/UBSAN 通过；支持环境下 TSAN 通过，未支持时保留未完成项和补跑条件。
- [ ] 公共头独立包含、最小 consumer 构建和文档检查通过。

## 9. 验证记录

2026-08-30：M2 计划创建，拆分工作项并冻结范围；同批完成 `M2-01` 至 `M2-04` 平台无关契约
（`include/mira/coordinates.hpp`、`include/mira/observation.hpp`、`src/observation/` 实现与
`tests/m2/m2_observation_coordinate_test.cpp`）。

2026-08-30：Linux x86_64 本地验证（GCC 13.3，CMake 3.28）完成。`debug`、`release`、
`asan`、`ubsan` 均执行构建与 `ctest`，各 13/13 通过；`tsan` 使用 CI 同等的
`setarch x86_64 -R ctest --test-dir build/tsan --output-on-failure`，13/13 通过。公共头独立
包含、安装 consumer、Markdown 文档、SBOM、平台边界测试均包含在上述 CTest 中并通过；
`cmake --build build/debug --target docs-check sbom-check platform-boundary-check` 也通过，
`git diff --check` 通过。

限制：本机没有 `clang++`、`clang-format`、`clang-tidy`，因此 Clang 构建、静态分析和格式
检查未执行；Windows 与 Android arm64 交叉编译因本机无对应工具链/NDK 未补跑。负责人为
Mira Maintainers，补跑条件为 CI 提供目标工具链并执行既有平台矩阵与 quality job；这些项
不得以本地 GCC 结果替代。

2026-08-30：提交 `01503b2` 的 CI run
[`33317049305`](https://github.com/Linductor-alkaid/mira/actions/runs/33317049305) 中，Linux
GCC/Clang、Android arm64、ASAN/UBSAN/TSAN 与 quality job 通过；Windows Debug/Release
在编译 `coordinates.hpp` 时因 MSVC `C3615` 拒绝 `std::isfinite` 出现在 `constexpr` 函数中而
失败。修复提交移除相关有限性校验、奇异性校验和齐次应用函数的 `constexpr` 修饰，不改变
运行时语义；修复后的本地五套 GCC 配置均再次通过 13/13，等待后续 CI 复验 Windows 矩阵。
