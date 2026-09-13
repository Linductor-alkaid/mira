# M18：Context Intelligence Stage C——Layer 2 重排对照实验

> 状态：In Progress（2026-09-13 立项；profile 与门禁先于任何正式评估运行冻结；
> 本地实现与首轮对照评估已完成，PR CI 回填后关闭）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 第 8 条 Stage C）
> 前置：M16（Stage A 基线）；M17（Layer 1 检索召回与检索评估 v1——实验矩阵
> B 列基线）；[Context Intelligence 设计](../design/context_intelligence_design.md)
> §5.3 已冻结
> 建议发布点：非发布物；产出 Embedding Top-K vs +Reranker 召回/成本对照数据
> （实验矩阵 C 列）
> 更新日期：2026-09-13

## 1. 目标

按 Context Intelligence 设计 §5.3/§12 交付 Layer 2 重排：`IContextReranker` 公共
契约（检索 Top-K 30~100 收敛到 5~20）、重排信号与既有排名语义的融合口径
（不整替 `RetrievalWeights` 语义）、确定性参考重排器（无模型）与 reranker 缺席/
失败的降级语义（使用检索序 Top-K），以及 `ContextMemorySupervisor` 的 Executor
路由包装。Core 不引入任何模型或推理后端（DEC-032 第 5 条）；重排器是 Layer 1
之后的可选消费者，不改变 Layer 0 `StandardContextManager` 的准入权威。

同时按 DEC-032「验证方式」第 2 条产出 reranker uplift 对照数据：复用 M17 检索
评估 v1 冻结的数据集与 token-hash 供给方口径，对照 B 列（混合三腿检索序）与
C 列（+参考重排器），冻结 C1–C4 门禁（见 §4）。

## 2. 范围与非目标

范围：`include/mira/context_rerank.hpp`（`IContextReranker`/`RankedContextItem`/
`ContextRerankConfig`/`ContextRerankWeights` + 参考实现）、
`src/context/context_rerank.cpp`（入 `mira_core`）、
`ContextMemorySupervisor::schedule_context_rerank`（Interactive）、`tests/m18/`
契约/集成测试与对照评估 harness、基准报告与文档同步。

非目标：不实现 cross-encoder/ONNX/量化模型重排（模型重排器经供应链复核后接入，
证据另行登记）；不实现 `ISemanticConsolidator`/`IPromptCompressor`/
`ContextIntelligenceService`（Stage D–F）；不修改 `InMemoryContextIndex` 的
Layer 1 排序语义；不把重排接入 `AgentLoop::build_request`（DEC-002 另行评审）；
不修改 `StandardContextManager`；对照指标为管线行为（token-hash 供给方），不得
外推为真实语义 uplift、真实模型延迟或真机成本（`RULE-10`）。

## 3. 设计与决策依据

- [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)
  第 2/5/6/8/9 条（Layer 2 可选、模型不硬编码、行为指标验收、实验矩阵 C 列）
- [Context Intelligence 设计](../design/context_intelligence_design.md)
  §5.3/§7/§8/§11/§12/§13（重排契约草案、Executor 路由、Reranker 缺失/超时降级、
  评估指标含 reranker uplift、Stage C 门禁、实验矩阵）
- [检索评估 v1](../benchmarks/context-intelligence-retrieval-v1.md)（B 列基线：
  混合轮 Recall@10 = 1.0、MRR 0.986；数据集与供给方冻结口径）
- [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)
  （Episode/Lesson 声明面净化纪律；重排器只见净化文本）
- `RULE-02`（新路由经既有 Executor 监督者）、`RULE-07`/`RULE-08`（重排不产生新的
  存储权威；全部输出有界）、`RULE-10`（指标口径诚实）

## 4. 冻结的重排对照 profile 与门禁（跑前冻结，2026-09-13）

**对照设计**：复用 M17 数据集（48 锚定查询、`dataset_digest`
`ffc59f6d0a0a375a6e8730906219339900b616ed1f861b274c437515a367649e`）与
token-hash embedding 供给方。B 列 = `InMemoryContextIndex` 混合三腿检索序
（重跑取得，不抄录报告值）；C 列 = 每查询将检索 Top-30 候选交给
`TokenOverlapContextReranker`（确定性参考重排器，无模型），输出 Top-10。
两列同查询同数据集，逐对计算 uplift。

