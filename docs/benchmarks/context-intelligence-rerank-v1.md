# Context Intelligence 重排对照评估 v1（Stage C 首轮）

> 状态：Active（首轮对照，登记于 2026-09-13）
> 负责人：Mira Maintainers
> 适用范围：[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)
> Stage C（[M18](../plans/m18-context-intelligence-stage-c.md)）；Layer 2
> `TokenOverlapContextReranker`（确定性参考重排器，无模型）对
> [检索评估 v1](context-intelligence-retrieval-v1.md) 冻结数据集的
> B 列（检索序 Top-10）vs C 列（重排 Top-30 → Top-10）管线行为对照
> 复现命令见文末

## 1. 目的与范围

这是 [M18](../plans/m18-context-intelligence-stage-c.md) 产出的 reranker uplift
对照首轮：复用 M17 检索评估 v1 的冻结数据集（48 锚定查询，`dataset_digest`
`ffc59f6d0a0a375a6e8730906219339900b616ed1f861b274c437515a367649e`，评估内断言
digest 相等后运行）与 token-hash embedding 供给方。B 列为
`InMemoryContextIndex` 混合三腿检索序 Top-10；C 列将检索 Top-30 候选交给
`TokenOverlapContextReranker`（查询 token 覆盖 F1 + `exact_terms` 加成，
min-max 融合 0.60/0.40）收敛到 Top-10。与 M17 相同，所有指标度量**管线行为**
（召回保持、uplift 方向、ACL 透传、确定性），不是语义重排质量声明——语义
uplift 需真实 embedder/reranker 证据（DEC-032 §5/§9）。

## 2. 方法与环境

- Harness：`tests/m18/m18_rerank_eval.cpp`（M18-05），数据集构造镜像 M17 冻结
  口径并以 `dataset_digest` 断言锚定；门禁 C1–C4（M18 §4，跑前冻结）。
- 对照口径：B 列 Recall@10/MRR 在检索序前 10 位内计算；C 列在重排输出
  （输入 Top-30、输出 Top-10）内计算；两列逐对同查询，uplift = MRR(C) − MRR(B)。
- 环境：Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H（14 线程）、GCC 13.3.0、
  CMake 3.28.3、CMake `debug` 预设；单轮对照 < 1 s。

## 3. 首轮结果（摘要）

| 轮次 | 列 | Recall@10 | MRR | ACL 违例 | 评分操作 | tokens 合计 |
| --- | --- | --- | --- | --- | --- | --- |
| 混合（全量 embedding） | B 检索序 | 1.000（48/48） | 0.986 | 0 | — | 30,164 |
| 混合（全量 embedding） | C +重排 | 1.000（48/48） | 1.000 | 0 | 1,440 | 30,164 |
| 降级（无 embedding） | B 检索序 | 1.000（48/48） | 1.000 | 0 | — | 7,362 |
| 降级（无 embedding） | C +重排 | 1.000（48/48） | 1.000 | 0 | 354 | 7,362 |

- C1–C4 全部通过；跨进程双跑 JSON 报告字节级一致（`cmp` 验证；耗时为
  report-only，不入报告以保持确定性）。
- 混合轮 uplift = **+0.0139**（0.9861 → 1.0000）：M17 观察一的 hash 供给方噪声
  干扰（少量 token 重叠进入候选前沿）被 F1 重排压回，锚定资产全部稳居首位。
  这复现了重排器在管线中承担的排序收敛职责，方向与设计预期一致。
- 降级轮 uplift = 0：词法单腿本就满分，重排不损失、无增益——组件缺席降级
  与重排叠加路径召回不损失。
- tokens 合计在 B/C 两列相等：重排只重排/截断候选，不改变 token 口径
  （Layer 0 准入前的打包仍由检索层完成）。

## 4. 发现与限制

- 对照量化了「重排器在 Layer 1 之上提供 uplift 方向正确的排序收敛、且不破坏
  召回与 ACL」；uplift 幅度受数据集限制——干扰资产为随机稀有 token，F1 与
  词法腿高度相关，真实语义区分度需真实模型证据。
- 限制：embedding 与重排供给方均为确定性函数而非模型——不得外推语义质量、
  真实 cross-encoder 延迟或真机成本（`RULE-10`）；重排耗时本轮 report-only
  （参考实现 ~0.3 ms/查询量级，仅表明进程内开销可忽略），不作为性能声明。
- Stage D 使用口径：固化（`ISemanticConsolidator`）对照沿用本实验矩阵方法学；
  真实 reranker（multilingual MiniLM cross-encoder / BGE 家族小型量化，设计 §14
  候选清单）接入后须以本数据集 + miracle 派生查询重测并另行登记。

## 5. 补跑与后续条件

- 本地复现：`cmake --preset debug && cmake --build --preset debug --target
  mira_m18_rerank_eval && ctest -R mira_m18_rerank_eval`（门禁失败非零退出；
  报告 JSON 经 argv[1] 落盘，跨进程 `cmp` 一致）。
- CI：`mira_m18_rerank_eval` 入 Linux 测试与 sanitizer 矩阵（Android 为编译级
  门禁既有范围）；Windows/Release/quality 由 PR CI 回填（M18-06）。
- 真实 embedder/reranker 供给方接入后的重测：供应链复核（DEC-032 §14）+
  本数据集 + miracle 派生查询三方对照后另行登记（归 `MNT-202609-27` 语料通道）。
