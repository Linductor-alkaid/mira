# DEC-037：Temporal Policy——高频条件策略的经验固化方向

> 状态：Accepted（方向冻结；Stage T1 已由 [M26](../plans/m26-temporal-policy-stage-t1.md)
> 立项承载（2026-09-24），其实现进度见该里程碑）
> 日期：2026-09-14
> 决策人：Mira Maintainers
> 需求来源：[Issue #50](https://github.com/Linductor-alkaid/mira/issues/50)
> 关联计划：[M26](../plans/m26-temporal-policy-stage-t1.md)（Stage T1）
> 上位决策：[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)
> 关联决策：[DEC-011](DEC-011-demo-first-external-validation.md)、
> [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
> [DEC-030](DEC-030-learning-loop-runtime-semantics.md)、
> [DEC-033](DEC-033-hybrid-visual-grounding.md)、
> [DEC-036](DEC-036-consolidation-model-supply.md)

## 背景与问题

Mira 现有能力建立在"观察 → 理解 → 一次决策 → 执行 → 再观察"的离散交互模式上，
适合低频 GUI 任务；游戏、实时交互与连续控制场景需要几十甚至上百 Hz 的条件判断，
不能依赖 LLM/VLM 逐帧推理。issue #50 提出 **Temporal Policy**：Agent 在低频探索中
识别稳定时序模式，把验证过的经验编译为本地 Runtime 可高频执行的条件策略，形成
"Workflow 固化低频任务流程，Temporal Policy 固化高频交互策略"的互补关系。

需要决策的是：是否接受该方向、与 DEC-014 双路径模型及 DEC-011（M5/M6 终止）门禁
的关系、感知侧（Mirador/Temporal Cache）与 Core 的边界，以及规则归纳的 authority
与安全纪律。

## 决策

1. **接受 issue #50 方向（Future）**：引入 Temporal Policy 作为与 Workflow 并列的
   经验固化机制，核心原则"Agent 负责探索和处理未知，Runtime 负责执行已经理解的
   行为"。方向冻结、实现未开始；阶段命名 **T1–T6**，与 DEC-032 Stage A–F、
   DEC-035 Stage W1–W5 并行不冲突。
2. **统一 Policy 抽象，不绑定传统 FSM**：用户与 Agent 面对统一 Policy 描述，Runtime
   后端按复杂度选择 FSM / HSM / Behavior Tree / Reactive Rule，避免正交状态的平面
   展开。
3. **感知边界**：时序感知（Tracking、Motion Descriptor、Pose/Skeleton 抽象、
   Motion Matching、Temporal Cache 视觉部分）位于感知侧，经适配器抽象接口接入
   Core；**Core 不引入 Mirador 源依赖或 ABI 依赖**，Mirador 是外部候选供给方，与
   DEC-033 的 OCR/检测后端同等待遇，是否引入由供应链复核与 benchmark 决定。匹配
   算法（DTW、Fourier、关键点、embedding）是感知侧实现细节，不进 Core 契约。
4. **模型供给**：未知动作的语义升级调用 VLM/Agent 时按 DEC-036 口径使用既有可用源
   模型（含主模型），不引入专用小模型依赖；初期优先传统 CV、关键点 descriptor 与
   DTW，不引入大型时序神经网络作为前提。
5. **规则归纳纪律**：候选规则是派生投影（RULE-07，携带 source episodes，可重建）、
   有界（RULE-08）、版本化衍生资产（对齐架构 W-03，EventStore 事实源）；晋升以重复
   验证证据为准（RULE-10），降级/冲突显式事件化。**规则不得编码安全、授权或用户
   约束语义（RULE-09 延伸）**——它是执行优化，不是权限来源。
6. **Executor 强制**：Reactive 层走 realtime lane（drop/背压显式可见），Policy 层走
   LowLatency/lockfree 或确定性周期任务，归纳/编译走普通 `submit_auto()`；不引入
   自建线程、定时器或 fire-and-forget 循环。
7. **与 DEC-011 的关系**：本方向不恢复 M5/M6 原范围，不改变 M7 `Blocked` 状态。
   纯 Core 确定性阶段（T1/T4/T5）按常规授权立项；涉及真机感知（T2/T3）与连续控制
   注入（T6）的阶段受 DEC-011 门禁约束，以 `MNT-202609-27` 证据立项。

## 非目标

- 不一次性构建完整的游戏 Agent 框架；游戏只是连续环境的首个验证场景，能力定位是
  Mira 基础设施（GUI 的 loading 等待、构建失败重试、机器人接近减速同属此类）。
- 不让 LLM/VLM 进入高频控制循环；不在无真机证据时宣称实时性（RULE-10）。
- 不建立平行的第二套资产治理体系：策略资产与 Workflow Runtime 同层（版本化、审计、
  激活状态共享）。
- 不改变既有里程碑状态；Stage T1 曾未立项（2026-09-24 更新：已由
  [M26](../plans/m26-temporal-policy-stage-t1.md) 立项并在其文件内冻结正式契约、
  门禁与八问，本决策语义不变）；T2–T6 进入实现前仍须按规范新建里程碑文件。

## 备选方案

- **只在 Agent Loop 内以更高轮询频率缓解**（否决）：LLM/VLM 延迟与成本在百 Hz 量级
  不可行，且 AGENTS.md 已明确低频决策不得进入高频控制循环。
- **绑定专用 Game Agent 框架/FSM**（否决）：正交状态组合爆炸导致平面状态机不可维护；
  且把能力锁死在游戏场景，违背 Mira 基础设施定位。
- **把时序感知放进 Core**（否决）：视觉算法属于平台/感知侧能力；Core 持有世界状态、
  规则与策略语义，依赖方向不得反向（DEC-033 同一边界）。
- **规则直接由模型生成并生效**（否决）：未经重复验证的模型产出不得获得执行权；
  归纳产物必须走 CandidateRule → Testing → RuntimePolicy 的证据晋升路径。

## 影响与风险

- 新增公共契约面（TemporalHistory/TrackedEntity/ReactiveRule/Policy Runtime），按
  DEC-002 版本化；全部为未来阶段加法式变更，既有契约不动。
- 风险：误触发注入错误输入。处置：Testing 沙盒化（dry-run/限速/Agent 监督并行）、
  证据晋升、显式降级、Takeover 无例外覆盖策略路径。
- 风险：实时性期待过早。处置：Stage T6 真机门禁 + RULE-10，未实测不声明。
- 风险：与 M5/M6 终止决策的边界混淆。处置：本决策第 7 条显式划定——方向冻结不受
  门禁，真机感知/连续控制实现受 DEC-011 门禁。

## 验证方式

- 方向冻结以本决策与 [Temporal Policy 设计](../design/temporal_policy_design.md)
  为准；Stage T1 立项时在里程碑文件内冻结正式契约、测试矩阵与评估门禁（确定性
  闭环证明，沿用 M16–M20 的冻结口径与跨进程 digest 一致性要求）。
- T2/T3/T6 的立项前置是 DEC-011 门禁（`MNT-202609-27` 证据），证据等级按项目管理
  规范 §10 记录。

## 关联文档和工作项

- [Temporal Policy 设计](../design/temporal_policy_design.md)
- [Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
- [视觉 Grounding 设计](../design/visual_grounding_design.md)
- [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)、
  [DEC-011](DEC-011-demo-first-external-validation.md)、
  [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](DEC-030-learning-loop-runtime-semantics.md)、
  [DEC-031](DEC-031-agent-recovery-orchestration.md)、
  [DEC-033](DEC-033-hybrid-visual-grounding.md)、
  [DEC-036](DEC-036-consolidation-model-supply.md)
- [Mira 实施总计划](../plans/mira-implementation-plan.md)
- [Issue #50](https://github.com/Linductor-alkaid/mira/issues/50)
