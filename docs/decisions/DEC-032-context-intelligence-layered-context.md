# DEC-032：Context Intelligence 分层上下文管理（长会话语义压缩方向）

> 状态：Accepted（方向冻结；实现未开始，进入里程碑的门槛见第 8 条）
> 日期：2026-09-12
> 负责人：Mira Maintainers
> 冻结里程碑：新里程碑（暂定，不预分配编号；Stage A 基线依赖 `MNT-202609-28` 评估 profile）
> 替代/被替代：无（扩展 [DEC-016](DEC-016-conversation-events-and-user-messages.md) 的会话事件面与
> Context 设计的确定性压缩层；不改变 [DEC-003](DEC-003-event-sourced-persistence.md)、
> [DEC-029](DEC-029-memory-domains-and-learning-contracts.md) 已冻结语义）

## 背景与问题

M4 交付的 `StandardContextManager` 解决了 context 随 Session 增长直接超过 Provider
window 的问题：分区选择、水位裁剪、artifact/event 引用替换、图片预算、checkpoint 推荐与
审计均已落地。但其"压缩"本质是 Context Reduction（结构替换与丢弃），不是 Semantic
Context Compaction。随着 miracle 进入长时间真实设备运行，User-visible Session 可以持续
增长数小时到数天，当前体系在两条链路上缺失：

- Cold History -> Relevant Context：旧会话内容一经裁剪即退出，无法在后续步骤按相关性
  重新进入模型请求。
- Conversation -> Semantic State：跨多条消息才能成立的用户约束、决策与未决线索
  （"以后都这样""除了张三"）没有固化机制，只能整体留在历史里随水位被裁掉。

