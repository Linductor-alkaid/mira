# Context 与 Memory

> 头文件：`mira/context_contracts.hpp`、`mira/context_manager.hpp`、
> `mira/context_retrieval.hpp`、`mira/memory_contracts.hpp`、
> `mira/sqlite_memory_store.hpp`、`mira/memory_consolidation.hpp`、
> `mira/provider_continuation.hpp`、`mira/context_memory_supervisor.hpp`、
> `mira/stateful_replay.hpp`

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

## context_retrieval.hpp：Layer 1 检索召回（M17，DEC-032 Stage B）

Cold History 重新进入模型请求的召回通道；只负责候选，排序终局归 Stage C 重排、
token/safety 准入归 Layer 0 `StandardContextManager`（语义组件无准入权）：

- `ContextIndexAsset`/`ContextAssetKind`：可索引对象覆盖三类资产——DEC-016 会话
  投影的窗口段（`segment_conversation()` 确定性切分，文本有界、provenance 透传、
  确定性资产 id 支持幂等重建）与 DEC-029 Episode/Lesson 声明面（净化标识符文本）。
  会话资产持有 session、学习资产持有 MemoryScope——ACL 恰好一方，默认拒绝。
- `IContextEmbedder`（外部供给契约，Core 无实现）+ `ContextEmbedding`
  （向量 + profile id）：向量供给沿用 `MemoryQuery::query_embedding` /
  `index_embedding` 语义——embedder 是供给方不是存储权威；供给失败只降级向量腿。
- `IContextRetriever::retrieve(ContextQuery, RetrievalBudget)`：
  `InMemoryContextIndex` 参考实现为进程内三腿混合——exact（逐字子串、强制过滤）、
  词法（token 覆盖率，FTS5 的进程内替身；耐久部署仍走 `IMemory` 三腿）、向量
  （有界线性 cosine 扫描）。`MemoryQueryQuality` 复用：逐腿运行/降级、
  `index_lag`、`deadline_exceeded` 部分结果；查询与预算带 `top_k`/`token_budget`/
  `deadline`/`max_vector_scan` 上界（RULE-08）。
- 失效与重建（RULE-07）：索引是可重建投影——同一资产 id 水位前进使旧 embedding
  失效（`index_lag` 上升直至重供给）；同水位内容变化、水位回退、禁止标记
  （`ContextIndexPolicy.forbidden_markers`，与 `ConsolidationPolicy` 同源）一律
  fail-closed 拒绝注册。
- `context_item_from_candidate()`：候选到 P4 `RetrievedMemory` `ContextItem` 的纯
  转换（authority 恒为 `RetrievedMemory`，RULE-09），随后由
  `StandardContextManager` 统一裁决。
- `ContextMemorySupervisor::schedule_context_retrieval(retriever, query, budget)`
  （Interactive）承载 Executor 路由；`begin_shutdown()` 后提交被拒绝。
- 候选 JSON 契约 `mira.context.candidate.v1`（`context_candidate_to_json`/
  `from_json`，DEC-002 版本纪律）。

## context_rerank.hpp：Layer 2 重排（M18，DEC-032 Stage C）

检索 Top-K（30~100）到模型请求候选（5~20）的可选收敛层；只重排/截断，不扩大
成员资格、不触存储、不改 Layer 0 准入权威：

- `IContextReranker::rerank(query, candidates)`：返回
  `RankedContextItem`（候选原样携带 + `rerank_score`/`fused_score`/`retrieval_rank`），
  截断到 `ContextRerankConfig::max_output`（默认 20）；空输入闭合空结果；错误
  表示「重排不可用」，调用方按设计降级为检索序 Top-K，无部分输出。
- `TokenOverlapContextReranker` 确定性参考实现（无模型）：查询/候选文本按
  Layer 1 同款 `[a-z0-9_]` 分词，重排分为查询 token 覆盖 F1 加 `exact_terms`
  逐条 verbatim 加成；`ContextRerankWeights`（默认 0.60/0.40）做集合内 min-max
  融合（全等集合取 0.5），同分保持检索序——融合不整替 Layer 1 排名语义。
  真实 cross-encoder 模型经供应链复核后作为宿主注入实现接入，Core 不携带。
