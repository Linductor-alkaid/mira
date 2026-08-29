# Mira EventStore、ArtifactStore 与崩溃一致性设计

> 状态：Active  
> 版本：1.0  
> 更新日期：2026-08-30  
> 适用范围：事件事实、资产、大载荷、Checkpoint 投影、Replay 和外部副作用恢复  
> 上位决策：[DEC-003](../decisions/DEC-003-event-sourced-persistence.md)

## 1. 目标与非目标

本设计定义 Mira 如何持久记录可复现事实、引用截图等大载荷，并在任意进程崩溃边界后安全恢复。
目标是：

- 已返回 durable ack 的 Event 在正常存储保证范围内不会因进程崩溃丢失。
- 同一 Session 的 Event 具有严格递增、无重复提交效果的 sequence。
- Artifact 要么完整可读，要么不可见，不能让 Event 引用半写文件。
- 外部副作用的崩溃窗口能分类为未派发、明确完成或结果不确定。
- Offline/Analysis Replay 不获得真实副作用 capability。
- 隐私删除与历史审计的冲突有明确降级语义。

非目标：

- 不提供跨设备分布式共识或 Exactly-once 外部输入。
- 不保证断电时超出文件系统、存储硬件和配置 durability 等级的事实不丢失。
- EventStore 不替代指标、完整模型资产仓库或通用数据湖。
- JSONL 是诊断导出格式，不作为生产事务格式的唯一约束。

## 2. 数据职责

| 数据 | 权威来源 | 可重建 | 典型保留 |
| --- | --- | --- | --- |
| 状态转换、命令、动作派发、receipt | EventStore | 否 | 审计/任务 retention |
| Screenshot、UI Tree、模型原始响应 | ArtifactStore payload + Event ref | payload 可删除 | 按敏感策略 |
| TaskCheckpoint | Event 投影 | 是 | 最近 N 个/任务 retention |
| Long-term Memory | Verified Event 派生记录 | 语义上可重新投影或明确导入 | scope retention |
| FTS/vector index | Memory/Artifact metadata 投影 | 是 | 可随时重建 |
| Metrics | 监控聚合 | 通常否 | 短期 |

EventStore 是“已提交事实”的权威来源，不意味着每个原始敏感 payload 永久保留。Artifact 删除后，
Event 可以保留 descriptor、hash、删除原因和 tombstone，但 Replay quality 必须标记为缺失。

## 3. EventEnvelope 与 Payload

```cpp
struct EventEnvelope {
    EventId event_id;
    RuntimeId runtime_id;
    SessionId session_id;
    std::optional<TaskId> task_id;
    SessionSequence session_sequence;
    std::optional<TaskSequence> task_sequence;
    ControlSequence cause_control_sequence;
    Timestamp timestamp;
    SchemaVersion schema_version;
    TraceContext trace;
    EventPayload payload;
    IntegrityMetadata integrity;
};
```

- `event_id` 由 producer 预先生成，用于幂等 retry。
- `session_sequence` 由 EventStore 单 writer 在提交时分配；同一 Session 严格递增。
- `task_sequence` 是同一 Task 的稠密投影顺序，可由 append 事务同步维护。
- `cause_control_sequence` 关联导致该事实的串行状态提交。
- Event payload 使用明确类型 ID 和 schema version，未知安全事件不能被当作普通诊断忽略。
- `integrity` 至少包含 header/payload checksum；加密部署还包含 key ID 与认证标签。

Event 分三类：

1. `Critical`：开放外部副作用、消费授权、提交终态或删除请求，必须 durable ack。
2. `State`：状态、Observation metadata、Decision、receipt 和 Verification，默认 durable 后才对
   恢复可见，可批量 group commit。
3. `Diagnostic`：性能、drop、详细 trace，可按有界策略采样，但被丢弃时必须有计数。

关键事实不能因队列满降级为 Diagnostic。

## 4. EventStore 接口

```cpp
enum class Durability {
    Buffered,       // 进程崩溃可能丢失，只允许 Diagnostic
    ProcessCrash,   // 已刷新到 OS/文件边界，满足配置声明
    PowerLoss,      // 采用 fsync/F_FULLFSYNC 等平台能力，仍记录硬件限制
};

struct AppendRequest {
    EventId event_id;
    SessionId session_id;
    std::optional<TaskId> task_id;
    EventPayload payload;
    ControlSequence cause;
    Durability required;
};

struct AppendReceipt {
    EventId event_id;
    SessionSequence session_sequence;
    std::optional<TaskSequence> task_sequence;
    DurableSequence durable_watermark;
    Durability achieved;
};

class IEventStore {
public:
    virtual ~IEventStore() = default;
    virtual Result<AppendReceipt> append(
        const AppendRequest&, const OperationContext&) = 0;
    virtual Result<std::vector<AppendReceipt>> append_batch(
        std::span<const AppendRequest>, const OperationContext&) = 0;
    virtual Result<EventPage> read(
        const EventQuery&, const OperationContext&) = 0;
    virtual Result<StoreRecoveryReport> recover(
        const RecoveryOptions&, const OperationContext&) = 0;
    virtual Result<void> flush(
        Durability, const OperationContext&) = 0;
};
```

