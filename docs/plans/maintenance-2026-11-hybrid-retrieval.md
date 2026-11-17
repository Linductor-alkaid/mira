# 维护计划：混合检索融合与远端 Reranker（2026-11）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M36 后维护轮）
> 前置：[MNT-202611-EP embedding 生产者](maintenance-2026-11-memory-embedding-producer.md)（同日交付）
> 建议发布点：Host contract alpha 修订（公开契约加法变更，见 DEC-112 影响与风险）
> 更新日期：2026-11-17

## 1. 目标

把业界收敛的混合检索范式（RRF 融合 BM25+向量 → cross-encoder 两阶段重排 →
元数据预过滤 → 先验叠加）落进 Mira 记忆存储面，并补齐 `IContextReranker` 的
远端协议适配器。融合模式选项制（默认 WeightedSum 零变化），全部公开契约变更
为加法。

调研依据：RRF（Cormack SIGIR 2009，k=60）为 Elasticsearch/Azure/Qdrant/
Weaviate/Milvus/Zep-Graphiti 收敛默认；两阶段参数通行 K=50–200、N=5–20；
bge-m3 dense 1024 维 + bge-reranker-v2-m3 经硅基流动双端点真实实测
（`/v1/embeddings`、`/v1/rerank`），确立三条边界：词面序被加权和翻转（RRF
修复）、reranker 只排主题相关不裁决冲突、近重阈值须按 embedder 标定
（bge-m3 改述近重 ≈0.86）。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/sqlite_memory_store.hpp` / `src/storage/sqlite_memory_store.cpp`：
  `RetrievalFusion` 枚举与 `fusion`/`near_dup_threshold` 选项、RRF 融合分支、
  近重去重、schema {1,3}（`labels`/`importance` 列 + 链式迁移）、labels 预过滤、
  Row/SELECT/write 路径同步。
- `include/mira/memory_contracts.hpp` / `src/context/memory_contracts.cpp`：
  `MemoryRecord.labels/importance`、validate 边界、record JSON 往返
  （`memory_schema_current()` {1,0}→{1,1}）、`MemoryQuery::labels`、
  `MemoryQueryQuality::dedup_suppressed`、`RetrievalWeights::importance`（默认 0.0）。
- 新增 `include/mira/reranker_provider.hpp` / `src/model/reranker_provider.cpp`：
  `RerankerProfile` + `OpenAiCompatibleReranker : IContextReranker`。
- 测试：m4 迁移/RRF/元数据/去重用例、新 `tests/m4/m4_fusion_eval.cpp`
  （确定性融合对照）、新 `tests/m3/m3_reranker_provider_test.cpp`、既有用例同步。
- 新增 `examples/reranker_host_consumer.cpp`（离线 scripted）。
- `CMakeLists.txt`（mira_core 源列表、示例目标）、`tests/CMakeLists.txt`、
  `tests/headers/CMakeLists.txt`。
- 文档：DEC-112（新增）、设计 §14、API 三处、术语表、reference-study §2、
  总计划注记、README 状态行、本计划。

### 2.2 非目标（DEC-112 明示）

- 250ms 记忆代理 recall 路径接远端 rerank；MMR 多样化；Layer1 lexical 升级
  BM25；access_count/last_access；ANN；labels 索引；把 reranker 并入
  ModelGateway。

## 3. 设计与决策依据

- 决策记录：[DEC-112](../decisions/DEC-112-hybrid-retrieval-fusion.md)。
- 外部调研：RRF 出处与生产系统参数（ES `rank_constant`=60、Azure rerank
  top-50、Zep/Graphiti BM25+cosine+图 RRF + bge-reranker-v2-m3/MMR recipe、
  mem0 写入侧去重、Generative Agents 先验叠加）；两阶段通行参数
  K=50–200 → N=5–20；MMR fetch_k=4×k、λ=0.5–0.7（留实验档）。
- 真实实测：`build/hybrid-probe/hybrid_probe.py`（五路对比：BM25/加权/RRF/
  RRF+重排/去重+MMR，三 archetype 查询）——词面序翻转修复、reranker 主题性
  边界、近重阈值标定三条结论。
- 复用：embedding_provider 先例（transport 注入/错误映射/limits 投影）、
  IContextReranker 契约与降级语义（m18）、m17/m18 确定性技巧（Id128 计数器
  id、digest 锚定）、MockHttpTransport、1.2 迁移模板。

## 4. 工作项（跑前冻结）

- [x] `MNT-202611-HR-01` 立项与跑前冻结：DEC-112 落成、本计划冻结。
- [x] `MNT-202611-HR-02` schema 1.3 元数据：MemoryRecord 字段/validate/JSON
      往返（record 文档 {1,1}）、store 列 + 链式迁移（纯加法）、labels 预过滤、
      write/Row/SELECT 同步。
- [x] `MNT-202611-HR-03` RRF 融合模式：`RetrievalFusion` 选项、weighted-RRF
      分支（FTS+cosine 排名、k=60、归一化、先验叠加含 importance）、
      quality 模式报告；默认 WeightedSum 零变化。
- [x] `MNT-202611-HR-04` 近重去重：`near_dup_threshold` 选项、排序后 top-64
      有界区间、身份向量豁免、`dedup_suppressed` 计数。
- [x] `MNT-202611-HR-05` Reranker 适配器：`RerankerProfile` +
      `OpenAiCompatibleReranker : IContextReranker`（/v1/rerank wire、
      fail-closed 解析、map_http_error_status、整批错误、0.60/0.40 融合镜像）；
      CMake 登记。
- [x] `MNT-202611-HR-06` 测试：m4 迁移/RRF 门/labels/importance/近重去重、
      m3 reranker 契约、m4_fusion_eval 确定性对照（digest 锚定）、既有用例同步。
- [x] `MNT-202611-HR-07` 离线示例：scripted transport 重放真实捕获响应 +
      故障降级轮（consumer 标签）。
- [x] `MNT-202611-HR-08` 真实冒烟：scratch 消费者真调 `/v1/rerank`
      （key 经 auth.json/env，代理 127.0.0.1:7897），证据入验证表。
- [x] `MNT-202611-HR-09` 文档同步：设计 §14、API 三处、术语表、
      reference-study §2、总计划注记、README 状态行、本计划验证记录。
- [x] `MNT-202611-HR-10` 接线缝收尾（同日追加）：`MemoryWorkBinding::labels`
      （bind 时 refuse 校验 → Recall 查询自动携带预过滤，m36 门）、
      `WorkingContextPromotionPolicy::labels`（提升候选携带宿主标签，
      policy 边界校验，m23 门）。

### 跑前冻结八问

1. **行为**：DEC-112 决策 1-4；设计 §14 增补融合模式与先验重排。
2. **所有者**：宿主拥有 reranker profile/transport/Executor 路由；Core 拥有
   契约与协议适配器与 store 融合实现。
3. **契约面**：`sqlite_memory_store.hpp`（选项/权重/枚举）、
   `memory_contracts.hpp`（记录/查询/quality/文档 minor）、store schema
   {1,2}→{1,3}、`reranker_provider.hpp`（新）。
4. **层与依赖**：core 新增文件仅 include std/`<mira/...>`；无新外部头族；
   policy 无需登记。
5. **复用**：transport/错误映射/limits 投影/IContextReranker 降级语义/
   迁移模板/测试基建全部复用，不新增平行 HTTP 或评分路径。
6. **时序幂等**：迁移幂等（逐列探测）且失败回滚不盖版本戳；RRF/去重均为
   纯函数式查询后处理；重放安全。
7. **Executor 路由**：rerank 为阻塞调用，仅在宿主 worker（supervisor
   Interactive `schedule_context_rerank`）执行；失败整批降级；250ms 路径
   不接线（非目标）。
8. **验证上下文**：离线全链测试 + 确定性融合对照 + 真实冒烟（HR-08）；
   门禁四目标 + tidy。

## 5. 风险与阻塞

- 公开契约加法变更（选项/字段/minor 版本）：默认配置行为零变化，风险可控；
  安装面消费者随 PR 同步。
- 近重阈值 per-embedder 经验值：换 embedder 需重标定；阈值不进契约、默认关。
- reranker 远端调用延迟：仅 opt-in + Interactive 路由 + 整批失败降级；
  250ms 路径明确不接。
- 融合对照评估的判据是 archetype 级（词面序保持/改述命中/确定性），不是
  检索质量增益证明——质量基准（LongMemEval 式金标集）留后续轮。
- 无 Executor、平台阻塞；真实冒烟依赖用户提供过授权的既有 key（已验证连通）。

## 6. 测试与退出条件

- [x] m4 迁移用例：1.2 降版 → `Migrated`（列加、数据存活）→ `UpToDate`。
- [x] RRF 门：标识符词面序保持、改述双腿命中、三腿全空返回空、两模式各自
      确定性、默认模式零变化（既有加权用例不改断言即通过）。
- [x] labels 预过滤 + importance 先验（显式权重生效/默认零变化）。
- [x] 近重去重：阈值上/下、计数、无向量豁免。
- [x] m3 reranker 契约：请求形状/凭据/解析/映射/批次上限/截断/非 2xx/失败降级全绿。
- [x] m4_fusion_eval：digest 锚定 + 三 archetype 判据全绿。
- [x] 全量 `ctest --preset debug` 通过（预期 ~190+ 项）。
- [x] `architecture-check`、`platform-boundary-check`、`check_docs`、
      `format-check`、static-analysis 改动文件零告警。
- [x] `reranker_host_consumer` 正常退出（0），含降级轮。
- [ ] 真实供应商 `/v1/rerank` 冒烟：见 §7。

## 7. 验证记录

### 2026-11-17：立项与跑前冻结

- DEC-112 落成；本计划冻结。前置实测证据：`build/hybrid-probe/hybrid_probe.py`
  五路对比（真实 API），两端点连通性验证。
- 未执行项：HR-02…09 与真实冒烟。

### 2026-11-17：实现、测试与文档同步（HR-02…09）

- **范围**：`RetrievalFusion` 选项与 weighted-RRF 分支（候选集内 min-max 归一化 +
  先验叠加，实现期发现：理论最大值归一化会让先验压过腿差距，已改为集合归一化）、
  schema 1.3（labels/importance 列 + 纯加法迁移 + 文档 JSON 为权威投影——迁移后
  列默认直到下次写刷新，document 仍可读）、labels 预过滤（json_each any-of）、
  近重去重（top-64 有界区间 + 身份豁免 + `dedup_suppressed`）、
  `RetrievalWeights` 挂进 options（此前是文档化默认值但宿主不可配置的潜在缺口）、
  `OpenAiCompatibleReranker` + 契约测试、`m4_fusion_eval`（7 门：标识符词面序
  保持/改述向量召回/空腿纪律/重开确定性/labels/importance/近重）、离线示例。
- **依据**：[DEC-112](../decisions/DEC-112-hybrid-retrieval-fusion.md)；真实探针
  `build/hybrid-probe/hybrid_probe.py`（五路对比）。
- **环境**：Linux 7.0.0-43-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；
  clang-tidy 18（miniconda）；14 核；基线 = 同日 DEC-111 工作树（连续实施轮）。
  工作树未提交，随 PR 提交。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警 |
  | `ctest --preset debug -j 14` | **186/186 通过**（新增 m3 reranker 契约、m4 fusion eval、1.3 迁移用例、reranker 示例；含全部回归） |
  | architecture / platform-boundary / docs / format 四门禁 | 全部通过（format 381→386 文件） |
  | static-analysis（clang-tidy 18） | 改动文件零告警 |
  | `mira_reranker_host_consumer` | 退出 0：重排反转 + 降级轮 |
  | **真实冒烟**（scratch 消费者 `build/hybrid-probe/rerank_smoke.cpp`，g++ 直链库） | **通过**：`OpenAiCompatibleReranker` 真调 `https://api.siliconflow.cn/v1/rerank`（BAAI/bge-reranker-v2-m3，4 候选），返回 relevance_score 与 Python 探针逐位一致（0.468769…）；矛盾偏好在"别太啰嗦"查询下按主题相关居首，再次实证 reranker 不裁决冲突 |

