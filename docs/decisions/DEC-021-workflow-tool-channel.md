# DEC-021：Workflow 操作的 Tool 通道表达

> 状态：Accepted
> 日期：2026-09-08
> 负责人：Mira Maintainers
> 冻结里程碑：[M8](../plans/m8-workflow-contracts.md)（`M8-03`/`M8-11`）
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §6.5 所述
  Decision 动作空间扩展中 Workflow 操作部分的契约冻结）

## 背景与问题

架构设计 §6.5 要求模型可见的 Workflow 操作首选经 Tool 通道表达（走 `ToolIntent`/
`ToolProposal` 与既有校验管线），避免为模型新增绕过工具校验的旁路；同时明确
`pause/resume/cancel` 等运行控制是宿主与用户命令，不经模型即可触发。
[DEC-015](DEC-015-builtin-tool-execution-boundary.md) 已冻结最小 BuiltIn 工具执行边界；
GitHub [#8](https://github.com/Linductor-alkaid/mira/issues/8)（AgentLoop 工具闭环）已随
DEC-015 落地关闭。本决策冻结五个 Workflow 操作的 wire schema、参数校验、错误语义与
权限挂钩点；执行闭环属阶段 B（Workflow Runtime），本决策只冻结 schema 与本地校验。

## 决策

### 1. 五个操作经 BuiltIn Tool 边界表达，与既有工具同构

模型可见的 Workflow 操作注册为 BuiltIn 工具（DEC-015 边界，进程内、无模组生命周期；
DEC-009 模组体系落地后原样吸纳），wire 名称与职责：

| wire 名称 | 职责 | `has_side_effects` |
| --- | --- | --- |
| `run_workflow` | 以指定版本 digest 与参数启动一次 WorkflowRun | `true` |
| `patch_workflow` | 对运行中 Run 提交参数/步骤 patch（三类目标见 [DEC-022](DEC-022-conversation-patch-semantics.md)） | `false` |
| `pause_workflow` | 请求暂停指定 Run（安全收敛在执行连续控制、释放输入后生效） | `false` |
| `resume_workflow` | 恢复暂停/等待中的 Run（步边界 + 重新观察，DEC-020） | `false` |
| `cancel_workflow` | 取消指定 Run（终态幂等） | `false` |

`has_side_effects` 按「该工具自身执行是否产生外部副作用」判定（对齐 `BuiltinToolSpec`
语义）：`run_workflow` 启动的 Run 将驱动环境动作，标记为 `true`；控制类操作只改变 Run
状态。Workflow 步骤的实际环境动作永远经 `W-06` 的单一执行通道（`IEnvironment`/
`IInputProvider`/`ITool`）与其自身校验，**工具标记不构成对下游动作的授权豁免**。

### 2. Wire schema（v1，`mira.workflow.tool.v1`）

每个操作冻结「参数 schema + 结果 schema + 错误 schema」三件套（JSON Schema 子集，与
`gate_schema_subset` 支持的关键词同源，不新造关键词）：

- 公共参数字段：`workflow_id`（32 位 hex 的 `WorkflowId`）、`run_id`（32 位 hex，控制类
  操作必填）、`ir_digest`（64 位 hex，`run_workflow` 必填，钉住创建时版本）。
- `run_workflow`：`parameters`（对象，按目标 Workflow 参数 schema 绑定校验——绑定本身在
  Runtime 侧进行，工具侧只校验「是对象」）、`policy`（可选，必须在目标 Workflow 的
  `allowed_policies` 内——工具侧校验枚举形状，集合成员资格由 Runtime 判定）。
- `patch_workflow`：`patch_entries`（数组，每项 `{target, op, path, value}` 的受限 patch
  形态，目标枚举 `run_parameters | step_arguments | execution_policy`，见 DEC-022）、
  `patch_id`（幂等键）。
- `pause_workflow`/`resume_workflow`/`cancel_workflow`：仅公共字段（`cancel_workflow` 结果
  对终态 Run 幂等成功，携带 `already_terminal` 标记）。
- 结果拆分模型面/宿正面（参考研究 §5.1 的 Pi 模式，与 `ToolExecutionRecord` 的
  result/large_payload 分工同构）：`output`（面向模型：Run 句柄、状态、安全摘要）与
  `details`（面向宿主：结构化诊断）。两者都过各自 schema。
- 错误语义：工具结果中的失败以统一错误信封表达
  `{code, domain, domain_code, retryable, safe_message, operation_id}`，`code` 取自既有
  `ErrorCode`；模型可见消息一律为已脱敏的 `safe_message`，不得携带参数原文或凭据。

### 3. 校验同源与 fail closed

- 五个 schema 的本地校验器与 `resolve_tool_calls` 的 fail-closed 语义同源：未知工具名、
  未知字段、超限实参（复用 `ToolBridgeLimits`）、重复调用 ID 冲突全部拒绝；同 digest 的
  重复调用折叠并记录 `deduplicated`。
- `validate_workflow_operation(operation, arguments)` 是同步纯函数：先过 JSON Schema 子集
  校验，再做语义校验（ID 形状、digest hex、枚举成员、`run_id` 必填性）。**只校验、不
  执行**；执行由阶段 B 的 Workflow Runtime 承接，届时注册为真正的 BuiltIn 工具 handler。
- M8 交付 `workflow_operation_specs()`：返回五个操作的参数/结果/错误 schema 与规格元数据，
  供宿主暴露给模型或做 contract test；不注册任何执行体。

### 4. 权限挂钩点与宿主直达通道

- 权限挂钩在两处：工具暴露时（`SafetyPolicy` 决定是否向该请求暴露该工具，既有
  ModelRequest.tools 机制）与 Run 启动/patch 时（Runtime 按 DEC-004 与 `W-05` 评估目标
  Workflow 的动作风险；高风险步骤确认策略在 Workflow 路径同样生效）。本决策不新增第三
  套权限体系。
- **宿主与用户命令不经模型**：`pause/resume/cancel`（以及宿主发起的 `run`）由宿主直接
  调用 Runtime 控制面（DEC-018 的操作准入规则：`Paused`/`SuspendedForTakeover` 态拒绝
  新操作）。Tool 通道只是模型发起路径；两条路径汇聚到同一控制面与同一事件序列。
- `update_memory` 沿用既有规则（DEC-004 的 User-scope 人工审批与脱敏约束），不在本决策
  范围；`request_user_input` 与 `WaitingUser` 决策点的表达席位在
  [DEC-022](DEC-022-conversation-patch-semantics.md) 预留，本决策不冻结其 schema。

### 5. 与 DEC-009、GitHub #8 的关系

- #8 已随 DEC-015 关闭：AgentLoop 工具执行闭环存在且被测试覆盖。本决策的五个操作在
  阶段 B 注册进该闭环即可被执行；Agent 发起 Workflow 调用的端到端验证随阶段 B 里程碑
  取证，M8 只交付 schema 与校验（`RISK-2026-035` 缓解）。
- DEC-009 模组边界不变：五个操作是 BuiltIn 工具，不是模组；模组体系落地后它们仍属
  BuiltIn 边界（宿主内置能力），不进入外部模组信任域。

## 备选方案

- **为模型新增 Decision 级 Workflow 动作类型（绕过 Tool 通道）**：制造第二条模型→执行
  的路径，重复校验、权限与审计逻辑，违反 §6.5 与 DEC-009 的通道统一原则。不采用。
- **把五个操作实现为 AgentLoop 内建分支（不经注册表）**：绕过 `BuiltinToolRegistry`
  的身份一致性与至多一次派发检查（DEC-015），且宿主无法选择不暴露。不采用。
- **M8 一并实现执行 handler**：执行依赖 Workflow Runtime（阶段 B），先行实现只能产出
  不可验证的桩；违反「schema 以阶段 B 最小闭环反推」（`RISK-2026-034`/`RISK-2026-035`）。
  不采用。
- **控制类操作也标记 `has_side_effects=true`**：与 `BuiltinToolSpec` 的语义（工具自身
  执行的外部副作用）不符，会让预算与审计把状态变更误计为环境动作。不采用。

## 影响与风险

- 公开契约新增 `workflow_operation_specs()` 与 `validate_workflow_operation()`；五个
  wire 名称进入 `is_known_hosted_tool_name` 一类的保留名单考量（注册时尚需与既有保留名
  不冲突，实现时验证）。
- `run_workflow` 的 `policy` 集合成员资格、参数绑定合法性在工具侧只能做形状校验，完整
  判定在 Runtime：schema 文档必须写明这一边界，避免读者误以为工具校验通过即代表 Run
  会被接受。
- 阶段 B 注册执行体时需补：工具身份快照与 Run 生命周期的绑定（暴露后注册表变更即
  fail closed，复用 DEC-015 规则）。

## 验证方式

- 契约测试：五组 schema 的正向/负向校验（未知字段、坏 ID 形状、坏 digest、非法枚举、
  缺 `run_id`、超限实参）、错误信封形状、`run_workflow` 参数对象形状、
  `cancel_workflow` 终态幂等标记。
- 与 `resolve_tool_calls` 的同源性以共享 `ToolBridgeLimits` 与同一 schema 子集校验器
  断言（不重复实现）。

## 关联文档和工作项

- [M8](../plans/m8-workflow-contracts.md)：`M8-03`、`M8-11`
- [DEC-009](DEC-009-tool-module-boundary.md)、
  [DEC-015](DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-018](DEC-018-takeover-input-release-and-operation-admission.md)、
  [DEC-019](DEC-019-workflow-ir-contract.md)、
  [DEC-020](DEC-020-workflow-run-lifecycle.md)、
  [DEC-022](DEC-022-conversation-patch-semantics.md)
- [Agent Harness 参考研究](../design/harness_reference_study.md) §5.1（output/details 拆分）
