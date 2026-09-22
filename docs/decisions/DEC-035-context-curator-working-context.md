# DEC-035：Context Curator 与 Working Context（可执行工作状态投影）

> 状态：Accepted
> 日期：2026-09-14
> 决策人：Mira Maintainers
> 需求来源：[Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)
> 关联计划：[M20](../plans/m20-working-context-stage-w1.md)（Stage W1）、
> [M21](../plans/m21-context-curator-stage-w2.md)（Stage W2）、
> [M22](../plans/m22-working-context-stage-w3.md)（Stage W3）、
> [M23](../plans/m23-memory-promotion-stage-w4.md)（Stage W4）、
> [M24](../plans/m24-context-curator-stage-w5.md)（Stage W5）
> 上位决策：[DEC-032](DEC-032-context-intelligence-layered-context.md)

## 背景与问题

DEC-032 Stage B–D（M17–M19）交付了检索召回、重排对照与会话语义固化。
`ConversationCheckpoint` 已能把跨消息才成立的约束、决策与未决线索固化为带
provenance 的 Warm 投影，但它面向**会话段**（"这段会话里出现了哪些重要信息"），
不面向**任务执行**（"现在继续执行这个任务，模型真正需要知道什么"）。长周期任务
缺少一个持续维护、可回溯、可增量更新的 Working Context，使主 Agent 不必反复重读
大量历史，也不因一次普通摘要丢失决策链、失败经验与当前状态（issue #48）。

需要决策的是：是否引入该方向、以什么形态引入、与既有 `ConversationCheckpoint` /
Memory / Layer 0 的边界如何划定，以及如何与 DEC-032 的 Stage E/F 排期共存。

## 决策

1. **接受 issue #48 方向**：在既有架构之上引入 `WorkingContextSnapshot`
   （task/session 导向的当前状态视图）与 `IContextCurator`（模型驱动的维护者，
   后续阶段），不另起平行的 summary memory，不把压缩权下放给 Provider
   continuation cache。
2. **快照是派生投影，不是权威**：语义组件提供候选，确定性组件掌握准入权威
   （DEC-032 第 2 条延伸）。快照只经 Layer 0 候选转换进入模型请求；Curator
   永不拥有上下文准入权与永久删除事实的权力；Trace Plane（EventStore/
   ConversationLog/Artifact）是唯一事实源。
3. **与 Memory 的边界**：快照是短期、任务导向、可频繁覆盖的状态缓存；跨任务
   知识只能经 `MemoryConsolidator` 既有纪律（policy、scope、验证、人工审批）
   进入长期记忆。Curator 可产生 Memory candidate，不得直接写 Memory。
4. **分阶段落地，阶段名 W1–W5**：与 DEC-032 的 Stage E（真机评估）/ Stage F
   （提示压缩）并行、不占用其编号；两方向交点在 Stage E 真机评估矩阵。
   - W1：`WorkingContextSnapshot` 确定性契约（store、水位、digest、epoch、
     provenance、Layer 0 转换、恢复），**无模型**，由 M20 承载；
   - W2：`IContextCurator` 契约 + model-backed 参考实现（previous snapshot +
     checkpoint + recent events 增量输入）；
   - W3：Supervisor 自动触发（watermark/event count/task boundary、coalescing、
     forced flush）；
   - W4：Memory Promotion（经 `MemoryConsolidator`）；
   - W5：Subagent fork / merge。（2026-09-23 注：场景边界冻结于
     [DEC-044](DEC-044-multi-agent-context-fork-boundary.md)——subagent 为
     Agent Harness 控制平面内父会话旁的子 Session，fork/merge 是 Working
     State Plane 投影操作而非环境动作或 Workflow 步骤；由
     [M24](../plans/m24-context-curator-stage-w5.md) 承载，契约语义随其
     §4 跑前冻结。）
5. **W1 语义边界**：快照三 section（constraints / decisions / open_issues）由
   已提交 checkpoint 确定性导出，整体替换语义与 Stage D 固化提交模型一致；
   提交沿用五元组纪律与终态幂等；恢复 = 从最近 checkpoint 重导出（幂等同 id
   同 digest）；W1 不引入 `previous` 参数、不引入新 `ContextItemKind`、不修改
   `StandardContextManager`；快照不含 `goal`（Layer 0 P1 已有权威任务帧，
   复述制造重复 authority）。
