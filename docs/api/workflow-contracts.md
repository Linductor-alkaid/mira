# Workflow 契约

> 适用头文件：`workflow_ir.hpp`、`workflow_run.hpp`、`workflow_versioning.hpp`、
> `workflow_events.hpp`、`workflow_tools.hpp`（CMake 目标 `Mira::workflow`）
> 状态：Active（M8 契约层；执行闭环属阶段 B）
> 依据：[DEC-019](../decisions/DEC-019-workflow-ir-contract.md)、
> [DEC-020](../decisions/DEC-020-workflow-run-lifecycle.md)、
> [DEC-021](../decisions/DEC-021-workflow-tool-channel.md)、
> [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)、
> [Workflow Runtime 设计](../design/workflow_runtime_design.md)

`Mira::workflow` 是 Workflow 双路径的契约层：Workflow IR、WorkflowRun 状态视图、
资产版本化、事件载荷与 Workflow 操作的 Tool 规格。本层全部为**同步纯函数**：没有
Executor 路由、没有执行、没有 I/O；执行闭环（阶段 B）以
[workflow_runtime_design](../design/workflow_runtime_design.md) 为规范实现。

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

## 兼容性与限制

- IR 的 minor 升级要求 reader 升级（未知字段不跳过）；这是本模块相对通用事件 schema
  更强的承诺。
- `Navigate` 目标与 `screen_state` 谓词在阶段 E 之前只做形状校验，不可求值；不得据此
  宣称导航或验证能力已实现（`RULE-10`）。
- `Strict` 为 IR 未声明默认策略时的暂定默认值（DEC-019/020，最迟阶段 B 冻结）。
- 执行闭环、五个操作的 BuiltIn 注册、SQLite Workflow Library 存储均属阶段 B。
