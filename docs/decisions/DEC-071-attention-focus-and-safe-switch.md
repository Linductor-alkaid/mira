# DEC-071：注意力焦点与安全切换协议

> 状态：Proposed
> 日期：2026-10-28
> 负责人：Mira Maintainers
> 冻结里程碑：M32
> 替代/被替代：无
> 来源需求：Mirage [持续性数字个体路线图](https://github.com/Linductor-alkaid/mirage)
> §4.3（Phase 2 注意力管理与安全任务切换）、§4.9（INV-1～INV-7）、§6.1 R2；
> 该部分**未**进入 [Issue #111](https://github.com/Linductor-alkaid/mira/issues/111)
> （Mirage 反馈计划明确只提报路线图 §4.2、§4.4～4.5、§5.2、§7.1）

## 背景与问题

### 现状核对

Mira 侧**已有**注意力管理的全部原语，只有**协议层缺失**：

| 已有 | 位置 |
| --- | --- |
| Task 状态机含 `Pausing`/`Paused`/`TakeoverSettling`/`SuspendedForTakeover` | [`include/mira/core_contracts.hpp:300-337`](../../include/mira/core_contracts.hpp#L300) |
| `pause_task` / `resume_task`（协作式挂起与恢复） | [`include/mira/runtime.hpp:100-101`](../../include/mira/runtime.hpp#L100) |
| `request_human_takeover` / `release_human_takeover`（挂起 + 环境纪元推进） | [`include/mira/runtime.hpp:115-116`](../../include/mira/runtime.hpp#L115) |
| `ActionLease`（Session 级唯一 active lease，绑定 task + epoch） | [核心契约设计 §9](../design/core_contracts_and_state_machine.md) |
| epoch 递增使在途操作结算为 stale | [`src/runtime/runtime.cpp:710-721`](../../src/runtime/runtime.cpp#L710) |
| `TaskCheckpoint` / `CheckpointBuilder` / `ICheckpointStore` | [`include/mira/task_checkpoint.hpp:81,149,172`](../../include/mira/task_checkpoint.hpp#L81) |
| `RecoveryAction`/`RecoveryOutcome`/`RecoveryPolicy`（按任务规划恢复） | [`include/mira/task_recovery.hpp:16,25,39`](../../include/mira/task_recovery.hpp#L16) |

**完全缺失**：

| # | 缺口 | 证据 |
| --- | --- | --- |
| A1 | **没有任何优先级概念**。`core_contracts.hpp` 中 `Priority` 零命中；`TaskSpec` 只有 `goal` | [`include/mira/runtime.hpp:24-26`](../../include/mira/runtime.hpp#L24) |
| A2 | **没有可中断性（interruptibility）的确定性判定**。现有 `pause_task` 会等待在途操作结算，但**没有**公开谓词让调用方预判「此刻能否抢占」 | 核心契约设计 §6.3 |
| A3 | **没有焦点（focus）概念**，也没有焦点变更事件 | 全仓无 `Focus` 类型 |
| A4 | **没有安全切换协议**。五阶段（Prepare→Suspend→Release→Commit→Resume）、每阶段超时、回滚目标、崩溃后如何识别中断的切换，均未定义 | — |
| A5 | **租约与焦点的耦合关系未冻结**。现有 lease 是 **Session 级唯一**，焦点是**跨 Session 概念**（一个主体可有多会话多任务） | 核心契约设计 §9 |
| A6 | **`TaskCheckpoint` 缺 `held_resources` 与「执行中步骤」语义**；挂起时强制保存检查点的入口不存在 | [`include/mira/task_checkpoint.hpp:81-110`](../../include/mira/task_checkpoint.hpp#L81) |
| A7 | **`resume_task` 只回到 `Observing`**，不支持从暂停点的执行级续跑；恢复会重置观察上下文 | [`include/mira/runtime.hpp:101`](../../include/mira/runtime.hpp#L101) |

### 职责边界（Mirage 侧已划定的分工）

Mirage 路线图把注意力拆成三层：**AttentionPolicy（决策，无状态修改）**、
**AttentionCoordinator（切换协调，单一提交点）**、**MiraPetProjection（单向表现投影）**。
按 issue #111 的边界主张与 AGENTS.md「Core 依赖方向应指向抽象」：

- **Mira 提供**：确定性的协议与事实——任务优先级、可中断性判定、安全点、
  挂起/恢复语义、租约纪律、焦点投影与焦点变更事件。
- **宿主提供**：策略——该关注什么、urgency 打分、稳定性阈值
  （`min_focus_duration`、`urgency_threshold_for_interrupt`、通知合并窗口）、
  桌面输入租约的实际持有、MiraPet 表现。

**Mira 不实现 `AttentionPolicy`/`AttentionCoordinator` 的策略算法**，
但**必须**提供它们所依赖的契约。

### 一个必须先拆开的歧义

「焦点」在生态里有**三个同名不同层**的对象：

| 层 | 焦点对象 | 判定方 | 是否需要 Mira |
| --- | --- | --- | --- |
| L1 **认知焦点** | **任务 / 活动** | 宿主的注意力策略 | **是**（本决策范围） |
| L2 感知焦点 | ROI / 对象引用 / 条件 | 宿主感知层（`desktop.vision.focus`） | 否 |
| L3 表现选择 | MiraPet 当前显示哪个 | 表现层（单向投影） | 否 |

本决策只冻结 **L1**。L2/L3 **不**进入 Mira 契约，避免第二套「焦点」语义。

## 决策

### 1. 焦点是任务/活动级的协调层概念

- **F1（粒度）** 焦点对象是**一个主体名下的一项活动**（由 Task 承载）。
  **不是**设备级、**不是**窗口级、**不是**感知 ROI 级。
- **F2（跨会话）** 一个主体可同时拥有多个 Session 与多个 Task；
  焦点是**协调层**（[DEC-068](DEC-068-interaction-coordinator-and-principal-input-entry.md)）
  的概念，**不是** `MiraRuntime` 的 Session 概念。
  ⇒ `MiraRuntime` 保持「Session 是环境隔离单元」的既有语义不变（核心契约设计 §3.3）。
- **F3（单一提交点）** 任意时刻**最多一个**已提交焦点。
  焦点的**唯一写入点**在协调层；`MiraRuntime` 只提供其依赖的任务状态事实。
- **F4（只读投影）** 焦点以**只读投影 + 变更事件**对外暴露，
  供表现层与审计消费；**读**焦点**不得**改变任何任务状态。

### 2. 优先级：两个独立维度（冻结）

Mirage 的 `GoalPriority{Background, Normal, Urgent}` 把「来源」与「紧迫度」混在一个枚举里，
而「用户任务优先级始终 > 自主任务」是**跨维规则**。Core 侧冻结为两个正交维度：

```cpp
enum class TaskOrigin : std::uint8_t {
    UserInitiated,   // 用户发起（显式输入或经协调层路由后的工作）
    Autonomous,      // 自主发起（见 DEC-072）
};

enum class TaskPriority : std::uint8_t {
    Background,      // 空闲时执行，随时可中断
    Normal,          // 与同级工作竞争
    High,            // 抢占级；抢占仍受可中断性判定与授权约束
};
```

- **PR1（正交）** `origin` 与 `priority` 正交：自主工作可以是 `High`，
  用户工作也可以是 `Background`。
- **PR2（非侵入性规则）** **用户发起的任务的优先级始终不低于自主任务**；
  同优先级时，用户任务先行。自主任务**不得**通过把自己标成 `High` 越过用户任务
  ——`High` 只影响**同级竞争**与**抢占资格**，不改变来源优先级。
  （这条规则的判定点属 DEC-072 的预算与仲裁面。）
- **PR3（High 不等于可抢占）** `High` 只是**抢占候选资格**；
  实际能否抢占由 §3 的可中断性判定与 §4 的协议决定。
- **PR4（既有能力零漂移）** `TaskSpec` 追加 `origin` 与 `priority` 字段，
  **默认值为 `UserInitiated` + `Normal`**，既有调用方行为**逐字节不变**。
  既有 `SchedulingMetricsSnapshot::priority_promoted_count`
  是 **Executor 调度指标**，与本 `TaskPriority` **不是同一概念**，不得混用。

### 3. 可中断性与安全点（补 A2）

Mira 提供**确定性**谓词，让调用方在发起切换**之前**就能预判：

```cpp
enum class Interruptibility : std::uint8_t {
    Interruptible,      // 此刻可挂起，无在途副作用
    WaitForSettlement,  // 有在途 operation，需等待其自然结算
    NotInterruptible,   // 处于不可中断区间，拒绝抢占
};

[[nodiscard]] Result<Interruptibility> interruptibility_of(TaskId task_id) const;
```

- **I1（判定依据是状态机，不是策略）** 判定只依据既有 `TaskState`、
  在途 `OperationKey` 状态与 lease 状态，**不**引入任何策略权重。
- **I2（不可中断区间）** `Acting` 且存在未结算 `OperationKey` 时为
  `WaitForSettlement`；`TakeoverSettling`/`SuspendedForTakeover` 为
  `NotInterruptible`；其余活动态为 `Interruptible`。
  **具体状态映射随 M32 冻结**，且必须是**纯函数**（同一状态 → 同一结论）。
- **I3（安全点）** `WaitForSettlement` 的**安全点**定义为「该 operation 结算为
  Applied 或明确 Failed」的时刻；不得以超时强行跨越。
  超时只产生**可观察的等待超时**，**不**构成强制终止
  （AGENTS.md：超时不是强制终止）。
- **I4（判定不得副作用）** `interruptibility_of` 是**只读**的，
  不改变任务状态、不释放 lease、不推进 epoch。

### 4. 安全切换协议（补 A4）

五阶段与失败处置由 Mira 冻结为**协议**；目标选择与优先级理由由宿主提供。

```text
Prepare → Suspend → Release → Commit → Resume
```

| 阶段 | 前置条件 | 失败处置 |
| --- | --- | --- |
| `Prepare` | 目标存在且非终态；当前焦点可中断性 ∈ {`Interruptible`, `WaitForSettlement`}；调用方给出目标与优先级 | 不可中断且优先级不足 → **拒绝**（不进入后续阶段） |
| `Suspend` | 等待当前焦点到达安全点（I3）；写入挂起检查点（A6） | 等待超时 → 显式失败，**不**强制释放 |
| `Release` | 焦点任务已挂起；桌面输入租约已释放 | 释放失败 → 记诊断并**标记为不一致**，不静默继续 |
| `Commit` | 原子提交新焦点（唯一写入点）；发布焦点变更事件 | 提交失败 → 显式失败并回滚 |
| `Resume` | 目标可恢复；从检查点恢复；重新申请所需租约 | 恢复失败 → **回滚到原焦点**，目标标记不可恢复 |

- **SW1（顺序不可交换）** 切换必须按 `Prepare → Suspend → Release → Commit → Resume`
  顺序推进；**旧焦点先释放，新焦点后提交**。
- **SW2（租约-焦点不变量）** 桌面输入租约持有者 ∈ {已提交焦点任务, `null`}。
  切换过程中：租约在 `Release` 释放，在 `Resume` 重新申请。
  **Mira 不管理桌面输入租约本身**（那是宿主能力），但协调层**必须**暴露
  「当前焦点」与「当前 lease 持有者」的对照视图，使不变量**可验证**。
- **SW3（崩溃可识别）** 切换过程**必须**可从持久化状态识别为「中断的切换」，
  不得被当作普通挂起恢复。恢复时**先重观测/验证**再决定继续或回滚
  （与 [DEC-068 §8](DEC-068-interaction-coordinator-and-principal-input-entry.md) 一致）。
- **SW4（回滚可判定）** 失败时回滚到**明确**的阶段目标；
  回滚本身同样走同一协议（可再失败），但**必须有界**且每次失败都可见。
- **SW5（不做第二套状态机）** 协议**复用** `MiraRuntime` 的既有命令面
  （`pause_task`/`resume_task`/takeover 家族）与 epoch 语义，
  **不**新建任务状态机、**不**新建队列或调度器。

### 5. 检查点对齐（补 A6）

- **CP1（挂起强制保存）** 焦点切换的 `Suspend` 阶段**必须**落一份任务检查点，
  且该检查点须能被崩溃恢复路径取到。
- **CP2（字段对齐）** `TaskCheckpoint` **追加**（不改既有字段语义）：
  - `held_resources`：本任务当前持有的独占资源标识（供恢复后重新申请）。
  - `in_progress_step`：执行中的步骤及其未决状态（与既有
    `verification_pending` 语义正交，不得互相替代）。
- **CP3（回滚与续跑的边界）** **明确声明**：本阶段**不**提供「从暂停点执行级续跑」。
  `resume_task` 仍回到 `Observing`，恢复路径是「重新观察 → 重新规划」
  （既有语义）。若后续需要执行级续跑，**须另立上位决策**——
  它涉及副作用重放边界，不能在本决策顺手带入。
- **CP4（保护）** 检查点属 DEC-069 §4 的**保护集合**成员（活动/挂起任务的最低必要状态）。

### 6. 恢复与终止

- **R1（恢复不自动重放副作用）** 焦点切换后的恢复**不得**重复执行已派发的副作用；
  未决副作用先重观测/验证。
- **R2（不可恢复必须显式）** 恢复失败的目标**必须**标记为不可恢复并**通知用户**，
  **不得**静默丢弃其检查点。
- **R3（终态不复活）** 焦点切换**不得**复活终态任务。

### 7. 与 DEC-068 协调层的关系

- **X1（协调层是焦点的 owner）** 焦点由协调层持有；
  `MiraRuntime` 保持唯一 Session/Task 状态机 owner。
- **X2（焦点不是会话）** 焦点**不**改写 `SessionState`（核心契约设计 §6.2）。
- **X3（输入路由与焦点分离）** 「这条输入属于哪个任务」（DEC-068 的路由裁决）
  与「当前焦点是谁」（本决策）是**两个独立概念**：
  焦点切换**不**改变任务的归属，输入路由**不**隐式抢占焦点。

## 备选方案

- **在 `MiraRuntime` 上加焦点与优先级**：否决。会把跨会话的协调概念塞进
  Session 隔离单元（核心契约设计 §3.3），并让 Session/Task 状态机背上
  跨主体语义。
- **把 `Priority` 与 `origin` 合成一个枚举**：否决。Mirage 的
  `GoalPriority{Background, Normal, Urgent}` 正是这种合成，
  导致「用户任务 > 自主任务」这条规则无处表达（PR2）。两个维度必须正交。
- **用 `pause_task` 的既有行为当作安全切换**：否决。既有 pause 是**协作式挂起**，
  没有「可中断性预判」「安全点」「租约-焦点耦合」「回滚目标」。
- **把 `High` 定义成「可以抢占」**：否决（PR3）。抢占资格还依赖可中断性判定与授权，
  优先级是必要不充分条件。
- **允许超时强制释放在途 operation**：否决（I3）。与 AGENTS.md「超时不是强制终止，
  deadline 也不能被当作抢占保证」冲突。
- **在切换协议里提供「从暂停点执行级续跑」**：否决（CP3）。涉及副作用重放边界，
  须另立上位决策。
- **让 Mira 实现注意力策略（该关注什么）**：否决。策略属宿主；
  Core 只提供确定性事实与协议。

## 影响与风险

- **新增公开契约面**：`TaskOrigin`/`TaskPriority`（`TaskSpec` 追加字段）、
  `Interruptibility` 判定、焦点只读投影与焦点变更事件、切换协议的阶段与失败语义、
  `TaskCheckpoint` 的两个追加字段。
- **既有契约**：`TaskSpec` 与 `TaskCheckpoint` 为**追加式**扩展，既有字段语义不变；
  `MiraRuntime` 的命令面**不新增**抢占专用命令——
  抢占经既有 `pause_task` + 协调层的协议编排实现。
- **风险 S-1（焦点与租约耦合面过宽）**：协调层同时持有焦点与任务，
  可能演变为「第二个 Runtime」。缓解：协调层只持有**焦点指针**与**协议状态**，
  任务状态仍全部在 `MiraRuntime`；焦点切换不得直接改写任务状态字段。
- **风险 S-2（可中断性判定与实际不符）**：缓解：I1 要求纯函数判定，
  判定依据与状态机同源；`M32-G` 门禁逐状态断言一致性。
- **风险 S-3（切换中间态的持久化缺口）**：缓解：SW3 要求可识别「中断的切换」；
  CP1 强制 `Suspend` 落检查点。
- **风险 S-4（优先级与 Executor 调度优先级混淆）**：缓解：PR4 明确两者不是同一概念，
  且禁止混用指标。
- **本决策不涉及 Executor 能力边界**：切换协议是**状态机编排**，
  全部经既有命令面与 Executor 路由；**不登记** Executor 反馈台账。

## 验证方式

由 [M32 计划](../plans/m32-attention-focus-and-safe-switch.md) 承载，
门禁矩阵 `M32-G1`–`M32-G8`：

- `M32-G1` 优先级正交性：`origin` × `priority` 四组合各有可复现用例；
  用户任务优先级始终不低于自主任务。
- `M32-G2` 可中断性判定：逐 `TaskState` 断言判定结果；判定为只读
  （调用前后任务快照与 epoch 不变）。
- `M32-G3` 五阶段顺序：正常切换逐阶段可观察；**旧焦点先释放、新焦点后提交**。
- `M32-G4` 拒绝路径：不可中断且优先级不足 → 明确拒绝，不进入 `Suspend`。
- `M32-G5` 回滚：`Release`/`Commit`/`Resume` 各阶段失败均回滚到原焦点，
  且回滚本身有界、失败可见。
- `M32-G6` 租约-焦点不变量：切换全程「lease 持有者 ∈ {焦点, null}」成立；
  崩溃后从持久化状态可识别「中断的切换」。
- `M32-G7` 检查点：`Suspend` 落检查点；追加字段可往返；
  恢复不重复执行已派发副作用；不可恢复显式标记。
- `M32-G8` 架构门禁与契约四件套同步；无自建线程/定时器。

延迟与切换时延**不在**本阶段门禁内：具体数值须按目标设备实测后冻结
（与 Mirage 路线图 §4.3 的分层时延指标一致），M32 只声明**协议正确性**。

## 关联文档和工作项

- 工作项：[`M32：注意力焦点与安全切换协议`](../plans/m32-attention-focus-and-safe-switch.md)
- 来源：Mirage 路线图 §4.3、§4.9、§6.1 R2（**未**进入 issue #111）
- 相关决策：[DEC-068](DEC-068-interaction-coordinator-and-principal-input-entry.md)（协调层）、
  [DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md)（保护集合）、
  [DEC-070](DEC-070-long-term-memory-behavior-contract.md)（记忆契约）、
  [DEC-072](DEC-072-autonomy-and-interruption-budget.md)（自主行为与打断预算）
- 设计：[核心契约与状态机设计](../design/core_contracts_and_state_machine.md)、
  [Runtime 设计](../design/mira_runtime_design.md)、
  [事件与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
- 术语：[公共术语表](../project/glossary.md)
