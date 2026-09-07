# 核心契约与 Runtime

> 头文件：`mira/core_contracts.hpp`、`mira/runtime.hpp`、`mira/runtime_baseline.hpp`、
> `mira/action_journal.hpp`

## core_contracts.hpp：值类型、错误与三层状态机

所有 Mira 模块共享的基础契约。

### 标识、时间与版本

- `Id128`：128 位 UUIDv4 形态标识；`MIRA_DEFINE_ID` 派生 26 个强类型 ID
  （`RuntimeId`、`SessionId`、`TaskId`、`CommandId`、`StepId`、`OperationId`、
  `ObservationId`、`ActionId`、`EventId`、`ArtifactId`、`ConfirmationId`、`LeaseId`、
  `FrameId`、`TenantId`、`DisplayId` 等）。
- `Timestamp{wall, monotonic}`：双时钟读数；跨时钟域时序问题见 Observation 的
  `ClockSyncQuality`。
- `EnvironmentEpoch`：环境侧单调计数，坐标有效性的锚点。
- `SchemaVersion{major, minor}` 与 `validate_schema_version()`：current/previous 读取
  语义，拒绝跨越两个 major 的旧数据与更新的未知 major。

### 错误模型

`Result<T>` / `Result<void>` / `Error`（见[手册首页](index.md#错误模型)）。`ErrorCode`
是稳定枚举，值得注意的语义专用码：`StaleObservation`、`InvalidModelOutput`、
`ContextOverflow`、`SafetyRejected`、`ConfirmationRequired`、`ExecutionUncertain`。

### 状态机

三层显式状态机，`is_terminal()` 与 `valid_*_transition()` 定义全部合法转换：

- `RuntimeState`：`Constructed -> Initializing -> Running -> Stopping -> Quiesced ->
  Stopped`（或任意失败点进入 `Failed`）。
- `SessionState`：`Opening -> Autonomous`，Human Takeover 走
  `TakeoverPending -> HumanControlled -> Resuming -> Autonomous`，`Closing -> Closed`。
- `TaskState`：`Idle -> Observing -> Reasoning -> Planning -> Acting -> Verifying` 闭环，
  加上 `Recovering`、`Pausing/Paused`、`TakeoverSettling/SuspendedForTakeover`、
  `Cancelling` 与终态 `Completed/Failed/Cancelled`。终态自转换幂等，其余终态出边为空。

### 命令与操作生命周期

- `CommandKind`：串行控制面命令（`OpenSession`、`SubmitTask`、`PauseTask`、
  `ResumeTask`、`CancelTask`、`CompleteTask`、`RequestTakeover`、`ReleaseTakeover`、
  `OperationCompletion`、`ShutdownRuntime` 等）。
- `CommandReceipt`（`Accepted/Rejected` + `control_sequence`）与 `CommandOutcome`
  （`SettlementStatus`：`Applied/NoOp/Failed/Superseded`）分离"收到"与"生效"。
- `OperationKey{task, epoch, step, operation}` 标识一次有界环境操作；
  `OperationState` 从 `Created` 推进到 `Settled`，迟到完成以
  `CompletionDisposition::Stale/Duplicate/NotFound` 丢弃——已取消或已终结的任务不能被
  迟到的模型响应或动作结果复活（RULE-03）。
- `TaskSnapshot` / `TaskOutcome`：任务的可观测状态投影。

## runtime.hpp：MiraRuntime

宿主面对的串行控制面入口。命令按提交顺序结算；`CommandHandle` 允许等待 receipt 或
settlement。

```cpp
mira::MiraRuntime runtime;                 // RuntimeConfig{worker_threads, executor_queue_capacity, max_in_flight}
runtime.initialize();
auto session = runtime.open_session(environment, SessionConfig{principal});
auto task = runtime.submit_task(session.value().id, TaskSpec{"goal"});
auto paused = runtime.pause_task(task.value().id);   // resume/cancel 同形
auto takeover = runtime.request_human_takeover(session.value().id);
auto op = runtime.begin_operation(task.value().id, step_id);
runtime.admit_operation_completion(op.value());
auto done = runtime.complete_task(task.value().id,
                                  TaskOutcome{TaskState::Completed, std::nullopt});
auto snapshot = runtime.task_snapshot(task.value().id);
runtime.request_shutdown();
auto report = runtime.finish_shutdown();   // ShutdownReport{clean, state, pending_commands}
```

- `open_session` 注入 `std::shared_ptr<IEnvironment>`；Runtime 不拥有平台资源。
- `begin_operation` / `admit_operation_completion` 是协调者接入点：外部驱动循环（如
  `AgentLoop`）用它声明操作边界并提交完成。`Paused` 与 `SuspendedForTakeover` 状态的
  任务不接受新操作（`InvalidState`，[DEC-018](../decisions/DEC-018-takeover-input-release-and-operation-admission.md)）；
  暂停/接管前已登记的操作按 epoch 语义结算为 stale。
- 暂停与恢复的语义：`pause_task` 递增 epoch，在途操作的迟到完成作废；`resume_task`
  把任务带回 `Observing` 并再次递增 epoch，宿主重新驱动其循环。循环上下文每轮从目标与
  常驻指令重建，重新驱动不丢失用户意图，但**不支持从暂停点的执行级续跑**——执行点
  continuation 由 WorkflowRun 恢复语义承载（M8-05，[架构设计 §7.6](../design/agent_harness_and_workflow_architecture.md)）。
- `request_human_takeover` 生效时任务转入 `SuspendedForTakeover` 并 best-effort 调用
  `IEnvironment::interrupt()` 释放在途平台输入与环境等待（DEC-018）；`release_human_takeover`
  后任务回到 `Observing`（恢复前重新观察）。单任务 `cancel_task` 不调用环境级
  `interrupt`——同会话多任务共享环境，取消单个任务不得打断其他任务的在途操作；会话级
  收敛（takeover/close/shutdown）才触发放释。
- `complete_task`（[DEC-017](../decisions/DEC-017-complete-task-command.md)）：以
  `Completed`/`Failed` 收尾任务。终态幂等按状态区分——重复同终态 `NoOp`、冲突终态拒绝；
  仅当 M1 转换表存在合法路径时接受（`Cancelling` 中的任务只能 `Cancelled` 收尾）；
  完成递增 epoch，使在途操作的迟到完成按 stale 语义丢弃。
- `request_shutdown()` 停止接受新命令；`finish_shutdown()` 返回 `ShutdownReport`，
  未决命令数量与 `clean` 标志必须被检查。

## runtime_baseline.hpp：RuntimeBaseline

M0 交付的最小命令循环基线，用于在完整 Runtime 语义就绪前验证 Executor 集成与命令
结算路径。`submit(BaselineCommand)` / `wait(id, timeout)` / `request_shutdown()` /
`finish_shutdown()`；`BaselineResultCode` 与 `BaselineRuntimeState` 是其专用枚举。新代码
应优先使用 `MiraRuntime`，Baseline 保留用于最小依赖的生命周期验证。

## action_journal.hpp：ActionJournal

离散动作的持久化副作用日志，消费 `IEventStore`：

- `ActionIntent` 描述一次动作意图（含坐标摘要与 display）。
- 写入路径：`prepare()` -> `dispatch_started()` -> `receipt()`（或
  `execution_uncertain()`）；每一步都是显式事件，崩溃后可由 `recover()` 重建
  `ActionJournalState`。
- 与恢复契约联动：未决副作用（`dispatch_started` 后无 receipt）在恢复时保持 pinned，
  必须先 Observe/Verify，禁止盲目重发（RULE-05）。

## 相关文档

- [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)
- [DEC-001 Runtime 的 Executor 所有权](../decisions/DEC-001-runtime-executor-ownership.md)
- [DEC-002 公共契约版本化](../decisions/DEC-002-public-contract-versioning.md)
