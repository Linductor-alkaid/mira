# Context Intelligence 检索评估 v1（Stage B 首轮）

> 状态：Active（首轮基线，登记于 2026-09-13）
> 负责人：Mira Maintainers
> 适用范围：[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)
> Stage B（[M17](../plans/m17-context-intelligence-stage-b.md)）；Layer 1 检索召回
> （`InMemoryContextIndex` 三腿混合）的管线行为基线，是 Stage C reranker 对照的
> 实验矩阵 B 列参考点
> 复现命令见文末

## 1. 目的与范围

这是 [M17](../plans/m17-context-intelligence-stage-b.md) 产出的检索评估首轮基线：
48 个锚定查询 ×（1 相关资产 + 8 同域干扰 + 4 跨域干扰）的确定性合成数据集驱动
`InMemoryContextIndex`（exact + 词法 + 有界 cosine 向量三腿），embedding 供给方为
确定性 token-hash 函数（非模型）。本报告只声明**管线行为**（召回路径、组件缺席
降级、ACL 零泄漏、确定性）的结构性结论；指标不是语义质量声明——语义召回
（Recall@K/MRR 在真实语义相关性下的取值）需要真实 embedder 证据（DEC-032 §5/§9，
供应链复核后接入或 Stage E 设备评估），本基线是其后对照的锚点。

## 2. 方法与环境

- Harness：`tests/m17/m17_retrieval_eval.cpp`（M17-07），splitmix64 固定种子
  `0x4d49533137535442`；查询/资产文本、ID 与注册顺序全部确定性派生。
- embedding 供给方：token-hash——`[a-z0-9_]` 小写化 token 经 FNV-1a 散列入
  64 维、按 hash 奇偶取 ±1、L2 归一化；查询与资产共用同一函数与 profile id。
  cosine 相似度等价于有符号 token 重叠度。
- 数据集：3 类资产各 16 个锚定（Conversation 段 / Episode / Lesson，声明面遵守
  DEC-029 净化纪律——仅标识符与 reason code）；同域干扰 384 个（离散稀有 token）；
  跨域干扰（陌生 session / 陌生 scope + 与查询共享 token 的文本）用于 ACL 零泄漏
  反向验证。注册顺序按资产 id 文本排序（避免锚定资产聚集影响有界扫描）。
- 冻结门禁（[M17 §4](../plans/m17-context-intelligence-stage-b.md)，跑前冻结）：
  R1 Recall@10 == 1.0；R2 MRR ≥ 0.90；R3 ACL 零泄漏（混合与降级路径）；
  R4 确定性（同进程双轮全指标一致）。
- `dataset_digest`：
  `ffc59f6d0a0a375a6e8730906219339900b616ed1f861b274c437515a367649e`
  （排序去重后资产文本的 SHA-256；本轮基线锚点）。
- 环境：Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H（14 线程）、GCC 13.3.0、
  CMake 3.28.3、CMake `debug` 预设；单轮 < 1 s。

## 3. 基线结果（摘要）

| 轮次 | Recall@10 | MRR | ACL 违例 | 词法腿 | 向量腿 | tokens 合计 |
| --- | --- | --- | --- | --- | --- | --- |
| 混合三腿（全量 embedding） | 1.000（48/48） | 0.986 | 0 | 48/48 | 48/48 | 10,804 |
| 降级（无 embedding 供给） | 1.000（48/48） | 1.000 | 0 | 48/48 | 0/48 | 4,482 |

- R1–R4 全部通过；跨进程双跑报告字节级一致（`cmp` 验证）。
- 观察一（降级路径反超）：词法单腿 MRR（1.000）略高于混合轮（0.986）——token-hash
  向量腿把与查询有少量 token 重叠的干扰资产带入候选前沿。这是 hash 供给方的噪声
  特性，不是排序缺陷；真实语义 embedder 下该关系需重测（Stage C 对照的直接输入）。
- 观察二（exact 腿零命中）：评估查询未携带 exact_terms（按设计），exact 腿 0/48
  属预期；其强制过滤语义由契约测试覆盖（`mira_m17_context_retrieval_test`）。
- 观察三（token packing）：混合轮 tokens 合计高于降级轮——向量腿把更多干扰召回
  Top-10 内；两者均在每查询 4,096 预算内。

## 4. 发现与限制

- 基线量化了「组件缺席降级不损失召回路径」：无 embedding 时请求照常闭合且召回
  不变（DEC-032 第 7 条 Layer 0 自足性在检索层的对应行为）。
- 限制：embedding 供给方是确定性函数而非模型——所有指标度量管线行为，不得外推为
  语义召回、真实延迟或真机成本；干扰资产为随机稀有 token，无真实语法的近义干扰；
  数据集规模 48 查询/528 资产，面向回归门禁而非统计功效。
- Stage C 使用口径：reranker 对照实验复用本数据集与供给方冻结口径，B 列基线即本
  报告混合轮；真实语义 uplift 声明须等真实 embedder/reranker 证据。

## 5. 补跑与后续条件

- 本地复现：`cmake --preset debug && cmake --build --preset debug --target
  mira_m17_retrieval_eval && ctest -R mira_m17_retrieval_eval`（门禁失败非零退出）。
- CI：`mira_m17_retrieval_eval` 入 Linux 测试与 sanitizer 矩阵（Android 为编译级
  门禁既有范围）；Windows/Release/quality 由 PR CI 回填（M17-08）。
- 真实语义 embedder 供给方接入后的重测：供应链复核（DEC-032 §14 候选清单）+
  本数据集 + miracle 派生查询（归 `MNT-202609-27` 语料）三方对照后另行登记。