`append()` 成功表示 receipt 指定的 durability 已实现。admission、写入内存 buffer 或拿到 sequence
不能提前返回成功。重复 `event_id` 且 canonical payload digest 相同，返回原 receipt；digest 不同
返回 `AlreadyExists/DataLoss` 并停止该 Session 的自动写入。

### 4.1 单 writer 与并发

- 每个 store 实例只有一个 append owner，通过 Executor blocking worker 或等价受管 I/O 路径运行。
- 调用方通过有界 request channel 提交；队满返回 `ResourceExhausted`，不阻塞实时或控制线程。
- 读取使用只读 snapshot/connection，不能观察部分 record。
- Session sequence 分配与 record commit 位于同一事务/record batch。
- group commit 可以合并多个 Session，但每个 receipt 只在其 durability 达成后返回。

### 4.2 参考磁盘布局

生产首期建议使用 SQLite/WAL 或带校验的 segment log；最终选择另立 compatibility evidence。逻辑
布局固定为：

```text
store-root/
├── manifest
├── events/
│   ├── segment-000001
│   └── segment-000002
├── artifacts/
│   ├── objects/ab/cd/<sha256>
│   ├── temp/
│   └── quarantine/
├── projections/
│   ├── checkpoints/
│   └── indexes/
└── recovery/
```

路径由 store 内部根据已验证 ID/hash 生成，外部字符串不能直接拼成 filesystem path。

## 5. ArtifactStore

### 5.1 Descriptor

```cpp
struct ArtifactDescriptor {
    ArtifactId id;                 // digest 派生或独立强 ID
    Sha256 digest;
    std::uint64_t byte_size;
    std::string media_type;
    ArtifactEncoding encoding;
    Sensitivity sensitivity;
    SchemaVersion content_schema;
    std::optional<EncryptionInfo> encryption;
};

class IArtifactStore {
public:
    virtual Result<ArtifactWriter> begin(
        const ArtifactWriteSpec&, const OperationContext&) = 0;
    virtual Result<ArtifactDescriptor> commit(
        ArtifactWriter&, const OperationContext&) = 0;
    virtual Result<ArtifactReader> open(
        const ArtifactDescriptor&, const OperationContext&) = 0;
    virtual Result<ErasureReceipt> erase(
        const ArtifactErasureRequest&, const ErasureContext&) = 0;
};
```

### 5.2 原子发布

写入顺序：

1. 在 store 私有 `temp/` 创建随机临时对象，拒绝符号链接和路径穿越。
2. 流式写入并同时计算 digest、size；应用单对象和总容量上限。
3. flush 到配置 durability，校验格式和声明大小。
4. 以 digest 命名原子 publish；相同 digest 已存在时复用并校验 size/metadata。
5. 更新 artifact metadata/ref transaction。
6. 返回 descriptor，此后 Event 才能引用该 artifact。

崩溃留下的 temp 对象在 recover 时按 age/owner manifest 隔离或清理。公开路径永远不包含半写对象。

### 5.3 引用与 GC

- Event 保存 descriptor，而不是可变文件路径。
- refcount 只是 GC 优化，不能作为唯一事实；可从 Event/Memory metadata 重建。
- retention 到期先写 `ArtifactErasureRequested`，阻止新 Context 引用，再删除索引、payload、wrapped
  key，最后写 completion/tombstone。
- Erasure Pending 时读取默认拒绝，避免部分删除仍发送给模型。
- dedup 跨 tenant 默认关闭；若开启，按 tenant 独立加密 key，删除一个 tenant 的授权不能泄露另一个
  tenant 的存在。

## 6. 副作用提交协议

### 6.1 正常流程

```text
Planner produces ExecutableAction
 -> Policy allow / confirmation consumed
 -> append ActionPrepared (State)
 -> acquire ActionLease
 -> append ActionDispatchStarted (Critical, durable ack)
 -> revalidate task epoch + environment epoch + lease
 -> call Environment/Tool
 -> append ActionReceipt or ActionExecutionUncertain (State/Critical)
 -> release/retain lease according to controller contract
 -> Observe and Verify
```

`ActionDispatchStarted` 包含 action digest、target descriptor、Task/Environment epoch、lease ID、
confirmation ID（如有）和 idempotency metadata。它表示 Mira 即将越过副作用边界，不表示平台已执行。

在 durable ack 与真实调用之间仍存在进程崩溃窗口，因此恢复分类为 uncertain，而不是未执行。这是
有意选择的安全保守性。

