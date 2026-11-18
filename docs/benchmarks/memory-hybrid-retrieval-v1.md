# 记忆混合检索质量基准 v1（DEC-112 首轮量化）

> 状态：Active（首轮量化，登记于 2026-11-17）
> 负责人：Mira Maintainers
> 适用范围：[DEC-112](../decisions/DEC-112-hybrid-retrieval-fusion.md)、
> [DEC-111](../decisions/DEC-111-memory-embedding-producer-and-vector-identity.md)；
> `SqliteMemoryStore` 五配置检索对照 + 远端 cross-encoder 重排
> 复现命令与 harness 见文末（人工工具，需真实供应商 key，不进 ctest）

## 1. 目的与范围

量化 DEC-111/112 检索能力的**真实质量增益**，关闭维护计划中"增益未量化"
的遗留项。与 [M17/M18 基准](context-intelligence-retrieval-v1.md)（token-hash
确定性 embedder，度量管线行为）不同，本基准用**真实 embedder/reranker**
（硅基流动 `BAAI/bge-m3` + `BAAI/bge-reranker-v2-m3`）度量语义检索质量，
并且驱动的是 shipped 的 `SqliteMemoryStore` 与协议适配器——度量对象是
交付代码本身，不是复刻实现。

明确不声称：本基准不是 LongMemEval 式的通用记忆能力评测（单数据集、金标
人工构造、20 查询、单 embedder）；数字支持的是**配置间的相对对照**与
**参数敏感性结论**，不构成对任意语料的绝对质量保证。

## 2. 数据集与方法

- **数据集**：`tests/benchmark/hybrid-retrieval/eval_dataset.json`，44 条
  双语记忆（8 偏好 / 10 环境事实（含 Latin 标识符）/ 6 流程 / 6 情节 /
  4 教训 / 3 近重对（6 条）/ 6 干扰项（其中 3 条与查询共享 Latin 词面））；
  20 条金标查询：paraphrase 9（与 gold 零词面重叠）、lexical 6（Latin 标识符
  锚点）、multi 5（金标 2–3 条）。
  **digest**：canonical JSON（sort_keys）SHA-256
  `c8767eba4343ca1f13bbee36efb700ad743963b4a112dcec08faa6c866b26fe6`。
- **bitemporal/per-kind 语义**：写入走真实 `apply` 闸门（per-kind TTL、验证
  地板、scope 允许表全部生效；Procedure/RecoveryLesson 落 Application/Agent
  scope；语义腿按 per-kind 策略对 Episode/RecoveryLesson/Procedure 扣留）。
- **配置**（全部经 shipped `SqliteMemoryStore::query`）：
  - **A 纯词法**：无查询向量（DEC-111 之前的形态）
  - **B 加权和**：默认 `WeightedSum` + 查询向量（DEC-111 后的默认形态）
  - **C RRF**：`ReciprocalRank` + 查询向量
  - **D RRF+近重去重**：`near_dup_threshold = 0.85`
  - **E D + 重排**：D 序 Top-8 → `OpenAiCompatibleReranker`（真 API）→ Top-5
- **指标**：Recall@{1,3,5,10}、MRR@10；两种金标口径——**strict**（字面金标）
  与 **dup-aware**（金标扩展近重孪生：内容等价的重复命中不计失败）；另报
  近重冗余计数（一对的两位同时出现在 Top-5）。
- **环境**：Ubuntu 24.04 x86_64、GCC 13.3.0、SQLite 3.53.4、硅基流动端点经
  本地代理；harness `tests/benchmark/hybrid-retrieval/`。

> 2026-11-18 注记（[DEC-113](../decisions/DEC-113-hybrid-retrieval-default-wiring.md)）：
> 本轮对照评估是默认翻转的量化依据——C/D/E 档组合（RRF 默认融合、近重去重 0.85、
> reranker 注入面）经 DEC-113 成为交付默认形态（`hybrid_retrieval_wiring.hpp`
> 一呼式接线器 + store 默认 `ReciprocalRank`）；A/B 档降为显式 opt-in 覆盖
> （`WeightedSum` 保留可选）。

## 3. 结果

### 3.1 总体（dup-aware，20 查询）

