# DEC-044：多 Agent 工作流场景与 Subagent 上下文隔离边界（Context Curator Stage W5）

> 状态：Accepted
> 日期：2026-09-23
> 决策人：Mira Maintainers
> 需求来源：[Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)
> 「Subagent 与上下文隔离」节（需求记录，非决策文档）
> 上位决策：[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)（双平面架构）、
> [DEC-035](DEC-035-context-curator-working-context.md)（Stage W5 归属）
> 关联计划：[M24](../plans/m24-context-curator-stage-w5.md)（Stage W5）
> 替代/被替代：无

## 背景与问题

[Context Curator 设计](../design/context_curator_design.md) §13 为 Stage W5
（Subagent fork / merge）设定的进入门禁有两项：W4 关闭（已满足，
[M23](../plans/m23-memory-promotion-stage-w4.md) 于 2026-09-22 关闭）与
**「多 Agent 工作流场景冻结」**。后者在既有文档语料中没有承载：issue #48 的
「Subagent 与上下文隔离」节只是需求记录（Developer/Test/Review 角色子 Agent、
只读基线继承、局部 delta、curated result、merge policy 二值），设计 §13 W5 行
与 [DEC-035](DEC-035-context-curator-working-context.md) §4 第 4 条均为一行式
占位。按 [DEC-035](DEC-035-context-curator-working-context.md) 验证方式的约定
（W2–W5 各自里程碑冻结验收方式后再进入实现），W5 立项前必须先回答：

1. fork/merge 场景绑定到哪个执行面——WorkflowRuntime 子运行、Agent Harness
   控制平面，还是新建独立 subagent 抽象？
2. fork 分支的身份语义——同会话新链，还是独立会话？
3. 子代理与既有纪律的边界——动作租约（W-07）、W3 自动触发链、W4 晋升通道、
   环境动作各自如何约束？

## 决策

1. **场景绑定 Agent Harness 控制平面，subagent 建模为 Session 内子代理**：
   一个 subagent 是宿主在父会话之旁创建的**子 Session**（自己的 `SessionId`、
   事件序列与 Task），不是新执行面。Core 不引入 `SubagentRuntime`、subagent
   框架或任何第三平面；[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)
   的双平面架构不变。子会话与父会话在控制平面是同等的普通会话，差别只在
   Working Context 的 fork 溯源（第 3 条）。
2. **fork/merge 是 Working State Plane 操作，不是环境动作、不是 Workflow
   步骤**：快照 fork 与 parent merge 属于上下文平面的投影操作，经既有
   `ContextMemorySupervisor` 路由与 §5.2 提交管线执行（设计 §5.2/§8），不
   触碰 `IEnvironment`/`IInputProvider`。Workflow 侧的「分支合并、并行、子
   Workflow 步骤」保持 [DEC-019](DEC-019-workflow-ir-contract.md) §2 声明的
   **未承诺扩展位**；issue #48 强制 flush 边界清单中的 `workflow fork` 属于该
   扩展链，不进入 W5 范围，若未来落地须另行决策（含 W-07 单租约仲裁设计）。
3. **只读基线继承 = 子会话 fork + 溯源**：子代理经确定性 fork 获得父快照的
   逐字副本作为自身快照链的初始 `previous`（只读基线），副本携带 fork 溯源
   （父快照 id、父会话、fork 点父水位）；父会话的 store 永不被子代理操作
   写入。fork 不复用 M20 的 task_epoch 新链语义——epoch 隔离服务于「旧分支
   作废」（cancel/pause/takeover 提升 epoch，旧链候选对新 live 丢弃，
   `commit_working_context` 只认单一 live 五元组），与「两分支并行存活」的
   fork 语义相反；同会话方案被 live 五元组校验、会话水位单调与 W-07 三重
   既有不变量系统性阻断。
4. **curated result = 子侧既有 Curator 管线的产物**：子代理结束时不把完整
   探索历史塞回父会话，只以带 provenance 的**局部 delta** 返回；delta 内容
   来自子会话按既有 `IContextCurator`/提交管线（W2/W3 契约不变）维护的自身
   快照链——fork 基线就是子链的 `previous`，W2 的编号转录与 supersede 引用
   纪律原样适用。Core 不新增「子代理专用 Curator」平行契约。
5. **parent merge policy = 机械确定性合并，裁决权在确定性组件**：合并按引用
   驱动的保留/supersede 纪律执行（沿 W2 指令契约 + M23 固定顺序先例），冲突
   不做语义取舍（stale supersede 丢弃、新增追加、上界固定顺序截断），产物是
   普通快照候选、经既有 §5.2 提交管线落库（不放宽同水位冲突 fail-closed，
   不设第二写路径）。merge policy 的二值结果对应 issue #48：accepted = 合并
   进父会话下一个已提交快照；rejected = 宿主不调用合并，子代理内容只存于子
   会话可重建投影与 Raw Trace（RULE-07）。模型介导的语义合并只能作为后续
   阶段的加法增强，不得替代机械校验层（设计 §2 原则 1：语义组件提供候选，
   确定性组件掌握准入权威）。
6. **环境动作边界**：W5 场景是推理角色隔离（Developer/Test/Review 等只推理、
   不操作环境的角色）。子代理如需环境动作，只能在其自身会话内按既有
   ActionLease 纪律执行（同一 Session 任意时刻最多一个 active lease；
   Planner/Reasoner 类角色不需要 lease——
   [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)
   §9）；父与子是不同 Session，W-07 按会话键控、天然无仲裁需求，Core 不新增
   跨会话租约机制。W5 交付物本身不含任何动作路径。
