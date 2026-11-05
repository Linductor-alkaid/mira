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
- `Timestamp{wall, monotonic}`：双时钟读数；`wall` 为 `WallTimePoint`
  （system_clock 上显式纳秒精度的 `time_point`）——wire 格式按 wall 纳秒整数
  精确往返、陈旧判定按 1 ns 粒度比较，MSVC 等 system_clock tick 粗于 1 ns 的
  标准库由此获得精确表示（纳秒 tick 标准库上与 `system_clock::time_point`
  同型）；跨时钟域时序问题见 Observation 的 `ClockSyncQuality`。
- `wall_nanos()` / `monotonic_nanos()` / `timestamp_from_nanos()`：Timestamp 与
  纳秒整数 wire 形式的唯一转换入口（`core_contracts.hpp`）。wall 侧接受任意
  system_clock 精度的 `time_point`；所有上下文/模型/存储面共用这一组转换，
  保证存储与回放解码一致。
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

`task_state_name(TaskState)` 与 `task_state_from_name(string_view)` 共用稳定名称表，
供宿主诊断、Interaction 和 Checkpoint 恢复使用（DEC-079）。输出为静态生命周期的
`string_view`；两函数同步、无分配且 `noexcept`，不启动任务、不依赖 Executor。
解析严格区分大小写，不接受别名或首尾空白；未知名称返回 `nullopt`。非法枚举输出
`Unknown`，该诊断字符串不能解析成合法状态。既有持久化名称与枚举值保持不变。

```cpp
#include <mira/core_contracts.hpp>

int main() {
    const auto state = mira::task_state_from_name("Verifying");
    if (!state) return 1;  // 拒绝未知持久化名称
    return mira::task_state_name(*state) == "Verifying" ? 0 : 1;
}
```

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

生命周期细则（[DEC-080](../decisions/DEC-080-runtime-lifecycle-transition-boundary.md)）：
`valid_runtime_transition` 允许 Failed→Stopping 清理资源，Stopping/Stopped 不得返回
Running。初始化和停止准入串行化；关闭命令被拒绝后准入仍关闭，begin_operation
也返回 InvalidState。即使 request_shutdown 返回容量错误，外部 owner 仍须执行
finish_shutdown：它消费已登记 submission future 后至多补交一次关闭命令，成功时
报告诊断含 `shutdown admission retry`。未初始化/初始化失败的实例不提交任务，
不会借关闭触发 Executor 懒初始化。

finish_shutdown 等待时不持有命令准入锁，最终关闭必须来自外部 owner。Runtime 自身
控制回调内调用返回 clean=false，诊断说明 `pending commands not scanned`；该拒绝报告
的 pending_commands 不表示最终未决数。环境 interrupt 错误/异常经 shutdown 命令
outcome 暴露；最终报告为 clean=false、Failed，重复收尾返回原报告，不能自动重开实例。
Adapter 仍须提供有界、幂等的 interrupt；超时不会强制杀死正在运行的平台调用。

```cpp
#include <mira/runtime.hpp>

int main() {
    mira::MiraRuntime runtime;
    if (!runtime.initialize()) {
        const auto cleanup = runtime.finish_shutdown();
        return cleanup.clean ? 1 : 2;
    }
    const auto requested = runtime.request_shutdown();
    // A rejected command still requires external finalization.
    const auto report = runtime.finish_shutdown();
    if (!report.clean || report.pending_commands != 0) return 2;
    return requested ? 0 : 1;  // Preserve the initial admission failure for the host.
}
```

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
- 每次写入要求 `ProcessCrash` 或更强的确认。存储返回较弱或非法的 `achieved`
  时返回 `DataLoss`，不推进本地 phase；append 错误原样返回。失败不意味着底层
  没有写入，start 的恢复仍须保守处理。真实宿主必须注入耐久存储，内存替身不能
  证明崩溃恢复（[DEC-074](../decisions/DEC-074-journal-ack-and-gateway-attempt-accounting.md)）。
- 与恢复契约联动：未决副作用（`dispatch_started` 后无 receipt）在恢复时保持 pinned，
  必须先 Observe/Verify，禁止盲目重发（RULE-05）。
- [DEC-076](../decisions/DEC-076-journal-identity-and-bounded-recovery.md)：ActionId 在本地
  绑定完整 intent，冲突及 owner runtime 不匹配返回 `InvalidArgument`；目标三字段
  各 1–1024 字节，禁止分隔符及控制字符；结果尾部至多 4096 字节、允许 `|` 文本。
  写入回执还必须匹配 event ID 与有效 sequence，存储异常转为固定摘要的 `Internal`。
- 可选第三个构造参数 `ActionJournal::RecoveryPolicy`：`page_size=1024`、
  `max_events_scanned=1'000'000`；所有会话事件均计入扫描预算，超限返回
  `ResourceExhausted`，坏页/坏载荷/持久化身份冲突返回 `DataLoss`，不返回部分恢复。
  `recover` 为只读状态查询，不重新授权派发，也不使跨进程重发 `prepare` 成为安全操作。

## 相关文档

- [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)
- [DEC-001 Runtime 的 Executor 所有权](../decisions/DEC-001-runtime-executor-ownership.md)
- [DEC-002 公共契约版本化](../decisions/DEC-002-public-contract-versioning.md)
