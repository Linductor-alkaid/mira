# Mira Temporal Policy 设计（连续环境高频条件策略）

> 状态：Active（方向冻结；实现未开始）
> 版本：0.1
> 更新日期：2026-09-14
> 负责人：Mira Maintainers
> 需求来源：[Issue #50](https://github.com/Linductor-alkaid/mira/issues/50)
> 决策依据：[DEC-037](../decisions/DEC-037-temporal-policy.md)

## 1. 文档目的

本文将 issue #50 提出的 **Temporal Policy（时序策略）** 方向冻结进 Mira 架构：让
Agent 在低频探索、推理与纠错中识别连续环境里的稳定时序模式，把经过验证的经验编译为
可由本地 Runtime 高频执行的条件策略（FSM / HSM / Behavior Tree / Reactive Rule），
使昂贵的 Agent Reasoning 逐步转化为廉价、确定、低延迟的执行策略。

它是 Mira"经验程序化"的第三块拼图，与既有机制分工明确：

| 机制 | 固化什么 | 时间尺度 |
| --- | --- | --- |
| Workflow（DEC-014/DEC-019–030） | 低频任务流程（步骤拓扑、工具链） | 步骤间隔数百 ms 至更久 |
| Context Intelligence（DEC-032/DEC-035） | 会话语义与工作状态（上下文投影） | 每次模型请求 |
| **Temporal Policy（本文）** | **高频交互策略（条件与转换）** | **ms 至几十 ms 级状态更新** |

核心原则（与 DEC-014 双路径模型同源）：

> **Agent 负责探索和处理未知，Runtime 负责执行已经理解的行为。**

本文只冻结方向、边界与目标契约；实现未开始。各阶段进入实现前按项目管理规范新建
里程碑文件（不预分配编号）。AGENTS.md 的 Executor 强制约束、平台边界与
`RULE-01`–`RULE-12` 对本文全部内容无例外适用。

## 2. 与既有决策和遗产的关系

- **DEC-014（双路径模型）**：DEC-014 已确立"Workflow Runtime 负责高频、稳定、低成本
  执行"。Temporal Policy 把同一哲学延伸到连续时间尺度：Workflow-native path 处理
  分钟级任务规划，Temporal Policy 处理毫秒级条件行为，二者共用"经验 → 版本化资产 →
  确定性 Runtime"的范式。它不是第三条平行路径，而是执行数据平面在连续域的扩展。
- **DEC-029/DEC-030/DEC-031（学习闭环）**：`WorkflowEpisodeRecord` /
  `WorkflowRecoveryLesson` 是规则归纳的输入资产。Temporal Policy 的规则归纳是
  learning loop 的新增归纳消费者，沿用其既有纪律：Episode 是自动写入的运行经验、
  lesson 的采纳是 Agent Harness 编排、scope 准入拒绝 `User` 域直接写入、
  lesson 不提升权限（RULE-09）。
- **DEC-032/DEC-035/DEC-036（上下文固化与模型供给）**：Context Intelligence 固化
  "会话语义"，Temporal Policy 固化"执行策略"，共享 provenance、可重建投影、有界输出
  的纪律。未知动作的语义升级调用 VLM/Agent 时按 DEC-036 口径使用既有可用源模型
  （含主模型），不引入专用小模型依赖。
- **DEC-033（视觉 grounding）**：Temporal 感知是其时间维延伸。DEC-033 的
  GroundingRegion / PerceptionEvidence 契约、事件驱动调度与缓存复用纪律继续有效；
  时序状态估计位于其结构化输出之上，不替换静态定位管线。
- **DEC-011（demo 优先）**：M5（端侧感知模型治理）与 M6（bounded-latency 反馈控制）
  已按 DEC-011 终止，其回归以 demo 证据（`MNT-202609-27` 通道）为门禁。本设计不恢复
  M5/M6 原范围：感知侧优先传统 CV / 关键点 / DTW 等低成本方案，不假设端侧自建大型
  模型；涉及真机感知或连续控制注入的阶段（§13 Stage T2/T3/T6）受 DEC-011 门禁约束，
  纯 Core 确定性部分（Stage T1/T4/T5）按常规授权立项，不受该门禁。
- **M7（Tool 模组体系）**：保持 `Blocked`，状态不因本方向改变。

## 3. 分层决策架构

引入 Temporal Policy 后，决策按频率与职责分为三层（频率是目标量级，不是硬性限制）：

```text
Agent（LLM/VLM，约 0.1–2 Hz）
    语义理解、探索、规划、规则归纳与修改、未知处理
Temporal Policy（约 10–60 Hz）
    已理解场景的条件状态判断（FSM / HSM / Behavior Tree / Reactive Rule）
Reactive Controller（约 30–240 Hz）
    低延迟、逻辑简单、确定性高的条件动作与时序动作
```

职责红线：

- Agent 不得进入高频控制循环（AGENTS.md 既有规定）；低频决策经结构化 Decision
  校验后才影响策略资产。
- Temporal Policy 不做语义规划，只对已定义的状态与条件做判断。
- Reactive Controller 不维护策略状态机，只执行编译后的确定性判定。
- 任一层遇到未知状态、低置信度、规则冲突、连续失败或环境变更时，显式升级给
  Agent（事件驱动，不轮询）。

## 4. 数据契约（草案，未实现）

> 以下为方向级契约草案，正式契约在 Stage T1 里程碑内冻结（对齐 DEC-002 公共契约
> 版本化）。字段命名以届时契约为准。

### 4.1 TemporalHistory

有界环形观测历史，使状态可以引用过去的观测结果：

- 条目为轻量结构化样本（实体 ID、位置/姿态、时间戳、来源观测引用）；
- 大体积原始帧不进入历史，使用稳定引用与摘要（对齐 Observation 引用纪律）；
- 容量与保留窗口有界（RULE-08），可从 EventStore/Artifact 重建投影语义在 T1 冻结。

### 4.2 TrackedEntity 与时序状态

```cpp
struct TrackedEntity {          // 契约草案（未实现）
    EntityId id;
    Pose pose;
    Velocity velocity;
    MotionId motion;            // 版本化动作命名空间
    float motion_phase;         // 0..1，动作进度
    float motion_confidence;
    Timestamp state_since;
    TemporalHistory history;
};
```

- 被跟踪对象不再只有位置/类别/置信度，还具有速度、动作类型、动作阶段与观测历史；
- 上层策略的输入形如"Enemy #2 正在执行 `HeavySlash`，phase≈0.63，置信度 0.91"，
  而不是逐帧原始坐标；
- 实体生命周期（出现、丢失、合并、ID 稳定性）必须由确定性规则维护，不依赖模型；
- 时序状态估计输出是派生投影（RULE-07）：可从底层观测与感知侧记录重建，不替代
  EventStore 事实源。

### 4.3 感知与策略的接口边界（草案）

```cpp
// 感知侧（平台 Adapter 或外部视觉库）→ Core：结构化时序状态
ITemporalStateEstimator {
    observe(frame_bundle) -> Result<span<const TrackedEntity>>;
};

// Core：策略 Runtime 消费结构化状态，产出有界动作
IPolicyRuntime {
    step(const WorldState&, TickContext) -> Result<PolicyActions>;
};
```

Core 不实现视觉算法；感知侧不持有规则与策略语义。

## 5. Temporal Cache 与感知边界

### 5.1 统一视觉缓存分类

在 DEC-033 静态缓存（图标/UI 元素/外观/场景的快速重复识别）之上扩展时间维：

```text
Visual Cache
├── Static Cache（DEC-033 既有方向）
│   └── Icon / UI Element / Object Appearance / Scene
└── Temporal Cache（本设计新增，感知侧）
    └── Motion / Animation / State Transition / Trajectory / Event Pattern
```

Temporal Cache 缓存的是"一段连续视觉变化正在发生什么"：首次遇到新动作模式时用较重
的手段（关键点/骨骼，必要时 VLM）理解并记录 descriptor；再次出现相似运动模式时用
廉价时序特征直接匹配出 `MotionId + phase + confidence`。匹配层级自低向高：

1. bounding box 轨迹、局部光流、轮廓变化、少量关键点等廉价 signature；
2. 归一化关键点序列 + 插值/Spline/DTW（解决播放速度、帧率与卡顿导致的时间轴
   不对齐）；周期动作可选用 Fourier 低维表达，但 Fourier 不是唯一表示；
3. 仍无法确定语义时升级 VLM/Agent 理解（按 DEC-036 使用可用源模型），并建立新的
   Motion Cache 条目。

算法选型（DTW、descriptor 形式、embedding）是感知侧实现细节，不进入 Core 契约；
初期优先传统 CV、关键点 descriptor 与 DTW，不引入大型时序神经网络作为前提。

### 5.2 Mirador 边界

issue #50 提出 Mirador 作为独立轻量视觉基础设施库承担 Object Tracking、Temporal
Feature、Motion Descriptor、Pose/Skeleton 抽象、Motion Matching、Motion Phase
Estimation 与 Temporal Cache 视觉部分。边界裁定：

- **Mira Core 不引入 Mirador 源依赖或 ABI 依赖**。Mirador 是感知侧能力的外部候选
  供给方，与 DEC-033 的 OCR/检测后端一样，只能通过适配器接口（§4.3 形态）接入；
  是否引入、何时引入由 Stage T2 立项时的供应链复核与 benchmark 决定。
- 分工示例：Mirador 报告 `entity=enemy_2, motion=heavy_slash_a, phase=0.67,
  confidence=0.92`；Mira 持有并执行 `IF motion==heavy_slash_a AND phase>0.55 AND
  distance<3 THEN dodge_left`。视觉库不知道游戏规则，Mira 不实现视觉算法。

## 6. 条件策略契约与执行语义

### 6.1 统一 Policy 抽象，不绑定 FSM

架构层面不绑定传统 FSM：状态组合爆炸（生命值、位置、敌人攻击、资源、冷却、目标等
正交状态同时存在）会使平面状态机不可维护。用户与 Agent 面对统一的 Policy 描述；
Runtime 后端按复杂度选择：

```text
简单状态变化        → FSM
复杂上下文层级      → Hierarchical State Machine
明确优先级的组合行为 → Behavior Tree
极高频极简条件动作   → Reactive Rule（编译为确定性判定）
```

候选规则示意（Stage T1 冻结正式 schema）：

```yaml
state: combat
transitions:
  - when:
      enemy.motion: heavy_slash_a
      enemy.motion_phase: "> 0.55"
      distance: "< 3.0"
    do: [dodge: left]
```

### 6.2 动作与验证语义

- 策略产出的动作沿用 Action 三层模型（高层 Intent / 中层控制动作 / 底层输入事件），
  与离散动作共用同一套可取消、有界、可观测的结果语义；平台 Adapter 只做最终映射。
- 连续控制沿用既有边界：高层意图由 Native Controller 转为有时间边界的轨迹，交给
  `IInputProvider`；策略不直接生成无界轨迹。
- **Verify 纪律不变**：规则执行后的效果确认走既有 Verify/低成本检测信号；"规则触发了"
  不等于"目标达成"。规则内部的高频判定不豁免动作级验证。
- 每次 tick 的状态转换、规则触发、升级均发射事件（§11），失败对调用方与 Observer
  可见。

## 7. 规则归纳与生命周期

### 7.1 生命周期

```text
Unknown → AgentControlled → CandidateRule → Testing → RuntimePolicy
                ↑                    ↓（验证失败）
                └────── 升级 ←── 冲突/低置信/连续失败/未知状态
```

- Agent 正常探索并记录 Episode；多次 Episode 中出现相似条件与相似结果时，归纳生成
  候选规则（CandidateRule）。
- 候选规则先进入 Testing：在受控执行（dry-run / 沙盒 / 限速）中与 Agent 监督并行
  验证；多次验证成功且置信度达标后晋升 RuntimePolicy。
- 出现失败、反例、规则冲突或环境变更时降级回 AgentControlled，并显式暴露事件。

### 7.2 纪律

- 候选规则是**派生投影**（RULE-07）：必须携带 source episodes/provenance，可从
  EventStore 与感知侧记录重建；不得只留"一条规则"而无来源。
- 规则资产是**版本化衍生资产**（对齐架构 W-03）：EventStore 是事实源，规则索引必须
  可重建；序列化与兼容性按 DEC-002 版本化。
- **authority 纪律（RULE-09 延伸）**：规则不得编码安全、授权或用户约束语义——那
  些只能来自权威运行时与确定性来源。规则是执行优化，不是权限来源；一条归纳出的
  "可以发送"规则永远不能绕过授权系统。
- scope 与审批对齐 learning loop：自动归纳的规则属于运行经验资产，不得直接写入
  `User` 域；涉及用户偏好的晋升沿用 `MemoryConsolidator` 人工审批语义。
- 晋升/降级以重复验证证据为准（RULE-10）：未经验证的规则不得宣称可独立执行。

## 8. 与 Workflow 的关系

- 双向衔接：Workflow 步骤可激活/停用策略（如"进入战斗场景 → Activate Combat
  Policy"）；策略可发出 Workflow Event（`boss_defeated` / `player_dead` /
  `unknown_scene`）驱动 Workflow 分支或 Agent 介入。
- 归属：Temporal Policy 资产与 Runtime 是执行数据平面的扩展，与 Workflow Runtime
  同层治理（版本化、审计、激活状态），不另建平行的第二套治理体系。
- 状态共享：既有 Conversation ↔ Agent ↔ Workflow Run 三方状态共享扩展第四方
  "Policy 激活状态"；Takeover 与取消语义覆盖全部四方（§9）。

## 9. Human Takeover 与安全

- Human Takeover 必须阻止所有策略驱动的新动作进入环境、取消正在执行的连续控制，
  并在恢复前重新观察环境（AGENTS.md 既有要求，对策略路径无例外）。
- 策略输出不是授权；紧急停止与取消路径独立于策略状态，停用策略不需要等待策略
  "同意"。
- 暂停/恢复/取消在策略层的转换有明确定义：暂停 = 停止发动作 + 保留状态；恢复 =
  重新观察后再继续；取消 = 终止并丢弃未决动作。
- 输入注入遵守平台 Adapter 的既有速率、窗口与权限语义；策略不获得任何特权通道。

## 10. Executor 路由与生命周期

全部执行与生命周期归 Executor 管理（AGENTS.md 强制约束），不引入自建线程、定时器
或 fire-and-forget 循环：

| 层 | Executor 路由 | 说明 |
| --- | --- | --- |
| Reactive Controller（30–240 Hz） | realtime lane（`register_realtime_task` / `push_realtime_task` / `RealtimeChannel`） | drop/背压经 `RealtimeExecutorStatus` 与 failure event 显式可见；不静默重试、不无限排队 |
| Temporal Policy（10–60 Hz） | LowLatency / lockfree 后端（显式 `preferred_executor`）或满足确定性要求的周期任务 | 不得用普通周期任务冒充实时控制（AGENTS.md） |
| Agent、规则归纳、策略编译 | 普通 `submit_auto()` 并消费 future | 归纳与编译是有限任务；Deferrable 分类与关闭顺序对齐 `ContextMemorySupervisor` 先例 |

- 取消是协作式的：策略循环必须定期检查 stop/cancellation，观察、输入与模型调用
  具有可解除阻塞路径。
- 关闭顺序：停止策略生产者 → 发出取消 → 回收 realtime/低延迟路径 → 有界等待有限
  任务 → 非 worker 线程执行 `shutdown(true)`。
- 时钟与相位估计位于感知侧；Core 不假设帧率或固定节拍。

## 11. 可观测性与复现

- 新事件族（T1 冻结正式载荷，全部版本化）：`PolicyActivated/Deactivated`、
  `PolicyTransitioned`、`RuleTriggered`、`RuleCandidateInduced`、
  `RuleTestingResulted`、`RulePromoted/Demoted/Conflicted`、
  `PolicyEscalatedToAgent`；大载荷用 ArtifactRef 引用，不在事件中复制。
- 事件与日志脱敏纪律不变：规则文本与历史样本不得包含 API key、密码或输入法敏感
  内容；归纳入口沿用既有标记过滤。
- Replay：区分已记录外部结果与真实副作用；回放不重新执行策略产生的输入动作与
  网络请求（既有纪律延伸到高频路径）。

## 12. 评估设计

- **T1 闭环证明**（issue #50 的"基本闭环"）：冻结的确定性模拟环境/录制时序数据集 +
  确定性供给方，证明"重复事件 → 候选规则 → 下次出现无需 Agent 介入即可正确执行"。
  确定性部分要求跨进程 digest 一致（沿用 M16–M20 口径）。
- 指标分层（RULE-10）：管线行为指标（归纳正确率、误触发率、升级率、升级后恢复率、
  Agent 调用次数随经验下降曲线）可先于真机测量；延迟/抖动/功耗/热等实时性与成本
  指标只能出现在 Stage T6 真机证据之后，未实测不得宣称实时性。
- 对照矩阵（方向级）：Agent-only / +Reactive Rule / +Temporal Cache / +规则归纳，
  比较任务成功率、Agent 调用数、升级次数与资源占用。

## 13. 分阶段落地与门禁（Stage T1–T6）

阶段命名 **T1–T6**（Temporal），与 DEC-032 Stage A–F、DEC-035 Stage W1–W5 并行
不冲突、不占用其编号。每阶段进入实现前新建里程碑文件（不预分配编号），门禁跑前
冻结。

| Stage | 内容 | 门禁 |
| --- | --- | --- |
| T1 | `TemporalHistory` / `TrackedEntity` / Event / `ReactiveRule` 契约 + 条件策略 Runtime 最小闭环（确定性模拟环境，无平台依赖、无感知依赖） | 本设计与 DEC-037 冻结；立项时新建里程碑并冻结契约与门禁 |
| T2 | Temporal Cache 抽象 + 感知侧低成本特征对接（适配器形态；Mirador 为外部候选，经供应链复核与 benchmark 决定） | T1 闭环证据；DEC-011 门禁——`MNT-202609-27` 真机感知证据 |
| T3 | Motion Descriptor / DTW / phase 估计 + 可选 Skeleton/Pose 后端 | T2 基线；感知侧 benchmark 冻结 |
| T4 | Agent Rule Induction：Episode → 候选规则 → 验证晋升（纯 Core，消费 DEC-029 资产） | T1 关闭；learning loop 纪律测试冻结 |
| T5 | HSM / Behavior Tree 后端 + 晋升/降级/冲突自动治理 | T4 关闭 |
| T6 | 真机连续控制验证（延迟、抖动、功耗、热、Takeover） | DEC-011 门禁——`MNT-202609-27` 通道；证据等级按项目管理规范 §10 |

## 14. 风险

- **误触发注入错误输入**：Testing 阶段沙盒化（dry-run/限速/监督并行），晋升以重复
  证据为准，降级路径显式；策略永不豁免授权与安全边界（§7.2）。
- **规则腐化与漂移**：规则是版本化资产，环境变更触发降级与重归纳；冲突不静默覆盖，
  审计事件完整。
- **状态组合爆炸**：统一 Policy 抽象 + 后端选择（§6.1），避免平面 FSM 展开正交状态。
- **实时性过度宣称**：所有实时性/性能声明受 RULE-10 与 Stage T6 门禁约束。
- **感知成本**：事件驱动调度、缓存复用与分层匹配（§5），继承 DEC-033 的性能纪律；
  不把重感知放进高频路径。

## 15. 关联文档

- [DEC-037](../decisions/DEC-037-temporal-policy.md)
- [Agent Harness 与 Workflow 架构设计](agent_harness_and_workflow_architecture.md)
  （双路径模型与 W-03 资产纪律）
- [视觉 Grounding 设计](visual_grounding_design.md)（静态感知管线与缓存调度）
- [Workflow Runtime 设计](workflow_runtime_design.md)（执行数据平面与 RULE 体系）
- [恢复编排设计](workflow_recovery_orchestration_design.md)（learning loop 消费先例）
- [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)、
  [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)、
  [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)、
  [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)、
  [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)、
  [DEC-036](../decisions/DEC-036-consolidation-model-supply.md)
- [评估与基准体系](evaluation_and_benchmark_design.md)
- [Issue #50](https://github.com/Linductor-alkaid/mira/issues/50)
