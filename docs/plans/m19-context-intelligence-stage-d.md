# M19：Context Intelligence Stage D——Layer 3 语义固化（ConversationCheckpoint）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 第 8 条 Stage D）
> 前置：M16（Stage A 基线）；M17（Layer 1 检索召回）；M18（Layer 2 重排对照）；
> [Context Intelligence 设计](../design/context_intelligence_design.md)
> §5.4/§6/§7/§8 已冻结
> 建议发布点：非发布物；产出固化管线行为基线（实验矩阵 D 列方法学锚点）
> 更新日期：2026-09-14

## 1. 目标

按 Context Intelligence 设计 §5.4/§6/§7/§12 交付 Layer 3 语义固化：

- `ISemanticConsolidator` 公共契约：把会话前缀段固化为结构化
  `ConversationCheckpoint`（约束、决策、未决线索、偏好候选）；固化模型经
  `IModelProvider` 配置（主模型/廉价云模型/本地小模型皆可），不绑定 Agent 主模型，
  Core 不引入推理后端（DEC-032 第 5 条）。
- `ConversationCheckpoint` 会话域投影：`source_events` provenance 与
  `through_event_sequence` 水位（派生投影纪律，DEC-032 第 3 条）；与 TaskCheckpoint
  同受 RULE-07 约束——可从源事件重新固化、终态幂等、迟到固化不得覆盖新状态。
- 异步固化提交校验五元组（`session_id / task_id / task_epoch / environment_epoch /
  through_event_sequence`，设计 §6.2）：任一不匹配丢弃候选并保留旧 checkpoint；
  会话/任务终态后到达的固化结果一律丢弃。
- 冲突优先级（近期显式用户指令 > 已验证任务状态 > ConversationCheckpoint > 检索
  记忆）的落地方式：checkpoint 语句以普通 `ContextItem` 候选进入 Layer 0（约束 →
  P1 `UserConstraint`，摘要/决策/线索 → P3 `CheckpointSummary`），携带源事件序列
  保持真实时序；不修改 `StandardContextManager`，语义层无准入权（DEC-032 第 2 条）。
- `ContextMemorySupervisor::schedule_context_consolidation`（Deferrable）Executor
  路由与 shutdown 语义。
- 按 DEC-032「验证方式」第 2 条产出固化管线行为基线：冻结数据集 + 脚本化确定性
  供给方，门禁 D1–D5（见 §4，跑前冻结）。

## 2. 范围与非目标

范围：`include/mira/context_consolidation.hpp`（语句/选项/检查点模型、
`ISemanticConsolidator`、`ProviderSemanticConsolidator` 参考固化器、
checkpoint store 与提交校验、Layer 0 候选转换、JSON 序列化）、
`src/context/context_consolidation.cpp`（入 `mira_core`）、
`ConversationSegment` 增补逐条目 `entries`（加法式契约变更，固化 provenance
绑定所需）、`ContextMemorySupervisor::schedule_context_consolidation`（Deferrable）、
`tests/m19/` 契约/集成测试与固化评估 harness、基准报告与文档同步。

非目标：不接入真实小模型（Qwen ~0.6B 级或廉价云模型经供应链复核后作为宿主注入的
`IModelProvider` profile 接入，证据另行登记）；不实现
`ContextIntelligenceService` 编排方与 `IPromptCompressor`（Stage E–F/后续）；
不修改 `StandardContextManager` 与 Layer 0 任何既有语义；不接入
`AgentLoop::build_request`（DEC-002 另行评审）；不实现 Preference 候选晋升
Memory 的人工审批管线（沿用 `MemoryConsolidator` 既有纪律，转换不在本里程碑）；
不引入向量持久化或 ANN；无真机/真模型证据（Stage E，`MNT-202609-27` 通道）；
评估指标为管线行为（脚本化供给方），不得外推为语义固化质量、真实模型延迟或
真机成本（`RULE-10`）。

## 3. 设计与决策依据

- [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)
  第 2/3/4/5/8/9 条（Layer 3 经 IModelProvider、派生投影纪律、五元组与滞回、
  模型不硬编码、行为指标验收）
- [Context Intelligence 设计](../design/context_intelligence_design.md)
  §5.4/§6/§7/§8/§9/§10/§11/§12/§13（固化契约草案、触发与提交校验、Executor
  路由、故障降级、安全隐私、可观测性、评估指标、Stage D 门禁、实验矩阵 D 列）
- [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)
  （Conversation 投影——固化输入的事实源边界）、
  [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)
  （Preference 人工审批纪律的沿用来源）
