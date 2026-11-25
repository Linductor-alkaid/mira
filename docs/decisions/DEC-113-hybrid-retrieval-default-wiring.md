# DEC-113：混合检索默认接线（默认融合翻转与一呼式组合辅助器）

> 状态：Accepted
> 日期：2026-11-18
> 负责人：Mira Maintainers
> 冻结里程碑：2026-11 混合检索默认接线维护轮
> 替代/被替代：修订 [DEC-112](DEC-112-hybrid-retrieval-fusion.md) 决策 1 的默认
> 条款（"WeightedSum 默认，行为零变化"）——执行 DEC-112 备选方案第 1 条预授权的
> "对照评估后切默认"路径，其前置（HR-11 检索质量基准）已于同日交付；
> DEC-112 其余决议继续有效
> 关闭的缺口：DEC-111/112 的全部检索能力处于"宿主手工装配"状态——store 默认
> 加权和、去重默认关、recall embedder 默认空、reranker 无注入面；最优形态
> （基准 E 档）需要宿主按正确顺序完成 4–8 步装配

## 背景与问题

DEC-111/112 交付了完整的混合检索能力，但采用"能力交付 + 默认保守"策略：默认
配置仍是 DEC-111 之前的形态（加权和、无语义腿、无重排、去重关）。盘点确认
（2026-11-18）：`RetrievalFusion`/`near_dup_threshold` 无任何生产代码设置，
`OpenAiCompatibleEmbeddingProvider`/`OpenAiCompatibleReranker` 只在
examples/tests/benchmark 中构造，`set_recall_embedder` 默认空，
`schedule_context_rerank` 无生产调用点。基准
[memory-hybrid-retrieval-v1](../benchmarks/memory-hybrid-retrieval-v1.md)
量化了最优形态：E 档（RRF + 去重 0.85 + rerank）R@1 0.742 / MRR 0.912，
对比纯词法基线 R@1 0.100。

两条支撑事实使"默认最优"成为低风险变更：

1. **RRF 无向量时退化为词法序**（实现验证：`src/storage/sqlite_memory_store.cpp`
   融合段——空查询向量时向量腿跳过，RRF 只剩 exact 常数票 + FTS rank 票，
   集合内归一化保序；`MemoryQuery` 无分数阈值字段，成员只受排序影响）。
   翻默认对不使用语义腿的宿主是零行为变化。
2. **RRF 对有向量宿主的 R@1 代价（−10.0pp）有缓解路径**：默认形态配套
   reranker 注入面（E 档实测 R@1 0.404→0.742）；确需首位优先且不重排的宿主
   可显式覆盖回 WeightedSum（枚举保留，非移除）。

同时，宿主手工装配存在真实摩擦：Profile 字段赋值 + validate + provider 构造 +
`set_recall_embedder`（不可逆、须在 `initialize()` 前）+ indexer/repair 路由 +
store options 设置，共 4–8 步且顺序敏感（校验失败后已注入的 embedder 无法撤销）。

## 决策

### 1. store 默认融合翻转为 ReciprocalRank

- `SqliteMemoryStoreOptions::fusion` 默认值 `WeightedSum` → `ReciprocalRank`
  （`include/mira/sqlite_memory_store.hpp`）。`WeightedSum` 保留为可选项，
  宿主可显式覆盖。
- 无向量查询在新默认下与旧默认结果序一致（零行为变化，测试锚定）；有向量
  查询获得基准确认的深度增益（R@3 +19.2pp / R@5 +15.0pp / MRR +3.2pp）。
- 这是治理规范第 8 节意义下的**产品默认值变更**，以本记录为据；DEC-112 备选
  方案第 1 条的拒绝理由（"加权和被大量既有测试与基准锚定"）由 HR-11 基准
  交付解除。

### 2. 一呼式组合辅助器（新公开契约）

新增 `include/mira/hybrid_retrieval_wiring.hpp` /
`src/context/hybrid_retrieval_wiring.cpp`，纯 Core（include std + `<mira/...>`，
无新外部头族，architecture-policy 不变）：

- `HybridRetrievalConfig`：宿主数据 struct（embedding origin/prefix/model/dims、
  rerank origin/prefix/model/max_output、共用 `credential_secret_name`（仅
  SecretRef 名，永不持有明文）、deadline、`fusion`（默认 ReciprocalRank）、
  `near_dup_threshold`（默认 0.85——bge-m3 中文短句改述近重对实测 ≈0.86 的
  标定值，换 embedder 须重标定，0 = 关）+ `validate()`。
- `hybrid_retrieval_store_options(config, base)`：open() 前调用，写入
  fusion/near_dup_threshold。
- `wire_hybrid_retrieval(config, transport, index*, agent*)`：open() 后一次调用
  返回 `HybridRetrievalStack`（持有 provider/indexer 与 EmbedderId 观测字段）。
  校验完全前置——两条 Profile 构建 + validate 全部成功后才执行不可逆注入
  （`set_recall_embedder`）与 indexer 构造；任一腿缺失则该腿为空（仅 embedding、
  仅 rerank、双无均为合法形态）。HTTP 栈仍由宿主注入 `IHttpTransport`
  （Core 不做 socket，DEC-111 分工不变）。
