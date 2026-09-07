# DEC-017：任务终态完成命令 `complete_task`

> 状态：Accepted
> 日期：2026-09-07
> 负责人：Mira Maintainers
> 冻结里程碑：M4 后维护轮（[maintenance-2026-09-agent-harness-closure.md](../plans/maintenance-2026-09-agent-harness-closure.md)）
> 替代/被替代：无（补齐 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) Harness
> 审计发现的 Task 终态缺口；语义与既有 `RULE-03`、M1 冻结的状态转换表一致）

## 背景与问题

2026-09-07 的 Agent Harness 审计与 Loop-Runtime 集成测试证实：`MiraRuntime` 控制面只有
`Pause/Resume/Cancel/Takeover` 命令，`TaskState::Completed` 与 `TaskState::Failed` 在
运行时中没有任何生产者（仅 Checkpoint 恢复路径会读取它们）。Agent Loop 成功验证目标后，
任务无法经控制面正常收尾——宿主只能取消任务或任其滞留活动态。参考实现
`RuntimeBaseline` 早已包含 `BaselineCommandKind::CompleteTask`，主 Runtime 缺失对应能力。

## 决策

1. `MiraRuntime` 新增 `complete_task(TaskId, TaskOutcome)`，`CommandKind` 相应新增
   `CompleteTask`；`TaskOutcome.terminal_state` 仅接受 `Completed` 或 `Failed`。
2. 终态语义遵循 `RULE-03`：终态幂等按状态区分——重复同一终态结算为 `NoOp`，请求冲突
   终态被拒绝；已终态任务永不复活。
3. 合法性按 M1 冻结的转换表判定：仅当从当前状态到目标终态存在合法路径时接受（如
   `Verifying -> Completed`、`Recovering -> Failed` 及其可达链）。路径中各中间相位在同一
   控制面命令内原子遍历，不被观察者视为独立状态；`Cancelling` 中的任务只能以
   `Cancelled` 收尾。
4. 完成时递增任务 epoch 并写入 `terminal_outcome`，使在途操作的迟到完成按既有
   stale 语义结算（epoch 不匹配 → `NoOp`）。

## 备选方案

- **复用 `admit_operation_completion` 携带终态**：操作完成不等于任务完成（多操作任务），
  混淆两个生命周期，不采用。
- **放宽转换表允许任意状态直达终态**：修改 M1 冻结契约，且掩盖"验证后才可完成"的语义，
  不采用。
- **维持现状由宿主取消收尾**：成功任务以 `Cancelled` 记录，违背事件语义与
  DEC-014 的 Harness 完整性要求，不采用。

## 影响与风险

- 公开 API 变更：`CommandKind` 枚举追加成员、`MiraRuntime::complete_task` 新增。
- `CommandKind` 无 wire 序列化，追加为非破坏性变更。
- 宿主必须只在其工作单元真正验证完成后调用；命令本身不校验"验证"真实性（与
  `ILoopVerifier` 职责分离）。

## 验证方式

- 集成测试 `mira_agent_harness_test`：loop 验证通过 -> `complete_task` -> `Completed`；
  同终态重复 `NoOp`；冲突终态拒绝。
- 单元级路径判定由转换表全覆盖测试间接保证（m1 既有测试）。

## 关联文档和工作项

- [维护计划：Agent Harness 闭合（2026-09）](../plans/maintenance-2026-09-agent-harness-closure.md)
- [DEC-001](DEC-001-runtime-executor-ownership.md)、[DEC-015](DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-016](DEC-016-conversation-events-and-user-messages.md)
- [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)