- [M17](m17-context-intelligence-stage-b.md)/[M18](m18-context-intelligence-stage-c.md)
  （`ConversationSegment`/`segment_conversation` 既有口径；supervisor 路由模式；
  评估 harness 方法学）
- `RULE-02`（新路由经既有 Executor 监督者，Deferrable 类）、`RULE-07`
  （checkpoint 是可重建投影，EventStore 仍是事实源）、`RULE-08`（语句计数、
  文本字节、source_events 全部有界）、`RULE-09`（模型输出不可信：标记过滤、
  不得提升为 policy、provenance fail-closed）、`RULE-10`（指标口径诚实）

## 4. 冻结的固化评估 profile 与门禁（跑前冻结，2026-09-14）

**对照设计**：确定性合成会话数据集（SplitMix64，seed `0x4d49523139534443`
（"MIR19SDC"）；12 个固化会话 × 每会话 40 条 `ConversationEntry`，每会话植入
6 约束 / 4 决策 / 4 未决线索 / 3 偏好候选 / 2 敏感标记条目，其余为噪声，植入
位置逐会话确定性打乱），经 `segment_conversation` 以覆盖全会话条目数的窗口
切分为单一前缀段——对齐设计 §6.1 滞回提交模型：固化输入为截至水位的
会话前缀，提交整体替换旧 checkpoint。ground truth = 植入条目及其 `EventId`；
跨会话违例按「已提交语句引用事件均属于本会话段」逐语句审计。
数据集以 `dataset_digest` 断言锚定（口径同 M17/M18）：
`a828a2aedb0a2550961a4deb00909554334b2a6aad00d224af73fe93310a984f`（首轮钉定，
复跑修复 RNG 顺序歧义后更新，见 §8 首条）。

**固化供给方（评估与契约测试用，确定性）**：脚本化 `IModelProvider`——按冻结
规则从编号转录中识别 `constraint:`/`decision:`/`thread:`/`preference:` 条目
标记并产出 JSON（引用条目号），模拟经 `IModelProvider` 配置的小模型；走完整
ModelRequest → infer → 严格 JSON 解析 → 校验 → provenance 映射 → checkpoint
管线。敏感标记条目不产出语句（真实模型纪律由 D3 输出侧过滤兜底）。

**门禁 D1–D5（跑前冻结；理由随条给出）**：

| 门禁 | 断言 | 依据 |
| --- | --- | --- |
| D1 管线召回与精确 | 混合轮提交 checkpoint 中约束/决策/未决三类语句对植入真值的 recall = 1.0 且 precision = 1.0（语句 content 与 provenance 均与真值一致） | 脚本供给方按标记抽取并引用条目号，管线须无损映射到 checkpoint；丢语句或引入伪语句即解析/验证/映射缺陷 |
| D2 provenance 准确 | 已提交语句 `source_events` 非空且 ⊆ 输入段事件；引用越界的模型语句被丢弃（不产生 fabricated provenance）；checkpoint `through_event_sequence` == 段水位；provenance accuracy = 1.0 | 派生投影纪律（DEC-032 第 3 条）：provenance 必须可回溯源事件；越界引用是幻觉引用，必须 fail-closed |
| D3 安全零违例 | 全部已提交 checkpoint 中含 forbidden/injection 标记的语句数为 0；跨会话内容为 0（语句事件均属于本会话段） | RULE-09 与设计 §9：敏感内容与跨域内容不得固化；标记过滤与 ACL 双保险 |
| D4 提交纪律 | 降级轮（provider 失败）0 提交且既有 checkpoint 原样保留；陈旧五元组（旧水位/epoch 不匹配/任务不匹配）与终态迟到候选 100% 丢弃；同会话已提交水位严格单调 | 设计 §6.2/§8：固化失败与迟到结果不得破坏既有状态；五元组校验兜底 |
| D5 确定性 | 同进程重复轮聚合与首轮逐项相等；跨进程 JSON 报告字节级一致（`cmp` 验证） | 数据集、供给方、解析、校验全确定性；声明可重复 |

**报告指标（只报告不判定）**：压缩比（checkpoint 语句文本估算 token / 前缀段
presented token）、各类语句计数、偏好候选计数、植入项分布、噪声条目数。
失败处置同 M17/M18 §4：实现缺陷登记 `BUG-YYYYMMDD-NNN`，阈值或口径错误先修订
本文件与设计文档再复跑。

## 5. 工作项

- [x] `M19-01` 里程碑立项与本节 profile/门禁冻结（本文件；时间戳先于任何正式
  固化评估运行）。
