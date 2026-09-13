# Context Intelligence 固化管线评估 v1（Stage D 首轮）

> 状态：Active（首轮评估，登记于 2026-09-14）
> 负责人：Mira Maintainers
> 适用范围：[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)
> Stage D（[M19](../plans/m19-context-intelligence-stage-d.md)）；Layer 3
> `ProviderSemanticConsolidator`（经 `IModelProvider` 的脚本化确定性供给方）对冻结
> 合成会话数据集的管线行为评估；门禁 D1–D5（M19 §4 跑前冻结）
> 复现命令见文末

## 1. 目的与范围

这是 [M19](../plans/m19-context-intelligence-stage-d.md) 产出的 Layer 3 语义固化
首轮评估：冻结合成会话数据集（12 会话 × 40 条 `ConversationEntry`，每会话植入
6 约束 / 4 决策 / 4 未决线索 / 3 偏好候选 / 2 敏感标记条目；`dataset_digest`
`31758a94b646e96f4a2ea133c1dec9d4bc0d62541d09b20d8ed941ebd4fd9866`，评估内断言
digest 相等后运行），经 `segment_conversation` 切分为单一前缀段，交给
`ProviderSemanticConsolidator` 走完整 ModelRequest → infer → 严格 JSON 解析 →
provenance 绑定 → 五元组提交管线。固化"模型"为脚本化确定性 `IModelProvider`
（按冻结规则从编号转录中抽取标记条目并引用条目号）。与 M17/M18 相同，全部指标
度量**管线行为**（provenance 绑定、标记/ACL 过滤、提交纪律、确定性），不是语义
固化质量——真实 Constraint Recall/Precision、矛盾率、幻觉率需真实小模型证据
（DEC-032 §5/§9，供应链复核通道）。

## 2. 方法与环境

- Harness：`tests/m19/m19_consolidation_eval.cpp`（M19-06），数据集 digest
  断言锚定；门禁 D1–D5（M19 §4，跑前冻结）。
- 轮次：混合轮（供给方正常）、降级轮（供给方在首个会话提交一次后持续失败）、
  重复轮（确定性对照）；混合轮附加陈旧水位与终态迟到候选的提交纪律探测。
- 环境：Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、CMake
  3.28.3、CMake `debug` 预设；单轮评估 < 1 s。

## 3. 首轮结果（摘要）

| 轮次 | 提交数 | 约束召回 | 决策召回 | 线索召回 | precision | provenance 准确 | 标记违例 | 跨会话违例 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 混合 | 12/12 | 1.000（72/72） | 1.000（48/48） | 1.000（48/48） | 1.000（204/204） | 1.000 | 0 | 0 |
| 降级 | 0（首会话种子提交后 12 轮全失败） | — | — | — | — | — | 0 | 0 |

- D1–D5 全部通过；跨进程双跑 JSON 报告字节级一致（`cmp` 验证）。
- 混合轮：36 个偏好候选全部留在 checkpoint 偏好区（未自动转换）；压缩比
  0.324（checkpoint 语句字节 4,608 / 前缀段 presented 字节 14,232；字节口径，
  仅报告不判定）。
- 提交纪律：12 个陈旧水位候选全部 `DiscardedStale`、12 个终态迟到候选全部
  `DiscardedTerminal`、0 个越权提交；降级轮 12 轮固化全部失败，0 提交且
  首会话种子 checkpoint 原样保留（`seeded_retained`）。
- 语句级审计：204/204 已提交语句 content 与 provenance 均与植入真值一致；
  敏感标记条目（`api_key=`/`password=`）零出现在 checkpoint。

## 4. 发现与限制

- 管线在「脚本化供给方 → 严格 JSON → provenance fail-closed → 五元组提交」
  全链路上对真值无损、对越界引用与敏感内容零容忍放行、对失败与迟到结果不破坏
  既有状态——Stage D 声明的管线行为边界全部得到量化。
- 限制：固化供给方为确定性抽取脚本而非真实模型——recall/precision 为管线行为
  上限，不得外推语义固化质量、真实小模型延迟或真机成本（`RULE-10`）。
- Stage E/F 使用口径：真实小模型（设计 §14 候选清单，经 `IModelProvider`
  配置）接入后须以本 harness + miracle 派生会话重测语义指标并另行登记；
  `ContextIntelligenceService` 编排与 AgentLoop 集成归后续里程碑。

## 5. 补跑与后续条件

- 本地复现：`cmake --preset debug && cmake --build --preset debug --target
  mira_m19_consolidation_eval && ctest -R mira_m19_consolidation_eval`（门禁
  失败非零退出；报告 JSON 经 argv[1] 落盘，跨进程 `cmp` 一致）。
- CI：`mira_m19_consolidation_eval` 入 Linux 测试与 sanitizer 矩阵（Android 为
  编译级门禁既有范围）；Windows/Release/quality 由 PR CI 回填（M19-07）。
- 真实固化模型接入后的重测：供应链复核（DEC-032 §14）+ 本 harness + miracle
  派生会话对照后另行登记（归 `MNT-202609-27` 语料通道）。