### 6.2 Tool 与远程 API

- 只读 Tool 不需要副作用协议，但仍记录 operation/result。
- 可写 Tool 使用同一协议，并在对端支持时传递稳定 idempotency key。
- 对端幂等只能降低重复风险，不能让 Mira 自动把超时当成功；仍需查询/Verify。
- 模型推理通常不是环境副作用，但有费用和隐私影响；使用独立 request event、budget 和 retry key。

### 6.3 连续动作

连续动作在整个 bounded trajectory 开始前写一次 `ActionDispatchStarted`，不为每个 sample 写盘。
Controller 在内存维护 sample/heartbeat counters，并在结束后写汇总 receipt。崩溃或心跳丢失依赖
平台/Host watchdog 释放输入；恢复仍将整段 action 标记 uncertain 并重新 Observe。

## 7. 崩溃窗口矩阵

| 崩溃点 | 恢复可见事实 | 恢复分类 | 默认动作 |
| --- | --- | --- | --- |
| Artifact temp 写入前/中 | 无公开 descriptor | 未产生 Observation artifact | 隔离/清 temp，重新 Observe |
| Artifact publish 后、Event ref 前 | 孤立完整 artifact | 无业务引用 | GC grace 后清理 |
| `ActionPrepared` 前 | 无 action fact | 未规划 | 重新 Observe/Plan |
| `ActionPrepared` 后、dispatch start 前 | 只有计划 | 明确未派发 | 新鲜度允许时重新规划，不直接复用 |
| dispatch start append 未 durable | record 不在 durable watermark | 按未提交处理，但 store recover 报告尾部 | 不调用平台；重新观察 |
| dispatch start durable、平台调用前 | start fact | `ExecutionUncertain` | Observe/Verify，禁止自动重发 |
| 平台调用中/后、receipt 前 | start fact | `ExecutionUncertain` | Observe/Verify，必要时 Human |
| receipt durable、状态提交前 | receipt fact | 明确 receipt | Replay receipt，推进 Verify |
| Verification artifact 后、event 前 | 孤立 artifact | 未验证 | GC 或重新 Verify |
| terminal event durable、API reply 前 | 终态 fact | 已完成 | 返回原 outcome，不重复动作 |

无法分辨的情况一律选择更保守的 `ExecutionUncertain`，不能基于“调用通常很快”猜测未执行。

## 8. Recovery

### 8.1 Store recovery

Runtime 接受 Session 前执行：

1. 校验 manifest、格式版本、加密 key availability 和 store identity。
2. 扫描到最后完整、校验通过的 committed record/batch。
3. 区分未提交尾部、checksum failure、sequence gap 和中段损坏。
4. 未提交尾部可以截断到最后 durable boundary，并保留 recovery report。
5. 中段损坏或 sequence gap 进入只读诊断/quarantine，禁止自动跳过继续写。
6. 清理/隔离 temp artifacts，验证已引用 artifact descriptor。
7. 从 Event 重建 Task snapshot、pending effect、Checkpoint watermark 和索引 lag。

任何自动修复都生成不可变 recovery report，包含原始范围、采取的操作和数据损失估计。

### 8.2 Task recovery

- durable terminal Event 直接恢复终态，不重新运行。
- 非终态 Task 递增 epoch，恢复到 `Observing` 或 `Verifying`，绝不直接 `Acting`。
- 对每个 durable dispatch start 查找 matching receipt；没有 receipt 时加入 `PendingEffect`。
- PendingEffect 必须由本地 predicate、结构化 UI、视觉或 Human 解决后才能考虑同类动作。
- 旧 confirmation、ActionLease、pointer owner 和 Provider opaque continuation 一律不恢复为有效授权。

## 9. Replay

### 9.1 Capability 隔离

Replay 构造不同依赖图，而不是依赖运行时 flag：

- `OfflineReplayEnvironment` 只返回已记录 Observation/receipt，`execute()` 永远返回
  `PermissionDenied`。
- `RecordedModelProvider` 只按 OperationId/RequestDigest 返回记录结果。
- `ReplayToolRegistry` 对副作用 Tool 只返回记录结果，不加载真实实现。
- SecretResolver、网络 client 和平台 input 不注入 Replay Runtime。

即使 Event 被恶意修改为 live action，Replay 也没有 capability 执行。

### 9.2 模式

- `OfflineReplay`：按记录事实重建原状态，验证 reducer 确定性。
- `AnalysisReplay`：允许新 Planner/Verifier 读取历史证据并输出 diff，仍无副作用。
- `LiveReplay`：不属于常规 replay；它创建全新 Task、ID、事件流和确认，不复用旧授权。

## 10. 删除、加密与 Replay 降级

数据删除与完整历史回放不可同时无限保证。Mira 明确采用：