7. **W3/W4 共存边界**：不实现自动 fork/merge，不把晋升挂接进自动链——晋升
   维持 [M23](../plans/m23-memory-promotion-stage-w4.md) 冻结的宿主显式触发
   现状（自动晋升触发若未来立项，须显式推翻该决策记录并论证动机，不得顺手
   带入）。`WorkingContextAutoCurator` 的每会话链键控语义不变：父、子会话
   各自独立链状态，宿主可自行选择是否为子会话运行自动 curation。
8. **与 Memory 的边界不变**（DEC-035 第 3 条重申）：合并进父快照的内容按
   M23 冻结的 section→kind 映射与宿主显式时点参与晋升；未合并分支内容不进入
   父快照，不随父晋升；子代理与 Curator 仍永不直接写 Memory。

## 备选方案

- **绑定 WorkflowRuntime 子运行**（否决）：把 fork/merge 做成 IR 子
  Workflow/并行步骤可以复用编排与回放基础设施，但
  [DEC-019](DEC-019-workflow-ir-contract.md) §2 明确「通用图结构（分支合并、
  并行、子 Workflow 步骤）是扩展位，本决策不承诺」，落地需要 IR schema 破坏
  性扩展、新决策与 W-07 单租约仲裁设计；且场景主体（推理型角色的上下文隔
  离）不是 Workflow 步骤所能表达。成本与风险远超 W5 的上下文范围。
- **新建独立 SubagentRuntime/subagent 框架**（否决）：语义上最"干净"，但构成
  DEC-014 双平面之外的第三执行面；当前唯一消费场景是 issue #48 的上下文隔
  离，无任何超出该场景的需求证据，属投机 API，违反「每阶段最小冻结」纪律。
- **同会话 fork（bump task_epoch 或兄弟 TaskId）**（否决）：M20 的 epoch 新链
  是「旧分支作废」语义（W1-G4 取证：旧链候选对新 live 100% 丢弃），
  `commit_working_context` 的单一 live 五元组校验使并行分支除最后提交者外
  全数 `DiscardedStale`；同会话多链使会话水位交错（同水位 fail-closed /
  stale-watermark 误杀）；W-07 阻止同会话并行环境操作；`erase_session` 会
  连坐两个分支。与 fork 需要的「并行存活」语义系统性错位。

## 影响与风险

- 新增公共契约（fork/delta/merge 一组确定性纯函数 + 快照 schema 1.2 加法
  minor），全部为加法式变更；`IContextCurator`、`IWorkingContextStore`、
  Layer 0、W3 触发与 W4 晋升的既有契约零修改。模块归属 core，
  `architecture-policy.json` 零差异。
- 风险：场景边界与宿主/产品侧对「多 Agent」的期待错位（若真实期待是并行环
  境操作或 Workflow 编排，本决策不匹配）。处置：本决策即立项评审的显式确认
  对象；workflow fork 需求归 DEC-019 扩展链另行决策。
- 风险：机械合并不做语义裁决，冲突条目双存或 stale supersede 丢弃可能造成
  内容重复，依赖 RULE-08 上限与父侧下一次 curation 收敛。处置：合并报告透
  出全部计数；语义收敛声明归真实模型轮与 Stage E（RULE-10）。
- 风险：合并条目的 provenance 指向子会话事件（`EventId` 为全局 128 位标识，
  跨会话引用合法），父会话 `erase_session` 不触及子会话、反之亦然；跨会话
  擦除编排（隐私场景连带删除派生内容）与 M23 晋升面同款，归宿主职责与既有
  Erasure 治理，W5 不扩展。

## 验证方式

- 由 [M24](../plans/m24-context-curator-stage-w5.md) 承载 Stage W5 实现与验
  证：门禁 W5-G1–G6（`tests/m24/`，label `integration;m24`）覆盖 fork 契约、
  delta 契约、机械合并策略、§5.2 提交与竞态贯通、生命周期边界（含「同会话
  fork 拒绝」「子操作零写父 store」两条场景负向冻结）与恢复重建/跨进程确
  定性；无模型、无语义质量声明（RULE-10）。
- 门禁冻结后不得静默放宽（[阶段冻结与决策协议](../project/stage_freeze_protocol.md)
  §6）；本决策若需修订（如未来引入模型介导合并或 workflow fork），须按项目
  管理规范 §8 走决策变更。

## 关联文档和工作项

- [Context Curator 与 Working Context 设计](../design/context_curator_design.md)
  §2/§5/§13（W5 行门禁与本决策的关系）
- [DEC-035](DEC-035-context-curator-working-context.md)（Stage W5 归属）、
  [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)（双平面）、
  [DEC-019](DEC-019-workflow-ir-contract.md)（扩展位声明）、
  [DEC-002](DEC-002-public-contract-versioning.md)（schema 1.2 加法 minor）
- [M24：Context Curator Stage W5——Subagent Fork / Merge](../plans/m24-context-curator-stage-w5.md)
- [M23](../plans/m23-memory-promotion-stage-w4.md)（W4 关闭前置与晋升边界）、
  [M20](../plans/m20-working-context-stage-w1.md)（五元组/epoch 语义）、
  [M21](../plans/m21-context-curator-stage-w2.md)（W2 指令契约与 supersede
  引用纪律——fork 基线作为子链 `previous` 的依据）
- [Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)
  「Subagent 与上下文隔离」节
