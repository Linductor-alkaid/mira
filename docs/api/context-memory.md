# Context 与 Memory

> 头文件：`mira/context_contracts.hpp`、`mira/context_manager.hpp`、
> `mira/context_retrieval.hpp`、`mira/memory_contracts.hpp`、
> `mira/sqlite_memory_store.hpp`、`mira/memory_consolidation.hpp`、
> `mira/provider_continuation.hpp`、`mira/context_memory_supervisor.hpp`、
> `mira/stateful_replay.hpp`、`mira/context_curator.hpp`、
> `mira/context_working_context_promotion.hpp`、
> `mira/context_working_context_fork.hpp`

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
- `MemoryConsolidator::consolidate_candidates(store, candidates, scope, now)`（M23）：
  对预先抽取的候选执行与 `consolidate()` 相同的策略管线（不做事件抽取、不挂模型
  钩子，候选 provenance 由调用方负责）；重复判定要求已存副本验证等级 ≥ 提案等级，
  杜绝验证等级降级。`consolidate()` 即"抽取 + 模型增强 + 本入口"，管线体只有一份。

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

## context_working_context.hpp：Working Context 快照（M20，DEC-035 Stage W1）

任务/会话导向的当前状态视图（issue #48 方向的 Stage W1，无模型）：从已提交
`ConversationCheckpoint` 确定性导出的 `WorkingContextSnapshot`——可重建投影
（RULE-07），只经 Layer 0 准入进入模型请求，语义层无准入权：

- `working_context_from_checkpoint(checkpoint, identity, options)`：纯确定性
  投影——checkpoint 的约束/决策/未决线索逐条携带 provenance、真实时序与
  confidence 映射进快照三 section；`identity` 必须与 checkpoint 自身五元组
  戳一致；边界（section 计数、单条字节、provenance 上限）任一超限整体拒绝，
  全有或全无，不产生部分快照（RULE-08）。标记过滤不在投影重复执行：输入是
  已提交 checkpoint（M19 固化阶段强制）。
- `WorkingContextSnapshot`：身份五元组 + 确定性派生 id（同输入重导出同 id，
  RULE-07）+ `state_digest()`（排除 id 与 `created_at`，重建时间不影响身份）
  + `source_checkpoints` 溯源；JSON 契约 `mira.working_context.snapshot.v1`
  （DEC-002 版本纪律）。
- `commit_working_context(store, candidate, live)`：终态幂等（会话/任务终态后
  迟到快照丢弃）→ 五元组校验（任一不匹配丢弃候选、保留已存快照）→ 幂等
  NoOp（同水位同 digest）→ 同水位异 digest fail-closed 冲突（W2 Curator
  非确定性行为的兜底）→ 提交；`IWorkingContextStore` 参考实现会话内水位单调、
  有界保留环，epoch 变化开启新身份链、旧链仍可按水位回查（快照条目携带
  epoch 标注，Layer 0 stale-build 可检测）。
- `context_items_from_working_context()`：快照到 Layer 0 候选的纯转换——约束
  → P1 `UserConstraint`、决策/未决 → P3 `CheckpointSummary`，authority 恒为
  `UntrustedExternalData`（RULE-09，不得自我提升为 policy）；条目 id 由快照
  id 确定性派生且与 checkpoint 条目空间分离（双投影并存可审计）；宿主应择一
  喂给 Layer 0。
- 恢复 = 幂等重导出：空 store 上从最近已提交 checkpoint 重新投影，id 与
  digest 逐字节一致（设计 §10；W1 无持久化格式，volatile store 与 checkpoint
  store 同档）。
- `ContextMemorySupervisor::schedule_working_context_commit(store, checkpoint,
  identity, live, options)`（Deferrable，设计 §8）：确定性投影 + 单调提交在
  一个受监督步骤内完成；Stage W2 将模型介导的 Curator 换到同一路由后方，
  shutdown 语义不变；`begin_shutdown()` 后提交被拒绝。
- 评估证据：
  [working-context 评估 v1](../benchmarks/context-intelligence-working-context-v1.md)
  （W1-G1–G6 门禁；确定性投影口径，不声明 token 收益或语义质量）。

## context_curator.hpp：Working Context Curator（M21，DEC-035 Stage W2）