- `HybridRetrievalStack` 便捷路由：`schedule_embedding_repair`（Deferrable）与
  `schedule_context_rerank`（Interactive）直接转发 `ContextMemorySupervisor`
  既有路径——接线器是同步 facade，不新增任何并发设施。
- 配置面为纯 struct，不做 JSON/TOML 解析（仓库无 profile 解析先例；解析面
  留给有真实宿主需求时立项）。

### 3. reranker 本轮只注入 provider

接线器构建并持有 `OpenAiCompatibleReranker`，宿主在自己的 Layer 2 调用点传入
（或经 stack 便捷路由）。Layer 2 生产链整合（谁在什么时机调
retrieve→rerank）**另立维护轮**——当前 `schedule_context_rerank` 无生产调用点，
本轮不虚构调用时机、不改变任何现有管线行为。

### 非目标

- 250ms 记忆代理 recall 路径挂 rerank（DEC-112 deadline 纪律原样保持）。
- Layer 2 生产链整合（上述决策 3）。
- JSON/TOML 配置解析面；近重阈值按 embedder 自动标定；ANN/MMR。
- 记忆写入路径自动 embedding（`MemoryEmbeddingIndexer` 头注释明示的既有设计：
  写路径每写必嵌会把模型调用放上控制路径，修复 pass 是宿主路由的 Deferrable
  工作——本轮只把路由变得更顺手，不改变该设计）。

## 备选方案

- **只加辅助器、store 默认保持 WeightedSum**：拒绝。默认最优的价值在无配置
  宿主也受益；RRF 无向量时与加权和等价（已验证），有向量时深度增益被基准
  证明，保留显式覆盖即可回退。DEC-112 预授权路径正是为这一步保留的。
- **只翻默认、不加辅助器**：拒绝。翻默认只解决融合一项；embedding/reranker
  的装配摩擦（顺序敏感的不可逆注入）才是"未生效"的主要成因。
- **辅助器顺带把 rerank 串进 working-context 加载链**：拒绝（本轮）。生产
  调用点不存在，需要定义载入时机/deadline 预算/降级路径，范围不成比例。
- **辅助器内自动调度 repair 循环**：拒绝。修复 pass 的触发时机是宿主策略
  （写入频率、成本预算）；辅助器提供路由便利，不发明触发策略。

## 影响与风险

- 产品默认值变更：无向量宿主零行为变化（测试锚定）；有向量宿主排序变化
  （增益为主，R@1 例外已记录）；既有测试如依赖旧默认断言加权和分值，改为
  显式 `WeightedSum` 保留断言语义。
- 公开契约加法：`hybrid_retrieval_wiring.hpp`（新头，契约四件套：本记录 +
  API 文档 + 离线示例 + 契约测试）；store options 默认值字段变更（enum 默认
  翻转，无 ABI 布局变化）。
- 供应链零新增：模型/端点全部宿主 profile 数据；不硬编码任何供应商路径。
- 近重默认 0.85 是 bge-m3 标定值：接线器默认即为 E 档形态的前提是该标定；
  换 embedder 的宿主应覆盖该字段（文档明示）。
- 无 Executor 能力边界变化，不登记反馈台账；无新外部头族，无
  architecture-policy 变更。

## 验证方式

- 契约测试（`tests/m4/m4_hybrid_wiring_test.cpp`，离线 MockHttpTransport）：
  全量接线（options 为 RRF+0.85、recall 携带正确身份向量、repair lag→repaired）、
  单腿形态、非法配置拒绝且 agent 未被触碰（不可逆注入前置校验回归门）、
  无向量查询默认 RRF 与显式 WeightedSum 结果序一致、stack 便捷路由冒烟。
- 既有回归：依赖旧默认的用例显式化后全量 ctest 通过。
- 离线示例（`examples/hybrid_retrieval_host_consumer.cpp`，consumer 标签）：
  一呼式接线 → 种子 → repair → 带向量 recall → Layer 2 rerank → 故障降级轮。
- 门禁四目标 + static-analysis 改动文件零告警 + 新文件 clang-format。

## 关联文档和工作项

- 工作项：[`maintenance-2026-11-hybrid-default-wiring`](../plans/maintenance-2026-11-hybrid-default-wiring.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §14
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 基准：[`docs/benchmarks/memory-hybrid-retrieval-v1.md`](../benchmarks/memory-hybrid-retrieval-v1.md)（默认翻转的量化依据）
- 相关：[DEC-112](DEC-112-hybrid-retrieval-fusion.md)（被本记录修订默认条款）、
  [DEC-111](DEC-111-memory-embedding-producer-and-vector-identity.md)
  （provider 分工与身份守卫）

## 修订说明（2026-11-25）

[DEC-147](DEC-147-lexical-segmentation-and-query-composition.md) 修复了词法腿的分词与
查询组合，本记录的以下表述随之调整，其余决议不变：

- "无向量查询在新默认下与旧默认结果序一致"只在词法差距大于先验差距时成立。此前
  两者同序，是因为 AND 组合下词法腿几乎没有多命中；多命中时 `WeightedSum` 线性叠加
  先验，可以盖过词法差距。锚点测试改为断言默认融合保持词法腿的相关度顺序。
- 作为默认翻转量化依据的基准数字（纯词法 R@*=0.100 及各配置差值）产生于词法腿对
  中文失效的代码，重测前不得继续引用。