- Event header、类型、时间、digest 和 erasure tombstone 可按审计策略保留。
- 敏感 inline payload 经过字段级 redaction 或 envelope encryption；删除时可销毁 wrapped data key。
- Artifact payload、thumbnail、OCR、embedding、FTS 和缓存都纳入同一 erasure dependency graph。
- 删除完成后 Replay 返回 `EvidenceErased`，不得用摘要或模型猜测重建原内容。
- 法规/宿主要求完全删除 metadata 时，允许 sequence 保留不可解释 gap marker；Replay quality 明确
  `IncompleteByErasure`。

加密 key 由宿主 key provider 管理。Event 中只保存 key reference；key 缺失进入受限读取，而不是
把密文当作普通 payload。

## 11. 背压、容量与故障

| 故障 | 默认行为 |
| --- | --- |
| Event request queue 满 | 拒绝新普通操作；Critical 失败导致副作用不派发 |
| Artifact quota 满 | Observation 标记降级；需要视觉证据的 Action 不继续 |
| 磁盘只读/满 | 停止新的不可逆副作用，允许安全释放和诊断 |
| Artifact digest mismatch | quarantine，Event 不得引用，报告 DataLoss |
| Event duplicate ID/different digest | Session 写入隔离，报告 DataLoss |
| flush timeout | 不返回 durable success；Task pause/fail，禁止越过副作用门禁 |
| projection/index lag | 记录 lag，读取回退权威 Event/record；不影响已提交事实 |
| encryption key unavailable | 不解密、不发送模型，进入受限诊断或请求宿主恢复 |

配额至少按 runtime、tenant、session 和单 artifact 配置。清理不能删除仍被活动 Task 的 minimum
safe context 引用的数据。

## 12. Executor 映射与关闭

- append owner、fsync、segment rotation 和 artifact stream I/O 使用 Executor blocking worker。
- checksum、压缩、加密和投影等有限 CPU 工作用普通 `submit_auto()`，保留 future。
- retention/GC 使用 soft periodic timer；重建是显式 maintenance task。
- 控制面从不等待 I/O，只接收 append/publish completion。
- Store worker 关闭前停止 producer，完成已接纳 Critical write，放弃可重建低优先级 projection 并记录。
- `wait_for_completion_ex()` 不覆盖 blocking worker；ExecutionSupervisor 必须显式 stop/inspect handle。

## 13. Schema migration

- manifest、Event envelope、每类 payload、Artifact metadata 和 projection 分别版本化。
- migration 前创建可验证 backup 或使用 copy-on-write 新 segment；失败不覆盖原数据。
- downgrade 不隐式进行；旧 binary 遇到新 major 只读拒绝。
- 大型 migration 可取消，但取消点只能位于事务/segment 边界。
- migration 完成写 `StoreMigrated` 和前后 digest/版本；首个随机 Task 不触发 migration。

## 14. 测试与验证

### 14.1 确定性与故障注入

- duplicate event same/different digest、sequence 并发、group commit。
- 每个写入 syscall/transaction 边界的 crash injection 和 reopen。
- temp artifact、publish、metadata、Event ref 之间的孤儿处理。
- disk full、read-only、short write、corrupt tail、middle corruption、missing key。
- 10,000 次随机命令/event/restart 后 snapshot 与无崩溃 reference reducer 一致。

### 14.2 副作用安全

- durable start 失败时 Fake Input 的调用次数为零。
- start durable 后任意崩溃恢复均不自动重复 execute。
- receipt durable 后崩溃可以恢复到 Verify，不调用 execute。
- continuous action crash 触发 Host watchdog/release contract，恢复保留 uncertain effect。

### 14.3 Replay 与删除

- Offline/Analysis Replay 没有真实 network/input/tool dependency。
- 恶意 Event 无法触发 live action。
- Erasure 后 payload、artifact、OCR、embedding、FTS 和缓存均不可读。
- Replay 对已删除证据报告明确 quality，不伪造完整性。

## 15. 尚待兼容性验证的实现选择

- SQLite/WAL 与自定义 segment log 的首期组合。
- `ProcessCrash`/`PowerLoss` 在 Android、Linux、Windows 上的精确 durability mapping。
- 每 tenant 独立 store、同 store 分区或 envelope encryption 的部署默认值。
- 大型 screenshot 的压缩编码和 streaming encryption 实现。

这些选择不改变 intent logging、durable ack 门禁、内容寻址、损坏不静默跳过和 Replay capability
隔离原则。

## 16. 关联文档

- [核心公共契约与状态机](core_contracts_and_state_machine.md)
- [Context 与 Memory](context_and_memory_design.md)
- [威胁模型与权限确认](../security/threat_model_and_confirmation.md)
- [M1 核心契约里程碑](../plans/m1-core-contracts.md)

