# 事件、Checkpoint 与恢复

> 头文件：`mira/event_store.hpp`、`mira/artifact_store.hpp`、`mira/json.hpp`、
> `mira/replay.hpp`、`mira/task_checkpoint.hpp`、`mira/task_recovery.hpp`、
> `mira/state_store.hpp`

EventStore 是已提交事实的权威记录；Checkpoint、Memory 和索引都是可重建投影
（[DEC-003](../decisions/DEC-003-event-sourced-persistence.md)，RULE-07）。

## event_store.hpp

- `EventEnvelope`：带 `EventId`、runtime/session/task 关联、`session_sequence`/
  `task_sequence`/`control_sequence`/`durable_sequence` 排序锚、`Timestamp` 与
  `EventPayload{type, canonical_json, EventClass}` 的事件信封。
- `EventClass`：`Critical` / `State` / `Diagnostic`；`Durability`：`Buffered` /
  `ProcessCrash` / `PowerLoss`，由 `flush(durability)` 显式结算。
- `IEventStore`：`append()` / `append_batch()` / `read(EventQuery)` / `recover()` /
  `flush()`。`MemoryEventStore` 是有界内存参考实现。
- `Sha256Digest`：事件与工件的规范摘要（`digest_bytes` / `digest_string` /
  `digest_from_hex`）；`canonical_json_digest()` 对规范化 JSON 计算摘要。

## artifact_store.hpp

大体积载荷（截图、原始模型响应）不进事件流：`ArtifactDescriptor`（含
`Sensitivity::Public/Internal/Sensitive/Secret` 分级与 `ArtifactEncoding`）、
`ArtifactWriter`/`ArtifactReader` 写读、`IArtifactStore` 边界与
`ArtifactErasureRequest`/`ErasureReceipt` 删除传播。Secret 级内容受脱敏与权限负向
测试约束。

## json.hpp

自包含 `JsonValue`（对象/数组/字符串/整数/浮点/布尔/null）与 `parse_json(text,
JsonLimits)`、`to_json_string()`、`canonical_json_string()`（键排序、无空白的规范形态，
是 digest 与事件比较的基础）。解析有深度/长度上限；注意大整数精度语义以文本保真。

## replay.hpp

`OfflineReplayEnvironment`（见[环境页](environment-observation.md#adapters)）与
Replay 侧 Provider（`model_replay.hpp`）共同保证：回放只返回录制的外部结果，默认不执行
网络、Tool 或输入副作用（通用门禁）。

## task_checkpoint.hpp

- `TaskCheckpoint`：确定性恢复投影——Goal、`CheckpointConstraint`、`VerifiedFact`、
  `CompletedStep`、`PendingObjective`、`UnresolvedIssue`、`UncertainSideEffect`（未决
  副作用 pinned，恢复后必须先 Observe/Verify）与 `through_event_sequence` 锚点。
  `checkpoint_to_json()` / `checkpoint_from_json()` 提供版本化序列化。
- `ICheckpointStore`：`put()` / `latest()` / `latest_at_or_before(task, max_sequence)`
  （恢复只选择不超过 durable sequence 的检查点）/ `count()` / `erase_task()`。
- `CheckpointCoordinator`：协调 EventStore 与 store 的确定性调度写入
  （pause/Takeover/shutdown/水位触发与投影缺失重建，见 M4-07）。

## task_recovery.hpp

崩溃恢复规划（M4-08）：

- `RecoveryPlanner(events, checkpoints, policy).plan(task, session, now)` 返回
  `RecoveryOutcome`。
- `RecoveryAction`：`ResumeObserving`（默认非终态恢复点）、`ResumeVerifying`（动作完成
  但验证未跑）、`AlreadyTerminal`（durable 日志已终结，绝不复活）、`NoState`。
- 恢复语义：非终态任务只恢复到 Observing/Verifying；未决副作用不重发；终态幂等。

## state_store.hpp：SQLite 参考后端（`Mira::state_store`）

`SqliteCheckpointStore`——`ICheckpointStore` 的 SQLite/WAL 实现：

```cpp
executor::Executor exec;
exec.initialize(executor::ExecutorConfig{});
mira::SqliteStoreOptions options;
options.path = root / "checkpoints.db";
auto store = mira::SqliteCheckpointStore::open(exec, options);  // Result<unique_ptr<...>>
```

- 单一 dedicated blocking-I/O worker（单 writer、有界准入）；初始化与迁移显式发生在
  `open()`，失败绝不清库或改写文件。
- `close()` 从宿主（非 worker）线程调用，幂等；Executor 必须活得比 store 长。
- `StoreDiagnostics` / `pending_requests()` / `set_worker_paused()` 提供有界队列
  可观测性与维护缝隙。
- schema 迁移按 current/previous 策略；新版本文件可退回只读诊断模式打开。

内存侧对应后端 `SqliteMemoryStore` 见
[Context 与 Memory 页](context-memory.md)。

## 相关文档

- [事件、资产与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
- [Stateful agent beta 发布说明](../releases/stateful-agent-beta.md)