模型介导的快照维护者（issue #48 方向的 Stage W2；M20 快照契约经加法式 schema
minor 升级到 1.1——新增 `active_tasks` / `verified_facts` / `failed_attempts` /
`important_refs` / `next_actions` 五个 Curator 填充 section 与 `generated_by`
标注，v1.0 载荷继续可读，`goal` 刻意缺席以免复述 Layer 0 P1 任务帧）：

- `IContextCurator::curate(previous, checkpoint, recent_events, options)`：以
  「previous snapshot + 新提交 checkpoint + recent events」为输入产出下一快照
  候选；失败表示「无新投影」，调用方保留已存快照（设计 §9）。`previous` 为
  nullptr 表示新身份链（epoch 失效后）；recent events 不得越过 checkpoint
  水位（候选水位恒等于 checkpoint 水位，同水位不同输入由提交管线冲突分支
  fail-closed 兜底）。
- `ProviderContextCurator` 参考实现（经注入 `IModelProvider`；DEC-036 口径：
  任意可用源模型，含主 Agent 模型，Core 无模型）：三段编号转录
  （`prev:` / `ckpt:` / `event:` 行格式；checkpoint 偏好语句刻意不入转录，归
  Memory 审批管线）+ `StrictJsonSchema` 输出契约
  （`working_context_curation_output_schema()`，根 `confidence` + 八 section）；
  模型输出逐条重新验证——内容/来源上界（RULE-08）、标记过滤、置信度下限、
  引用越界即整条丢弃（RULE-09）；**退化防护**：previous 非空而候选绑定条目
  零 previous 引用时整体拒绝（`degenerate-merge`），富快照不被无 supersede
  纪律的输出覆盖。
- 增量 merge 语义是**指令契约 + 机械校验**：保留 / supersede / 冲突保留由模型
  经引用表达（provenance 随引用继承、`source_sequence` 取被引用输入最小值），
  运行时不执行编辑策略；候选经 `commit_working_context` 既有提交管线落库，
  同水位 digest 冲突保持 fail-closed。
- 候选身份与 W1 同公式（五元组种子派生 id）；`source_checkpoints` 累积链
  （保序去重、保留最近 64）；`generated_by` 记录实际模型 profile。
- `context_items_from_working_context()` 扩展：constraints → P1
  `UserConstraint`，其余七 section → P3 `CheckpointSummary`，条目 id 的
  section 标签互异（不引入新 `ContextItemKind`）。
- `ContextMemorySupervisor::schedule_working_context_curate(curator, store,
  previous, checkpoint, recent_events, live, options)`（Deferrable，设计 §8）：
  curation + 单调提交在一个受监督步骤内完成；取消探针经 supervisor 注入，
  Curator 失败不提交、future 以错误 resolve；`begin_shutdown()` 后提交被拒绝。
- 评估证据：
  [working-context curation 评估 v1](../benchmarks/context-intelligence-working-context-curation-v1.md)
  （W2-G1–G6 门禁；脚本化确定性供给方口径，语义质量与 token 收益声明归真实
  模型轮与 Stage E，RULE-10）。

## context_working_context_auto.hpp：Working Context 自动触发（M22，DEC-035 Stage W3）

Supervisor 驱动的快照链自动维护（issue #48 方向的 Stage W3）：宿主上报信号，
协调器按冻结策略决定「现在刷新 / 等待 / 吸收进在途刷新」，工作仍全部经
`schedule_working_context_curate` 既有 Deferrable 路由——无隐藏后台循环，
每会话链至多一个在途 policy 刷新：

- `evaluate_working_context_trigger(policy, last_attempt, events, current)`：
  纯策略函数——watermark 轴（输入 checkpoint 水位与上次尝试的距离 ≥
  `watermark_interval`，设计 §8 的"token watermark"按序列水位实现，请求体量
  预算留在 `ContextCurationOptions`）优先，其次 event count 轴（宿主上报的
  执行事件增量 ≥ `event_count_interval`）。
- `WorkingContextAutoCurator::on_signal(session, input)`：落账就绪在途 →
  在途则吸收（coalescing，不排队副本）→ 评估策略；燃点还需 checkpoint
  未落库（`through_event_sequence > settled_watermark`，同水位重 curate
  要么 NoOp 要么冲突，均不制造）。checkpoint 空窗期距离与事件增量继续
  累计，新 checkpoint 一到即追燃。返回 `shared_future`，调用方与协调器各持
  一份（协调器副本用于后续 drain 落账）。
