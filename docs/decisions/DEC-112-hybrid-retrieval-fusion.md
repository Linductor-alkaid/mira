# DEC-112：记忆检索的 RRF 融合模式、结构化元数据与远端 Reranker

> 状态：Accepted
> 日期：2026-11-17
> 负责人：Mira Maintainers
> 冻结里程碑：2026-11 混合检索维护轮
> 替代/被替代：无（延伸 [DEC-060](DEC-060-memory-embedding-provider.md)、
> [DEC-111](DEC-111-memory-embedding-producer-and-vector-identity.md) 的检索供给面，
> 不修改其决议）。2026-11-18 修订：决策 1 的默认条款（"WeightedSum 默认，
> 行为零变化"）被 [DEC-113](DEC-113-hybrid-retrieval-default-wiring.md) 执行
> 备选方案第 1 条预授权的"对照评估后切默认"路径而翻转为 ReciprocalRank 默认
> （前置 HR-11 基准已交付）；其余决议继续有效
> 关闭的缺口：检索融合只有固定加权和（分数分布不可比时排序被单腿翻转）；记忆缺
> 自由标注与重要性维度；`IContextReranker` 契约没有远端 cross-encoder 的协议适配器

## 背景与问题

记忆向量腿有了生产者与身份守卫（DEC-111）之后，混合检索的融合公式成为下一个
缺口。三腿（exact / FTS5 bm25 / 有界余弦）的分数分布不可比：BM25 无界且随
IDF 漂移，余弦有界。固定加权和（0.30/0.20/0.30 + 先验）在"词面精确命中"与
"语义改述命中"竞争时会被单腿翻转——真实供应商双模型（BAAI/bge-m3 +
bge-reranker-v2-m3，经既有 provider key）实测确认：标识符查询的词面序被向量分
翻转；RRF（Reciprocal Rank Fusion，Cormack et al. SIGIR 2009，k=60）只用排名，
保持词面序的同时改述仍可召回。业界（Elasticsearch、Azure AI Search、Qdrant、
Weaviate、Milvus、Zep/Graphiti）已收敛到 RRF 融合 + cross-encoder 两阶段重排
（召回 top-K → 重排 top-N）+ 元数据预过滤。

同时暴露两个次要缺口：记忆没有自由标注（labels）与重要性（importance）维度；
`IContextReranker` 契约（m18 交付，注释明示"真实 cross-encoder 经供应链复核以
宿主注入到达"）没有协议适配器。

实测还确立一条边界：**reranker 排的是主题相关，不是主张一致**——矛盾偏好
（"喜欢长答案"）在"别太啰嗦"查询下合法排到"偏好简洁回复"前面。冲突裁决归
记忆语义（claim-key / verification / 审批闸门），不归检索。

## 决策

### 1. store 检索新增 RRF 融合模式（选项制，默认不变）

- `SqliteMemoryStoreOptions::fusion`：
  `RetrievalFusion::WeightedSum`（默认，行为零变化）| `RetrievalFusion::ReciprocalRank`。
- RRF 只融合**排名腿**：FTS5 bm25 rank 与余弦 rank。exact 保持强制过滤器
  （0/1），不进排名；per-kind 语义腿扣留语义不变。
- **weighted-RRF**：leg 权重复用 `RetrievalWeights.fts/vector`（相对权重），
  `rrf = Σ w_leg · 1/(60 + rank + 1)`，按两腿权重和归一化到 [0,1]，k=60 为
  文档化常量（不进契约）。
- **先验项叠加在融合之后**（与 WeightedSum 模式同一套权重字段）：
  `+ w_verification·rank/3 + w_confidence·conf + w_recency·recency_norm +
  w_importance·importance`。语义腿是先验、验证是信任权重的既有分层不变；
  重要性是新增的先验维度（决策 2），不进相似度。
- `MemoryQueryQuality` 增 `fusion` 模式名与去重统计（决策 3），纯加法字段，
  对 Context 侧复用者可见但默认零值。

### 2. schema 1.3：labels 与 importance（纯加法，不清数据）

- store schema {1,2} → {1,3}：`memory_records` 加
  `labels TEXT NOT NULL DEFAULT '[]'`（canonical JSON 数组）与
  `importance REAL NOT NULL DEFAULT 0.5`。**迁移只加列不清数据**——与 1.2
  不同，这里没有正确性问题需要清场：新列是新增维度，存量行的中性默认
  （空标注、0.5）不使先验偏置。