- [x] `M19-02` Layer 3 契约：`ConversationStatement`（约束/决策/未决线索/偏好
  别名，content + provenance + confidence + source_sequence）、
  `ConsolidationIdentity`/`ConsolidationOptions`（有界与标记配置）、
  `ConversationCheckpoint`（五元组、`validate()`、权威字段 `projection_digest()`
  排除叙事摘要、JSON `mira.context.checkpoint.v1`）、`ISemanticConsolidator`；
  `ConversationSegment` 增补 `entries`（加法式，`segment_conversation()` 回填）。
- [x] `M19-03` 参考固化器 `ProviderSemanticConsolidator`：ModelRequest 组装
  （StrictJsonSchema 固化 schema、编号转录引用规则、预算与 data policy）、
  响应严格解析与 fail-closed（InvalidModelOutput）、provenance 越界丢弃、
  标记过滤、边界裁剪（超限语句丢弃、摘要截断）、deadline/cancellation 透传。
- [x] `M19-04` 提交管线：`IConversationCheckpointStore`/
  `InMemoryConversationCheckpointStore`（有界保留、水位单调）、
  `commit_conversation_checkpoint`（五元组校验、终态幂等、幂等 NoOp、
  同水位冲突 fail-closed）、`context_items_from_checkpoint`（P1/P3 映射、
  RULE-09 authority 标注、偏好不自动转换）。
- [x] `M19-05` Executor 路由：`ContextMemorySupervisor::schedule_context_consolidation`
  （Deferrable、取消探针透传、shutdown 拒绝与在途取消 resolve Cancelled）。
- [x] `M19-06` 契约/集成测试矩阵（§7）与固化评估 harness（D1–D5、JSON 报告、
  dataset digest 锚定）、本地全门禁。
- [x] `M19-07` 文档同步（总计划索引与注记、设计 §3 现状表/§5.4 实现注记/
  §12 Stage D 行、README 能力表、API 手册、基准报告）与实现同批变更；
  CI 取证待 PR 全绿后回填关闭。

## 6. 风险与阻塞

- 风险：脚本化供给方与真值标记高度相关，recall/precision 为管线行为上限而非
  语义固化质量。处置：报告与设计文档显式限定口径（管线行为，`RULE-10`）；
  语义指标（真实 Constraint Recall/Precision、矛盾率、幻觉率）归真实小模型
  证据（供应链复核通道，DEC-032 第 5/9 条）。
- 风险：整体替换语义下，若固化轮输出退化（空结果）会覆盖既有富 checkpoint。
  处置：D1 召回门禁约束供给方质量；整体替换是设计 §6.1 冻结语义（「校验通过后
  替换 Warm 中旧 checkpoint」），不引入未冻结的合并语义；降级路径（固化失败/
  迟到/陈旧）由 D4 门禁与设计 §8 兜底。
- 风险：模型输出 JSON 解析面扩大 `InvalidModelOutput` 处理路径。处置：严格
  schema + fail-closed（整体拒绝或逐语句丢弃，无部分采纳），契约测试覆盖
  畸形/越界/标记/超限输入。
- 外部：无（本里程碑不依赖真机、凭据或外部语料；真实小模型接入为后续里程碑）。

## 7. 测试与退出条件

- [x] 契约/集成测试进 ctest（label `integration;m19`）：正常固化完成（语句、
  provenance、水位、digest、JSON 往返）；组件缺席/失败降级（consolidator 错误
  → 无 checkpoint 更新、旧状态保留）；畸形模型输出 fail-closed（非 JSON、
  schema 违例、越界引用丢弃、无 fabricated provenance）；forbidden/injection
  标记语句不进入 checkpoint；边界裁剪（语句计数、文本字节、source_events
  上限、超限丢弃、摘要截断）；五元组提交竞态（旧水位、task/epoch 不匹配、
  幂等 NoOp、同水位冲突 fail-closed）；终态后迟到固化丢弃（会话/任务）；store
  水位单调与有界保留；`context_items_from_checkpoint` 映射与 RULE-09 authority、
  偏好不转换；`schedule_context_consolidation` Deferrable 路由、shutdown 拒绝
  与在途取消。
- [x] 固化评估 harness 进 ctest（label `integration;m19`，跑前冻结 D1–D5，
  失败非零退出）；首轮报告落
  `../benchmarks/context-intelligence-consolidation-v1.md`（环境、命令、混合/
  降级轮摘要、门禁结果、限制）。
- [x] 本地门禁：debug 全量 ctest（74 → 76 目标）全绿、ASAN/UBSAN/TSAN m19
  目标零报告、`format-check`/`docs-check`/`platform-boundary-check`/
  `sbom-check` 通过。
- [x] 文档同步完成（§5 `M19-07` 清单；与实现同一变更提交）。
- [ ] PR CI（Linux/Windows/Android 编译级/sanitizers/quality）全绿后回填验证
  记录并关闭本里程碑。