**参考重排器（评估与契约测试用，确定性）**：`TokenOverlapContextReranker`——
查询与候选文本经 Layer 1 同款 `[a-z0-9_]` 小写化切分，重排分为查询 token 覆盖
候选的 F1（精确 token 交叉，无哈希噪声）加 `exact_terms` 逐条 verbatim 命中
加成；融合分 = `rerank_weight × minmax(重排分) + retrieval_weight ×
minmax(检索分)`（集合内 min-max，全等时取 0.5），默认权重 0.60/0.40——重排
信号为主、Layer 1 既有信号保留（设计 §5.3「融合不整替」）。权重为文档化默认值，
非冻结契约。

**门禁 C1–C4（跑前冻结；理由随条给出）**：

| 门禁 | 断言 | 依据 |
| --- | --- | --- |
| C1 召回保持 | B 列与 C 列 Recall@10 均为 1.0（48/48 锚定资产仍在输出 Top-10） | 锚定资产与查询共享稀有 token，检索 Top-30 必含锚定资产，重排器对其打满 F1；掉出即实现缺陷 |
| C2 uplift 非负 | C 列 MRR ≥ B 列 MRR（同一运行内逐对计算，uplift ≥ 0.0） | 干扰资产与查询无共享 token，F1 重排应把锚定资产稳居首位并压低 hash 噪声干扰；负 uplift 即融合或实现缺陷 |
| C3 ACL 零泄漏 | C 列输出中跨 session/scope 候选数为 0 | 重排器只重排/截断输入集合，不扩大成员资格；出现跨域候选即安全缺陷，零容忍 |
| C4 确定性 | 同进程两次完整对照评估（建索引 + 全查询 + 全重排）输出报告逐字节相等 | 数据集、供给方、重排器与融合全确定性；uplift 声明要求可重复 |

**报告指标（只报告不判定）**：每查询重排分/F1 分布、检索 Top-30 内锚定资产
命中位次、融合前后排序变化数、逐查询评分操作数与耗时 tick、空候选集与降级
检索（无 embedding）路径的对照轮、tokens_estimate 分布。失败处置同 M17 §4：
实现缺陷登记 `BUG-YYYYMMDD-NNN`，阈值或口径错误先修订本文件与设计文档再复跑。

## 5. 工作项

- [x] `M18-01` 里程碑立项与本节 profile/门禁冻结（本文件；时间戳先于任何正式
  对照评估运行）。
- [x] `M18-02` Layer 2 契约：`IContextReranker`、`RankedContextItem`
  （候选 + 重排分 + 融合分 + 原位次）、`ContextRerankConfig`（`max_output`，
  默认 20，覆盖设计 5~20 收敛带）、`ContextRerankWeights`、确定性参考重排器
  `TokenOverlapContextReranker`。
- [x] `M18-03` 降级语义：reranker 失败/异常返回错误、调用方使用检索序 Top-K
  （设计 §8）；空候选集闭合；`max_output` 截断保持成员资格不变（只重排不扩充）。
- [x] `M18-04` Executor 路由：`ContextMemorySupervisor::schedule_context_rerank`
  （Interactive、future 必须消费）与 shutdown 后提交拒绝测试。
- [x] `M18-05` 契约/集成测试矩阵（§7）与对照评估 harness（C1–C4、JSON 报告、
  复用 M17 数据集口径）、本地全门禁。
- [x] `M18-06` 文档同步：总计划索引与注记、设计 §3 现状表/§12 Stage C 行、
  README 能力表、API 手册（`context-memory.md`）、基准报告
  （`context-intelligence-rerank-v1.md`）已完成（与实现同批变更）。
  CI 取证待 PR 全绿后回填关闭。

## 6. 风险与阻塞

- 风险：token-hash 供给方下 uplift 被误读为真实语义收益。处置：报告与设计文档
  显式限定指标口径（管线行为对照），语义 uplift 声明留待真实 embedder/reranker
  证据（DEC-032 第 5/9 条，供应链复核通道）。