- `MemoryRecord` 增 `labels`（`vector<string>`，≤16 条、每条非空 ≤64 字节）
  与 `importance`（float，[0,1] 有限），插在 `source_namespace` 之后；
  `validate()` 边界检查镜像 confidence 区块；record 文档
  `memory_schema_current()` 升 {1,1}（`validate_schema_version` 对 minor 双向
  兼容，旧文档照读；未知字段忽略语义不变）；from_json 对越界值静默回落
  默认（镜像 confidence 先例）。
- `MemoryQuery::labels`：any-of 硬预过滤（与 scope/claim_key 同层），SQL
  `EXISTS (SELECT 1 FROM json_each(labels) WHERE value IN (...))`，过滤集
  ≤16 条；走授权候选集内的既有额度纪律。两条接线缝随本决策交付：
  `MemoryWorkBinding::labels`（宿主在绑定时声明，refuse-at-bind 校验，
  该 work 的每个 Recall 查询自动携带预过滤）；提升策略
  `WorkingContextPromotionPolicy::labels`（宿主定义的标签随提升候选落库，
  投影本身不发明标签——确定性抽取与提升路径不再是无标 Medium 产物）。
- 不做 labels 索引（JSON1 扫描在候选池内，4096 上限）；重要性权重默认
  0.0（`RetrievalWeights` 新字段，宿主显式启用），默认配置下排序零变化。

### 3. 近重去重（可选，默认关）

`SqliteMemoryStoreOptions::near_dup_threshold`（0.0 = 关闭；建议 0.85 起步，
**按 embedder 实测标定**——bge-m3 中文短句改述近重对实测 ≈0.86，矛盾主张对
≈0.68，阈值把"重复"与"冲突"分开）。仅在查询携带向量时生效：排序后的
top-64 有界区间内，与已选集合的余弦 ≥ 阈值者丢弃（statement 全等去重的
语义扩展）；无该查询身份向量的候选豁免（无法评估，不假装比较）。
`dedup_suppressed` 计数入 quality。去重主力仍在写入侧（claim-key 冲突检索，
DEC-058/061），本项只是召回面补充。

### 4. OpenAiCompatibleReranker：IContextReranker 的远端协议适配器

- 新增 `include/mira/reranker_provider.hpp` / `src/model/reranker_provider.cpp`：
  `RerankerProfile`（endpoint/prefix/model/credential SecretRef/deadlines/
  max_redirects/allow_private_endpoint/max_candidates≤128）+
  `OpenAiCompatibleReranker : IContextReranker`。HTTP 栈经注入的
  `IHttpTransport`（与 `OpenAiCompatibleEmbeddingProvider` 同一分工，DEC-111
  先例整段镜像）；凭据只在 transport 边界解析。
- wire：`POST {prefix}/rerank`，`{model, query, documents: [candidate.text…],
  top_n: min(n, max_output)}`（Cohere 风格）；响应
  `results[{index, relevance_score}]` fail-closed 解析：index 越界/重复、分数
  非有限 → ProtocolViolation；非 2xx → 共享 `map_http_error_status`；
  超 `max_candidates` → InvalidLimits（镜像 TokenOverlap 的批次上限拒绝）。
- 输出契约不变：只重排不增员、截断 `max_output`、整批错误（宿主降级到检索
  序，m18 语义）；融合公式镜像 TokenOverlap（0.60·minmax(rerank) +
  0.40·minmax(retrieval score)，全等集 0.5），`retrieval_rank` 回传。
- **语义边界（实测确立）**：reranker 只做主题相关重排。它不裁决记忆冲突，
  不构成准入，不改变任何授权语义（context_intelligence 设计 §5.6"语义组件
  无准入权"原样适用）。

### 非目标

- 250ms 记忆代理 recall 路径不接远端 rerank（deadline 纪律；Layer1/2s 路径
  经宿主 opt-in 使用）。
- MMR 等多样化重排：`(scope,kind)` 上限已提供确定性多样性；实测显示
  λ 调参不当会伤相关性，留实验档（需检索质量基准先行）。
