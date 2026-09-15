# DEC-041：会话 World State 投影——Runtime 当前环境认知的共享表示

> 状态：Accepted（方向冻结；实现未开始）
> 日期：2026-09-15
> 决策人：Mira Maintainers
> 需求来源：[Issue #55](https://github.com/Linductor-alkaid/mira/issues/55)
> （World State / Environment Model 缺口、Observation 与 Runtime 各子系统之间的
> 统一语义）
> 上位决策：[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)
> 关联决策：[DEC-005](DEC-005-observation-coordinate-host-boundary.md)、
> [DEC-019](DEC-019-workflow-ir-contract.md)、
> [DEC-027](DEC-027-app-model-contract-and-confidence.md)、
> [DEC-028](DEC-028-navigation-planner-and-navigate-resolution.md)、
> [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
> [DEC-031](DEC-031-agent-recovery-orchestration.md)、
> [DEC-032](DEC-032-context-intelligence-layered-context.md)、
> [DEC-033](DEC-033-hybrid-visual-grounding.md)、
> [DEC-035](DEC-035-context-curator-working-context.md)、
> [DEC-037](DEC-037-temporal-policy.md)、
> [DEC-038](DEC-038-unified-behavior-trace.md)

## 背景与问题

Mira 已有的环境知识表示各自服务于一个时间尺度：Observation 是单次快照
（DEC-005），App Model 是跨任务的长期 UI 状态图与置信度（DEC-027），
`MemoryDomain::EnvironmentModel` 是记忆的组织视图（DEC-029），Working Context
是面向模型调用的会话投影（DEC-035）。缺的是中间一档：**会话级的「Runtime 当前
认可的环境状态」**——当前前台应用与页面假设、哪些可交互实体仍然存活、最近发生了
什么状态变化。

[Issue #55](https://github.com/Linductor-alkaid/mira/issues/55) 指出其后果：Agent
反复从原始 Observation 重新理解环境；Context、Memory、Workflow 导航、Recovery
与未来 Temporal Policy 各自从 Observation 或历史事件构造自己的环境假设，同一视觉
实体或页面状态在不同系统中产生不同表示。此前规划的 GUI Topology 方向（App Model）
只覆盖「这个应用长什么样」，不覆盖「现在 believed 处于哪个状态」。

需要决策的是：这一层以什么身份存在（事实源还是投影）、与 Observation/App Model
/各消费者的边界、更新语义与生命周期。

## 决策

1. **会话 World State 是投影，不是事实源**。它表示 Runtime 当前认可的环境状态
   （前台应用与页面假设、存活实体引用及其置信、最近状态变化），从 L0 事件
   （Observation 记录、DEC-033 结构化感知证据、App Model 导航观测
   `WorkflowNavigationObserved`、工具执行结果、验证结果）**确定性重建**
   （RULE-07/W-03）：同事件前缀同投影状态（digest 确定性为立项验收目标，对齐
   DEC-035 checkpoint 先例）。EventStore 仍是唯一事实源；Runtime 对 World State
   的每次认可与修正以事件留痕，不存在「直接写状态」的旁路。
2. **纯函数更新语义**。状态转移是纯函数：当前投影 + 新事件 → 新投影；时间由调用方
   传入，不读时钟（DEC-027 置信度纯函数纪律）。观测冲突、识别失败、证据过期
   显式表达为置信衰减与陈旧标记；无供给或不可判定时进入显式 `Unknown`，不得以
   猜测冒充事实（RULE-09/RULE-10 精神）。
3. **与 App Model 的关系：当下假设 vs 长期知识**。AppModel 回答「这个应用有哪些
   状态、迁移多可信」（跨任务版本化资产）；World State 回答「现在 believed 在
   哪个状态、哪些实体存活」（会话内投影）。World State 引用 App Model 的
   `state_id` 表达页面假设；宿主感知供给边界不变——状态识别由宿主
   `ScreenStateProvider` 提供，Core 不做任何识别（DEC-027 §3），本决策不引入
   感知实现。
4. **与 Observation 的关系**：Observation 契约不变（DEC-005）；World State 消费
   Observation 产生的记录与结构化证据（含 DEC-033 `ElementRef`），不修改
   Observation schema（加法版本化除外）。坐标与 epoch 语义沿用既有边界。
5. **与 Behavior Trace 的关系（DEC-038）**：两者共享实体语义（`ElementRef`/
   App Model `state_id` 词表）：Trace L1 行为携带实体引用，World State 维护实体
   存活与当前假设。两者并列投影、互不隶属，都从 L0 重建；词表对齐是 DEC-038
   与本决策的共同立项前置。
6. **消费者只读，经既有门禁**：Context（DEC-032/035 的输入源之一）、Recovery
   （DEC-031 续跑上下文的环境事实）、Workflow 导航（DEC-028 当前状态判定的会话侧
   对应物，`navigate-no-screen-state` 等 fail-closed 语义不变）、未来 Temporal
   Policy（DEC-037）。World State 只是数据：进入模型上下文按脱敏规则处理，不提升
   权限、不构成动作授权（authority 仍在 DEC-004 门禁与 W-05/W-06 路径）。
7. **生命周期与恢复**：World State 随 Session/Task 生命周期由显式 owner 管理，
   更新任务经 Executor 承载（W-01）；checkpoint/恢复时从事件 + checkpoint 重建
   （DEC-035 恢复重建先例）。Human Takeover 或恢复后的第一次推进必须以新
   Observation 事件刷新投影（AGENTS.md「恢复前重新观察」的投影侧表达），
   Takeover 期间的旧假设不得作为新动作的依据。
8. **有界性**：实体数、状态历史深度、单条载荷字节设上限（RULE-08）；历史陈旧
   实体按显式可配置策略淘汰，淘汰过程可从事件重放验证。

## 非目标

- 不建立跨系统的「大记忆数据库」：感知层记忆（视觉外观、特征、track id 的时间
  演化）属 Mirador/宿主侧，Core 只消费其结构化证据；本决策不改变该边界。
- 不做感知识别、不维护跨帧视觉身份：`region_id` 只在单一 Observation 与 epoch 内
  有效（DEC-033 §2），跨帧实体 identity 是感知侧职责；Core 只维护引用、置信与
  陈旧标记。
- 不改变 Observation/坐标契约、App Model 契约与 DEC-028 导航语义。
- 不承诺跨设备或跨命名空间的世界模型（DEC-027 v1 单命名空间同源约束）。
- 不在本决策冻结投影的具体 schema、更新算子清单与缺省淘汰策略（首阶段里程碑
  冻结；进入 `Planned` 前新建里程碑文件）。

## 备选方案

- **把 World State 内嵌 Observation 快照**：DEC-027 已否决同型方案——Observation
  是单次快照，会话状态是跨事件聚合，生命周期与更新主体都不同。不采用。
- **维持各子系统自持环境假设（现状）**：重复重建与表示漂移正是 Issue #55 的核心
  缺口；每加一个消费者（Temporal Policy、Workflow Editor）漂移面扩大一次。
  不采用。
- **扩展 App Model 承载会话当前状态**：长期图知识是版本化资产，会话状态高频更新
  会污染版本链与置信度语义（「图结构可信度」≠「当前在哪」）。不采用。
- **World State 作为可写事实源（Runtime 直接提交状态）**：违反 RULE-07 单一事实源
  与 W-03 投影可重建，且与 DEC-003 事件溯源架构冲突。不采用。
- **让 Context Manager 兼职维护环境状态**：上下文投影的职责是「为模型调用选取
  有界内容」，不是「维护环境认知」；混职会让 DEC-035 的确定性快照语义被高频状态
  更新打破。不采用；Context 是 World State 的消费者。

## 影响与风险

- 投影更新贴近观察热路径（每步观察后）：需要增量更新与水位策略控制成本，全量
  重建仅用于恢复与审计（DEC-035 先例）；立项时给出更新延迟与预算上限。
- 「believed 状态」被消费方误用为事实的风险：命名、文档与类型设计必须显式表达
  假设性（置信、陈旧、`Unknown`）；负向测试覆盖「以陈旧假设授权动作被拒绝」。
- 实体身份稳定性依赖感知侧：DEC-033 落地前，无结构证据场景的实体引用只能来自
  宿主供给；投影在该场景下的表达力有限，文档如实披露（RULE-10）。
- 新增公开契约面（投影读取 API 与类型），DEC-002 版本化，installed-consumer
  需覆盖；与 DEC-038/DEC-040 的词表对齐是共同前置，任何一侧先行都必须预留
  对齐点。

## 验证方式

- 方向以本决策为准；首阶段立项时在里程碑文件内冻结：重建确定性（同事件序列同
  状态 digest）、纯函数性（更新算子不读时钟、同输入同输出）、恢复重建矩阵
  （checkpoint + 事件 → 等价投影）、`Unknown`/陈旧/冲突的 fail-closed 负向矩阵、
  消费者只读性（无写旁路）、Takeover 后强制重观察、脱敏矩阵、RULE-08 上限。

## 关联文档和工作项

- [Issue #55](https://github.com/Linductor-alkaid/mira/issues/55)
- [DEC-005](DEC-005-observation-coordinate-host-boundary.md)、
  [DEC-019](DEC-019-workflow-ir-contract.md)、
  [DEC-027](DEC-027-app-model-contract-and-confidence.md)、
  [DEC-028](DEC-028-navigation-planner-and-navigate-resolution.md)、
  [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-031](DEC-031-agent-recovery-orchestration.md)、
  [DEC-032](DEC-032-context-intelligence-layered-context.md)、
  [DEC-033](DEC-033-hybrid-visual-grounding.md)、
  [DEC-035](DEC-035-context-curator-working-context.md)、
  [DEC-037](DEC-037-temporal-policy.md)、
  [DEC-038](DEC-038-unified-behavior-trace.md)
- [Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
  （App Model 定位、W-03/W-05/W-06/W-07）
- [Context Curator 设计](../design/context_curator_design.md)（checkpoint/重建先例）
- 专项设计与里程碑文件随首阶段立项交付。
