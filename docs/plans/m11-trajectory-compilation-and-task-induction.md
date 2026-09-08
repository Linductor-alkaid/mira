# M11：成功轨迹编译与任务归纳（阶段 D）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：[M9](m9-workflow-runtime-minimal-loop.md)（已完成；阶段 D 的直接前置是阶段 B，
> 阶段 C 的 patch 生效态是固化语义的输入）
> 建议发布点：Workflow compilation alpha
> 更新日期：2026-09-09

## 1. 目标

依据 [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 D、
[DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md) 与
[DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md)，交付 Workflow
数据平面的资产生产端：成功轨迹契约与采集（`capture_trajectory`，生效态快照）、
确定性编译器（`compile_workflow`：字面量化、默认值固化、跳过步剔除）、任务归纳
（`induce_parameters` 结构 diff + 显式候选，参数化重写）与入库门禁
（`publish_validated`：结构校验 -> DryRun 驱动 -> `DryRunPassed` 版本 + 证据摘要，
失败库零变更），使「成功经验 -> 版本化可复用 Workflow」闭环成立（`W-03`/`W-04`）。

选择新增 M11 而非扩展 M10 的理由：M10 已按其退出条件关闭（介入与策略全集）；编译与
归纳是独立能力增量（资产生产端 vs 执行介入端），验收证据（捕获/编译/门禁/归纳矩阵）
与 M10 测试不同源，按规范使用新里程碑文件承载。不对阶段 E/F 预分配编号。

## 2. 范围与非目标

### 2.1 范围

- 轨迹契约与采集：`WorkflowTrajectory`（溯源、有效参数、有效策略、生效步骤含
  provenance 原始实参）；`WorkflowRuntime::capture_trajectory`（仅 `Completed` 且
  派发副作用的 Run；DryRun 完成/非终态/失败 Run/未知 Run fail closed）；宿主直接
  构造轨迹入口（Agent 工具调用路径的数据形态）。
- 编译器纯函数：`compile_workflow`（字面量化、参数默认值固化 required -> 可选 +
  观测默认、跳过步剔除与 `jump_to` 负向、策略沿用与 options 门禁、`compile-*`
  确定性错误码、同输入同 digest）。
- 任务归纳：`WorkflowParameterCandidate` 与 `induce_parameters`（2..16 条同骨架
  轨迹结构 diff；provenance 命名优先、auto 命名稳定有序；常量叶保持字面量）；
  参数化编译重载（`{"$param": name}` 重写、类型推断、`required=false` + anchor
  默认；名字冲突/保留成员/非标量叶/类型不一致 fail closed）。
- 入库门禁：`publish_validated`（DryRun 同步驱动、`Completed` 要求、证据摘要
  digest、幂等 NoOp、`DryRunPassed` + 证据版本记录）；回退不变量（失败库零变更）；
  模型不可直达（无工具注册）。
- 事件：`WorkflowPublishProposed/Applied/Rejected` 三员入 v1 事件闭集（构建/解析、
  fail closed、审计序列）。
- 端到端取证：成功 Run（含运行中 patch 修复）-> 捕获 -> 归纳 -> 编译 -> 门禁入库 ->
  新版本以固化默认/参数复用再运行；installed-consumer 覆盖新公共面。

### 2.2 非目标

- Agent 工具调用轨迹的事件流自动抽取与「何时编译」的 harness 编排（Agent 侧行为，
  随后续 Agent Harness 编排里程碑）。
- 自然语言 -> 候选提议/patch 条目的解释编排（M10 §2.2 既定推迟不变）。
- 对象/数组叶参数化、跨 Workflow 聚类归纳、统计型参数发现（DEC-026 §4）。
- App Model、Navigation Planner、`screen_state` 谓词求值（阶段 E）。
- Memory 四类组织与学习闭环（阶段 F）。
- SQLite Workflow Library 持久化（`RISK-2026-038` 沿袭推迟；publish 事件为跨进程
  重建留事件基础，不改变本轮进程内投影现状）。
- 子 Workflow、并行步骤、通用图（IR 边界不变）。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M9 `Completed`（阶段 B 冻结：执行闭环、DryRun 语义、版本化与 Run 表）；M10
  `Completed`（patch 生效态是固化语义的输入）。PR #30/#31 CI 全绿。
- 本里程碑经维护者评审由 `Proposed` 转 `Planned`（用户指示依设计与计划推进下一步
  开发，与 M8/M9/M10 同一授权模式）；专项设计与决策（DEC-025/026、设计 v0.4 §12）
  随本里程碑创建并冻结。

### 3.2 设计与决策依据

- [Workflow Runtime 设计](../design/workflow_runtime_design.md) §12（阶段 D 实施规范）
- [DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md)、
  [DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md)
- [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) §8/§14/§16、
  [DEC-019](../decisions/DEC-019-workflow-ir-contract.md) 至
  [DEC-024](../decisions/DEC-024-conversation-patch-execution.md)
- [核心公共契约与状态机](../design/core_contracts_and_state_machine.md)、
  [Mira Runtime 设计](../design/mira_runtime_design.md)

## 4. 工作项

### 4.1 轨迹契约与采集

- [x] `M11-01` 冻结并实现 `WorkflowTrajectory` 契约与 `capture_trajectory`：
  生效态快照（patch 覆盖实参、有效参数、有效策略）、跳过集剔除、provenance 原始
  实参保留；准入（`Completed` + 派发副作用）fail closed 与确定性错误码。

### 4.2 编译器与门禁

- [x] `M11-02` 实现字面量编译 `compile_workflow`：步骤映射与谓词/钩子/循环保留、
  默认值固化、跳过剔除与 `jump_to` 负向、策略门禁、`compile-*` 错误码、
  `validate_workflow_definition` 同源校验、确定性 digest。
- [x] `M11-03` 实现入库门禁 `publish_validated`：DryRun 同步驱动与 `Completed`
  要求、证据摘要（内容派生：ir digest、逐步结算、unevaluable 计数；源 Run 与门禁
  Run 绑定经事件审计）、幂等 NoOp、`DryRunPassed` 版本追加、失败库零变更；
  `publish_workflow` 原始路径行为不变。
- [x] `M11-04` 实现 publish 三员事件（构建/解析/闭集扩展）与审计序列；门禁失败
  `WorkflowPublishRejected`（机器可读原因码）。

### 4.3 任务归纳

- [x] `M11-05` 实现候选契约与结构 diff `induce_parameters`：同骨架校验、标量叶
  比较、provenance/auto 命名、常量保持、上限约束；确定性。
- [x] `M11-06` 实现显式候选与参数化编译重载：`{"$param": name}` 重写、类型推断、
  anchor 默认、冲突/保留成员/非标量/类型不一致 fail closed；与既有绑定纯函数一致。

### 4.4 端到端与取证

- [x] `M11-07` 端到端取证：含 patch 修复的成功 Run -> 捕获 ->（多轨迹）归纳 ->
  编译 -> 门禁入库 -> 新版本运行（固化默认与参数复用两条路径）；installed-consumer
  用例。
- [x] `M11-08` 测试矩阵取证与文档同步：本机 Release/Debug/ASAN/UBSAN（TSAN 按环境
  限制记录补跑条件）、quality 门禁、installed-consumer；总计划、设计文档（v0.4）、
  API 手册与 DEC-019/020/022 注记同步后关闭里程碑。

## 5. Executor 路由与关闭表（细化设计 §7/§12）

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| 轨迹捕获（capture_trajectory） | 调用线程（互斥下快照拷贝） | 无 | 生效态一致性由互斥保证 |
| 编译/归纳（compile_workflow、induce_parameters） | 调用线程同步（纯函数） | 无 | 无 |
| 门禁驱动（publish_validated 内 DryRun） | 宿主调用线程（复用 execute_run 同步路径） | 调用方 | 门禁 Run 占用 Run 表容量；失败即拒绝发布 |

理由：阶段 D 不引入新任务类别；门禁驱动与 `execute_run` 同一宿主同步路径（文档已
披露不得在持有宿主关键锁的上下文调用）。关闭顺序不变（M9 §5）：
`WorkflowRuntime::shutdown()` -> `MiraRuntime` 关闭 -> 宿主非 worker 线程
`executor.shutdown(true)`。

## 6. 风险与阻塞

- `RISK-2026-045`：DryRun 对 `step_result` 谓词必然不可求值（工具未派发），证据
  摘要如实记录 unevaluable 计数而非视作失败——「DryRunPassed」是结构门禁结论，
  执行证据在源 Run；两者共同构成 `W-04` 证据链。Owner：Mira Maintainers。缓解：
  DEC-025 §3 显式化 + API 手册披露 + 测试锁定证据摘要形状。
- `RISK-2026-046`：少量样本下「常量」可能是偶然一致（归纳错误，DEC-014 §风险）。
  Owner：Mira Maintainers。缓解：归纳是宿主可编辑草稿（DEC-026 §3），候选观测值
  保留在宿主侧供审阅；v1 不做统计推断，文档披露。
- `RISK-2026-038`（沿袭）：库为进程内投影；publish 事件为重建留事件基础，持久化
  载体仍推迟。
- `RISK-2026-039`（沿袭）：中断副作用步骤判定仍只有结构化谓词证据层（阶段 E 扩展）。

## 7. 测试与退出条件

- [x] `M11-01` 至 `M11-07` 全部完成并有可复现验证记录。
- [x] 捕获矩阵：生效态快照（覆盖实参、跳过剔除、有效参数与策略）、DryRun 完成拒绝、
  非终态/Failed/Cancelled/未知 Run 拒绝、宿主构造轨迹形状。
- [x] 编译矩阵：默认值固化（required -> 可选 + 默认）、跳过步剔除、`jump_to` 指向
  剔除步拒绝、tool 绑定形状、策略不在允许集拒绝、同输入两次编译 digest 相等、
  M9/M10 全路径回归。
- [x] 门禁矩阵：好草稿入库（`DryRunPassed` + 证据）且新版本可 Run、按固化默认值
  完成；坏草稿（DryRun 不达 Completed）库零变更、head 不变、拒绝事件与原因码；
  幂等重放 NoOp（同 digest 同证据）；版本链 parent 正确；历史不可变回归。
- [x] 归纳矩阵：双轨迹单叶差异 -> 单候选（provenance 命名、多步引用合并）；常量叶
  保持字面量；auto 命名稳定有序；骨架不匹配/超上限/类型不一致拒绝；显式候选合法
  提升与名字/pointer/保留成员/非标量/冲突负向；参数化编译后绑定解析（默认与显式
  传参）与 `bind-*` 错误码透出。
- [x] 事件：publish 三事件序列与载荷、未知字段 fail closed、闭集扩展回归；门禁
  Run 自身的 `WorkflowRunStarted/Settled` 事件如实记录。
- [x] 门禁：ASAN/UBSAN 全绿；TSAN 在本机限制下按 M9/M10 模式取证；quality
  （clang-format、docs、sbom、platform-boundary）通过；Windows/Android 构建组合与
  clang-tidy 由 PR CI 补验全绿；installed-consumer 覆盖新公共面。
- [x] 总计划第 4/5 节、`workflow_runtime_design` v0.4、API 手册、DEC-019/020/022
  （阶段 D 注记）与本文件同步。

## 8. 验证记录

2026-09-09：依据 M10 退出条件与总计划阶段 D 规则创建本里程碑（`Proposed`）：先完成
专项决策（DEC-025、DEC-026）与设计更新（`workflow_runtime_design` v0.4 §12），再
创建本文件。同日经维护者评审（用户指示依设计与计划推进下一步开发，与 M8/M9/M10
同一授权模式）转 `Planned` 并进入实施（`In Progress`）。

2026-09-09：实现与本地验证（Ubuntu 24.04，x86_64，g++ 13.3.0，CMake 3.28.3，Unix
Makefiles；本机无 clang/clang-tidy，由 PR CI quality job 补验；`clang-format` 使用
miniconda 发行版）。

- 实现：`workflow_compiler.hpp/cpp`（`WorkflowTrajectory` 契约、字面量编译
  `compile_workflow`、`WorkflowParameterCandidate` 与结构 diff `induce_parameters`、
  参数化编译重载；RFC 6901 定位与函数式叶重写；`WorkflowCompileError` 确定性错误
  码）；`workflow_events` 新增 publish 三员（构建/解析/闭集扩展）；
  `WorkflowRuntime` 新增 `capture_trajectory`（`Completed`+派发副作用准入、生效态
  快照、provenance 原始实参）与 `publish_validated`（结构校验 → DryRun 同步门禁 →
  内容派生证据摘要 → 幂等 NoOp → `DryRunPassed` 版本追加；失败库零变更 + 拒绝事件）；
  `publish_workflow` 与 `append_version_record` 共享库追加逻辑（原语义不变）。
- 实现中的语义修正（相对决策草案，随本轮冻结）：证据摘要改为内容派生（同定义 + 同
  DryRun 结算序列 ⇒ 同证据，幂等 NoOp 依赖该性质），源 Run 与门禁 Run 绑定经
  `WorkflowPublishProposed/Applied` 事件审计承载；归纳 provenance 候选合法复用来源
  参数名（多步引用合并位置），同名 explicit/structural 候选仍 fail closed。
- 新增测试：`mira_m11_compile_test`（捕获准入矩阵：DryRun 完成/非终态/未知 Run 拒绝、
  WaitingAgent 修复 patch 后的生效态与 provenance、默认值固化与结构保留、确定性
  digest、options/jump/tool 绑定负向、宿主构造轨迹）、`mira_m11_publish_test`
  （门禁入库后按固化默认复跑、DryRun 不达 Completed 库零变更 + 拒绝事件、幂等 NoOp、
  非法结构与 required 参数拒绝、三事件载荷往返与未知字段 fail closed、原始
  `publish_workflow` 路径无事件）、`mira_m11_induction_test`（结构 diff 命名/常量
  保持/骨架与类型负向、参数化重写的绑定解析与 `bind-*` 透出、显式候选全负向矩阵、
  修复固化端到端与归纳版本复用端到端）。
- 安装包：`mira_installed_consumer` 增加 M11 用例（成功 Run → 捕获 → 编译 → 门禁入库
  → 固化默认复跑 → 双 Run 归纳 → 派生工作流参数复用）。
- 结果：Release/Debug/ASAN/UBSAN 各 60/60 通过；TSAN（`setarch x86_64 -R`，mbedtls
  portable 按配置禁用）59/59 通过；`format-check`、`docs-check`、`sbom-check`、
  `platform-boundary-check`、`consumer-check` 通过。
- 限制：Windows/Android 构建组合与 clang-tidy 由 PR CI 补验后随 `M11-08` 回填（见下
  条）；阶段 E/F 不在本轮（范围外）。
- 同步：DEC-025/026（本轮冻结）、DEC-019/020/022（阶段 D 注记）、
  `workflow_runtime_design`（v0.4 §12 与路由/事件/模块/测试表）、API 手册
  （workflow-contracts 增 M11 节）、总计划（§4/§5）、本文件。

2026-09-09：PR [#32](https://github.com/Linductor-alkaid/mira/pull/32) CI 全绿（head
`5594f83`，push pipeline run
[`34262665602`](https://github.com/Linductor-alkaid/mira/actions/runs/34262665602)、
pull_request pipeline run
[`34262669837`](https://github.com/Linductor-alkaid/mira/actions/runs/34262669837)）：
Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android arm64-v8a 与
x86_64（NDK）、ASAN/UBSAN/TSAN 与 quality（clang-tidy 18 + clang-format + docs/sbom/
platform-boundary 检查）全部 24 项通过，补齐本机缺失的 clang-tidy 与跨平台验证。首轮
quality 在 `workflow_runtime.cpp` 报 1 处 `performance-move-const-arg`（对
trivially-copyable 的 `std::optional<Sha256Digest>` 使用无效 `std::move`），去除后复验
通过；语义不变（本地 m8–m11 套件复跑 14/14 通过）。PR 已合并（merge `06330a3`）。
`M11-01` 至 `M11-08` 全部完成，退出条件逐项满足，本里程碑关闭（`Completed`）。阶段 E
（App Model 与导航）里程碑可依据 `workflow_runtime_design` 与 DEC-014 创建并进入
`Planned`（前置为阶段 B，已满足；感知能力边界按 DEC-011）。