- Layer1 lexical 腿升级 BM25、access_count/last_access（const 查询语义，
  归宿主层）、ANN、labels 索引。

## 备选方案

- **直接替换默认融合为 RRF**：拒绝。加权和是被大量既有测试与基准锚定的
  现状，"默认不变 + 选项制 + 对照评估后切默认"是可回退路径。
  （2026-11-18 修订：HR-11 基准交付后，[DEC-113](DEC-113-hybrid-retrieval-default-wiring.md)
  执行本条预授权路径，默认融合翻转为 ReciprocalRank。）
- **RRF 融合三腿（exact 计排名）**：拒绝。exact 是 0/1 布尔，给"命中者"
  都记 rank-1 会让 RRF 退化为第二张 exact 过滤票；保持强制过滤语义更诚实。
- **importance 进相似度/进 RRF**：拒绝。先验不是相关性信号；分层叠加
  （Generative Agents 式）可独立关停。
- **reranker 并入 ModelGateway provider 体系**：拒绝（本轮）。gateway 的
  重试/熔断按 `ModelProfileId` 组织且服务 infer 补全链路；reranker 单次
  低频调用绕开 gateway、自持 transport + 整批失败降级（IContextReranker
  契约已定义失败语义）更贴合现状。复用 gateway 留给后续评估。
- **labels 用独立关联表**：拒绝。候选池 ≤4096、过滤集 ≤16，json_each 扫描
  成本有界；关联表是提前优化。

## 影响与风险

- 公开契约加法：`SqliteMemoryStoreOptions` 两字段、`RetrievalWeights` 一字段
  （默认 0.0）、`MemoryRecord` 两字段、`MemoryQuery::labels`、
  `MemoryQueryQuality::dedup_suppressed`、record 文档 minor {1,0}→{1,1}、
  store schema {1,2}→{1,3}（迁移只加列）。默认配置行为零变化。
- reranker 引入一次远端调用（仅宿主 opt-in，Interactive 路由，超时/失败
  降级不阻塞）；成本量级 ≈ 一次 infer 的零头，M31 容量预算口径内。
- 供应链：模型经 profile 配置、endpoint 经既有 provider key（DEC-032 决策 5
  口径：既有 Provider 已配置源模型不新增供应链项）；`bge-reranker-v2-m3`/
  `/v1/rerank` 路径不硬编码进 Core，全部 profile 字段。
- 近重阈值是 per-embedder 经验值：换 embedder（DEC-111 身份自动切换）后
  宿主需重新标定；阈值不进契约。
- 不涉及 Executor 能力边界，不登记反馈台账；无新外部头族，无
  architecture-policy 变更。

## 验证方式

- store：schema 1.3 迁移用例（降版 → Migrated → 列加数据存活 → UpToDate）；
  RRF 门（标识符查询保词面序、改述双腿命中、三腿全空返回空不变、两模式
  各自确定性）；labels 预过滤；importance 先验（显式权重生效、默认零变化）；
  近重去重（阈值上/下、`dedup_suppressed`、无向量豁免）。
- 融合对照评估：确定性语料（Id128 计数器 id + 固定向量，digest 锚定），
  weighted vs RRF 三类 archetype 查询对照。
- reranker 契约：MockHttpTransport 覆盖请求形状/凭据解析/成功映射/形状畸/
  非 2xx/批次上限/截断/全等集融合/整批失败。
- 示例：离线 scripted 重放真实捕获响应 + 故障降级轮（consumer 标签）。
- 真实供应商冒烟：宿主授权 key 后真调 `/v1/rerank`，证据入维护计划验证表。

## 关联文档和工作项

- 工作项：[`maintenance-2026-11-hybrid-retrieval`](../plans/maintenance-2026-11-hybrid-retrieval.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §14
- API：[`docs/api/context-memory.md`](../api/context-memory.md)、
  [`docs/api/model-agent-loop.md`](../api/model-agent-loop.md)
- 相关：[DEC-060](DEC-060-memory-embedding-provider.md)、
  [DEC-111](DEC-111-memory-embedding-producer-and-vector-identity.md)、
  [DEC-032](DEC-032-context-intelligence-layered-context.md)（reranker 供给
  供应链口径）、[DEC-058/061](DEC-058-mutation-ledger-scope-binding.md)
  （写入侧 claim 去重）