- 风险：F1 重排在同域干扰与锚定资产共享 token 的极端用例下与 Layer 1 词法腿
  高度相关，对照区分度有限。处置：报告补充分布与位次变化统计，不以单点 uplift
  下结论；真实区分度归真实模型证据。
- 风险：min-max 归一化在退化集合（全等分）下的行为不确定。处置：契约冻结
  「全等取 0.5」并在契约测试覆盖。
- 外部：无（本里程碑不依赖真机、凭据或外部语料）。

## 7. 测试与退出条件

- [ ] 契约/集成测试进 ctest（label `integration;m18`）：参考重排器确定性排序
  （同输入同结果）；F1 与 exact_terms 加成；min-max 融合与全等退化（0.5）；
  `max_output` 截断只减不增（成员资格 ⊆ 输入集合）；空候选集闭合；reranker
  错误返回（调用方降级为检索序 Top-K）；ACL 跨域候选不因重排出现；禁止标记
  文本不因重排上浮（重排器只重排不生成文本）；`schedule_context_rerank` 路由
  与 `begin_shutdown` 后拒绝。
- [ ] 对照评估 harness 进 ctest（label `integration;m18`，跑前冻结 C1–C4，
  失败非零退出）；首轮报告落
  `../benchmarks/context-intelligence-rerank-v1.md`（环境、命令、B/C 两列
  摘要、uplift、限制）。
- [ ] 本地门禁：debug 全量 ctest、ASAN/UBSAN/TSAN m18 目标零报告、
  `format-check`/`docs-check`/`platform-boundary-check`/`sbom-check` 通过。
- [ ] 文档同步完成（§5 `M18-06` 清单；与实现同一变更提交）。
- [ ] PR CI（Linux/Windows/Android 编译级/sanitizers/quality）全绿后回填验证
  记录并关闭本里程碑。

## 8. 验证记录

2026-09-13：`M18-01`～`M18-05` 本地实现与首轮对照评估（分支待提交）。

- **交付**：`include/mira/context_rerank.hpp` + `src/context/context_rerank.cpp`
  （入 `mira_core`）——Layer 2 全部契约与 `TokenOverlapContextReranker`
  参考重排器；`ContextMemorySupervisor::schedule_context_rerank`（Interactive）。
  测试 `tests/m18/m18_context_rerank_test.cpp`（7 组：确定性排序/F1 与 exact
  加成/min-max 融合与全等退化/max_output 只减不增与配置校验/空输入与错误
  降级路径/检索→重排端到端不扩员不重写与 ACL 透传/supervisor 路由与
  shutdown 拒绝）与 `tests/m18/m18_rerank_eval.cpp`（复用 M17 冻结数据集，
  `dataset_digest` 断言锚定；冻结 C1–C4、JSON 报告）。
- **首轮对照评估**（正式运行一次，登记
  [context-intelligence-rerank-v1](../benchmarks/context-intelligence-rerank-v1.md)）：
  C1–C4 全绿——B/C 两列 Recall@10 = 1.0（48/48）；混合轮 MRR uplift +0.0139
  （B 0.9861 → C 1.0000，F1 重排压回 hash 供给方噪声干扰）；两轮 ACL 违例 0；
  跨进程双跑报告字节级一致（耗时 report-only，不入报告以保持确定性）。
- **本地门禁**（Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、
  CMake 3.28.3）：debug 全量 ctest **74/74**（原 72 + 本里程碑 2 目标）；
  ASAN/UBSAN/TSAN（`setarch -R`）m18 两目标通过零报告；
  `format-check`、`docs-check`、`platform-boundary-check`、`sbom-check` 通过；
  miniconda clang-tidy 18.1.8 预检新增文件（一处
  `performance-move-const-arg` 已修复；`std::exit` 的 `concurrency-mt-unsafe`
  与 M17 已合入评估 harness 同型，CI 同型通过先例）。
- **限制与未执行项**：确定性供给方下 uplift 为管线行为方向非语义声明
  （`RULE-10`）；真实 reranker 模型、AgentLoop 集成、Stage D 固化为显式非目标；
  Windows/Android/Release/quality 由 PR CI 回填后本里程碑方可关闭（`M18-06`）。
