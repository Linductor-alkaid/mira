# Workflow 契约

> 适用头文件：`workflow_ir.hpp`、`workflow_run.hpp`、`workflow_versioning.hpp`、
> `workflow_events.hpp`、`workflow_tools.hpp`、`workflow_compiler.hpp`、
> `workflow_navigation.hpp`、`workflow_learning.hpp`、`workflow_runtime.hpp`
> （CMake 目标 `Mira::workflow`）
> 状态：Active（M8 契约层；M9 起含执行闭环 `WorkflowRuntime`；M11 起含编译层；
> M12 起含导航层；M13 起含学习层）
> 依据：[DEC-019](../decisions/DEC-019-workflow-ir-contract.md)、
> [DEC-020](../decisions/DEC-020-workflow-run-lifecycle.md)、
> [DEC-021](../decisions/DEC-021-workflow-tool-channel.md)、
> [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)、
> [DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)、
> [DEC-024](../decisions/DEC-024-conversation-patch-execution.md)、
> [DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md)、
> [DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md)、
> [DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)、
> [DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md)、
> [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
> [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)、
> [Workflow Runtime 设计](../design/workflow_runtime_design.md)

`Mira::workflow` 是 Workflow 双路径的契约层、（M9 起）执行层、（M11 起）编译层、
（M12 起）导航层与（M13 起）学习层：Workflow IR、WorkflowRun 状态视图、资产版本化、
事件载荷、Workflow 操作的 Tool 规格、`WorkflowRuntime` 执行闭环、成功轨迹编译/任务
归纳、App Model 导航与 Memory 学习闭环。
契约函数全部为**同步纯函数**；`WorkflowRuntime` 的 Executor 路由与关闭顺序见
[workflow_runtime_design](../design/workflow_runtime_design.md) 第 7/8 节。

## Workflow IR（`workflow_ir.hpp`）

- `WorkflowDefinition`：参数 Schema（`WorkflowParameterSpec`，类型
  `string|integer|number|boolean`、约束与默认值）、有序步骤（`WorkflowStep`，种类
  `ToolCall|Navigate|Verify|Control`）、默认与允许执行策略（`WorkflowPolicy`）。
- v1 控制流是「有序序列 + 前置条件跳过 + 受限后向跳转」：`Control` 步骤只能跳到序列中
  更早且标注 `loop_head` 的步骤，且携带 1..64 的 `max_iterations`；通用图、子 Workflow
  步骤是扩展位，未承诺。
- 步骤实参中的参数引用使用唯一封闭形式 `{"$param": "<name>"}`；`resolve_step_arguments`
  做纯函数替换，未声明引用与未知成员 fail closed。
- 序列化与解码：`workflow_definition_to_json` / `workflow_definition_from_json` /
  `parse_workflow_definition`。**未知字段与版本不匹配 fail closed**（IR 是可执行内容，
  不适用宽松跳过）；reader 支持的 minor 以内才可读，minor 提升（新增字段）要求 reader
  升级。容量上限由 `WorkflowLimits`（`RULE-08`，可收紧不可放宽）约束：文档 256 KiB、
  深度 16、步骤 256、参数 64 等。
- `bind_workflow_parameters`：参数绑定为纯函数，错误码是稳定
  `WorkflowBindError`（`UnknownParameter`/`MissingRequired`/`TypeMismatch`/
  `ConstraintViolated`/`InvalidDefault`）；声明的默认值必须通过自身约束。
- 谓词 DSL（`WorkflowPredicate`）：`{"signal": "<kind>:<ref>", "op": ..., "value": ...}`，
  signal 种类封闭为 `run_parameter|step_result|screen_state`（`screen_state` 在阶段 E 前
  不可求值）；`evaluate_workflow_predicate` 对缺信号与类型不匹配返回 `NotEvaluable`
  （fail closed），`Exists` 直接回答存在性。
- 内容寻址：`workflow_definition_digest` 返回 canonical JSON digest；版本记录与 Run
  引用 digest，不内联定义。
- `validate_workflow_definition`（M9）：对结构体定义执行与 JSON 解码同源的结构校验
  （经规范化往返实现）；宿主手工构建的定义与解码产物流经同一 fail-closed 检查。
- ToolCall 工具绑定（M9 冻结）：`arguments` 必须为对象且携带保留成员 `"tool"`
  （字符串，BuiltIn wire 名），其余成员构成工具输入并经注册表 schema 校验；与 Navigate
  的 `arguments["target"]` 同属按步骤种类的实参约定。

## WorkflowRun 视图与转换（`workflow_run.hpp`）

- `WorkflowRunState`：`Created/Running/Paused/WaitingUser/WaitingAgent/Completed/Failed/
  Cancelled`；`valid_workflow_run_transition` 是冻结转换表（DEC-020 §1），终态无出边。
- `apply_workflow_run_transition`：纯函数应用转换；终态重复提交同一终态是合法
  `NoOpTerminal`（epoch 不变），其余一律 `Rejected`（`InvalidState`）。合法转换推进
  `run_epoch`，离开 `WaitingUser` 时清空挂起决策点。
- `admit_workflow_run_completion`：迟到完成隔离（`W-01`/`RULE-03`）——终态 Run 对任何
  epoch 都结算 `Stale`，活动 Run 只接受当前 epoch。
- `task_state_for_run_state` / `run_task_state_compatible`：Run↔Task 两层视图映射
  （DEC-020 §2）：`WaitingUser → Paused`（区别是挂起原因）、`WaitingAgent → Recovering`、
  `Running` 覆盖五个执行相位加控制暂态（`Pausing/Cancelling/TakeoverSettling`）。
  `SuspendedForTakeover` 是 `Paused/WaitingUser` 在接管场景的合法承载态。
- `validate_workflow_policy_compatibility`：Run 创建时的策略门禁——策略必须在
  `allowed_policies` 内，`AgentEscalation` 恢复钩子要求 agent-capable 策略。

## 资产版本化（`workflow_versioning.hpp`）

- `WorkflowVersionRecord`：`Who/Why/What Changed/Validation Result/Timestamp`；`What
  Changed` 以内容 digest + 父 digest 链表达。
- `WorkflowVersionHistory` 只追加：版本必须严格递增、父 digest 必须链到当前头、首条父
  digest 为 nil；已验证记录必须携带证据 digest，`NotValidated` 记录不得携带。
- `resolve_workflow_version`：Run 回放与续跑按 digest 解析创建时版本（`W-03`），
  未知 digest fail closed。
- `workflow_version_is_runnable` / `latest_runnable_workflow_version`：只有
  `DryRunPassed`/`Validated` 的版本可被 Run 引用（`W-04`）。

## 事件载荷（`workflow_events.hpp`）

- 十三类 `mira.workflow.*.v1` 载荷（`to_event_payload`/`parse_*`）：RunStarted/StepStarted/
  StepSettled/RunSettled（Critical），patch 三事件、PolicySwitched、决策点两事件与
  publish 门禁三事件（State）。
- 解析 fail closed：schema 字符串不匹配、未知字段、非法 ID/digest/枚举、非终态的
  RunSettled 全部拒绝；摘要字段受 2 KiB 上限。
- 脱敏（DEC-022 §5）：载荷只含 ID、digest、枚举、原因码与受限摘要；参数明文与用户文本
  以 digest 或 `ArtifactRef` 引用，不内联。
- `is_workflow_event_type`：OfflineReplay 识别 workflow 事件；回放只重建投影，不产生
  副作用（`W-08`）。

## Workflow 操作 Tool 规格（`workflow_tools.hpp`）

- `workflow_operation_specs()`：五个操作（`run/patch/pause/resume/cancel_workflow`）的
  参数、结果、宿主 details 与统一错误信封 schema（`mira.workflow.tool.v1`）；只有
  `run_workflow` 标记 `has_side_effects`。
- `validate_workflow_operation`：本地 fail-closed 校验（JSON Schema 子集 + 语义检查：
  ID/digest 形状、枚举成员、`run_id` 必填性、patch 条目规则）。**只校验、不执行**；
  策略集合成员资格与参数绑定是 Runtime 检查（阶段 B）。
- patch 条目（`WorkflowPatchEntry`）：目标 `run_parameters|step_arguments|
  execution_policy` 与操作 `set|unset|skip` 的封闭组合；`execution_policy` 仅允许
  `set` 且 path 固定为 `policy`。幂等键为 `patch_id` + `workflow_patch_digest`
  （条目顺序敏感）。
- `workflow_operation_error_envelope`：统一错误信封 `{code, domain, domain_code,
  retryable, safe_message, operation_id}`；`safe_message` 必须已脱敏。
- `parse_workflow_patch_entries`：patch 条目数组的共享解码（`patch_workflow` 的
  `patch_entries` 与 `request_user_input` 的 `proposal` 同形）；未知形状 fail closed。
- `workflow_request_user_input_spec()`（M10，DEC-024 §4）：`request_user_input` 工具
  规格——arguments `{workflow_id, run_id, prompt, proposal?}`（prompt 为已脱敏摘要，
  ≤2 KiB；proposal 为可选 patch 条目数组，≤32 条），结果 `{decision_id,
  state:"waiting_user"}`；`validate_request_user_input_arguments` 本地校验（schema 子集
  + 条目规则），Run 准入（Interactive、非终态、无未决议决策点）是 Runtime 检查。

## WorkflowRuntime 执行闭环（`workflow_runtime.hpp`，M9）

- 构造绑定一个宿主 Executor、一个 `MiraRuntime` 会话与 `IEnvironment`；每个 Run 由一个
  承载 Task（经控制面创建）支撑，Run 视图变更全部经冻结转换表提交。Run 表容量、整 Run
  步执行预算与并发异步驱动上限由 `WorkflowRuntimeConfig` 约束（`RULE-08`）。
- `create_run`（宿主直达，宿主是信任边界）与 `create_run(workflow_id, ir_digest, ...)`
  （库路径，仅 `DryRunPassed`/`Validated` 版本可运行，`W-03`/`W-04`）。准入 fail closed：
  `allowed_policies` 成员资格、非 agent-capable 策略下的 `AgentEscalation` 钩子拒绝、
  dispatching 策略下 Navigate 步骤与缺失验证声明的副作用步骤拒绝、参数绑定确定性
  错误码透传（M9 的两策略限制已随 M10 移除，五种策略均可执行）。
- `execute_run`（调用线程同步驱动）与 `start_run`（异步驱动，受并发上限约束）；
  `wait_run` 有界等待当前驱动的结果。DryRun 不派发任何工具或环境动作，不可求值验证按
  「已规划、未宣称验证」结算并在结果中计数披露（`RULE-10`）。
- `pause_run` 在步边界安全点收敛：被截断步骤按 `Stale` 结算且游标不推进；
  `resume_run` 重新观察后从游标继续（无副作用工具或 DryRun 重执行；副作用工具复评验证
  谓词，不可恢复时按 `ExecutionUncertain` 失败，禁止盲目重发；`WaitingAgent` 经同一
  路径续跑，`WaitingUser` 自 M10 起拒绝并指引 `resolve_decision`）；`cancel_run` 幂等，
  迟到完成隔离为 `Stale`。Run/Task 终态保持一致（`run_task_state_compatible`）。
- `operation_tool_registrations()` 返回 run/pause/resume/cancel 四个 BuiltIn handler
  （`patch_workflow` 执行属阶段 C）；handler 捕获 `this`，Runtime 必须覆盖注册条目
  生命周期。
- `shutdown()`：停止生产者 → 经控制面取消活动 Run → 有界清算驱动 future → 返回
  `WorkflowShutdownReport`（含事件发射/任务结算失败计数）。不自行关闭 Executor；
  宿主关闭顺序：`WorkflowRuntime::shutdown` → `MiraRuntime` 停止 → Executor
  `shutdown(true)`。

## WorkflowRuntime 介入与策略全集（M10，DEC-023/024）

- 策略准入：五种策略全部可执行（`allowed_policies` 成员资格与 agent-capable 钩子校验
  沿用契约函数）。失败分流（恢复链耗尽点，步边界）：`Strict`/`DryRun` → `Failed`；
  `Recoverable`/`AgentAssisted` → `WaitingAgent`（载体任务经 `MiraRuntime::
  begin_task_recovery` 进入 `Recovering`）；`Interactive` → `WaitingUser` 决策点（载体
  走 pause 族）。升级预算 `max_escalations_per_run`（默认 32）耗尽按 `Failed`
  终态化（`escalation-budget-exceeded`）；检查点交出不消耗该预算。
- 检查点（`AgentAssisted`）：声明 `AgentEscalation` 钩子的步骤即检查点，到达驱动——
  每次到达先 `WaitingAgent` 交出再执行；「到达」按游标进入计（全新启动或驱动内移动），
  等待恢复消费既有到达。`AgentEscalation` 钩子在 agent-capable 策略下失败时立即升级。
- `agent_continuation(run_id)`：仅 `WaitingAgent`；返回当前步（ID/种类/尝试数）、有效
  参数（宿主边界明文，进模型上下文走既有脱敏）、步历史、失败原因与 pending 决策。
- Patch 平面：`patch_run`（宿主直达）与 `patch_workflow` handler 同源。准入矩阵：
  `Interactive` 在全部边界态接受；agent-capable 策略在 `WaitingAgent` 接受（修复）；
  其余拒绝（`policy-not-interactive`）；终态拒绝（幂等重放除外）。`Running` Run 的
  patch 排队至步边界（上限 `max_pending_patches_per_run`，默认 16）；等待态直达应用。
- Patch 语义：整 patch 原子生效（任一条目校验失败全部拒绝，`run_patch_epoch` 每
  patch +1）；幂等键 `patch_id`+digest 写前与应用双检（同 ID 同 digest NoOp、异
  digest `id-conflict`）；参数 Set/Unset 以绑定纯函数整体重建（Unset 回落默认值，
  必选缺失拒绝），未执行步骤重解析、已结算不回溯；`step_arguments` Set 经 `$param`
  解析与工具绑定校验、Unset 清除该步覆盖（含跳过）、Skip 入跳过集；`execution_policy`
  Set 校验 `allowed_policies` 成员资格与 Navigate 剩余门禁（含 Navigate 步骤的定义
  拒绝切换到 dispatching 策略）并发出 `WorkflowPolicySwitched`。审计：逐条目
  `WorkflowPatchProposed` → `WorkflowPatchApplied`/`Rejected`（原因码稳定命名）。
- 回退：`rollback_run_patch(run_id, target_patch_id)` 从应用前快照构造差异条目，以新
  `patch_id` 走同一管线（无隐式快照恢复；无差异时报 `InvalidArgument`）。
- 决策点：`StepFailure`（Interactive 失败，提议=跳过失败步骤）与 `AgentPrompt`
  （`request_user_input`）两类；`pending_decision_request` 读取当前决策（含类别、
  提议与 payload digest）；`resolve_decision(run_id, decision_id, payload_digest,
  resolution)` 按 ID+digest 匹配（不匹配 `decision-mismatch` 拒绝）——accept 应用提议
  并续跑、reject 按类别终态化（StepFailure 经 `Running` 边转 `Failed`）或直接续跑、
  cancel_run 取消；每 Run 至多一个 pending 决策；无自动超时（宿主可取消）。
  `WaitingUser` 的唯一出口是决议：`resume_run` 对其拒绝并指引 `resolve_decision`。
- `decision_tool_registrations()` 返回 `request_user_input` 的 BuiltIn handler
  （捕获 `this`，生命周期约束同五操作 handler）；`operation_tool_registrations()`
  自 M10 起返回全部五个操作（含 `patch_workflow`）。

## 轨迹编译与任务归纳（`workflow_compiler.hpp`，M11，DEC-025/026）

- `WorkflowTrajectory`：一次**真实成功执行**的结构化快照——溯源三元组（workflow、
  钉住版本 digest、来源 Run）、有效参数表、有效策略、生效步骤序列（字面量生效实参 +
  含 `$param` 的原始实参作为归纳 provenance）。宿主可直接构造（Agent 工具调用路径）。
- `WorkflowRuntime::capture_trajectory(run_id)`：仅对 `Completed` 且实际派发副作用的 Run
  开放（DryRun 完成的 Run 从未真实执行，`RULE-10` 诚实声明下 fail closed）；
  跳过步剔除、patch 覆盖后的生效态入快照。确定性错误码
  `WorkflowCompileError::CaptureNotCompleted/CaptureNotExecuted`。
- `compile_workflow(trajectory, options)`：字面量编译纯函数——生效实参原样落步、
  参数默认值固化为观测值（`required` 转可选）、谓词/钩子/循环结构保留、
  `allowed_policies` 沿用来源集合、跳过步的 `jump_to` fail closed
  （`CompileJumpTargetSkipped`）、产物过 `validate_workflow_definition` 同源校验；
  同输入同 digest（确定性）。产物是**宿主可编辑草稿**。
- `WorkflowParameterCandidate` / `induce_parameters(trajectories)`：任务归纳提议。
  结构 diff 要求 2..16 条同骨架轨迹（步数/种类/名称/实参树同形，递归校验）；类型相同、
  值不同的标量叶成候选，常量叶保持字面量；命名优先取 `$param` provenance（多步引用
  同名合并），否则按稳定路径序生成 `param_N`（避开已用名）。类型不一致
  （`InductTypeMismatch`）与骨架不同形（`InductSkeletonMismatch`）fail closed。
- `compile_workflow(trajectory, options, candidates)`：参数化编译——候选叶重写为
  `{"$param": name}`（复用既有绑定纯函数），新增参数规格按观测类型推断
  （`required=false`，默认值取 anchor 观测值）；provenance 候选复用来源参数规格，
  名字冲突（`InductNameConflict`）、保留成员 `"tool"`（`InductReservedMember`）、
  非标量叶（`InductLeafNotScalar`）与超上限（`InductLimitExceeded`）fail closed。
  候选与观测值是宿主侧草稿数据，不入 IR、版本记录与事件（归纳是提议而非事实）。
- `WorkflowRuntime::publish_validated(definition, actor, reason, source_run_id?)`：
  入库门禁——结构校验 → 空参数 DryRun Run（调用线程同步驱动，草稿必须默认值完备）→
  要求 `Completed` → **内容派生**证据摘要（ir digest、逐步结算、unevaluable 计数；同
  定义 + 同 DryRun 结算序列 ⇒ 同证据）→ 幂等（head 同容同证 NoOp 返回
  `WorkflowPublishOutcome{idempotent=true}`）→ 追加 `DryRunPassed` + 证据的版本记录。
  门禁失败**不触碰库**（head 不变、无 Rejected 记录），仅以事件与错误可见；该 API 是
  宿主 API，模型不可直达（`W-04`）。`publish_workflow` 维持宿主信任边界原始路径
  （`NotValidated` 缺省、无事件）。
- Publish 审计事件：`WorkflowPublishProposed`（workflow_id、ir_digest、source_run_id）/
  `WorkflowPublishApplied`（含 evidence 与 dry_run_id）/`WorkflowPublishRejected`
  （机器可读原因码 `validation-failed|gate-run-failed|publish-dryrun-failed|
  append-failed`）；会话级（无 Run 上下文）。源 Run 绑定与门禁 Run 绑定经事件审计
  承载，证据摘要保持内容派生。
- Executor 路由（设计 §7）：捕获 = 调用线程互斥下快照；编译/归纳 = 调用线程纯函数；
  门禁驱动 = 复用 `execute_run` 宿主同步路径（不得在持有宿主关键锁的上下文调用）。

## App Model 与导航（`workflow_navigation.hpp`，M12，DEC-027/028）

- `AppModel`：UI 状态图公共契约（v1，内容寻址 digest）——状态节点
  （`AppModelState`：id + 层次描述 page/modal/context + summary + 置信度）与迁移边
  （`AppModelTransition`：id、from/to、action（保留成员 `"tool"`，与 ToolCall 步骤同一
  约定）、可选 guard（v1 谓词 DSL 复用）、代价向量 `NavigationCosts`、置信度）。
  `app_model_to_json/from_json/parse` 往返无损；未知字段、悬垂引用、重复 ID、越界数值
  与 `AppModelLimits` 上限 fail closed（`AppModelError` 确定性错误码）；
  `validate_app_model` 与解码同源（结构构造模型同受检）；`app_model_digest` 同内容同
  digest。
- `ConfidenceRecord` + 纯函数（DEC-027 §2）：`note_transition_outcome`（拉普拉斯平滑，
  成功/失败分别单调推高/压低）、`apply_confidence_decay`（自 `last_verified_ms` 起指数
  衰减，单调不增、`Δt<=0` 或零半衰期为 NoOp、不动计数与时间戳）、`needs_exploration`
  （阈值判定）。时间由调用方传入，同输入同输出（投影可从事件重建，`W-03`）。
  `source` 是封闭集 `host|agent|trajectory` 的溯源标注，不构成授权（`RULE-09`）。
- `plan_navigation(model, from, to, profile, context, options)`：确定性 Dijkstra 纯函数。
  代价 = `NavigationCostProfile` 权重线性和（权重是配置，缺省全 1 为**暂定默认值**，
  未经目标平台校准，`RULE-10`）；guard 以谓词上下文求值，`NotSatisfied` 计
  `guards_blocked`、`NotEvaluable` 计 `guards_unevaluable`（均使边不可用但分开披露）；
  等代价按边序列 `(edge_cost, edge_id, to_state)` 字典序打破平局；`agent_required` 边
  缺省不可用（`allow_agent_edges` 为宿主保留口）；`max_edge_evaluations`（缺省 4096）
  与 `max_path_edges`（缺省 64）预算（`RULE-08`）；`NavigationError` 区分未知端点、
  同状态空路径、无路径（消息携带 guard 计数，可分辨图不连通与 guard 全挡）与预算
  超限；`NavigationPlan.plan_digest` 为边 ID 序列的规范化 digest。
- `ScreenStateProvider` / `ScreenStateSnapshot`（DEC-027 §3）：宿主侧 UI 状态识别
  边界——宿主决定如何得出状态名（Accessibility/OCR/VLM/人），Core 只消费快照
  `{state_id, observed_at_ms}`；回调在驱动/调用线程同步执行，**必须廉价非阻塞**；
  缺席或返回空时一切依赖当前状态的判定 fail closed。
- `WorkflowRuntime::set_navigation_context(model, provider)` / `set_app_model(model)` /
  `app_model_snapshot()`：安装/重装（如衰减后内容）/读回置信度演化的投影。模型必须
  过 `validate_app_model`（fail closed，`Result<void>`）。
- Navigate 步骤解析（DEC-028 §3）：准入——`arguments["target"]` 必须解析为非空字符串
  （`NavigateTargetInvalid`）；派发策略额外要求导航上下文已安装（否则维持 M9 的
  `navigate-unresolvable` 拒绝；policy patch 切入派发策略的门禁同条件放松）。执行——
  读屏（缺席 `navigate-no-screen-state`）→ 目标已声明
  （`navigate-target-unknown`）→ 规划（失败原因码透出 `navigate-no-path`/
  `navigate-budget-exceeded` 等）→ 逐边经工具注册表派发（同 ToolCall 通道与 schema
  校验、计入 Run 步预算、同一取消探针）→ 每边后读屏到达验证
  （`navigate-arrival-unverified`，禁盲目重发边动作，恢复钩子照常）→ 步级
  verification 谓词在到达后求值。置信度按边成败回写投影（纯函数，互斥下更新、事件
  锁外发射）。
- `screen_state` 谓词绑定（设计 §4.1 兑现）：快照存在时谓词上下文注入
  `screen_state:<state_id>`（true，仅当前状态）与 `screen_state:current`（ID 字符串）；
  Provider 缺席时不注入任何条目，全部 `screen_state` 谓词维持 `NotEvaluable`
  （fail closed 不变）。
- DryRun 语义：不派发边动作、不回写置信度；有导航上下文时执行**真实规划**并以
  `WorkflowNavigationPlanned` 留痕，规划失败即步失败（`publish_validated` 门禁因此对
  导航可达性有约束力）；无上下文维持 M9 形状规划结算（`planned navigation`）。
- 导航事件（DEC-028 §4，v1 闭集扩展两员，State 类）：
  `WorkflowNavigationPlanned`（run/step、from/to、edge_count、plan_digest、total_cost、
  guards_blocked/unevaluable；DryRun 与派发策略都发）与
  `WorkflowNavigationObserved`（run/step、transition_id、from/to、success、回写后
  confidence；仅真实派发后发）。OfflineReplay 识别且无副作用。
- `WorkflowRuntimeConfig` 新增：`nav_cost_profile`（权重）、`nav_max_edge_evaluations`、
  `nav_max_path_edges`（预算）。
- Executor 路由（设计 §7）：规划 = 调用线程纯函数；读屏 = 宿主回调（同步、无锁调用，
  宿主保证非阻塞）；边动作派发 = 复用步骤执行 `submit_auto` 通道（计入步预算）；
  置信度回写 = 调用线程互斥下投影更新。

## Memory 四类组织与学习闭环（`workflow_learning.hpp`，M13，DEC-029/030）

- `MemoryDomain`（封闭集 `environment|user|procedural|episodic`）：`MemoryKind` 之上的
  确定性全映射 `memory_domain_of`（与 DEC-014 §10.2 逐行一致）与逆视图
  `memory_kinds_of_domain`（供 `MemoryQuery::kinds` 域级过滤）。组织视图而非新存储分层：
  不新增表、不改变 `IMemory` 与 M4 的 scope/ACL/审批规则；域不承载授权（`RULE-09`）。
- 学习契约（DEC-029 §2–§4）：`WorkflowEpisodeRecord`（终态 Run 的结构化情景：身份、
  钉住 digest、有效策略、outcome、失败签名、升级/检查点计数）、
  `WorkflowRecoveryLesson`（失败签名 + 恢复动作序列 + 恢复完成；只在观察到成功恢复后
  记录）与 `WorkflowFailureSignature`（消毒标识符：`[A-Za-z0-9._:-]`、≤128，超集字符
  fail closed）。JSON 往返无损；未知字段/版本/枚举、空 ID、负计数与
  `WorkflowLearningLimits` 上限（`RULE-08`）全部 fail closed（`WorkflowLearningError`
  确定性错误码）；内容寻址 digest；确定性 ID 派生（SHA-256 前 16 字节，mutation-id
  幂等友好）。
- 纯转换：`episode_to_memory_record` / `recovery_lesson_to_memory_record` 产出
  `kind=Episode|RecoveryLesson`、`verification=Verified`、`confidence=1.0`（「发生过」，
  不是「有用」）、事件 provenance 的 `MemoryRecord`（时间由调用方传入）；
  `recovery_lesson_from_record` 只接受 canonical statement 并对其他文本 fail closed。
  statement 只含 ID/digest/枚举/原因码/计数器（DEC-022 §5 同源脱敏面）。
- `failure_retrieval_query(signature, scope, limits)`：确定性查询构建——kinds 恒为
  `{Episode, RecoveryLesson}`、exact_terms 为签名标识、无 embedding 腿；检索条数/
  token/deadline 缺省 8/1024/250ms（**暂定默认值**，未经目标语料校准，`RULE-10`）。
- `WorkflowRuntime::set_learning_context(memory, scope, limits)`（DEC-030 §1）：安装即
  启用学习闭环；scope 拒绝 `User`（Episode/Lesson 是自动运行经验，User 域保持人工审批
  通道）；未安装时一切学习路径 NoOp（M12 行为不变）。
- 结算期 Episode 记录（DEC-030 §2）：非 DryRun 终态在结算线程同步写入（溯源锚点 =
  `WorkflowRunSettled` 事件 ID；无事件存储则无可锚定记录，按设计跳过）；DryRun 跳过
  （设计行为，无事件）；写失败不影响已结算终态，转诊断计数器与
  `WorkflowEpisodeRecorded`（failed + 原因码）。
- 失败检索（DEC-030 §3）：仅**失败驱动升级**（`escalate_waiting_agent`）在驱动线程
  同步查询（互斥锁外），结果存 RunRecord 并经 `agent_continuation().relevant_lessons`
  透出（每项 kind/statement/confidence，statement 为 canonical 学习契约 JSON、按构造
  脱敏）；检查点让渡不检索；查询失败降级为空结果 + 诊断计数器（升级路径不受阻）；
  结果条数/token/deadline 受配置约束（`RULE-08`）。
- `WorkflowRuntime::record_recovery_lesson(run_id)`（DEC-030 §4）：宿主专用（不注册为
  模型工具，`W-04`）；准入 = `Completed` + 非 DryRun + 失败升级计数 > 0 + 学习上下文
  已安装；恢复动作 = 最后一次失败升级后应用的 patch（patch_id/digest/目标摘要，无
  参数值），无 patch 时 `resumed_without_patch`；同 run mutation-id 幂等；失败对调用方
  可见（`Result` 错误）+ `WorkflowLessonRecorded` 审计。
- 学习事件（DEC-030 §5，v1 闭集扩展两员，State 类）：`WorkflowEpisodeRecorded` 与
  `WorkflowLessonRecorded`（run/workflow、digest、outcome=recorded|failed、原因码）。
  OfflineReplay 识别且不调用 IMemory；学习投影按 DEC-030 §5 配方从事件流重建
  （`W-03`）。
- Executor 路由（设计 §7）：episode/lesson 写与失败检索均为同步 IMemory 门面调用
  （后端自路由 store worker，M4 §17）；无新任务类别、timer 或私有并发；关闭顺序不变。

## 兼容性与限制

- IR 的 minor 升级要求 reader 升级（未知字段不跳过）；这是本模块相对通用事件 schema
  更强的承诺。
- `Navigate` 解析与 `screen_state` 谓词自 M12 起落地（DEC-028）：**前提是宿主安装了
  导航上下文**（App Model + ScreenStateProvider）。未安装时 dispatching 策略仍在创建
  Run 时拒绝含 Navigate 步骤的定义（`navigate-unresolvable`），DryRun 维持形状规划，
  全部 `screen_state` 谓词维持不可求值（fail closed）。UI 状态识别本身始终在宿主侧
  （DEC-011）；代价权重与置信度参数（缺省全 1 / 半衰期由宿主传入）为暂定默认值，
  未经目标平台校准（`RULE-10`）。
- `Strict` 为 IR 未声明默认策略时的默认值（DEC-019/020，暂定状态已于 M9 冻结）。
- Workflow 库与 Run 表为进程内投影（EventStore 仍是权威记录）；SQLite Workflow Library
  存储与跨进程续跑未交付（`RISK-2026-038`）。publish 门禁三事件自 M11 起为库变更提供了
  可审计事件流，持久化载体仍推迟。
- 自然语言 → patch 条目的解释编排（含三类目标判定）与 Workflow 定义/偏好两类目标属
  Agent 侧与后续阶段（DEC-024 §7）；M10 交付的是解释结果的机器侧闭环。定义级「本次 ->
  默认」的机器侧路径自 M11 起经轨迹编译与门禁入库承载（DEC-025 §2），其对话解释编排
  仍属 Agent 侧。
- Agent 工具调用轨迹的事件流自动抽取与「何时编译」的 harness 编排不在 M11（宿主构造
  入口与门禁已交付，编排属后续里程碑）。
- 异步驱动上限必须小于 Executor worker 数（一个驱动占用一个 worker 等待步 future）。
- 学习闭环自 M13 起落地（DEC-029/030）：**前提是宿主安装了学习上下文**
  （`set_learning_context`）且 Run 结算有事件溯源锚点（接了事件存储的
  `WorkflowRunSettled`）。未安装时学习路径全部 NoOp。失败检索为 exact+FTS 两腿的保守
  首期（无 embedding 腿、无召回质量声明，`RULE-10`）；`relevant_lessons` 是
  `WorkflowAgentContinuation` 的**增量字段**（旧消费者源码兼容，紧序列化消费者需留意）；
  Agent 对 lesson 的采纳编排属 Agent Harness 侧（数据面只到续跑上下文为止）。
