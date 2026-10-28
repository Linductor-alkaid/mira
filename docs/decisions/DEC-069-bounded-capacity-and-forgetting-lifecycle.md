# DEC-069：有限容量资源模型、分层遗忘与会话退出生命周期

> 状态：Proposed
> 日期：2026-10-28
> 负责人：Mira Maintainers
> 冻结里程碑：M31（容量准入与归档回收轮；本决策先于实现冻结数值档位）
> 替代/被替代：无
> 来源需求：[Issue #111（`MIRA-20261027-001`）](https://github.com/Linductor-alkaid/mira/issues/111)
> §6（有限容量、主动遗忘与会话归档清理）

## 背景与问题

### issue #111 §6 的三条硬性否定

1. **遗忘不能只等于降低召回分数，归档也不能变成另一个无限增长的数据池。**
2. **仅有单次清扫上限或请求 token 上限不能证明持久化总量有界。**
3. **分别报告逻辑删除、可重用空间与实际文件占用/峰值**——不能仅以「删掉若干行」
   证明磁盘预算达标。

本决策冻结**资源模型与资格规则**（哪些数据计入、何时淘汰、什么不许淘汰、
归档需要满足什么、怎么证明真的回收了），**不冻结具体数值**。数值是部署档位参数，
须按目标设备验证后冻结。

### 现状盘点（2026-10-28 静态审计）

**已有的限额，逐层列举**：

| 数据层 | 现有上限 | 位置 | 上限的**真实语义** |
| --- | --- | --- | --- |
| EventStore（内存） | `max_events = 100'000` | [`include/mira/event_store.hpp:116`](../../include/mira/event_store.hpp#L116) | 只约束**内存索引**；满时 `ResourceExhausted`，**拒绝而非淘汰** |
| EventStore（文件） | 同上 | [`include/mira/event_store.hpp:136`](../../include/mira/event_store.hpp#L136) | **磁盘 `events.log` 无上限**（永不截断）。`max_events` 只约束**内存索引**；索引满时 `memory.append` 返回 `ResourceExhausted`，`FileEventStore::append` 随即把 store **永久置 `read_only`** 并返回 `DataLoss`（[`src/storage/event_store.cpp:392-397`](../../src/storage/event_store.cpp#L392-L397)）——而该事件此前**已 durable 写入日志** |
| ArtifactStore（内存） | `max_total_bytes = 256MB`、单件 `max_bytes = 16MB` | [`include/mira/artifact_store.hpp:71,126`](../../include/mira/artifact_store.hpp#L71) | 拒绝而非淘汰 |
| ArtifactStore（文件） | `max_total_bytes = 2GB` | [`include/mira/artifact_store.hpp:142`](../../include/mira/artifact_store.hpp#L142) | 约束的是**内存副本**（[`src/storage/artifact_store.cpp:290-291`](../../src/storage/artifact_store.cpp#L290)）；磁盘对象在内存提交失败时会被删除（`:352-357`），但 **`manifest.log` 每次提交追加且擦除不删 manifest 行**（`:358-366`、`:418-447`）、**`tombstones/` 每件一文件**，二者无上限；启动时**全量载入内存**（`:203-287`） |
| Memory store（SQLite） | `keep_versions_per_record = 2`、`purge_horizon`、`max_vector_scan = 1'024`、`max_per_scope_kind = 3` | [`include/mira/sqlite_memory_store.hpp:16-34`](../../include/mira/sqlite_memory_store.hpp#L16) | **全部是单次操作上限**；`validate()` 里**没有** `max_records`/`max_bytes`（[`src/storage/sqlite_memory_store.cpp:279-292`](../../src/storage/sqlite_memory_store.cpp#L279-L292)） |
| Checkpoint store | `max_checkpoints_per_task = 64` | [`include/mira/state_store.hpp:40`](../../include/mira/state_store.hpp#L40) | **每 task** 上限；**task 总数无上限** |
| TaskCheckpoint 内容 | `max_recent_actions=16` 等 5 项 | [`include/mira/task_checkpoint.hpp:135-140`](../../include/mira/task_checkpoint.hpp#L135-L140) | 单对象内容上限 |
| ActionJournal | 无 | [`include/mira/action_journal.hpp:38-58`](../../include/mira/action_journal.hpp#L38) | 进程内 `unordered_map` 无界 |
| Conversation/Working Context store | 每 session 8 条环形 | [`include/mira/context_working_context.hpp:197`](../../include/mira/context_working_context.hpp#L197) | **每 session** 上限；**session 总数无上限**；纯内存 |
| `ContinuationCache` | 无 | [`include/mira/provider_continuation.hpp:91-99`](../../include/mira/provider_continuation.hpp#L91) | 无 size 上限；`invalidate_all` 只打标记不删 |
| WorldState 投影 | `max_entities=128`、`max_recent_changes=64` | [`include/mira/world_state.hpp:87-88`](../../include/mira/world_state.hpp#L87) | **全仓唯一的价值驱动淘汰**（见下） |

**完全没有上限的 append-only 层**：

- `memory_mutations`（**DEC-058 幂等台账**）：每次成功 `apply()` 追加一行
  （[`src/storage/sqlite_memory_store.cpp:80-86`](../../src/storage/sqlite_memory_store.cpp#L80)、
  `:794-812`）；`compact()`、`sweep()`、`erase_locked()` **都不碰它**。
- `memory_erasure_log`：每次擦除一行，只增不减
  （[`src/storage/sqlite_memory_store.cpp:94-107`](../../src/storage/sqlite_memory_store.cpp#L94)）。
- `memory_provenance`：每记录携带的事件数无上限
  （[`src/storage/sqlite_memory_store.cpp:76-79`](../../src/storage/sqlite_memory_store.cpp#L76)）。
- `memory_scope_holds`：每 scope 一行，**scope 总数无上限**。
- `FileEventStore.events.log`、`FileArtifactStore.manifest.log` 与每工件一个的
  `tombstones/` 文件；`FileEventStore` 的**内存索引**满时会把 store 永久置为
  `read_only` 并返回 `DataLoss`
  （[`src/storage/event_store.cpp:392-397`](../../src/storage/event_store.cpp#L392-L397)）。

**SQLite 空间回收缺失**：`src/storage/sqlite_support.cpp:39-81` 只有
`journal_mode=WAL` 与只读模式的 `query_only`——**没有** `auto_vacuum`、
**没有** `wal_autocheckpoint`、**没有** `page_size`、**没有** `VACUUM`。
删除记录后**文件不会缩小**，只有 `page_count` 诊断（只观测，不参与限额判定）。
⇒ **「回收」在磁盘上不可证明**——这正是 issue #111 §6 第三条否定的对象。

**「遗忘」当前到底是什么**：Memory 的清扫是**纯时间驱动**的——
Phase 1 `ORDER BY expires_at`（最早的先清）、Phase 3 `ORDER BY updated_at`
（最老的先清）
（[`src/storage/sqlite_memory_store.cpp:1799`](../../src/storage/sqlite_memory_store.cpp#L1799)、
`:1856`）。它**不读** `confidence`、`verification`、`sensitivity`、命中次数或最近访问时间；
`MemoryRecord`（[`include/mira/memory_contracts.hpp:157-183`](../../include/mira/memory_contracts.hpp#L157)）
里**根本没有** `accessed_at` / `hit_count` 字段。
per-kind TTL 表（[`src/context/memory_contracts.cpp:306-354`](../../src/context/memory_contracts.cpp#L306)）
是**静态常量**（7d/30d/90d/180d），与重要度、命中频率无关。

> **因此 issue #111 §6 的第一条在当前代码里成立且比预想更尖锐**：
> 现在的「遗忘」**就是** TTL 到期即物理删除，与召回排序**完全解耦**；
> 一条 `HumanConfirmed`、被频繁命中的记录，只要 kind 的 TTL 到了就会被删。
> 召回降权（`RetrievalWeights`，[`include/mira/sqlite_memory_store.hpp:41-48`](../../include/mira/sqlite_memory_store.hpp#L41)）
> 与遗忘是两条互不相干的路径，**没有任何一层在做「价值驱动的分层淘汰」**。

**会话退出完全没有存储语义**：`MiraRuntime::close_session`
（[`src/runtime/runtime.cpp:583-627`](../../src/runtime/runtime.cpp#L583-L627)）只做状态迁移、
取消未终态 task、interrupt environment——**不触碰任何持久化数据**。
`MemoryScopeKind::Session` 已存在且被 per-kind 白名单允许
（[`src/context/memory_contracts.cpp:312,319,326,332,344,350`](../../src/context/memory_contracts.cpp#L312)），
**数据模型已支持按 session 分区**——这是实现归档的有利条件，但编排完全空白。

**清扫没有周期宿主**：设计文档承诺 `Retention/GC | soft periodic timer`
（[Context 与 Memory 设计 §17](../design/context_and_memory_design.md)），
但 `src/` 中**没有任何 `submit_periodic` 调用**；
`ContextMemorySupervisor::schedule_global_retention_sweep`
（[`include/mira/context_memory_supervisor.hpp:189`](../../include/mira/context_memory_supervisor.hpp#L189)）
只返回一个 future，**全仓无生产调用点**（`examples/` 里的宿主只 open store，从不调
`compact`/`sweep`）。

### 关键区分（最容易被混淆的一点）

Mira 现有两套「预算」，**互不相干**：

- **Context 预算**（[Context 与 Memory 设计 §9](../design/context_and_memory_design.md)）是
  **单次模型请求**的 token 预算：`ContextLimits{context_window_tokens=128000,
  reserved_output_tokens=16384, safety_margin_tokens=1024, trim_watermark=0.70,
  checkpoint_watermark=0.85, hard_watermark=0.95}`
  （[`include/mira/context_contracts.hpp:47-64`](../../include/mira/context_contracts.hpp#L47)）。
  Layer 0 是**单次请求**的纯函数
  （[`include/mira/context_manager.hpp:105-116`](../../include/mira/context_manager.hpp#L105)），
  **不跨任务、不跨主体、不跨请求**。
- **容量预算**（本决策引入）是**持久化与驻留资源**的总量上限，跨数据层、跨时间。

issue #111 §6 否定「请求 token 上限不能证明持久化总量有界」正是指这两者不可互证。
本决策**不修改** Context 预算模型，也不把容量预算塞进 Context 装配路径。

### 缺口清单

| # | 缺口 | 层级 |
| --- | --- | --- |
| C1 | 无跨数据层的**统一总量准入**：每个 store 各有各的上限，加起来没有上界；无 `CapacityExceeded` 错误码（[`include/mira/memory_contracts.hpp:431-440`](../../include/mira/memory_contracts.hpp#L431)） | P0 |
| C2 | `memory_mutations` 幂等台账**永不回收**（compact/sweep/erase 都不碰它） | P0 |
| C3 | `memory_erasure_log` / `memory_provenance` / `memory_scope_holds` 无上限 | P0 |
| C4 | EventStore 磁盘日志无上限 + 索引满后**永久只读** | P0 |
| C5 | ArtifactStore 磁盘 `objects/` + `manifest.log` + `tombstones/` 无上限；启动全量载入 | P0 |
| C6 | SQLite 无空间回收（`auto_vacuum`/`wal_autocheckpoint`/`VACUUM` 全无）⇒ 回收不可证明 | P0 |
| C7 | 无**价值驱动的淘汰**：无 importance/hit_count/accessed_at 字段，清扫只按时间排序 | P1 |
| C8 | per-kind TTL 是静态常量，无法表达「分层遗忘」；TTL 到期即物理删除 | P1 |
| C9 | scope 无配额字段；无「每 scope 多少条/字节」 | P1 |
| C10 | 无**按主体（tenant/principal）份额** | P1（**Executor 缺口**，`EXE-20261028-001`） |
| C11 | 无**软/硬水位**与回收预留 | P1 |
| C12 | 无**会话驻留数上界**与归档层 | P1 |
| C13 | 无**保护集合**规则 | P1 |
| C14 | 无**周期清扫宿主**（设计承诺的 timer 不存在） | P1 |
| C15 | 无**可证明回收度量**（逻辑删除/可重用空间/实际占用/峰值） | P2 |
| C16 | 回收失败与**隐私删除（Erasure Pending）**没有分别结算的规则 | P2 |
| C17 | 存储占用不可观测（只有 `page_count` 诊断） | P2 |

### 与 Mirage 既有前提的冲突

Mirage DEC-101 曾默认 `max_sessions=0`（不限制会话数），路线图曾规定
「遗忘只是激活降低、原始记录仍可检索」。维护者已以 **Mirage DEC-119** 以「有限容量」
取代这两个前提。**Mira 侧尚未迁移、未执行任何数据清理**，本决策是迁移的前提。

## 决策

### 1. 引入分层容量预算 `CapacityBudget`

```cpp
struct CapacityBudget final {
    // 总量上限（字节或条目，按层计量）。
    std::size_t total_bytes = 0;
    // 按主体的份额上限；未列出的主体走默认份额。
    std::map<TenantId, std::size_t> tenant_share_bytes;
    // 受管理 / 驻留会话数上限（名额，不是字节）。
    std::size_t max_resident_sessions = 0;
    // 归档的数量 / 字节 / 保留期上限（归档有自己的额度，不占驻留名额）。
    std::size_t max_archived_sessions = 0;
    std::size_t max_archive_bytes = 0;
    std::chrono::seconds archive_retention{0};
    // 软水位：达到后主动巩固/回收。
    double soft_watermark = 0.75;
    // 硬水位：达到后有界背压/拒绝。
    double hard_watermark = 0.90;
    // 回收与取消所需的预留，不得被普通数据占用。
    std::size_t reclamation_reserve_bytes = 0;
};
```

> **2026-10-28 修订（承接 Mirage DEC-119）**：本结构体原先只有字节与水位维度，
> 而「受管理/驻留会话数、归档数量/字节/保留期」只出现在 §10 的数值清单里，
> **结构上无处承载**。上述四个字段补齐后，§1/B1「任一数据层的占用都计入一个总量预算」
> 才在会话与归档维度上成立。

冻结规则：

- **B1（总量优先）** 任一数据层的占用都必须计入**一个**总量预算。不存在「不在预算内」
  的数据层——清单见 §2。
- **B2（主体份额）** 每个租户有份额上限；单主体触及份额上限时**限制新工作**，
  不得挤占其他主体的份额，也不得自动扩容总量。
- **B3（水位分级）** 软水位下主动巩固/回收；硬水位下**有界背压/拒绝**并报告阻塞原因。
  跨过硬水位时不得「默认扩大容量或静默丢数据」。
- **B4（预留不可占用）** 回收与取消所需的预留空间**不得**被普通数据占用。
  磁盘满或回收失败时必须报告阻塞原因，**不得**默认扩容。
- **B5（峰值计入）** 预算必须计入摘要生成、索引重建、数据库/WAL/临时文件的**峰值**占用，
  不能只算稳态。

> **Executor 边界（B2 的实现落点）**：按主体的份额准入**必须作用在 Executor 的接纳点**，
> 否则只能做「先拒后放」的二段式，破坏 Executor「恰好一次释放」与
> 「future 就绪前计数已释放」的不变式。kairo 当前的 admission 是
> **实例级单值计数器**（`max_in_flight_tasks`，无 key 维度），
> `IScheduler` 注入点**只被 `submit_auto` 咨询**（其余提交路径绕过），
> 且 `TaskRouter::Request`/`TaskOptions` **没有任何主体身份字段**。
> ⇒ 这是**确认的 Executor 能力缺口**，已按 [AGENTS.md](../../AGENTS.md) 登记为
> [`EXE-20261028-001`](../executor_feedback/ledger.md)，并于 2026-10-28 提交上游
> [kairo#222](https://github.com/Linductor-alkaid/kairo/issues/222)。
>
> **2026-10-28 上游已交付**：kairo PR
> [#223](https://github.com/Linductor-alkaid/kairo/pull/223) 在同一接纳点实现了分主体
> 份额准入（`set_subject_max_in_flight_tasks` / `TaskOptions::subject` /
> `TaskBuilder::subject()`），冻结设计
> `third_party/kairo/docs/design/subject_bucket_admission.md`，
> 台账状态转 `Accepted`（**尚未发布含该修复的 tag**，Mira 未迁移）。
>
> M31 的实施约束随之变为三条：
>
> 1. **消费原生能力，不再自建份额判定**：版本可用后**必须**直接使用 Executor 原生
>    分桶，**移除**台账记录的「嵌套预过滤」临时边界，**不得**与原生能力叠加
>    （否则出现双重份额判定）。
> 2. **迁移提交形态**：上游分桶键只在带键形态上被消费。Mira 现有 11 处
>    `submit_auto` 调用**全部是裸 lambda 形态**，不受分桶覆盖；受分桶约束的提交
>    **必须**迁移到带键形态。上游 PR #224 已为此新增裸形态重载
>    `submit_auto(SubjectKey, F&&, Args&&...)`（用法
>    `executor.submit_auto(kairo::subject(key), fn)`，
>    `include/kairo/task_options.hpp:385-394`），与裸形态语义逐位一致，
>    因此**迁移是每处前插一个实参**，无需绕道 `TaskBuilder`。
>    `SubjectKey` 构造**刻意 explicit**：`submit_auto("key", fn)` 会**编译失败**
>    （而非把键静默错绑为 callable），这把「配了份额却不生效」的集成陷阱
>    从运行期搬到了编译期。
> 3. **记录两处已知行为**：热更新**弱一致**（与更新并发交错的已开始接纳可按旧快照
>    完成，瞬时超额有界且随终态自愈）；代际语义（`set(key, 0)` 解除后同名重配
>    **从 0 起计**，跨代在途计数**不承诺可观测**）。
>    **不得**宣称「份额绝不超额」或把跨代计数缺席读作泄漏。
>
> 无论是否已消费上游能力，M31 都**不得**因此引入私有线程池或第二套调度器。
>
> **消费版本**：[DEC-073](DEC-073-kairo-master-pin-subject-quota.md) 已把 pin 升到
> `78c6f557`（`v0.7.2-14-g78c6f55`，上游未发布 master）。本节的三条实施约束
> 以该版本的上游公开面为准：`set_subject_max_in_flight_tasks` 等运行期接口与
> `kairo::subject(key)` 裸形态带键重载均已可用。
>
> **上游后续调整已落实（PR #224）**：下游在上游 #222 提出的三点后续调整
> （A 裸形态带键入口 / B 键脱敏约定 / C 跨代低报警示）已由上游 PR
> [#224](https://github.com/Linductor-alkaid/kairo/pull/224) 全部落实
> （设计文档「修订（2026-10-28，下游反馈）」R-1/R-2/R-3）：
>
> - **R-1**：新增 `submit_auto(kairo::subject(key), fn)` 裸形态带键重载
>   ＋ `SubjectQuotaStatus::admitted_total` 诊断面。**诊断配方**：
>   份额已配置但 `admitted_total` 停在 `0` = 键未流过接纳点。
>   这条诊断面应当成为 M31 的**常规验收项**，而不是只看在途计数。
> - **R-2**：键脱敏约定成文——键被逐字复制进拒绝消息、`ExecutorFailureEvent::subject`
>   与状态快照，**必须**是不可识别的派生标识（内部编号/哈希/代号），
>   不得使用账号、租户名、外部 ID 原文；键随主体生命周期增删的清理责任在调用方。
>   ⇒ **Mira 侧必须以派生标识作键**，不得直接使用 `TenantId`/`UserId` 原文。
> - **R-3**：跨代低报提升为 API 层 ⚠ 显式注意事项与监控口径
>   （**超额上界 + 自愈时间**）；上游**不**暴露 `retired_in_flight`
>   （维持永久 graveyard 的成本取舍）。⇒ **低报本身按上游裁定保留**，
>   本决策 §7/X5 的三种度量口径不变，但配额判定**不得**把
>   `get_subject_in_flight_submissions()` 当精确值使用。
>
> **仍不带键的上游路径**（上游声明不做静默）：公开 `submit()`、
> `submit_after` 裸形态、`submit_cancellable*`、`submit_on*`、`submit_batch*`，
> 以及定时/周期、GPU、realtime、Blocking I/O、lockfree。
> M31 的受分桶约束提交点**必须**使用上游新增的裸形态带键重载。

### 2. 计入范围（闭集）

下列数据**全部**计入 §1 的总量预算：

1. Event 记录（含 header、载荷、墓碑/gap marker）
2. Artifact payload 与 thumbnail/OCR 等派生产物；`manifest.log` 与 tombstone 文件
3. Memory 记录、版本历史、provenance
4. Working Context 快照与 TaskCheckpoint
5. 附件/截图引用与实体引用
6. 向量索引（可重建但**占空间**，必须计入）
7. 会话内对话投影（conversation log）与 provider 不透明状态缓存（`ContinuationCache`）
8. **输入幂等台账**（DEC-068 §4）与**主体注册表投影**
9. `memory_mutations` / `memory_erasure_log` / `memory_scope_holds` 等审计与台账层
10. ActionJournal 进程内状态（虽可重建，但计入**驻留**预算）
11. 摘要、索引、引用与受管备份

### 3. 分层遗忘：四层，各有触发与退出条件

| 层 | 机制 | 触发 | 退出 |
| --- | --- | --- | --- |
| L1 | **召回降权** | 记录长期未被实际使用 | 被实际使用即恢复；**不释放空间** |
| L2 | **去重 / 抽象巩固** | 存在语义等价或可合并的记录 | 合并完成后旧记录进入 L3/L4 |
| L3 | **有限归档** | 会话满足归档资格（§5） | 保留期到期或归档总量超限 → L4 |
| L4 | **物理删除** | 保留期到期 / 归档超限 / 价值评估低于阈值 | 不可逆；原文不可回查 |

冻结规则：

- **F1（价值裁决，不是 LRU）** 候选策略综合**经验证的**重要性、时效、实际用途、冗余、
  可重获性、敏感性与保存成本。**纯 LRU、年龄或未经验证的模型评分不得作为全部规则**，
  也**不**在本决策冻结具体公式——公式随 M31 立项冻结并须有确定性数据集与评估。
- **F2（冻结的淘汰序先例）** 仓库已有唯一正确的价值驱动淘汰先例：
  WorldState 的冻结淘汰序 `stale-first → earliest last_seen_at → lowest confidence →
  ElementRef canonical JSON 升序`
  （[`include/mira/world_state.hpp:331-340`](../../include/mira/world_state.hpp#L331)）。
  M31 的记忆层淘汰序**应当沿用同一形状**（稳定、可重放、无随机），并同样冻结。
- **F3（失败安全顺序）** 采用「**先验证并可靠提交摘要，再清理旧内容**」。
  摘要未验证或未可靠提交时**不得**清理原文。
- **F4（摘要可追溯）** 摘要保持来源与有效性标记，防止歪曲事实、自我强化与
  「层层摘要无限积累」；摘要链必须有深度上限。
- **F5（不可伪造证据）** 原文被清理后**明确不可回查**，不得用摘要或模型猜测重建原内容
  （与[事件与崩溃一致性设计 §10](../design/event_artifact_crash_consistency.md) 一致）。
- **F6（L1 不是遗忘）** L1 只是召回降权，**不释放空间**，**不计入**「已遗忘」。
- **F7（TTL 不是价值）** per-kind TTL 降级为**四层之一**（L4 的一条触发），
  **不得**继续作为唯一生命周期驱动力；TTL 到期**不**直接等于物理删除。

### 4. 保护集合（Protection Set）

保护集合 = 不得按普通情景记忆淘汰的最小必要状态：

- 活动/挂起任务的最低必要状态
- 未决副作用（pending side effects）
- 待批准 / 待澄清事项
- 当前安全与授权约束
- **未兑现的承诺实体**（[DEC-070](DEC-070-long-term-memory-behavior-contract.md) §4）——
  承诺是受配额保护的独立实体，不按普通情景记忆淘汰。
- **已结算的失败轮次正文**（同上 §5/C5）——宿主后置核验判失败后，
  模型已产生的最终答案**仍保留为正文**；它不是「低价值内容」，
  淘汰时须保留「正文存在但核验未过」的语义标记。

冻结规则：

- **P1（同样需要预算）** 保护集合**不是**预算豁免——它同样占用 §1 的总量预算，
  并且**有自己的上限**。
- **P2（保护集合满则限制新工作）** 保护集合已满时**限制新工作**（背压/拒绝并报告），
  **不得**无限增加保护项，也**不得**把普通数据提格为保护项来绕过上限。
- **P3（用户固定记忆有配额）** 用户显式固定的重要记忆享有**有限配额**，
  **不是**无限期容量豁免。
- **P4（不得静默丢弃）** 达到硬水位且无法回收时，报告阻塞原因与归属，
  **不得**静默丢数据、不得默认扩容。
- **P5（保护判定的来源）** 保护集合的成员**只能**来自既有可验证事实源：
  活动 Task 的 `TaskSnapshot`、未决副作用登记、待确认/澄清事项、当前 grant 集合。
  **不得**引入第二份「什么重要」的判定源。

### 5. 会话退出生命周期（高层协调状态）

这是**待设计的高层状态**，**不直接修改**既有 `SessionState`
（[核心契约设计 §6.2](../design/core_contracts_and_state_machine.md)）：

```text
Managed ──► Executable ──► Converging ──► Archivable ──► Archived ──► Purged
   │            │              │              │             │
   └────────────┴──────────────┴──────────────┴─────────────┴──► (回到 Managed/Release)
```

| 状态 | 含义 | 名额 |
| --- | --- | --- |
| `Managed` 受管 | 已登记，占用受管会话名额 | 占用 |
| `Executable` 可执行 | 有活动或可恢复的工作 | 占用 |
| `Converging` 收敛中 | 等待在途工作、租约与更新结算 | 占用 |
| `Archivable` 可归档 | 满足 §6 归档资格 | 占用（直到归档完成） |
| `Archived` 已归档 | 已释放受管名额，保留有限摘要/元数据与必要工件 | **不占用** |
| `Purged` 已清理 | 原文已物理删除 | 不占用 |

冻结规则：

- **L-1（不删既有状态机）** 这套状态由协调层持有，**不**改写 `SessionState`；
  二者是不同层的概念。
- **L-2（名额与归档绑定）** 归档**释放受管/驻留会话名额**；归档有自己的总额与保留期，
  超限或到期后可清理（→ `Purged`）。
- **L-3（终态不复活）** 终态任务**不复活**；后续工作可**关联旧摘要新建任务**，
  而不是复活旧任务。
- **L-4（可解释）** 同一入口仍能解释「目前保留了什么、什么已被清理、什么不可恢复」。

### 6. 归档资格（前置条件全部满足才可归档）

- 全部关联任务处于终态
- 无在途工具、无待处理输入/结果、无待确认/澄清
- 无未决副作用、无未释放租约
- 上下文/记忆更新已结算（无未结算的提交）
- 必要结果已可靠记录（durable ack）
- 满足明确的交付/过期策略（后台结果长时间无人读取须有有限、明确的交付过期规则）

冻结规则：

- **Q1（不是 Task 终态）** 单个 Task `Completed` 或「空闲很久」**都不等于**可直接删除。
- **Q2（清理前重查）** 清理前必须**重查版本与存活引用**，处理「新输入重新关联旧会话」的竞态。
- **Q3（不删别人需要的证据）** 删除前检查共享工件/记忆的所有权与存活引用；
  清理一个会话**不得**删掉其他工作仍需的证据。
- **Q4（恢复按需）** 恢复/查询归档**按需**且占用接纳预算，**不**在启动时全量驻留。
- **Q5（按 session scope 清理）** 记忆侧清理走既有 `IMemory::erase(ErasureRequest{scope})`
  与 `MemoryScopeKind::Session`（数据模型已支持），**不新造**会话清理路径；
  跨 store 的会话清理**必须**经既有 `IMemory` 擦除路径
  （[Context Curator 设计 §13](../../docs/design/context_curator_design.md) 已冻结此纪律）。

### 7. 跨存储一致性与可证明回收

- **X1（复用既有能力）** 复用既有 TTL/retention/erase、checkpoint 与 ArtifactStore 能力
  （[DEC-062](DEC-062-memory-retention-sweep.md)、
  [事件与崩溃一致性设计 §5.3](../design/event_artifact_crash_consistency.md)）。
  **新增的是**总容量准入、价值/压力驱动的遗忘、跨会话归档协调。
- **X2（分别结算）** **容量回收失败**与**用户隐私删除**分别结算：
  不得借回收解除 `Erasure Pending` 的 hold（既有纪律：记录级清除与保留期失败
  **都不**释放 hold），也不得把归档当成「忘掉了」。
- **X3（幂等、可取消、可恢复）** 跨 store 清理必须幂等、可取消/可恢复，
  覆盖中途崩溃、迟到写入与迁移。
- **X4（墓碑有上界）** 删除记录、摘要、墓碑与回收日志**自身也有上界**；
  定义过期引用与旧输入重放行为，**防止删除后复活**。
- **X5（三种度量分开报）** 分别报告**逻辑删除**、**可重用空间**与
  **实际文件占用/峰值**。三者不得互相替代。
  ⇒ 落地前提是 SQLite 空间回收能力（`auto_vacuum=INCREMENTAL` + 定期
  `incremental_vacuum` + WAL checkpoint 策略）与 EventStore/ArtifactStore 的
  **磁盘侧**占用统计，二者当前**都不存在**。
- **X6（隐私删除后不可重建）** 隐私删除后**不得**从摘要、索引、引用或受管备份重建正文；
  摘要/索引/引用/备份的生命周期与原文一起裁决。
- **X7（不复活）** 清理后旧输入重放不得复活已删除的内容；台账过期后的旧请求
  按 DEC-068 §4 的窗口外语义处理。
- **X8（存储侧必须先可观测）** 在能报告「实际文件占用」之前，
  **不得**宣称容量达标；`page_count` 诊断不足以支撑该声明。

### 8. 容量预留与并发接纳一致

容量预留必须与并发接纳**一致**：同一份预算同时约束「新工作是否接纳」与
「数据是否落盘」。不允许「先接纳、后超限」，也不允许「先落盘、后拒绝」。

冻结的实现纪律（沿用 `BudgetLedger` 的预留—结算模式，
[`include/mira/model_budget.hpp:79-113`](../../include/mira/model_budget.hpp#L79)）：

- 接纳前**预留**（reserve），落盘后**结算**（reconcile）；未结算的预留**保留**，
  不被当作零消耗清掉。
- 预留失败以稳定错误码（`ResourceExhausted`）显式返回，**不得**静默降级。

### 9. 周期清扫的 Executor 承载（冻结）

M31 必须交付设计文档承诺但**当前不存在**的清扫宿主：

- 周期驱动：**不得**用 `submit_periodic`（非 cancellable 变体）的 tick
  （其 tick **无取消状态**，只能阻止后续 tick、无法取消在途 tick）。
  两条可选路径（见 [kairo API](https://github.com/Linductor-alkaid/kairo)）：
  (a) `submit_periodic_cancellable` + 每次 tick 独立 `StopToken`；
  (b) `submit_delayed` **重装链**——重装发生在上一次 tick 完成之后，
      **天然满足「不重叠」**，且是 Mira 已在用的范式
      （[`src/workflow/workflow_runtime.cpp:1348-1353`](../../src/workflow/workflow_runtime.cpp#L1348)，
      退出守卫 `:1414-1434`）。**建议 (b)**。
- 周期 tick **不受** `max_in_flight_tasks` 约束，过载时无总量背压
  ⇒ 清扫必须自带过载让步机制（对应设计 §17.1「GC/重建：在过载时让位于 Task 热路径」）。
- `submit_delayed`/`submit_periodic` 派发被拒时**丢当前 tick 且不重试**
  ⇒ 必须自建「下一趟再来」的收敛策略；既有 `MemoryCompactionResult::more_pending`
  语义正好承担此角色（[DEC-062](DEC-062-memory-retention-sweep.md)）。
- 「不重叠」由 Mira 侧保证（串行化），**不**登记为 Executor 缺口：
  kairo 已提供通用串行原语（`SerialExecutionContext`）与通用周期原语（timer），
  缺的只是二者的直接组合，属**应用层串行化需求**。

### 10. 数值档位

本决策**不冻结数值**。M31 立项时冻结部署档位（至少：桌面常驻、移动端受限、
受限嵌入式），每个档位给出：总字节、分主体份额、软/硬水位、回收预留、
受管/驻留会话数上限、归档数量/字节/保留期、并行任务/输入上限、单对象大小上限、
进程内缓存上限，并说明摘要生成、索引重建与 WAL/临时文件的峰值计入方式。

数值须经目标设备验证后冻结；**未经测量不得**写入默认值并宣称为已验证。

> **2026-10-28 修订（承接 Mirage DEC-119 的责任分工）**：本节原先把数值全部归于
> Mira 侧「M31 立项时冻结」。Mirage DEC-119 明确「Mirage 提供**部署预算配置、
> 存储/平台测量**与产品呈现」——Mirage 是唯一知道目标设备实际存储与负载的一方。
> 因此责任落表为：
>
> | 项 | 责任方 | 交付物 |
> | --- | --- | --- |
> | 档位骨架、计量口径、验收门禁 | **Mira**（本决策 §1–§2、§10） | 冻结的契约与门禁 |
> | 具体数值（字节/条目/会话数/保留期） | **Mirage 在目标设备实测后回填** | 测量报告 + 回填值 |
> | 双方共同冻结 | 双方 | 冻结稿 + `budget_version` |
>
> 若完全由 Mira 侧独立测量冻结，会与 Mirage 的分工冲突。
> **未经 Mirage 实测回填的数值一律视为暂定默认值**，不得写入产品默认。

## 备选方案

- **只加 per-store 上限就宣称总量有界**：否决。issue #111 §6 明确否定；
  各 store 上限之和不是总量上界。
- **把容量预算并入 Context 预算模型**：否决。两者作用域不同（请求期 token vs
  持久化字节），混用会让「请求装得下」被误读为「存储装得下」。
- **用 `IMemoryRetentionSweep` 的 `max_records`/`max_versions` 当总量上限**：否决。
  那是一趟有界的**保留期清扫** pass，`sweep` 的语义是「按到期回收」，
  不是「按总量准入」；pass 有界不等于总量有界（DEC-062 §2 已明确）。
- **遗忘 = 降低召回分数**：否决（F6）。
- **继续让 TTL 到期即物理删除**：否决（F7）。当前这正是「遗忘只有时间一个维度」的根源。
- **归档 = 换个目录继续存**：否决（issue #111 §6；归档有自己的总额与保留期）。
- **保护集合无限豁免**：否决（P1–P3）。
- **删除时只报「删了 N 行」**：否决（X5、X8）。
- **把清理做成一笔跨 store 长事务**：否决。既有存储是单写者通道且有操作超时
  （DEC-062 §2），长事务要么整笔回滚要么让调用方拿着超时错误而工作在后台继续；
  跨 store 清理必须是**幂等、可重入的分步删除**，而不是一笔事务。
- **在 Mira 内自建定时器做周期清扫**：否决。周期清扫必须经 Executor；
  自建定时器属 AGENTS.md 禁止项。
- **把按主体配额完全放在 Mira 侧做二段式准入**：否决（会破坏 Executor 的
  接纳不变式）。已登记 `EXE-20261028-001`；上游 PR #223 已交付原生能力，
  M31 期间按台账的临时边界过渡，版本可用后**移除**该过渡层。
- **在 Mira 侧维护一份与 Executor 并行的「主体在途计数」并据此放行**：否决。
  原生能力可用后会出现两份真相源；分桶计数必须由 Executor 单一持有。

## 影响与风险

- **新增公开契约面**：容量预算与准入接口（`CapacityBudget`、准入裁决）、
  归档资格与退出生命周期状态、回收度量（三种口径）。
- **新增持久化/投影**：容量账本（可重建投影）、归档索引、回收日志。
  它们自身**计入** §1 的预算。
- **行为变更（既有）**：`SqliteMemoryStore` 的清扫不再只是「按 TTL 到期」，
  而是「按 TTL 到期 + 按价值/压力裁决」，且清扫结果要报告三种度量；
  `EventStore` 的「内存索引满 → 永久 `read_only`」必须改为**淘汰/归档**；
  `ArtifactStore` 的 `manifest.log` 必须可压实。
- **风险 X-R1（准入误拒）**：硬水位下拒绝新工作可能影响正常任务。
  缓解：预留空间保障回收与取消；拒绝必须带明确原因与归属，不得静默。
- **风险 X-R2（保护集合饥饿）**：保护集合占满后可能让系统无法接纳任何新工作。
  缓解：保护集合有上限且受背压保护（P2）；回收预留确保至少能完成清理。
- **风险 X-R3（迁移风险）**：现网已有数据可能已超出新预算。
  缓解：M31 必须提供**迁移与清扫**路径，并在迁移期明确「哪些数据先被降级/清理」的顺序；
  **不得**在迁移中静默删除用户数据。
  > **2026-10-28 修订（承接 Mirage DEC-119 与其前置清单）**：Mira 侧原只要求
  > 「路径 + 顺序 + 不静默删除」。Mirage 明确要求三项**验收物**与一项**缓冲**：
  >
  > 1. **占用/损失预估**——迁移前给出将被降级/清理的数据量与影响范围；
  > 2. **可观察的迁移结果**——迁移后能证明约束**已生效**，
  >    「不能把文档更新写成新容量约束已生效」；
  > 3. **失败恢复**——迁移中途失败可恢复且不留下半迁移状态；
  > 4. **迁移宽限期**——给存量用户查看/导出/备份的时间窗。
  >
  > 这四项进入 M31 的退出条件。
- **风险 X-R4（归档不可信）**：归档后原文不可回查，若归档过早会导致证据丢失。
  缓解：§6 归档资格是前置条件的**合取**；Q2 清理前重查；X7 防复活。
- **风险 X-R5（EventStore 只读死锁）**：当前「内存索引满 → 永久 `read_only`」是一个隐藏的
  不可恢复状态；在引入归档前必须先解除，否则容量问题会变成停机问题。
- **Executor 边界**：见 §1 的 B2 说明与 §9 的周期承载；缺口登记
  `EXE-20261028-001`（上游 PR #223 已交付，等待可用版本）。
  跨 store 的**崩溃可恢复分步删除**是存储层语义
  （幂等 + 重入 + 墓碑），**不**要求 Executor 提供事务或 saga——
  那属于应用层职责，不是 Executor 缺口。
- **风险 X-R6（上游能力的覆盖路径窄于预期）**：上游分桶键只在带键提交形态上
  被消费，而 Mira 现有提交多为裸 lambda 形态。若 M31 只接入原生能力而**不迁移
  提交形态**，按主体份额将对大部分实际提交**不生效**——一个看似已实现的配额
  实际不约束任何东西。
  缓解：M31 的退出条件必须包含「受分桶约束的提交点已全部迁移到
  `submit_auto(kairo::subject(key), fn)`」这一项，并据此断言
  「A 用满自身份额后 B 仍可接纳」在**真实提交路径**上成立；
  同时用上游新增的 `admitted_total` 诊断面**逐主体**确认键确实流过了接纳点
  （份额已配置而 `admitted_total == 0` 即为未生效）。
- **风险 X-R7（弱一致被误读为违约）**：热更新弱一致允许瞬时超额。
  缓解：在验收与诊断中把「瞬时超额、随终态自愈」列为**已知行为**，
  监控口径按「超额上界 + 自愈时间」而非「是否为零」设定。
  与上游 R-3 确定的监控口径一致。
- **风险 X-R8（键使用身份原文导致泄漏）**：分桶键被逐字复制进拒绝消息、
  failure 事件与状态快照，三者都流向日志。
  缓解：Mira 侧**必须**以不可识别的派生标识作键（上游 R-2 约定），
  不得直接使用 `TenantId`/`UserId` 原文；M31 验收须检查事件与诊断输出中
  不出现主体身份原文。

## 验证方式

M31 立项时冻结门禁矩阵，至少覆盖：

- **容量有界**：用小配额持续写入不同大小的事件/附件/记忆，越过软/硬水位，
  验证驻留/受管理会话数、归档总量、**实际文件占用与峰值**始终符合冻结预算。
- **保护集合**：保护集合满、磁盘满、回收失败**可见、有界**，
  **不以无限归档或删除计数代替资源测量**。
- **安全归档/遗忘**：完全结束的会话释放管理名额并最终清理；仍有工具、未读结果、
  更新或共享证据引用时**不能误删**。覆盖查询归档、新输入重新引用、清理中崩溃/取消、
  原始证据过期、隐私删除后不复活，以及去重台账过期后旧请求不重复副作用。
- **分别结算**：容量回收失败**不**解除 Erasure Pending 的 hold；归档**不**被计为「已遗忘」。
- **度量分离**：逻辑删除、可重用空间、实际占用三者分别可测且不互相替代。
- **Executor 纪律**：清扫周期在过载时让步；tick 被拒后下一趟收敛；
  取消与 shutdown 路径闭合，无重叠执行。

## 关联文档和工作项

- 工作项：M31（容量准入与归档回收轮）——待立项
- 来源需求：[Issue #111](https://github.com/Linductor-alkaid/mira/issues/111) §6
- 相关决策：[DEC-062](DEC-062-memory-retention-sweep.md)（保留期清扫与窄接口先例）、
  [DEC-057](DEC-057-scoped-memory-read-and-apply-check-order.md)（scope/ACL）、
  [DEC-058](DEC-058-mutation-ledger-scope-binding.md)（台账 scope 绑定）、
  [DEC-068](DEC-068-interaction-coordinator-and-principal-input-entry.md)（输入台账计入预算）
- Executor 反馈：[`EXE-20261028-001`](../executor_feedback/ledger.md)（按主体份额准入；
  上游 [kairo#222](https://github.com/Linductor-alkaid/kairo/issues/222)）
- 设计：[Context 与 Memory 设计](../design/context_and_memory_design.md)、
  [Context Curator 设计](../design/context_curator_design.md)、
  [事件与崩溃一致性设计](../design/event_artifact_crash_consistency.md)、
  [核心契约与状态机设计](../design/core_contracts_and_state_machine.md)
- 术语：[公共术语表](../project/glossary.md)