## 8. 验证记录

2026-09-14：`M19-01`～`M19-07` 本地实现与首轮固化评估（分支
`feat/m19-context-intelligence-stage-d`）。

- **交付**：`include/mira/context_consolidation.hpp` +
  `src/context/context_consolidation.cpp`（入 `mira_core`）——Layer 3 全部契约
  与 `ProviderSemanticConsolidator` 参考固化器；`ContextMemorySupervisor::
  schedule_context_consolidation`（Deferrable、取消探针透传）。
  `include/mira/context_retrieval.hpp`：`ConversationSegment` 增补 `entries`
  （加法式；`segment_conversation()` 回填，含截断单例窗口）。
  测试 `tests/m19/m19_context_consolidation_test.cpp`（10 组：正常固化与
  provenance 绑定与 JSON 往返/provider 失败保留旧状态/畸形输出 fail-closed 与
  越界引用丢弃/标记与置信度过滤/边界裁剪/五元组提交竞态/终态迟到丢弃/store
  水位与保留/Layer 0 映射与 RULE-09 authority/supervisor Deferrable 路由与
  shutdown 拒绝与在途取消）与 `tests/m19/m19_consolidation_eval.cpp`
  （12 会话 × 40 条冻结数据集，`dataset_digest` 断言锚定；冻结 D1–D5、JSON
  报告）。
- **首轮固化评估**（正式运行一次，登记
  [context-intelligence-consolidation-v1](../benchmarks/context-intelligence-consolidation-v1.md)）：
  D1–D5 全绿——混合轮 12/12 提交，约束/决策/线索召回与语句 precision 均 1.0
  （204/204 语句 content+provenance 与真值一致）、provenance 零违例、标记与
  跨会话零泄漏、36 偏好候选全部留在偏好区；降级轮 12 轮全失败 0 提交且种子
  checkpoint 保留；陈旧水位 12/12 与终态迟到 12/12 候选全部丢弃、0 越权提交；
  跨进程双跑报告字节级一致（`cmp` 验证）。压缩比 0.324（字节口径，report-only）。
- **本地门禁**（Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、
  CMake 3.28.3）：debug 全量 ctest **76/76**（原 74 + 本里程碑 2 目标）；
  ASAN/UBSAN/TSAN（`setarch -R`）m19 两目标通过零报告（一处测试桩
  `calls_` 计数 TSAN 数据竞争已改 `std::atomic` 修复）；
  `format-check`、`docs-check`、`platform-boundary-check`、`sbom-check` 通过；
  miniconda clang-tidy 18.1.8 预检（CI quality 仅挂库目标，与 M17/M18 同口径）：
  库源一处 `performance-move-const-arg` 已修复、复验零违例；评估 harness 内
  `std::exit` 的 `concurrency-mt-unsafe` 与 M17/M18 同型（CI 同型通过先例），
  测试目标不在 tidy 门禁范围。
- **限制与未执行项**：脚本化供给方下 recall/precision 为管线行为上限非语义
  质量声明（`RULE-10`）；真实小模型、AgentLoop 集成、`ContextIntelligenceService`
  编排、Stage E 真机评估为显式非目标；Windows/Android/Release/quality 由 PR CI
  回填后本里程碑方可关闭（`M19-07`）。

2026-09-14：PR CI（[#47](https://github.com/Linductor-alkaid/mira/pull/47)）两轮
修复后复验。

- 第一轮：Linux clang 报 `-Wunused-lambda-capture`（`validate_statements`
  的未使用 `this` 捕获，GCC 不告警）——移除捕获；Android NDK libc++ 与 MSVC
  报 `chrono` 时钟 duration 隐式转换拒绝——`timestamp_from_nanos` 改为对
  `system_clock::duration`/`steady_clock::duration` 显式 `duration_cast`
  （本机同版本 NDK 两 ABI 交叉编译预演通过后推送）。
- 第二轮：clang 的 `mira_m19_consolidation_eval` 报 dataset digest mismatch
  （GCC 全绿）——定位为噪声条目两次 `rare_token` 抽取在同一表达式内、求值
  顺序未指定导致数据集跨编译器分叉；改为顺序语句，digest 重新钉定
  `31758a94…` → `a828a2aedb0a2550961a4deb00909554334b2a6aad00d224af73fe93310a984f`
  （实现代码零变更；植入结构、供给方规则、门禁与全部聚合指标与首轮逐项
  一致，[基准报告](../benchmarks/context-intelligence-consolidation-v1.md)
  已同步口径注记）。修复轮本机复验：debug ctest 76/76、TSAN m19 零报告。