6. **Executor 纪律**：快照提交经
   `ContextMemorySupervisor::schedule_working_context_commit`（Deferrable），
   沿用既有关闭顺序与取消语义；W2 模型调用在同一路由后替换实现。

## 非目标

- 不实现 Curator 模型调用与增量 merge（W2）；不实现自动触发与 coalescing（W3）；
  不实现 Memory promotion 与 subagent fork/merge（W4/W5）。
- 不修改 `StandardContextManager`、Layer 0 既有语义、M19 checkpoint 契约。
- 不引入新持久化格式；W1 store 为 volatile 可重建投影（RULE-07）。
- 不接入真实模型（供应链复核通道，与 DEC-032 §14 同源）；无真机证据
  （Stage E，`MNT-202609-27` 通道）。2026-09-14 注：W2 起模型供给按
  [DEC-036](DEC-036-consolidation-model-supply.md) 口径——可用源模型即可（含主
  Agent 模型），不要求专用小模型，"小模型供给通道"不再是 W2 门禁。
- 不声明 token 收益或任务成功率：W1 评估对象是确定性管线行为（RULE-10）。

## 备选方案

- **等待 DEC-032 Stage E 真机证据后再立项**（否决）：Stage E 被
  `MNT-202609-27` 外部证据通道阻塞，且 W1 是纯本地确定性基础设施，不依赖
  真机或模型；等待会白白串行化两条无依赖的链。
- **一次性实现完整 Curator（模型 + merge + 触发 + promotion）**（否决）：
  违反"每阶段进入实现前冻结对应契约"的既有纪律；模型边界、增量合并语义与
  触发策略各自需要独立验证面。
- **把 Working Context 做成 Provider continuation cache 的职责**（否决）：
  continuation cache 是供应商协议适配，不具备 provenance、终态幂等与 ACL
  语义，无法承载可回溯投影（DEC-032 第 2/3 条）。
- **W1 即引入专用 `ContextItemKind::WorkingContextState`**（推迟）：W1 快照
  内容与 checkpoint 语句同源，复用既有 kind 可维持"不修改 Layer 0 既有语义"
  纪律；待 W2 内容分道且确需独立审计粒度时，以加法式变更引入。
- **快照含 `goal` section**（否决）：Goal 是 Layer 0 P1 确定性任务帧内容，
  模型介导投影复述会制造重复 authority（RULE-09）。

## 影响与风险

- 新增公共契约（`context_working_context.hpp`）、store、提交管线、supervisor
  路由与测试；全部为加法式变更，既有契约与 Layer 0 语义不变。
- 风险：W1 快照与 checkpoint 投影内容在 W1 高度重叠，被质疑为重复 API。
  处置：W1 的交付物是**生命周期基础设施**（身份、水位、digest、epoch 失效、
  幂等提交、恢复）与 W2 Curator 的输入形态；W2 起快照内容由 Curator 语义
  维护，与 checkpoint 分道。设计文档 §4.2/§6 明确该演进路径。
- 风险：W2 Curator 的非确定性输出破坏状态一致性。处置：同水位 digest 冲突
  fail-closed、五元组竞态丢弃、终态幂等在 W1 即冻结并测试，W2 candidate 走
  同一提交管线。
- 双投影（checkpoint 条目与快照条目）并存可能重复进入 Layer 0。处置：id
  空间分离保证可审计；文档约束宿主择一喂给；Layer 0 去重不在本决策范围。

## 验证方式

- W1 由 [M20](../plans/m20-working-context-stage-w1.md) 交付并验证：契约/集成
  测试矩阵（正常投影、降级、竞态、终态迟到、epoch 失效、恢复重建、shutdown
  路由）与冻结门禁的确定性评估 harness；本地门禁与 PR CI 按 M19 同一口径。
- W2–W5 各自里程碑冻结验收方式后再进入实现；语义质量与 token 收益声明只能
  出现在有真实模型/真机证据的对应阶段（RULE-10）。

## 关联文档和工作项

- [Context Curator 与 Working Context 设计](../design/context_curator_design.md)
- [M20：WorkingContextSnapshot 确定性契约（Stage W1）](../plans/m20-working-context-stage-w1.md)
- [M21：IContextCurator 契约与模型供给参考实现（Stage W2）](../plans/m21-context-curator-stage-w2.md)
- [DEC-032](DEC-032-context-intelligence-layered-context.md)、
  [M19](../plans/m19-context-intelligence-stage-d.md)（五元组提交纪律与
  Provider 语义组件范式的来源）
- [Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)