- `flush(session, input)`：task boundary 屏障——有界排干在途（预算 = curation
  deadline），对未落库 checkpoint 无视阈值强制燃；已覆盖边界立即以
  `IdempotentNoOp`（reason `auto-refresh-current`）resolve，零 curator 调用。
  宿主须先等待其 future 再翻转 session/task terminal；终态后迟到结果按
  提交纪律丢弃，不重激活。
- 失败回退（设计 §9）：curator 失败 future 以错误 resolve、store 不动、阈值
  已在燃点重臂（无紧重试）；下次阈值越过重试，成功后 `consecutive_failures`
  归零。不做确定性投影自动回退（同水位混源必然冲突且会静默丢失五个 Curator
  section）；宿主如需 W1 兜底可显式走 `schedule_working_context_commit`。
- 拒绝路径：信号会话与 checkpoint 会话不一致 → `InvalidArgument`；超过
  `max_tracked_sessions` → `ResourceExhausted`（均为已 resolve 的错误
  future，必须消费）。构造期校验 policy 与 curation options（失败抛
  `std::invalid_argument`，同 Supervisor 约定）。
- 观测：`session_view()`（in_flight / last_attempt / settled / 事件累计 /
  失败连击）与 `stats()`（信号、吸收、燃点分列、forced、NoOp、disposition、
  错误计数）；`drain()` 供宿主显式落账；析构执行总预算 2×deadline 的有界
  drain。
- 评估证据：
  [auto-trigger 评估 v1](../benchmarks/context-intelligence-working-context-auto-trigger-v1.md)
  （W3-G1–G6 门禁；脚本化确定性供给方口径，RULE-10）。

## context_working_context_promotion.hpp：Working Context Memory Promotion（M23，DEC-035 Stage W4）

Working State Plane 进入长期 Memory 的唯一受控通道：快照中具有跨任务耐久价值的
语句经确定性投影变为 `MemoryCandidate`，再走 `MemoryConsolidator` 既有纪律管线
（marker 过滤、record 校验、scope 内冲突检索、duplicate 判定、人工审批、apply）。
快照与 Curator 永不直接写 Memory；晋升是宿主显式操作（典型时点：任务终态边界），
不是 curation 的自动副作用：

- 冻结 section→kind 映射：`constraints`→`Preference`（默认落入人工审批门）、
  `decisions`→`ApplicationFact`、`verified_facts`→`EnvironmentFact`、
  `failed_attempts`→`RecoveryLesson`；任务导向 section（`active_tasks`/
  `next_actions`/`open_issues`）与 `important_refs` 不晋升——投影函数读不到
  它们，调用方无法把当前任务状态固化进长期记忆。
- 晋升候选恒为 `Unverified` + `model_assisted` +
  `source_namespace="working-context"`（模型介导派生投影，RULE-09，不可被调用方
  覆盖），`evidence` 绑定 item 源事件 provenance；record id 从独立 seed 空间
  确定性派生（同输入同 id，跨进程可复现）。
- `WorkingContextPromotionPolicy`：`min_confidence`（默认 0.5，低于即丢弃并
  计数）与 `max_candidates_per_run`（固定 section 顺序截断，丢弃计数不受影响）。
- `promote_working_context_to_memory(consolidator, memory, snapshot, scope, now,
  policy)`：投影 + 共享管线组合入口；报告同时携带投影产物（含候选与丢弃计数）
  与管线 `ConsolidationReport`。scope 由宿主权威给定（ACL），Core 不发明
  tenant/subject 策略。
- 边界：晋升只读快照面（不写 store、不推水位）；`erase_session` 不触及已晋升
  记录（受 Memory 侧 retention/erasure/approval 治理，跨 store 擦除编排是宿主
  职责）；宿主经既有 `ContextMemorySupervisor::submit<T>(…, Deferrable, …)`
  泛型路由执行，无新 Supervisor 方法。
- 验证证据：`tests/m23/`（W4-G1–G6 契约门禁；无模型无数据集，无 benchmark
  声明，RULE-10）。