- **实现期发现（后续轮次参考）**：
  1. RRF 先验尺度失配：理论最大值归一化下候选间腿差距（~0.004）小于先验差距
     （~0.015），先验反客为主——改为候选集内 min-max 后腿差距恒占优。
  2. `apply` 按 kind 策略补默认 TTL（EnvironmentFact 7 天）：测试语料用固定历史
     时间戳会"写入即过期"，被 universe 的 `expires_at > now` 过滤——测试时间必须
     锚定当前时间附近。
  3. `fts_leg_ran`/`vector_leg_ran` 语义是"腿执行过"而非"腿命中过"（零命中也是
     ran）；正交 0 分是合法命中（DEC-077），构造"全腿空"用例需三腿皆真空。
  4. 近重对抑制的是融合排名更低者（不保证语义"正本"存活）；多样性 cap 会截断
     无排序查询的结果数——去重测试需先解除 cap 干扰。
- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 与 Windows/Android 未本地执行，随 PR CI；按测试分层，
    发版前全量独立验证另行委派。
  - 融合/去重的检索质量增益未量化（archetype 级判据 + 确定性锚定，非金标集
    评估）；LongMemEval 式基准留后续轮。
  - reranker 成本/延迟分布未测量（单次调用实测亚秒级，量级为 infer 零头）。
- **同步**：`docs/design/context_and_memory_design.md`（§14.1a）、
  `docs/api/context-memory.md`、`docs/api/model-agent-loop.md`、`docs/api/index.md`、
  `docs/project/glossary.md`（RRF 融合/近重去重词条）、
  `docs/design/memory-storage-reference-study.md`（§2 表）、
  `docs/plans/mira-implementation-plan.md`（顶部注记）、`README.md`（状态增补）、
  `docs/decisions/DEC-112-…`（新增）、本计划。
