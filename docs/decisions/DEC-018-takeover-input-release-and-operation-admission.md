# DEC-018：Takeover 平台输入释放与暂停态操作准入

> 状态：Accepted
> 日期：2026-09-08
> 负责人：Mira Maintainers
> 冻结里程碑：M4 后维护轮（[maintenance-2026-09-agent-harness-closure.md](../plans/maintenance-2026-09-agent-harness-closure.md)）
> 替代/被替代：无（补齐 `AGENTS.md` Human Takeover 约束在 Runtime 控制面的执行缺口）

## 背景与问题

2026-09-08 对用户终止/取消能力的审计发现两处与既定约束的差距：

1. `request_human_takeover` 只翻转 Session/Task 状态，不调用
   `IEnvironment::interrupt()`。`AGENTS.md` 要求 Human Takeover "阻止新的自主动作进入
   环境，取消或安全收敛正在执行的连续控制"——在途平台输入与阻塞中的环境等待不会被
   释放，只能等其有界完成。
2. `begin_operation` 仅拒绝终态任务；`Paused`/`SuspendedForTakeover` 状态的任务仍可
   登记新操作并正常结算完成——即暂停或被接管后，新的自主动作边界仍可被打开。

## 决策

1. `request_human_takeover` 的 `Applied` 路径在状态翻转后调用
   `session.environment->interrupt(make_control_context())`，与 `close_session`、
   `request_shutdown` 使用同一 best-effort 释放模式；`interrupt` 按契约幂等，不支持
   `input_release` 能力的环境不受影响。`NoOp`/`Rejected` 路径不释放。
2. `begin_operation` 拒绝 `Paused` 与 `SuspendedForTakeover` 状态的任务
   （`InvalidState`）。`Pausing`/`TakeoverSettling` 为控制面内瞬态，在该互斥下不可观测，
   不单列。暂停/接管前已登记的操作照常按 epoch 语义结算为 stale。
3. 单任务 `cancel_task` 仍**不**调用环境级 `interrupt`：一个会话的多个任务共享同一
   `IEnvironment`，取消其中一个任务不得打断其他任务的在途操作。会话级收敛
   （close/takeover/shutdown）才触发放释。该权衡写入 API 手册。

## 备选方案

- **Takeover 不释放，仅靠状态机阻止新动作**：在途输入与阻塞等待仍占用环境，与
  `AGENTS.md` 的收敛要求不符，不采用。
- **`cancel_task` 也调用 `interrupt`**：会误伤同会话其他任务的在途操作，不采用。
- **在 AgentLoop 内轮询 takeover 状态代替平台释放**：循环协作取消已存在，但平台侧
  阻塞（如长截图）只有 `interrupt` 能解除，两者互补而非替代，不采用。

## 影响与风险

- 公开 API 行为变更：`begin_operation` 对暂停/接管态任务新增拒绝（`InvalidState`）；
  `request_human_takeover` 生效时可能释放平台在途输入（宿主可观测）。
- `interrupt` 为 best-effort：不支持释放的平台不受影响，也不因此宣称接管具备实时
  收敛保证（`RULE-10`）。
- 连续控制的 Takeover 安全收敛仍随 M6 终止（DEC-011），本决策只覆盖既有
  `IEnvironment::interrupt` 契约内的释放。

## 验证方式

- 集成测试 `mira_agent_harness_test` 第二场景：takeover 后 `interrupt` 恰好被调用一次；
  `Paused`/`SuspendedForTakeover` 下 `begin_operation` 拒绝、暂停前操作结算 `NoOp`
  （stale）；release 后任务回 `Observing` 且重新接受操作。
- m1 既有 pause/resume/takeover 流程回归通过。

## 关联文档和工作项

- [维护计划：Agent Harness 闭合（2026-09）](../plans/maintenance-2026-09-agent-harness-closure.md)
  `MNT-202609-19`
- [`AGENTS.md`](../../AGENTS.md)（Runtime 与状态模型、Human Takeover 约束）
- [DEC-004](../decisions/DEC-004-security-authority-confirmation.md)、
  [DEC-017](../decisions/DEC-017-complete-task-command.md)
- [API 手册：Core Runtime](../api/core-runtime.md)