## context_working_context_fork.hpp：Working Context Subagent Fork / Merge（M24，DEC-035 Stage W5）

issue #48「Subagent 与上下文隔离」的确定性契约面（场景边界冻结于 DEC-044：
subagent = Agent Harness 控制平面内父会话旁的子 Session；fork/merge 是
Working State Plane 投影操作，无第三执行面、无环境动作路径、无模型调用）。
快照 schema 1.2 加法 minor：可选 `fork` 溯源（`WorkingContextForkProvenance`）
仅出现在子会话 fork 基线上，仅非 nil 时进入 digest canonical 对象与 JSON，
v1.0/v1.1 载荷读回保留原 `schema_version` 且 digest 逐位一致（DEC-002）：

- `fork_working_context(parent_base, seed, options)`：从父会话已提交快照产出
  子会话只读基线——八 section 逐字副本、`source_checkpoints` 逐字继承、身份
  换轨为子五元组（`child_watermark ≥ 1`）、携带 fork 溯源；同一输入组幂等
  （同 id 同 digest 同字节）；非法/越界父快照或零水位整体
  `InvalidArgument`，无部分基线。父会话 store 永不被子操作写入。
- `WorkingContextDelta`（独立 schema `mira.working_context.delta.v1`）与
  `working_context_delta_from_fork(fork_base, child_snapshot, options)`：把
  子链最终快照对基线做机械三分类投影——content 与基线逐字节相等 → inherited
  （剔除并计数，不回传完整探索历史）；`source_events` 与同 section 基线条目
  交集（W2 编号转录 provenance 绑定，父子事件空间不相交）→ `Supersede`（取
  最小基线规范编号，编号 = 声明序 × 向量序 0..N-1）；其余 → `Addition`。
  固定八 section 词表、逐条 provenance、`max_item_chars`/`max_items_per_section`
  上界（RULE-08），越界整体拒绝；`working_context_delta_to_json`/
  `from_json` 往返保真。
- `merge_working_context_delta(fork_base, parent, delta, parent_identity,
  watermark, options)`：引用驱动机械合并——supersede 经基线引用在父 section
  原位替换（父先序保持，无命中重分类为 addition 并计数）、addition 按 delta
  序追加、逐 section 固定顺序截断；前置三拒绝（`fork-base-mismatch`/
  `same-session-fork`/身份不一致，均 `InvalidArgument`）。候选 = 父身份 +
  新水位、`source_checkpoints` 逐字继承、`generated_by` 继承 `parent.generated_by`
  （零效果 delta 候选与父 digest 逐字段相同，同水位提交判 `IdempotentNoOp`）、
  `fork` = nil；本入口不写 store——候选经既有 `commit_working_context` §5.2
  管线落库（合并只在父水位严格前进时可提交，同水位异 digest
  `conflicting-watermark` fail-closed 不豁免），rejected = 宿主不调用合并。
  报告携带全部决策计数（resolved/stale/appended/truncated）供宿主审计与晋升
  决策；跨进程字节确定。
- 边界：fork/merge 均宿主显式操作（不接 W3 自动触发链、无隐藏后台循环），
  提交经既有 `ContextMemorySupervisor::submit<T>(…, Deferrable, …)` 泛型
  路由；合并产物按 M23 冻结 section→kind 映射参与晋升，未合并分支内容零
  晋升路径；`erase_session(子会话)` 不触及父快照与已合并内容；跨会话隐私
  擦除编排归宿主职责。
- 验证证据：`tests/m24/`（`mira_m24_fork_merge_test` 22 用例 +
  `mira_m24_fork_merge_eval` 冻结链确定性 harness，W5-G1–G6 门禁；无模型、
  无 benchmark 声明，RULE-10）。

## stateful_replay.hpp：AnalysisReplay

只读分析回放：`AnalysisReplay(events, checkpoints, memory, artifacts).inspect(task,
session, scope)` 返回 `AnalysisReplayReport`——checkpoint 存在性、当时态
（`ReplayAsOf` 双时查询）Memory 视图与 `MissingArtifactNote` 显式降级；无
Network/Tool/Input 能力，不加载真实 Provider continuation。

## 相关文档

- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)
- [Stateful agent beta 发布说明](../releases/stateful-agent-beta.md)
