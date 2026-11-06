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
  `ProcessCrash` / `PowerLoss`；append 回执 `achieved` 表示实际等级，不能回显请求。
- `IEventStore`：`append()` / `append_batch()` / `read(EventQuery)` / `recover()` /
  `flush()`。`MemoryEventStore` 是有界内存参考实现。
- `Sha256Digest`：事件与工件的规范摘要（`digest_bytes` / `digest_string` /
  `digest_from_hex`）；`canonical_json_digest()` 对规范化 JSON 计算摘要。

`EventEnvelope::timestamp_origin` 区分 AppendProcess、PersistedWall 和
UnavailableLegacy。FileEventStore 的 v2 行精确持久化 wall 纳秒；恢复后的 monotonic
不可用，旧行缺失的时间不再重建为 now。物理版本标记、旧前缀续写/降级拒绝、
损坏修复与 **C++ 消费者重编译**要求见
[文件日志 v2 兼容性](../compatibility/file-event-log-v2.md)。原 integrity 为请求
幂等摘要，文件 record_digest 额外覆盖时间和序号，不代表密码学认证或 commit 时钟。

[DEC-088](../decisions/DEC-088-event-store-durability-and-external-kill.md) 校准参考实现：
MemoryEventStore append（含重复）总是 Buffered；更强 flush 返回
UnsupportedCapability。FileEventStore 每次新写入刷新并检查关闭，实际回执为
ProcessCrash（请求 Buffered 也如此）；PowerLoss 的 append/flush 在写入前拒绝。
两者都拒绝非法 durability 枚举；File flush 与 append/recover 串行并遵守只读门禁。
当前无系统级掉电同步，不能把成功回执解释成 PowerLoss。裸内存 store 不能满足
ActionJournal/VerifiedFactPublisher 的强确认门槛；测试模拟回执不用于真实宿主。

## artifact_store.hpp

大体积载荷（截图、原始模型响应）不进事件流：`ArtifactDescriptor`（含
`Sensitivity::Public/Internal/Sensitive/Secret` 分级与 `ArtifactEncoding`）、
`ArtifactWriter`/`ArtifactReader` 写读、`IArtifactStore` 边界与
`ArtifactErasureRequest`/`ErasureReceipt` 删除传播。Secret 级内容受脱敏与权限负向
测试约束。`sensitivity_name()`/`sensitivity_from()` 是 Sensitivity wire 词表的唯一
序列化入口（定义在枚举旁；context/memory/model 契约与 SQLite 存储共用）。

## json.hpp

自包含 `JsonValue`（对象/数组/字符串/整数/浮点/布尔/null）与 `parse_json(text,
JsonLimits)`、`to_json_string()`、`canonical_json_string()`（键排序、无空白的规范形态，
是 digest 与事件比较的基础）。解析有深度/长度上限；注意大整数精度语义以文本保真。

## replay.hpp

