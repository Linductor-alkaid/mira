# Context 与 Memory

> 头文件：`mira/context_contracts.hpp`、`mira/context_manager.hpp`、
> `mira/memory_contracts.hpp`、`mira/sqlite_memory_store.hpp`、
> `mira/memory_consolidation.hpp`、`mira/provider_continuation.hpp`、
> `mira/context_memory_supervisor.hpp`、`mira/stateful_replay.hpp`

M4 交付的有状态 Agent 基础：每次模型调用前构建有预算、可审计、不裁剪安全约束的
Context；以带 scope、来源与有效期的长期 Memory 作为可选增强。

## context_contracts.hpp 与 context_manager.hpp

- `ContextLimits`：按 `ModelProfile` 的 token 预算与水位；所有水位使用保守上界
  （`TokenEstimate` + `TokenCountQuality`），exact count 失败可降级但绝不记零。
- `ContextItem`（`ContextItemKind`）按 `ContextPriority` 分区；`partition_of()` 把
  item 映射到 P0–P5 分区：P0（安全约束/授权）、P1（Goal 与未决副作用）等不可裁剪；
  超过最低可执行集合时按 `MinimumSetTooLarge` 显式拒绝或只路由到显式授权的大窗口
  profile。
- `IContextManager::prepare(ContextRequest) -> PreparedModelContext`：确定性装配——
  相同请求产生相同 selection digest，逐项输出选入/排除/替换/压缩理由（稳定 reason
  code）；item epoch 与请求边界不一致时按 `StaleBuild` 拒绝。`StandardContextManager`
  是纯函数实现；`ITokenCounter`/`IExactTokenCounter` 是计数边界
  （`ConservativeTokenConfig` 配置保守上界）。
- 多模态与 Tool 配对：历史大载荷转 `ArtifactRef`，Tool call/result 配对原子决策，
  未消费 result 进入最低集合。

## memory_contracts.hpp

- `MemoryScope{kind, subject_id, tenant_id}`（`MemoryScopeKind`：Task/Session/
  Environment/Application/User/Agent/TaskSkill）是 ACL 过滤器而非排序特征：检索
  永不因相似度跨 scope（RULE-09 配套）。
- `MemoryRecord` 带 `MemoryKind`、`MemoryVerification`、双时态 validity、TTL、
  `MemoryStatus`（含 Tombstone）；`MemoryMutation` 幂等于 mutation id，乐观版本冲突
  返回 `VersionConflict` 而不是静默覆盖；`Supersede` 在原 id 上闭合前驱区间。
- `IMemory`：`query(MemoryQuery)`（scope 过滤的混合检索）/ `get()` / `apply()` /
  `compact(scope)`（保留期清扫，连带删除引用的 Artifact）/ `erase(ErasureRequest)`。
  隐私删除部分失败时整体保持 `ErasureResult` Pending 并 fail-closed 阻止该 scope 进入
  Context；审计只含 id/计数/原因，不含被删正文。

## sqlite_memory_store.hpp：SqliteMemoryStore（`Mira::state_store`）

`IMemory` 的 SQLite/WAL 参考后端（`SqliteMemoryStoreOptions`，含 `RetrievalWeights`）：

- 混合检索三腿：exact（`exact_terms`）、FTS5（`text`，短语引用，操作符注入被拒绝）、
  有界 cosine 向量腿（`index_embedding()` 维护，投影可重建）；合并去重、多样性与
  token packing。
- 向量索引损坏或维度失配按 index lag 降级为 exact/FTS，不阻塞控制面
  （`index_lag()` / `clear_embeddings()`）。
- 生命周期语义与 `SqliteCheckpointStore` 相同：单 writer、`open()` 显式迁移、宿主线程
  `close()`、Executor 长寿命（见[事件页](events-recovery.md)）。

## memory_consolidation.hpp

Verified Event 到 Memory candidate 的确定性管道：

- `MemoryConsolidator::consolidate(store, events, scope, now)` 产出
  `ConsolidationReport`（`CandidateDisposition` 含重复 no-op、禁止内容与注入拒绝、
  需人工审批的 Preference 类目）；`ConsolidationPolicy` / `IConsolidationModel` 控制
  可选的模型辅助路径，但模型文本不能绕过 policy、scope 与 authority 校验。

## provider_continuation.hpp

Provider 专属优化的受限生命周期：`ContinuationCache.store()/lookup()` 以
`ContinuationBinding`（provider/profile/conversation/task/session/epoch/schema/policy/TTL
绑定矩阵）为键；`ContinuationInvalidation` 枚举描述失效原因。切换 Provider、取消、
Takeover 或进程恢复后缓存失效，且始终可由本地 checkpoint 重建。
`ExactCountGate`/`IFinalTokenCounter`（`profile_supports_exact_count()`）约束 exact
token count 能力门与降级路径。

## context_memory_supervisor.hpp：ContextMemorySupervisor

Context/Memory 操作的 Executor 监督者（M4-16）：

- `SupervisorConfig` 配置容量；操作按 `SupervisedOpClass`（`Critical` / `Interactive` /
  `Deferrable`）分类调度：checkpoint 写入与 Erasure 是 Critical（不可静默放弃），
  查询是 Interactive，consolidation/GC 可延迟合并。
- 典型用法：`schedule_checkpoint(coordinator, task, session, trigger, now).get()`、
  `schedule_memory_query(store, query).get()`（返回 future，必须消费）。
- `begin_shutdown()` 返回 `SupervisorShutdownReport`（含 `critical_drain_complete`），
  按固定顺序停 producer、结算 Critical、回收 worker；`SupervisorToken` 是可移植的
  取消令牌。
- 完整示例见 [`examples/stateful_agent_consumer.cpp`](../../examples/stateful_agent_consumer.cpp)。

## stateful_replay.hpp：AnalysisReplay

只读分析回放：`AnalysisReplay(events, checkpoints, memory, artifacts).inspect(task,
session, scope)` 返回 `AnalysisReplayReport`——checkpoint 存在性、当时态
（`ReplayAsOf` 双时查询）Memory 视图与 `MissingArtifactNote` 显式降级；无
Network/Tool/Input 能力，不加载真实 Provider continuation。

## 相关文档

- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)
- [Stateful agent beta 发布说明](../releases/stateful-agent-beta.md)
