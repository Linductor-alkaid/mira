# 维护计划：混合检索默认接线（2026-11）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M36 后维护轮）
> 前置：[MNT-202611-HR 混合检索](maintenance-2026-11-hybrid-retrieval.md)
> （含 HR-11 质量基准，同日交付）
> 建议发布点：Host contract alpha 修订（新公开契约 + 产品默认值变更，见
> DEC-113 影响与风险）
> 更新日期：2026-11-18

## 1. 目标

把 DEC-111/112 的混合检索能力从"宿主手工装配"推进为"默认最优接线"：
① store 默认融合翻转为 `ReciprocalRank`（执行 DEC-112 预授权路径，量化依据
为 [memory-hybrid-retrieval-v1](../benchmarks/memory-hybrid-retrieval-v1.md)
基准）；② 新增一呼式组合辅助器 `wire_hybrid_retrieval`（校验前置、不可逆
注入后置、双腿可选、便捷路由经 supervisor 既有路径）。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/sqlite_memory_store.hpp`：`fusion` 默认值翻转。
- 新增 `include/mira/hybrid_retrieval_wiring.hpp` /
  `src/context/hybrid_retrieval_wiring.cpp`：`HybridRetrievalConfig`/
  `HybridRetrievalStack`/`hybrid_retrieval_store_options`/`wire_hybrid_retrieval`。
- 测试：新 `tests/m4/m4_hybrid_wiring_test.cpp`；既有依赖旧默认的用例显式化；
  无向量查询两模式等价门。
- 新增 `examples/hybrid_retrieval_host_consumer.cpp`（离线 scripted 全链）。
- `CMakeLists.txt`（mira_core 源列表、示例目标）、`tests/CMakeLists.txt`、
  `tests/headers/CMakeLists.txt`。
- 文档：DEC-113（新增）、DEC-112 修订注记、设计 §14、API、术语表、
  基准文档注记、总计划注记、README 状态行、本计划。

### 2.2 非目标（DEC-113 明示）

- 250ms recall 路径挂 rerank；Layer 2 生产链整合（另立维护轮）；
  JSON/TOML 配置解析面；近重阈值自动标定；写路径自动 embedding；
  ANN/MMR。

## 3. 设计与决策依据

- 决策记录：[DEC-113](../decisions/DEC-113-hybrid-retrieval-default-wiring.md)
  （修订 DEC-112 默认条款）。
- 量化依据：HR-11 基准五配置对照——E 档（RRF+去重 0.85+rerank）R@1 0.742/
  MRR 0.912；RRF 无向量退化为词法序（实现读码验证：向量腿跳过、无分数阈值
  字段）。
- 复用：Profile/validate 模式（embedding/reranker provider）、
  `set_recall_embedder` 降级语义（m36 已接线）、`MemoryEmbeddingIndexer`
  修复 pass 与 supervisor 既有 schedule 路由、MockHttpTransport/MapSecretResolver、
  consumer 注册模板。

## 4. 工作项（跑前冻结）

- [x] `MNT-202611-HRW-01` 立项与跑前冻结：DEC-113 落成、DEC-112 修订注记、
      本计划冻结。
- [x] `MNT-202611-HRW-02` store 默认融合翻转：默认值 `ReciprocalRank`；
      全量 ctest 零回归（无既有用例依赖旧默认断言）；无向量查询两模式等价门
      入 m4_hybrid_wiring_test。
- [x] `MNT-202611-HRW-03` 接线器契约：`HybridRetrievalConfig`/`Stack`/
      `hybrid_retrieval_store_options`/`wire_hybrid_retrieval`（校验完全
      前置后才不可逆注入）；CMake/headers 登记。
- [x] `MNT-202611-HRW-04` 契约测试：全量接线/单腿形态/非法配置拒绝且 agent
      未触碰/两模式等价/便捷路由冒烟。
- [x] `MNT-202611-HRW-05` 离线示例：一呼式接线 → 种子 → repair → 带向量
      recall → Layer 2 rerank → 故障降级轮（consumer 标签）。
- [x] `MNT-202611-HRW-06` 文档同步：设计 §14（默认融合 + 接线器节 14.1b）、
      `docs/api/context-memory.md`（store 默认值 + 接线器节）、
      `docs/api/model-agent-loop.md` 注记、术语表词条（一呼式默认接线）、
      基准文档注记（E 档现为默认形态）、总计划注记、README 状态行。
- [x] `MNT-202611-HRW-07` 全量验证：构建 + `ctest --preset debug` 全过 +
      门禁四目标 + static-analysis 零告警 + clang-format。
- [x] `MNT-202611-HRW-08` 证据回填：本计划验证记录；CI 证据合并后补记。

### 跑前冻结八问

1. **行为**：DEC-113 决策 1–3；无向量宿主零行为变化（等价门锚定），有向量
   宿主获得基准深度增益；设计 §14 增补默认融合与接线器。
2. **所有者**：宿主拥有 config/profile 数据、transport、凭据解析与
   Executor；Core 拥有接线器契约与 store 默认值。
3. **契约面**：`hybrid_retrieval_wiring.hpp`（新头）、
   `sqlite_memory_store.hpp`（fusion 默认值变更）。其余契约零变更。
4. **层与依赖**：core 新增文件仅 include std/`<mira/...>`；无新外部头族；
   policy 不变。
5. **复用**：全部复用既有契约与基建（Profile 模式/降级语义/修复 pass/
   schedule 路由/测试替身），不新增平行 HTTP、评分或并发路径。
6. **时序幂等**：`wire_hybrid_retrieval` 校验前置（失败时 agent/indexer 零
   触碰——`set_recall_embedder` 不可逆，回归门锚定）；`store_options` 纯
   函数；接线器无隐藏状态，重入安全（对同一 agent 重复 wire 会被
   `set_recall_embedder` 的既有拒绝语义挡住）。
7. **Executor 路由**：接线器全部为同步 facade；跨线程路由走 supervisor
   既有 schedule 路径（repair=Deferrable、rerank=Interactive）；250ms recall
   路径不接线（DEC-112 非目标保持）。
8. **验证上下文**：离线契约测试 + 等价门 + 全量 ctest + 四门禁 + tidy；
   CI 覆盖平台矩阵（合并前 PR）。

## 5. 风险与阻塞

- 产品默认值变更：有向量但无 reranker 的宿主 R@1 −10.0pp（基准确认）；
  缓解 = reranker 注入面配套（E 档）+ 显式覆盖回 WeightedSum 的文档化路径。
- 近重默认 0.85 为 bge-m3 标定值：换 embedder 宿主须覆盖
  `near_dup_threshold`（DEC-113 风险节明示）。
- 无向量等价性若被打破（实现缺陷）：等价门测试直接拦截。
- 无 Executor、平台阻塞；本轮无真实供应商依赖（离线确定性验证）。

## 6. 测试与退出条件

- [x] m4 契约测试：全量接线（options RRF+0.85、recall 带正确身份向量、
      repair lag→repaired）、仅 embedding/仅 rerank 形态、非法配置拒绝且
      agent 未触碰、两模式无向量等价、便捷路由冒烟。
- [x] 既有回归全绿：默认翻转后全部既有用例不改断言即通过（187 项零回归）。
- [x] 全量 `ctest --preset debug` 通过（189 项，含新增契约测试与 consumer）。
- [x] `architecture-check`、`platform-boundary-check`、`docs-check`、
      `format-check`、static-analysis 改动文件零告警。
- [x] `hybrid_retrieval_host_consumer` 正常退出（0），含降级轮。
- [x] 文档同步完成（设计/API/术语表/基准注记/总计划/README）。

## 7. 验证记录

### 2026-11-18：立项与跑前冻结

- DEC-113 落成（修订 DEC-112 默认条款）；DEC-112 加修订注记；本计划冻结。
- 未执行项：HRW-02…08。

### 2026-11-18：实现、测试与文档同步（HRW-02…08）

- **范围**：store 默认融合翻转（`sqlite_memory_store.hpp` 默认值 +
  枚举注释）；新公开契约 `hybrid_retrieval_wiring.hpp` /
  `src/context/hybrid_retrieval_wiring.cpp`（`HybridRetrievalConfig` 两腿可选
  配置 + validate、`hybrid_retrieval_store_options` 纯投影、
  `wire_hybrid_retrieval` 校验前置组装、`HybridRetrievalStack` 便捷路由转发
  supervisor 既有 Deferrable/Interactive 路径）；`tests/m4/m4_hybrid_wiring_test.cpp`
  五门契约测试；`examples/hybrid_retrieval_host_consumer.cpp` 离线全链
  （单 scripted transport 同时服务 /v1/embeddings 与 /v1/rerank）；文档七处同步。
- **依据**：[DEC-113](../decisions/DEC-113-hybrid-retrieval-default-wiring.md)；
  量化依据 [memory-hybrid-retrieval-v1](../benchmarks/memory-hybrid-retrieval-v1.md)。
- **环境**：Linux 7.0.0-43-generic x86_64；gcc 13.3.0；clang-tidy 18
  （miniconda，static-analysis preset）；基线 = master `3ed0869`（PR #128 合并后）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警 |
  | `ctest --preset debug -j 14` | **189/189 通过**（默认翻转后 187 项既有回归零失败；新增 wiring 契约测试与 consumer） |
  | architecture / platform-boundary / docs / format 四门禁 | 全部通过（format 391 文件） |
  | static-analysis（clang-tidy 18，static-analysis preset） | mira_core + 新测试/示例目标：改动文件零告警（1 处 `performance-move-const-arg` 修复：`MemoryEmbeddingRepairOptions` 平凡可拷贝，移除 `std::move`） |
  | `mira_m4_hybrid_wiring_test` | 5 门全过 |
  | `mira_hybrid_retrieval_host_consumer` | 退出 0：wired→repair(2)→语义 recall→rerank 反转→降级轮 |

- **实现期发现（后续轮次参考）**：
  1. `MemoryEmbeddingIndexer` 修复 pass 是**逐条 `embed()`**（一行一 POST），
     不是批量 `embed_batch`——脚本化宿主按单条向量响应排布；批量供给只有
     provider 契约默认实现那条路。
  2. `set_recall_embedder` 在 `initialize()` 之后被 `InvalidState` 拒绝：
     "失败后可重接线"的回归门必须用**全新未初始化实例**断言，同一实例的
     二次注入不可用。
  3. `SqliteMemoryStoreOptions::validate()` 含 path 非空检查：接线器投影后
     的 options 断言需先落 path；这也意味着 `hybrid_retrieval_store_options`
     的产物在 open 时被既有校验兜底（纯投影设计的验证依据）。
  4. 默认翻转的等价前提一次通过：无向量查询在新默认（RRF）与显式
     `WeightedSum` 下结果序逐条一致（4 记录/混杂 confidence 语料锚定）。
  5. 既有 187 项测试无一依赖旧默认加和序——RRF 模式下向量在场时的排序
     断言此前均显式构造或与序无关，印证 DEC-112 选项制的隔离效果。
- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 与 Windows/Android 未本地执行，随 PR CI 覆盖；按测试
    分层，发版前全量独立验证另行委派。
  - 真实供应商冒烟不在本轮范围（本轮为离线确定性验证；DEC-111 的真实
    embedding 冒烟仍是登记在案的独立遗留项，补跑条件见其维护计划）。
  - Layer 2 生产链整合（retrieve→rerank 的生产调用时机）另立维护轮，
    DEC-113 决策 3 明示。
- **同步**：`docs/design/context_and_memory_design.md`（§14.1a 修订 +
  §14.1b 新增）、`docs/api/context-memory.md`（store 默认值 + 接线器节）、
  `docs/api/model-agent-loop.md`（reranker 注记）、
  `docs/project/glossary.md`（一呼式默认接线词条）、
  `docs/benchmarks/memory-hybrid-retrieval-v1.md`（DEC-113 注记）、
  `docs/plans/mira-implementation-plan.md`（顶部注记）、`README.md`
  （状态增补）、`docs/decisions/DEC-113-…`（新增）、
  `docs/decisions/DEC-112-…`（修订注记）、本计划。

### 2026-11-18：PR CI 验收（回填）

- PR [#129](https://github.com/Linductor-alkaid/mira/pull/129)（squash 合入
  master `6bf831d`）：head `c1f4ab3` 的 push + pull_request 双管线 **24/24
  job 全绿**（run `44219327534` / `44219365641`，含 ASAN/UBSAN/TSAN、
  Windows Debug/Release 与 Android 双 ABI——本地未执行的两类平台就此覆盖），
  **首轮一次通过，无修复轮**。
- master 合并提交 `6bf831d` 的 push 管线 **12/12 job 全绿**（run
  `44223885645`）。
- 本计划 §6 的全部退出条件达成，计划状态转 Completed。
- gh REST 轮询教训（供后续轮复用）：`gh api --jq` 的**多行表达式**会以
  "expected an object but got: array" 假错失败，轮询脚本的 jq 必须单行。
