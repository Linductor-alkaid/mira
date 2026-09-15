# DEC-038：统一 Behavior Trace——执行轨迹的三层语义投影

> 状态：Accepted（方向冻结；实现未开始）
> 日期：2026-09-15
> 决策人：Mira Maintainers
> 需求来源：[Issue #56](https://github.com/Linductor-alkaid/mira/issues/56)（Agent 行为轨迹
> → Workflow 编译）、[Issue #55](https://github.com/Linductor-alkaid/mira/issues/55)
> （Observation 与 Runtime 各子系统之间的统一语义）；外部佐证：Agent S 系列
> （arXiv:2410.08164、arXiv:2504.00906、Simular Agent S3）的 facts/behavior narrative
> 经验
> 上位决策：[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)
> 关联决策：[DEC-003](DEC-003-event-sourced-persistence.md)、
> [DEC-019](DEC-019-workflow-ir-contract.md)、
> [DEC-025](DEC-025-success-trajectory-compilation-and-publish-gate.md)、
> [DEC-026](DEC-026-task-induction-and-parameterization.md)、
> [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
> [DEC-032](DEC-032-context-intelligence-layered-context.md)、
> [DEC-035](DEC-035-context-curator-working-context.md)、
> [DEC-036](DEC-036-consolidation-model-supply.md)、
> [DEC-041](DEC-041-session-world-state-projection.md)

## 背景与问题

Mira 的执行事实已按 DEC-003 以事件形式落入 EventStore（Goal、Observation 元数据、
Model Request/Response 元数据、Decision、Action、Execution Result、Verification
Result、状态转换），Workflow 域另有两类结构化快照：DEC-025 的 `WorkflowTrajectory`
（Workflow Run 域的编译输入）与 DEC-029 的 Episode/RecoveryLesson（学习闭环）。
但在三处，消费方仍直接面对原始事件流：

1. **Agent 自由运行轨迹没有语义层**。DEC-025 §1 的宿主构造入口要求人工把成功任务的
   工具调用按序填入 steps；DEC-026 §4 显式把「从事件流自动抽取 Agent 工具调用轨迹」
   列为非目标并推迟到 Agent Harness 后续里程碑。因此 Issue #56 的「成功执行轨迹 →
   可编辑 Workflow」在 Agent 探索路径上没有数据来源。
2. **Context 缺少压缩语义层**。DEC-032/DEC-035 的整理与 Curator 直接消费原始事件与
   会话内容；长任务上下文增长没有「先结构化、再叙事化」的中间表示，同一执行过程的
   压缩产物与 Workflow 学习产物有各自漂移的风险。
3. **失败分析与调试依赖原始事件**。Episode/Lesson 只覆盖 Workflow Run 域的终态摘要，
   Agent 路径的失败没有同型的结构化经验载体。

外部研究（Agent S 系列）验证了同型结论：先把原始轨迹转换为 facts，再把 facts 连接为
behavior narrative，使下游（judge、context、蒸馏）在行为语义层消费，而不是直接处理
大量截图与冗余中间状态。该经验与 Mira 既有体系兼容，缺的是契约层的显式冻结。

## 决策

1. **三层语义，单一事实源不变**：
   - **L0 Raw**：EventStore 既有事件，事实源（DEC-003/RULE-07）。本决策不新增事实
     闭集成员；确需加法扩展事件时按 DEC-002 版本化。
   - **L1 Behavior**：语义行为单元（如 `Locate`/`Open`/`Select`/`Send`/`Verify`），
     携带源事件引用、实体引用（与 DEC-041 World State 共享 `ElementRef`/App Model
     `state_id` 语义）、参数摘要 digest、结果与验证状态。可结构化消费，是 Workflow
     编译、失败分析与状态机学习的工作层。
   - **L2 Narrative**：L1 的有界压缩叙事，供模型上下文与长期经验使用。
   L1/L2 均为**可重建投影**：任何内容必须能从 L0 事件序列 + 显式配置重建；重建失败
   即投影失效，不得作为事实来源（W-03/RULE-07）。同事件前缀的确定性重建（同输入同
   digest）是立项时的验收目标。
2. **语义提升的两类供给方，provenance 必须区分**：
   - **确定性提升**（纯函数）：工具执行记录、App Model 导航观测、Workflow 步骤结算
     等结构化事件 → L1，同输入同输出，不读时钟（对齐 DEC-027 置信度纯函数纪律）。
   - **模型辅助压缩**（DEC-036 口径的可用源模型）：原始事件 → L1 语义标注与 L2
     narrative。产物是**派生数据**：携带 `generated_by`（对齐 DEC-035 快照先例），
     是 `UntrustedExternalData`（RULE-09），不进入事实闭集、不构成权限与授权依据、
     不得绕过既有校验管线直接驱动执行。
3. **与 Workflow 编译的衔接（承接 DEC-026 §4 非目标）**：从 L1 抽取 Agent 工具调用
   轨迹的编排产出 DEC-025 `WorkflowTrajectory`（宿主构造路径的同型契约），不引入
   第二套轨迹契约；抽取产物是**草稿**，唯一入库路径仍是 `publish_validated` 门禁
   （W-04）。失败与中间态轨迹不进入编译输入，作为失败分析数据存在。
4. **与 Context 的衔接**：L2 narrative 是 DEC-032 分层上下文与 DEC-035 Working
   Context 的候选输入源；消费方式（触发、预算、水位）随首阶段里程碑冻结。Working
   Context 快照 schema 只做加法扩展（DEC-035 schema 1.1 先例）。
5. **与 World State 的关系**：L1 行为的实体引用与会话 World State（DEC-041）共享
   实体语义；两者并列投影、互不隶属，都从 L0 重建。Trace 不维护「当前 believed
   状态」，World State 不承载行为历史。
6. **边界与生命周期**：Trace 是数据面记录与投影，不引入新执行通道、不进入控制平面
   决策路径、不改变动作租约语义（W-07）；回放中 L1/L2 按「已记录投影」处理，回放不
   重放副作用（W-08）。压缩与提升是普通有限任务（`submit_auto()`），长会话增量提升
   经既有 Deferrable 路由（DEC-035 Supervisor 先例），不新增线程、循环或 fire-and-forget
   工作（W-01）。
7. **脱敏**：L1/L2 载荷只含 ID、digest、枚举名、有界摘要；截图等大内容用稳定引用与
   摘要，不在多个投影条目中复制（DEC-022 §5 / DEC-029 §2 同源边界）。

## 非目标

- 不做并行 rollout 选优（bBoN）作为运行策略；多个历史轨迹的比较属学习闭环
  （DEC-029/DEC-030 已有方向）。
- 不做自动 Policy 学习；Temporal Policy 归纳走 DEC-037 自身纪律。
- 不在本决策冻结 L1/L2 的具体 schema、抽取编排 API 与预算缺省值；它们在首阶段
  里程碑文件内冻结（进入 `Planned` 前按规范新建里程碑文件，不预分配编号）。
- 不改变 EventStore 既有事件的 schema 与语义。

## 备选方案

- **各模块自建执行摘要**（Context 压缩与 Workflow 学习各自维护摘要）：同一执行过程
  出现多个互不一致的语义表示，正是本决策要消除的漂移源。不采用。
- **模型生成的 narrative 直接存为事实**：违反 RULE-07（不可重建）与 RULE-09（不可信
  内容提升）。不采用；narrative 只能是带 provenance 的派生投影。
- **扩展 `WorkflowTrajectory` 承载 Agent 自由运行轨迹**：该契约的语义是「一次真实
  成功执行」的编译输入（DEC-025 §1），把失败与中间态观测塞入同一契约会污染编译输入
  语义并放大门禁面。不采用；抽取编排产出该契约即可。
- **跳过 L1 直接从 L0 压缩为 L2**：失去可结构化消费层——Workflow 编译与失败签名需要
  结构化行为而非叙事文本，且确定性提升无从谈起。不采用。

## 影响与风险

- 新增公开契约面（Trace 投影读取 API 与 L1/L2 类型），按 DEC-002 版本化；
  installed-consumer 需覆盖。
- L1 语义提升质量缺乏评估基准：确定性提升可契约测试，模型辅助压缩的质量声明受
  RULE-10 约束，立项时需扩展 [discrete-workflow-eval-v1](../benchmarks/discrete-workflow-eval-v1.md)
  或新建 profile 提供口径。
- 模型辅助压缩引入成本与延迟：预算与 deadline 沿用既有配置模式
  （`WorkflowLearningLimits`/Context 既有配置），缺省值是暂定默认值。
- 重建成本随事件量增长：需要水位/增量/checkpoint 策略（DEC-035 先例），全量重建
  仅作为恢复与审计路径。
- 与 DEC-041 的实体语义必须在首阶段对齐（同一 `ElementRef` 词表），否则两个投影会
  各自漂移——这是本决策与 DEC-041 的共同前置。

## 验证方式

- 方向以本决策为准；首阶段立项时在里程碑文件内冻结：L1/L2 schema、重建配方确定性
  （同事件序列同投影 digest）、纯函数性（确定性提升不读时钟）、脱敏负向矩阵、
  回放一致性（OfflineReplay 下投影不产生副作用）、增量与水位语义、抽取编排产物
  与 `WorkflowTrajectory` 的同型性测试。
- 全部实现经 Executor 路由（W-01），取消与 shutdown 路径闭合后才能关闭里程碑。

## 关联文档和工作项

- [Issue #55](https://github.com/Linductor-alkaid/mira/issues/55)、
  [Issue #56](https://github.com/Linductor-alkaid/mira/issues/56)
- [DEC-003](DEC-003-event-sourced-persistence.md)（事实源）、
  [DEC-019](DEC-019-workflow-ir-contract.md)、
  [DEC-025](DEC-025-success-trajectory-compilation-and-publish-gate.md)、
  [DEC-026](DEC-026-task-induction-and-parameterization.md)、
  [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](DEC-030-learning-loop-runtime-semantics.md)、
  [DEC-032](DEC-032-context-intelligence-layered-context.md)、
  [DEC-035](DEC-035-context-curator-working-context.md)、
  [DEC-036](DEC-036-consolidation-model-supply.md)、
  [DEC-041](DEC-041-session-world-state-projection.md)
- [Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
  （W-01/W-03/W-04/W-07/W-08、`RULE-07`/`RULE-09`）
- 专项设计随首阶段立项交付（本决策只冻结方向与边界）。