[Issue #39](https://github.com/Linductor-alkaid/mira/issues/39) 提出把单一
Select/Trim/Reference/Drop 模型扩展为 Reduce/Retrieve/Rerank/Consolidate/Compress 的
多层上下文管理体系。本决策冻结其架构方向与边界，专项设计见
[Context Intelligence 设计](../design/context_intelligence_design.md)。

## 决策

1. **三级上下文生命周期正式化**。Hot Context（当前请求直接进入：近期对话、当前
   Observation、活动工作流状态、未决工具结果与不确定副作用）保持现有最小执行集与
   P0–P2 语义；Warm Context（已固化投影：TaskCheckpoint、ConversationCheckpoint、
   活动约束、相关记忆）经 ContextManager 准入进入；Cold History（EventStore、
   ArtifactStore、完整会话历史）默认不进入模型请求，只能经检索重新进入。Session
   生命周期与 Model Context 生命周期解耦：Session 可无限增长，每次请求的有效上下文
   必须有界。
2. **五层管线，Layer 0 即现有实现且保持最终裁决权**。Layer 0 确定性 Reduce 继续由
   `StandardContextManager` 承担，不调用模型，并保持最终 token/safety admission 权威；
   Layer 1 检索（`IContextRetriever` + `IContextEmbedder`）只负责候选召回；Layer 2
   重排（`IContextReranker`）为可选组件，embedding-only 配置必须可正常运行；Layer 3
   语义固化（`ISemanticConsolidator`，经 `IModelProvider` 可配置独立小模型）产出
   `ConversationCheckpoint`；Layer 4 提示压缩（`IPromptCompressor`）为可选优化，仅
   作用于允许压缩的内容集合。编排方 `ContextIntelligenceService` 只有建议权：其输出
   作为 ContextItem 候选进入 Layer 0，由 `StandardContextManager` 决定实际进入模型的
   内容。语义组件不得获得最终准入权。
3. **派生投影纪律（RULE-07/RULE-09 的延伸）**。所有语义产物（摘要、约束、决策、
   偏好候选、embedding、检索结果）一律是 Derived Projection，必须携带 `source_events`
   provenance 与 `through_event_sequence`；EventStore 仍是唯一事实源。冲突时优先级为：
   近期显式用户指令 > 已验证任务状态 > ConversationCheckpoint > 检索记忆；允许对派生
   记录执行"失效 -> 取回源事件 -> 重新固化"。
4. **ConversationCheckpoint 是会话级投影，不是第二份聊天记录**。与 TaskCheckpoint
   同受 RULE-07 约束：可从 EventStore 重建、终态幂等、迟到固化不得覆盖新状态。异步
   固化的提交校验五元组为 `session_id / task_id / task_epoch / environment_epoch /
   through_event_sequence`。触发采用滞回（约 60–70% 利用率预备、80–85% 提交、目标回落
   40–55%），禁止高频 compaction。
5. **模型不硬编码进 Core**。`IContextEmbedder`/`IContextReranker`/`ISemanticConsolidator`/
   `IPromptCompressor` 全部经接口注入；issue #39 列出的候选（bge-small-zh-v1.5、MiniLM
   cross-encoder、Qwen ~0.6B、LLMLingua-2）仅为首轮实验候选，选用前必须经供应链复核
   （许可证、provenance）与本决策第 9 条的 benchmark 证据。Core 不引入推理后端依赖。
6. **检索对象扩展到会话与学习资产**。Layer 1 的索引对象从 MemoryRecord 扩展到
   Conversation 段、WorkflowEpisodeRecord 与 WorkflowRecoveryLesson（DEC-029 资产）；
   embedding 供给复用 `MemoryQuery::query_embedding` / `index_embedding` 的外部供给
   语义。向量索引是可重建投影，向量库不是事实源。
7. **Layer 0 自足性是正确性依赖**。任一语义组件失败、缺失或降级时，现有确定性管线
   必须仍能闭合每次模型请求（复用 `MemoryQueryQuality` 的逐腿降级语义）；语义层是
   质量与连续性优化，不是可用性前提。
8. **实施门禁**。Stage A（100/500/1000 轮 long-session benchmark，衔接
   `MNT-202609-28` 的 Eval profile 与 `MNT-202609-29` 的评估运行）先行建立基线，
   之后按 Stage B–F 逐层引入（见专项设计 §12）；Stage E 需 miracle 真机证据
   （`MNT-202609-27` 通道）。`MNT-202609-31/32` 的语义召回扩展在本方向框架内按证据
   立项，本决策不使其自动就绪。
9. **验收以行为指标为准**。约束召回（Constraint Recall）为最高优先级指标；同时报告
   决策/未决线索召回、矛盾率、幻觉率、provenance 准确率、压缩率、上下文 token 是否
   趋于有界、任务成功率与真机延迟/RAM/功耗。实验矩阵 A–E（无检索 → 全管线）在专项
   设计 §13 冻结。

## 非目标

- 不把完整 Conversation 永久保留在模型上下文；不用单一模型递归 summarize 全部历史。
- 不让 embedding 向量、semantic summary 或检索结果成为事实源或替代 EventStore。
- 不立即引入完整向量数据库，不在 Android 打包大型生成模型。
- 不恢复已取消的 M5/M6；不改变 M7 Blocked 状态。
- 不改变 Provider 压缩（§15）作为 Adapter 优化的定位。

## 备选方案

- 维持现状（仅确定性 Reduce）：长会话连续性与用户约束保持率不可控，与 miracle 长时
  运行目标冲突，不采用。
- 递归全量摘要（history -> summarize -> history）：事实漂移累积、无法恢复、无
  provenance，Context 设计 §23 已明确不采用。
- 让语义组件直接决定模型上下文（跳过 Layer 0 准入）：削弱 token/safety 单一裁决点，
  违反本仓库分层纪律，不采用。
- 向量库为中心（embedding-first）：无法可靠处理 ACL、时间失效、精确标识与否定约束，
  不采用。

## 影响与风险

- 新增五个 Provider 接口与 `ContextIntelligenceService` 编排方，公共契约面扩大；
  头文件与版本化按 DEC-002 处理，实现前不得宣称能力。
- 本地 embedding/重排/固化模型引入内存、延迟与功耗成本；Android 侧按 Stage E 实测
  决定默认配置，未实测前不设默认。
- 异步固化与提交校验引入新的竞态面；五元组校验、终态幂等与取消路径必须在里程碑测试
  矩阵中逐项覆盖。
- 语义固化依赖模型输出，存在幻觉与约束错误固化的风险；以 provenance、冲突优先级、
  Preference 类人工审批（沿用 MemoryConsolidation 纪律）与 benchmark 门禁缓解。

## 验证方式

- Stage A long-session benchmark 可重复执行且产出基线（token 趋势、选择/丢弃审计）。
- 检索评估数据集（Recall@K、MRR（平均倒数排名）、nDCG（归一化折损累计增益）、
  reranker uplift）与固化评估（Constraint
  Recall/Precision、矛盾率、幻觉率、provenance 准确率）在实现里程碑内冻结阈值。
- miracle Android 真机至少一轮完整 benchmark（延迟、RAM、存储、功耗、任务连续性），
  证据等级按项目管理规范 §10 记录。
- 每层引入前后以实验矩阵 A–E 对照，token 收益不得以任务成功率或约束召回显著下降为
  代价。

## 关联文档和工作项

- [Context Intelligence 设计](../design/context_intelligence_design.md)（专项设计，与本决策同日冻结）
- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)（Layer 0 所在）
- [Issue #39](https://github.com/Linductor-alkaid/mira/issues/39)
- [DEC-003](DEC-003-event-sourced-persistence.md)、[DEC-016](DEC-016-conversation-events-and-user-messages.md)、
  [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、[DEC-030](DEC-030-learning-loop-runtime-semantics.md)
- [阶段 F 后续维护计划](../plans/maintenance-2026-09-post-stage-f.md)（`MNT-202609-27/28/29/31/32`）
- [Mira 实施总计划](../plans/mira-implementation-plan.md)