| 配置 | R@1 | R@3 | R@5 | R@10 | MRR@10 |
|---|---|---|---|---|---|
| A 纯词法 | 0.100 | 0.100 | 0.100 | 0.100 | 0.100 |
| B 加权和+向量 | 0.504 | 0.646 | 0.750 | 0.887 | 0.697 |
| **C RRF+向量** | 0.404 | **0.838** | **0.900** | **0.925** | 0.729 |
| D C+去重 | 0.404 | 0.796 | 0.858 | 0.871 | 0.729 |
| **E D+重排** | **0.742** | 0.796 | 0.833 | 0.833 | **0.912** |

（strict 口径数字略低但方向一致；分类型明细由 `eval_metrics.py` 复现。）

### 3.2 分类型要点（dup-aware）

| 类型 | A | B | C | D | E |
|---|---|---|---|---|---|
| paraphrase R@1（9） | 0.000 | 0.444 | 0.389 | 0.389 | **0.833** |
| paraphrase R@5（9） | 0.000 | 0.667 | 0.889 | 0.833 | 0.833 |
| lexical R@1（6） | 0.167 | 0.833 | 0.500 | 0.500 | **1.000** |
| lexical R@5（6） | 0.167 | 1.000 | 1.000 | 1.000 | 1.000 |
| multi R@3（5） | 0.000 | 0.400 | 0.800 | 0.760 | 0.760 |
| multi R@5（5） | 0.000 | 0.600 | 0.800 | 0.760 | 0.800 |

近重冗余（同对两位共现 Top-5，全查询累计）：A=0、B=2、C=2、**D=0**、E=0。

### 3.3 五条量化结论

1. **语义腿是决定性增量（DEC-111 的价值）**：A→B 的 R@10 从 0.100 到
   0.887（+78.7pp）；9 条 paraphrase 查询在纯词法下全军覆没（R@*=0.000）。
2. **RRF 相对加权和的提升在深度上（DEC-112）**：R@3 +19.2pp（0.646→0.838）、
   R@5 +15.0pp（0.750→0.900）、R@10 +3.8pp；代价是 R@1 −10.0pp——RRF 把
   词面 rank-0 与向量 rank-0 平票化，首位被两类腿共享。MRR +3.2pp。
3. **重排的提升在首位上**：D→E 的 R@1 +33.8pp（0.404→0.742）、MRR
   +18.3pp（0.729→0.912）；lexical 类 R@1 到 1.000（cross-encoder 修复了
   RRF 首位平票）。代价：multi 类 R@1 从 0.400 降到 0.267（strict）——
   重排把多意图收敛到单一最相关项，分散金标的首位命中下降，R@5/R@10
   不受损。
4. **近重去重达成冗余目标但有召回代价**：冗余共现 2→0；但 strict 口径
   R@5 −5.0pp、R@10 −6.6pp——本数据集中被抑制的近重成员（pr1/pr5 对）
   与金标成员内容等价，抑制可能落在金标侧。dup-aware 口径下代价缩小到
   R@5 −4.2pp / R@10 −5.4pp。**工程结论**：去重适合"重复挤占上下文预算"
   场景；金标/下游对近重成员敏感时应用更严阈值（≥0.90）或允许近重并存。
5. **per-kind 语义腿扣留是结构性上限（DEC-059 预期行为）**：Episode/
   RecoveryLesson/Procedure 的语义型 gold 在所有配置下不可达（语义腿
   扣留 + CJK 整词面不匹配词法腿），QM1 上限 R@10=0.5 即源于此。放宽需
   改 per-kind 策略表，不是检索融合问题。

## 4. 局限与后续

- 单数据集、人工金标、20 查询：数字支持配置间相对对照，不支持跨语料外推；
  扩集与 LongMemEval 式评测留后续轮。
- reranker 的 multi-R@1 代价提示：多意图查询的重排输入应保留更多候选或
  按 per-query intent 分组——需要意图感知的检索前端（未立项）。
- 未测：RRF k 敏感性、min_confidence/importance 权重扫描、延迟/成本分布。

## 5. 复现

harness 与命令：`tests/benchmark/hybrid-retrieval/README.md`。首轮数字由
commit 时的 libmira_core/state_store 静态库 + 硅基流动端点产出；数据集
digest 断言见 §2。嵌入与重排分数可复现（同端点同模型确定性解码，实测
C++/Python 双实现 relevance_score 逐位一致）。
