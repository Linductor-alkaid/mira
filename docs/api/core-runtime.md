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
  `ResumeTask`、`CancelTask`、`RequestTakeover`、`ReleaseTakeover`、`OperationCompletion`、
  `ShutdownRuntime` 等）。
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
auto snapshot = runtime.task_snapshot(task.value().id);
runtime.request_shutdown();
auto report = runtime.finish_shutdown();   // ShutdownReport{clean, state, pending_commands}
```

- `open_session` 注入 `std::shared_ptr<IEnvironment>`；Runtime 不拥有平台资源。
- `begin_operation` / `admit_operation_completion` 是协调者接入点：外部驱动循环（如
  `AgentLoop`）用它声明操作边界并提交完成。
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