`OfflineReplayEnvironment`（见[环境页](environment-observation.md#adapters)）与
Replay 侧 Provider（`model_replay.hpp`）共同保证：回放只返回录制的外部结果，默认不执行
网络、Tool 或输入副作用（通用门禁）。

## task_checkpoint.hpp

当前文档 schema 为 2.0，兼容读取旧 1.x。VerifiedFact 的可选 valid_from/valid_until
必须成对；新事实缓存消费者须检查 valid_at，Builder 在重建时按 now 清除过期事实。
旧 reader 对 2.0 返回 UnsupportedVersion。字段布局扩展要求下游与库一起重编译；
SQLite 容器 schema 不变，迁移与回退约束见
[事实与 Checkpoint 迁移](../compatibility/verified-fact-v1-and-checkpoint-v2.md)。

`checkpoint_trigger_name()` 与 `recovery_action_name()`（task_recovery.hpp）是宿主
诊断契约，分别解释 checkpoint 触发与恢复下一步；无效枚举返回 `Unknown`。
它们不发起任务、恢复或真实副作用，保留公开名称不依赖仓库内部是否打印日志。
保留依据与编号兼容性见[公开契约审计](../compatibility/2026-11-public-contract-audit.md)。

- `TaskCheckpoint`：确定性恢复投影——Goal、`CheckpointConstraint`、`VerifiedFact`、
  `CompletedStep`、`PendingObjective`、`UnresolvedIssue`、`UncertainSideEffect`（未决
  副作用 pinned，恢复后必须先 Observe/Verify）与 `through_event_sequence` 锚点。
  `checkpoint_to_json()` / `checkpoint_from_json()` 提供版本化序列化。
- `ICheckpointStore`：`put()` / `latest()` / `latest_at_or_before(task, max_sequence)`
  （恢复只选择不超过 durable sequence 的检查点）/ `count()` / `erase_task()`。
- `CheckpointCoordinator`：协调 EventStore 与 store 的确定性调度写入
  （pause/Takeover/shutdown/水位触发与投影缺失重建，见 M4-07）。

## task_recovery.hpp

`ActionPrepared` 的合法 pipe 只更新 checkpoint 身份/epoch 元数据，不设置
`verification_pending`，也不误计为 malformed。journal 与 reducer 的 pipe 解析规则
共享，尾部含 `|` 的不确定性说明完整保留（DEC-076）。

崩溃恢复规划（M4-08）：

- `RecoveryPlanner(events, checkpoints, policy).plan(task, session, now)` 返回
  `RecoveryOutcome`。
- `RecoveryAction`：`ResumeObserving`（默认非终态恢复点）、`ResumeVerifying`（动作完成
  但验证未跑）、`AlreadyTerminal`（durable 日志已终结，绝不复活）、`NoState`。
- 恢复语义：非终态任务只恢复到 Observing/Verifying；未决副作用不重发；终态幂等。
- `RecoveryPolicy::max_events_scanned` 计入所有扫描的会话事件（含其他任务），必须为
  正数。超限返回 `ResourceExhausted`；跨会话、非递增或无进展的页返回 `DataLoss`，
  不把部分扫描当作完整恢复（[DEC-076](../decisions/DEC-076-journal-identity-and-bounded-recovery.md)）。

## state_store.hpp：SQLite 参考后端（`Mira::state_store`）

`SqliteCheckpointStore`——`ICheckpointStore` 的 SQLite/WAL 实现：

```cpp
kairo::Executor exec;
exec.initialize(kairo::ExecutorConfig{});
mira::SqliteStoreOptions options;
options.path = root / "checkpoints.db";
auto store = mira::SqliteCheckpointStore::open(exec, options);  // Result<unique_ptr<...>>
```

- 单一 dedicated blocking-I/O worker（单 writer、有界准入）；初始化与迁移显式发生在
  `open()`，失败不清库；初始化/迁移内容与版本标记在同一事务中提交或回滚。
- `close()` 从宿主（非 worker）线程调用，幂等；Executor 必须活得比 store 长。
- `StoreDiagnostics` / `pending_requests()` / `set_worker_paused()` 提供有界队列
  可观测性与维护缝隙。
- schema 迁移按 current/previous 策略；新版本文件可退回只读诊断模式打开。
- 两个参考后端通过 storage 内部 `open_reference_store` 复用连接、schema 校验、
  事务和诊断编排，DDL/迁移策略与错误域仍归各后端。版本必须是两个完整 uint16
  十进制分量，畸形/溢出返回 DataLoss；需要迁移的旧库在只读模式明确返回
  UnsupportedVersion，不尝试写入（[DEC-078](../decisions/DEC-078-shared-sqlite-store-initialization.md)）。

内存侧对应后端 `SqliteMemoryStore` 见
[Context 与 Memory 页](context-memory.md)。

## 交互事件分区

M28 的幂等台账把 `mira.interaction.*.v1` 事件写在 EventStore 的一个**主体级专用
分区**（`interaction_ledger_partition()`，见[交互协调层页](interaction-coordinator.md)），
而不是混入业务会话或任务流。理由：一次主体输入跨会话、跨重启，而
`IEventStore::read` 以 `session_id` 分区；若交互事件散落在多个会话分区，跨重启的
重建就必须扫描全部会话流，与「台账是一个可重建投影」相悖。该分区是 **Mira 拥有的
常量** id，使重启后的进程无需持久化 id 即可找到同一条耐久流。

代价：交互事件不进入会话事件流——宿主若要观察交互生命周期需读该分区；会话对话
投影与 checkpoint 投影因此不受交互事件影响。

## 相关文档

- [持久化恢复的离线宿主](offline-composition-hosts.md#recovery规划输出控制下一步行为)
- [交互协调层](interaction-coordinator.md)
- [事件、资产与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
- [Stateful agent beta 发布说明](../releases/stateful-agent-beta.md)
