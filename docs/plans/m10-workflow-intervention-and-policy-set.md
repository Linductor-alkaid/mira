# M10：Workflow 介入与执行策略全集（阶段 C）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：[M9](m9-workflow-runtime-minimal-loop.md)（实现已交付；Android 验收重开）
> 建议发布点：Workflow intervention alpha
> 更新日期：2026-09-09

> 2026-09-09 状态复核：实现已交付；`BUG-20260909-001` 发现 Android CI 未构建
> `mira_workflow`，本里程碑的矩阵取证项及相关退出条件重新打开。历史验证记录保留，
> 当前状态以本注记为准；依赖方可复用冻结契约，跨平台关闭共同等待
> [阶段 F 后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-22`。

## 1. 目标

依据 [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 C、
[DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md) 与
[DEC-024](../decisions/DEC-024-conversation-patch-execution.md)，交付 Workflow 数据平面的
介入能力：`Recoverable`/`AgentAssisted`/`Interactive` 三种策略的运行时行为（失败升级、
`AgentEscalation` 钩子两时机、检查点）、对话驱动 patch 的执行闭环（准入、幂等、步边界
生效、审计、回退、运行中策略切换）与 `WaitingUser` 决策点交互（运行时失败决策、
`request_user_input` 工具、决议 API），并将 `patch_workflow` 注册进 BuiltIn 工具闭环，
使五操作 schema 全部有执行体。

选择新增 M10 而非扩展 M9 的理由：M9 已按其退出条件关闭（Strict/DryRun 执行闭环）；
介入与策略全集是独立能力增量，验收证据（策略矩阵、patch 管线与决议交互的运行时行为
测试）与 M9 测试不同源，按规范使用新里程碑文件承载。不对阶段 D–F 预分配编号。

## 2. 范围与非目标

### 2.1 范围

- 策略准入门更新：五种策略全部可执行（`allowed_policies` 成员资格与 agent-capable
  钩子校验沿用既有契约函数）；移除 M9 的两策略限制。
- 失败升级路径：恢复链耗尽点按策略分流（`Failed` / `WaitingAgent` / `WaitingUser`
  决策点）；`AgentEscalation` 钩子的失败驱动与到达驱动（`AgentAssisted` 检查点）两
  时机；每次到达至多交出一次的计数语义。
- 载体任务迁移：`MiraRuntime::begin_task_recovery(TaskId)`（`WaitingAgent` 的
  `Recovering` 进入路径）；`resume_task` 自 `Recovering` 的 epoch 递增补全。
- 有界性：跨 `WaitingAgent` 周期的尝试/迭代/步预算累计；无 Agent-运行时重试环。
- Agent 续跑上下文：`agent_continuation(run_id)`（仅 `WaitingAgent`，设计 §7.6 最小
  实现：当前步、有效参数、步历史、失败原因、尝试计数、pending 决策）。
- Patch 执行闭环：`patch_run`（宿主直达）与 `patch_workflow` handler（模型）同源；
  准入矩阵（策略 × 状态 × 例外通道）、幂等键写前+应用双检、整 patch 原子性、
  三事件审计、`run_patch_epoch` 语义、每 Run 排队上限（16）。
- Patch 条目应用语义：步边界排水（Running）与等待态直达；参数 Set/Unset 绑定纯函数
  重建（未执行步骤重解析、已结算不回溯）；step_arguments Set/Unset/Skip；policy Set
  （成员资格 + Navigate 剩余步骤门禁 + `WorkflowPolicySwitched`）。
- Patch 回退：`rollback_run_patch`（应用前快照差异 → 显式回退 patch → 同一管线）。
- 决策点交互：`StepFailure`（Interactive 失败，提议=跳过失败步骤）与 `AgentPrompt`
  （`request_user_input` 工具，schema 冻结）两类；`resolve_decision`（ID+digest 匹配、
  accept/reject/cancel_run 全路径、accept/reject 后异步续跑）；`WaitingUser` 唯一出口
  收紧（`resume_run` 拒绝并指引）；每 Run 至多一个 pending 决策；无自动超时。
- 工具闭环：`patch_workflow` 与 `request_user_input` 注册进 `BuiltinToolRegistry`；
  模型经 AgentLoop 发起 patch 与用户决策的端到端取证。

### 2.2 非目标

- 自然语言 → patch 条目的解释编排（含三类目标判定与歧义升级提示）：Agent 侧行为，
  随后续 Agent Harness 编排里程碑。
- Workflow 定义修改与用户偏好记忆两类 patch 目标的对话编排：分别走
  `publish_workflow` 版本化与 `update_memory` 既有规则（阶段 D 起编排）。
- 成功轨迹编译、任务归纳与 DryRun 证据入库链（阶段 D）。
- App Model、Navigation Planner 与 `screen_state` 谓词求值（阶段 E；policy 切换的
  Navigate 门禁在阶段 E 解除）。
- Memory 四类组织与学习闭环（阶段 F）。
- timer 周期健康检查：无已验证消费者，随首个需要它的阶段立项（设计 §11.4）。
- Verify 步骤更高级证据声明（screen diff / 感知 / VLM）：依赖阶段 E 感知（设计 §11.4）。
- SQLite Workflow Library 存储：沿用 `RISK-2026-038` 的推迟记录。
- 连续控制步骤、子 Workflow、并行步骤、通用图。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M9 `Completed`：`WorkflowRuntime` 执行闭环、四操作 BuiltIn、DEC-019..022 契约与
  `workflow_runtime_design` v0.2 已冻结（PR #30 CI 全绿）。
- 本里程碑经维护者评审由 `Proposed` 转 `Planned`（用户指示依设计与计划推进下一步
  开发，与 M8/M9 同一授权模式）；专项设计与决策（DEC-023/024、设计 v0.3 §11）随本
  里程碑创建并冻结。

### 3.2 设计与决策依据

- [Workflow Runtime 设计](../design/workflow_runtime_design.md) §11（阶段 C 实施规范）
- [DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)、
  [DEC-024](../decisions/DEC-024-conversation-patch-execution.md)
- [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)、
  [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)、
  [DEC-018](../decisions/DEC-018-takeover-input-release-and-operation-admission.md)、
  [DEC-019](../decisions/DEC-019-workflow-ir-contract.md) 至
  [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)
- [核心公共契约与状态机](../design/core_contracts_and_state_machine.md)、
  [Mira Runtime 设计](../design/mira_runtime_design.md)（控制面单写者与 epoch 结算）

## 4. 工作项

### 4.1 契约与控制面补全

- [x] `M10-01` 冻结并实现 `begin_task_recovery` 控制面命令：任务状态表既有边
  （活跃态 → `Recovering`）的串行提交；`resume_task` 自 `Recovering` 起点递增
  epoch；同步 `Mira Runtime 设计`与 API 手册。
- [x] `M10-02` 冻结 `request_user_input` 工具 schema（DEC-024 §4）：
  `workflow_tools.hpp` 新增规格构建（arguments `{workflow_id, run_id, prompt,
  proposal?}`、结果 `{decision_id, state}`、错误信封），本地校验与 JsonSchema 子集
  同源；注册入口 `decision_tool_registrations()`。

### 4.2 策略全集运行时

- [x] `M10-03` 实现策略准入门更新与失败分流：移除两策略限制；`handle_step_failure`
  恢复链耗尽点按策略分流（Strict/DryRun `Failed`、Recoverable/AgentAssisted
  `WaitingAgent`、Interactive `WaitingUser` 决策点）；`AgentEscalation` 钩子失败驱动
  分支。
- [x] `M10-04` 实现检查点（到达驱动）：`AgentAssisted` 下声明 `AgentEscalation` 钩子
  的步骤到达即 `Running -> WaitingAgent`（不结算步骤、游标不动）；到达/已交出计数，
  每次到达至多交出一次；resume 消费交出义务后执行。
- [x] `M10-05` 实现 `WaitingAgent` 升级提交与出口：载体任务 `begin_task_recovery`；
  `resume_run` 重新观察 + `WaitingAgent -> Running` + 有界重试（计数器不重置）；
  `agent_continuation` 上下文 API。
- [x] `M10-06` 实现跨等待周期有界性：尝试/迭代/步预算累计；Agent 多轮 resume 的
  预算耗尽终态化（无重试环）。

### 4.3 Patch 执行闭环

- [x] `M10-07` 实现 patch 管线骨架：`patch_run` 公开 API 与 `patch_workflow` handler
  同源；准入矩阵（DEC-024 §1）；幂等键写前+应用双检（NoOp / `id-conflict`）；
  排队上限与 shutdown 拒绝。
- [x] `M10-08` 实现条目应用语义：步边界排水（Running）与等待态直达；run_parameters
  Set/Unset 绑定重建与未执行步骤重解析；step_arguments Set/Unset/Skip（工具绑定校验、
  跳过集结算）；execution_policy Set（成员资格 + Navigate 门禁 +
  `WorkflowPolicySwitched` + 视图有效策略更新）；未知 path 拒绝；整 patch 原子性。
- [x] `M10-09` 实现审计与回退：逐条目 `WorkflowPatchProposed`、`WorkflowPatchApplied`/
  `Rejected` 事件；`run_patch_epoch` 语义；应用前快照记录与 `rollback_run_patch`
  显式回退 patch（同一管线）。

### 4.4 决策点交互与工具闭环

- [x] `M10-10` 实现 `WaitingUser` 决策点：Interactive 失败决策（StepFailure，提议=
  跳过失败步骤）与 `request_user_input` handler（AgentPrompt，准入与 payload digest）；
  `pending_decision` 暂存 + `WorkflowDecisionRaised`；载体任务 pause 族迁移。
- [x] `M10-11` 实现 `resolve_decision`：ID+digest 匹配（不匹配拒绝并指引）；accept
  （应用提议 + 异步续跑）/ reject（按类别终态化或直接续跑）/ cancel_run；
  `WorkflowDecisionResolved`；`WaitingUser` 唯一出口收紧（`resume_run` 拒绝）；
  每 Run 单决策点；无自动超时。
- [x] `M10-12` 端到端取证：模型经 AgentLoop 对 Interactive Run 发起 `patch_workflow`
  （步边界生效）与 `request_user_input`（宿主决议回流）的端到端；`WaitingAgent` 的
  模型修复-续跑路径。
- [ ] `M10-13` 测试矩阵取证与文档同步：本机 Release/Debug/ASAN/UBSAN（TSAN 按环境
  限制记录补跑条件）、quality 门禁、installed-consumer 覆盖新公共面；总计划、设计
  文档、API 手册与 DEC-020/022 注记同步后关闭里程碑。

## 5. Executor 路由与关闭表（细化设计 §7/§11）

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| 决议续跑（resolve_decision accept/reject、AgentPrompt reject） | `submit_auto` 普通有限任务，与 resume 共用 `max_concurrent_async_drives` | Workflow Runtime（Run 表内 future） | future 由 `wait_run`/shutdown 消费；异常转失败结算与诊断 |
| `begin_task_recovery` / 决策点 pause 族迁移 | 既有 `MiraRuntime` 串行控制面 | Workflow Runtime（CommandHandle） | 有界等待回执；失败转诊断并保持两层视图一致 |
| patch 应用（等待态直达） | 调用线程（控制面提交，互斥下串行） | 无新任务 | 确定性拒绝码；事件锁外发射 |
| patch 排水（Running 步边界） | 驱动任务内联 | 驱动任务 | 同上；正在执行步骤不受影响 |

理由：阶段 C 不引入新任务类别；决议续跑复用 M9 的异步驱动容量（防 worker 占用饥饿
控制面），patch 应用是控制面提交（单写者扩展到 patch 平面）。

关闭顺序不变（M9 §5）：`WorkflowRuntime::shutdown()`（含决策点与排队 patch 的生产者
拒绝）→ `MiraRuntime` 关闭 → 宿主非 worker 线程 `executor.shutdown(true)`。

## 6. 风险与阻塞

- `RISK-2026-042`：检查点复用 `AgentEscalation` 钩子使钩子语义按策略分流（失败驱动
  vs 到达驱动），读者可能误判触发时机。Owner：Mira Maintainers。缓解：DEC-023 §2、
  设计 §4.2 补注与 API 手册显式披露；运行时测试锁定两种时机互斥。
- `RISK-2026-043`：patch 排水与步执行的竞态——排队 patch 在驱动线程排水、等待态
  patch 在调用线程应用，二者与状态转换共享同一互斥。Owner：Mira Maintainers。缓解：
  全部有效状态变更在互斥下原子完成、事件锁外发射；竞态测试（Running 提交 vs 边界
  排水）锁定顺序可观察结果。
- `RISK-2026-044`：Agent 未装配时 `WaitingAgent` Run 长期等待。Owner：Mira
  Maintainers。缓解：等待是安全状态（载体 `Recovering`，无自主动作）；`cancel_run`
  出口常在；文档披露宿主装配义务。
- `RISK-2026-038`（沿袭）：库与 Run 表为进程内投影，跨进程续跑需持久化载体；patch
  快照与决策点同样为进程内状态。
- `RISK-2026-039`（沿袭）：中断副作用步骤判定仍只有结构化谓词证据层（阶段 E 扩展）。

## 7. 测试与退出条件

- [x] `M10-01` 至 `M10-12` 全部完成并有可复现验证记录。
- [x] 策略矩阵：三种新策略的失败升级、钩子两时机、检查点到访计数（含循环回归）、
  跨 `WaitingAgent` 计数器累计与预算终态化、`Strict`/`DryRun` 全路径无回归。
- [x] 状态一致性：`WaitingUser -> Paused`、`WaitingAgent -> Recovering`（含
  `begin_task_recovery` 拒绝路径）、resume 出口、Task 终态 ⇒ Run 终态不变量扩展到
  新路径；takeover 与等待态交互。
- [x] Patch：幂等双检、三事件序列、epoch 语义、边界生效（执行中步骤不受影响）、参数
  重建（Unset 回落/必选拒绝/重解析/不回溯）、Skip 结算、policy 门禁（成员资格 +
  Navigate）、未知 path 与非法条目拒绝、整 patch 原子性、回退（含链式）、排队上限、
  准入矩阵全格。
- [x] 决策点：两类来源、digest 匹配/不匹配、accept/reject/cancel_run 全路径、
  `WaitingUser` 唯一出口、单决策点上限、无自动超时（等待可被取消）。
- [x] 工具闭环：`patch_workflow`/`request_user_input` 经注册表执行与错误信封；模型
  发起端到端（`M10-12`）。
- [ ] 门禁：ASAN/UBSAN 全绿；TSAN 在本机限制下按 M9 模式取证；quality
  （clang-format、docs、sbom、platform-boundary）通过；Windows/Android 构建组合与
  clang-tidy 由 PR CI 补验全绿；installed-consumer 覆盖新公共面。
- [x] 总计划第 4/5 节、`workflow_runtime_design`、API 手册、DEC-020/022（阶段 C
  注记）与本文件同步。

## 8. 验证记录

2026-09-08：依据 M9 退出条件与总计划阶段 C 规则创建本里程碑（`Proposed`）：先完成
专项决策（DEC-023、DEC-024）与设计更新（`workflow_runtime_design` v0.3 §11），再创建
本文件。同日经维护者评审（用户指示依设计与计划推进下一步开发，与 M8/M9 同一授权
模式）转 `Planned` 并进入实施（`In Progress`）。

2026-09-08：实现与本地验证（Ubuntu 24.04，x86_64，g++ 13.3.0，CMake 3.28.3，Unix
Makefiles；本机无 clang/clang-tidy，由 PR CI quality job 补验；`clang-format` 使用
miniconda 发行版）。

- 实现：`MiraRuntime::begin_task_recovery`（控制面 `Recovering` 进入路径；resume 自
  `Recovering` 递增 epoch）；`workflow_tools` 新增 `request_user_input` 规格与校验、
  `parse_workflow_patch_entries` 共享解码；`WorkflowRuntime` 新增失败分流/检查点（到达
  计数）/升级预算、patch 管线（准入矩阵、幂等双检、步边界排水、绑定重建、Skip、策略
  切换门禁、三事件审计、快照回退）、决策点（两类来源、`resolve_decision` 唯一出口、
  `request_user_input` handler）与 `agent_continuation`；`operation_tool_registrations`
  注册全部五操作（含 `patch_workflow`）。
- 实现中的契约对齐：驱动启动将承载任务自 `Idle` 激活至 `Observing`（对齐 DEC-020 §2
  映射，M9 遗留的载体态空缺因此闭合）；`WaitingUser` 的 `Failed` 结算经合法边
  `WaitingUser -> Running` 两步提交（冻结表无直连边，DEC-024 §5 注记）；被暂停截断的
  步骤若被 patch 跳过，恢复时按 `Skipped` 结算而非重执行（DEC-024 §3 生效点优先）。
- M9 回归修订：策略范围测试改为全策略准入断言（阶段 B 两策略门按 DEC-023 移除）；
  四操作注册数断言改为 5。
- 新增测试：`mira_m10_policy_test`（失败分流、钩子两时机、检查点到访与循环回归、
  Interactive 失败决策 accept/reject/cancel、digest 不匹配拒绝、升级预算终态化、
  Strict/DryRun 不升级、等待态载体与取消幂等）、`mira_m10_patch_test`（准入矩阵、
  幂等 NoOp/冲突、排队边界生效与在途步骤不受影响、参数重建与 Unset 回落、Skip 与
  策略切换门禁（成员资格 + Navigate）、WaitingAgent 修复 patch、三事件审计与 epoch
  语义、快照回退、排队上限）、`mira_m10_interaction_test`（`request_user_input` 注册表
  闭环与单决策点、AgentPrompt reject 续跑、准入与 schema 负向、`patch_workflow` 工具
  闭环与 NoOp 重放、模型经 AgentLoop 发起 patch 与决策端到端）。
- 安装包：`mira_installed_consumer` 增加 M10 用例（Interactive 失败 → `WaitingUser` →
  patch → accept 决议 → 续跑完成）。
- 结果：Release/Debug/ASAN/UBSAN 各 57/57 通过；TSAN（`setarch x86_64 -R`，mbedtls
  portable 按配置禁用）56/56 通过；`format-check`、`docs-check`、`sbom-check`、
  `platform-boundary-check`、`consumer-check` 通过。
- 限制：Windows/Android 构建组合与 clang-tidy 由 PR CI 补验后随 `M10-13` 回填（见下
  条）；阶段 D–F 不在本轮（范围外）。
- 同步：DEC-020/022（阶段 C 补注）、DEC-023/024（本轮冻结）、
  `workflow_runtime_design`（v0.3 §11 与路由表）、`mira_runtime_design`
  （`begin_task_recovery`）、API 手册（workflow-contracts 增 M10 节并修订 M9 描述）、
  总计划（§4/§5）。

2026-09-08：PR [#31](https://github.com/Linductor-alkaid/mira/pull/31) CI 全绿（head
`3757629`，push pipeline run
[`34253314526`](https://github.com/Linductor-alkaid/mira/actions/runs/34253314526)、
pull_request pipeline run
[`34253318615`](https://github.com/Linductor-alkaid/mira/actions/runs/34253318615)）：
Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android arm64-v8a 与
x86_64（NDK）、ASAN/UBSAN/TSAN 与 quality（clang-tidy 18 + clang-format + docs/sbom/
platform-boundary 检查）全部 24 项通过，补齐本机缺失的 clang-tidy 与跨平台验证。首轮
quality 在 `workflow_runtime.cpp` 报 1 处 `performance-move-const-arg`（对
trivially-copyable 的 `WorkflowPatchOutcome` 使用无效 `std::move`），去除后复验通过；
语义不变（本地全量测试复跑 57/57）。PR 已合并（merge `8be7707`）。`M10-01` 至
`M10-13` 全部完成，退出条件逐项满足，本里程碑关闭（`Completed`）。阶段 D（成功轨迹
编译与任务归纳）里程碑可依据 `workflow_runtime_design` 与 DEC-014 创建并进入
`Planned`。

2026-09-09：阶段 F 后状态审计重新打开 `M10-13` 与跨平台退出条件，
里程碑恢复 `In Progress`。`BUG-20260909-001`：Android CI 显式目标列表未包含
`mira_workflow`，且已有 consumer 不依赖该目标，历史 job 全绿不足以支撑本模块
Android 构建声明。已交付功能项及其他历史证据保留；这是验收覆盖缺口，尚无 Android
编译失败证据。负责人 Mira Maintainers；按
[后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-22` 补齐两 ABI
实际编译与安装消费链接证据后，逐项复核并关闭。本轮本地构建因未初始化子模块未能
复跑，环境与补跑条件见后续计划第 5 节。