- `ContextMemorySupervisor::schedule_context_rerank(reranker, query, candidates)`
  （Interactive）承载 Executor 路由；`begin_shutdown()` 后提交被拒绝。
- 对照证据：[重排对照 v1](../benchmarks/context-intelligence-rerank-v1.md)
  （C1–C4 门禁；确定性供给方口径，非语义质量声明）。

## context_consolidation.hpp：Layer 3 语义固化（M19，DEC-032 Stage D）

会话级语义固化：把跨多条消息才成立的约束、决策、未决线索与偏好候选固化为
`ConversationCheckpoint`——与 `TaskCheckpoint` 并列的 Warm 投影，只经 Layer 0
准入进入模型请求，语义层无准入权（DEC-032 §2）：

- `ISemanticConsolidator::consolidate(segment, options)`：把会话前缀段固化为
  checkpoint 候选；失败表示「无新投影」，调用方保留既有 checkpoint，不返回
  部分结果（设计 §8）。`ConversationSegment.entries`（M19 加法式增补）承载
  逐条目 provenance。
- `ProviderSemanticConsolidator` 参考固化器（经注入 `IModelProvider`，不绑定
  主模型，Core 无模型）：编号转录 + `StrictJsonSchema` 固化 schema 请求；模型
  输出逐语句重新验证——语句/来源计数与字节上界（RULE-08）、`forbidden_markers`/
  `injection_markers` 过滤、置信度下限、引用越界即整条丢弃（不产生 fabricated
  provenance，RULE-09）；deadline 与取消探针经 `ConsolidationOptions` 透传。
- `ConversationCheckpoint`：五元组（`session_id / task_id / task_epoch /
  environment_epoch / through_event_sequence`）、四类语句 + provenance 并集 +
  `generated_by`；`projection_digest()` 排除叙事摘要与置信度（与
  `TaskCheckpoint::narrative_summary` 同纪律）；JSON 契约
  `mira.context.checkpoint.v1`（DEC-002 版本纪律）。
- `commit_conversation_checkpoint`：终态幂等（会话/任务终态后迟到结果丢弃）→
  五元组校验（任一不匹配丢弃候选、保留旧 checkpoint）→ 幂等 NoOp（同水位同
  digest）→ 同水位异 digest fail-closed 冲突 → 提交；`IConversationCheckpointStore`
  参考实现内存有界保留、水位回退拒绝（RULE-07：可重建投影）。
- `context_items_from_checkpoint()`：语句到 Layer 0 候选的纯转换——约束 → P1
  `UserConstraint`、摘要/决策/线索 → P3 `CheckpointSummary`，authority 恒为
  `UntrustedExternalData`（模型媒介派生投影，不得自我提升为 policy）；偏好候选
  不转换，归 `MemoryConsolidator` 人工审批管线。
- `ContextMemorySupervisor::schedule_context_consolidation(consolidator, segment,
  options)`（Deferrable，设计 §7）承载 Executor 路由；取消探针经 supervisor 注入，
  shutdown 在途取消以 `Cancelled` 错误 resolve，`begin_shutdown()` 后提交被拒绝。
- 评估证据：[固化管线评估 v1](../benchmarks/context-intelligence-consolidation-v1.md)
  （D1–D5 门禁；脚本化供给方口径，非语义质量声明）。

## stateful_replay.hpp：AnalysisReplay

只读分析回放：`AnalysisReplay(events, checkpoints, memory, artifacts).inspect(task,
session, scope)` 返回 `AnalysisReplayReport`——checkpoint 存在性、当时态
（`ReplayAsOf` 双时查询）Memory 视图与 `MissingArtifactNote` 显式降级；无
Network/Tool/Input 能力，不加载真实 Provider continuation。

## 相关文档

- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)
- [Stateful agent beta 发布说明](../releases/stateful-agent-beta.md)
