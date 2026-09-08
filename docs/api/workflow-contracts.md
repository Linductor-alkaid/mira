# Workflow 契约

> 适用头文件：`workflow_ir.hpp`、`workflow_run.hpp`、`workflow_versioning.hpp`、
> `workflow_events.hpp`、`workflow_tools.hpp`、`workflow_runtime.hpp`（CMake 目标 `Mira::workflow`）
> 状态：Active（M8 契约层；M9 起含执行闭环 `WorkflowRuntime`）
> 依据：[DEC-019](../decisions/DEC-019-workflow-ir-contract.md)、
> [DEC-020](../decisions/DEC-020-workflow-run-lifecycle.md)、
> [DEC-021](../decisions/DEC-021-workflow-tool-channel.md)、
> [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)、
> [Workflow Runtime 设计](../design/workflow_runtime_design.md)

`Mira::workflow` 是 Workflow 双路径的契约层与（M9 起）执行层：Workflow IR、WorkflowRun
状态视图、资产版本化、事件载荷、Workflow 操作的 Tool 规格与 `WorkflowRuntime` 执行闭环。
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

- 十类 `mira.workflow.*.v1` 载荷（`to_event_payload`/`parse_*`）：RunStarted/StepStarted/
  StepSettled/RunSettled（Critical），patch 三事件、PolicySwitched、决策点两事件（State）。
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

## 兼容性与限制

- IR 的 minor 升级要求 reader 升级（未知字段不跳过）；这是本模块相对通用事件 schema
  更强的承诺。
- `Navigate` 目标与 `screen_state` 谓词在阶段 E 之前只做形状校验，不可求值；dispatching
  策略在创建 Run 时即拒绝含 Navigate 步骤的定义（`navigate-unresolvable`），patch 切换
  到 dispatching 策略同样拒绝；不得据此宣称导航或验证能力已实现（`RULE-10`）。
- `Strict` 为 IR 未声明默认策略时的默认值（DEC-019/020，暂定状态已于 M9 冻结）。
- Workflow 库与 Run 表为进程内投影（EventStore 仍是权威记录）；SQLite Workflow Library
  存储与跨进程续跑未交付（`RISK-2026-038`）。`DryRunPassed` 证据入库链属阶段 D。
- 自然语言 → patch 条目的解释编排（含三类目标判定）与 Workflow 定义/偏好两类目标属
  Agent 侧与后续阶段（DEC-024 §7）；M10 交付的是解释结果的机器侧闭环。
- 异步驱动上限必须小于 Executor worker 数（一个驱动占用一个 worker 等待步 future）。
