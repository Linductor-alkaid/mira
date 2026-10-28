# M32：注意力焦点与安全切换协议

> 状态：Planned（2026-10-28 立项并跑前冻结；实现未开始）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-071](../decisions/DEC-071-attention-focus-and-safe-switch.md)；
> 来源为 Mirage [持续性数字个体路线图](https://github.com/Linductor-alkaid/mirage)
> §4.3/§4.9/§6.1 R2，**未**进入
> [Issue #111](https://github.com/Linductor-alkaid/mira/issues/111)）
> 前置：DEC-071 方向冻结（已满足——随本立项以 DEC-071 冻结）；
> M28 关闭（**未满足**——协调层是焦点与路由的共同 owner）；
> M29 关闭（**未满足**——并发归属与恢复路径）；DEC-002 公开契约版本化纪律（已满足）
> 建议发布点：非发布物；[M33](m33-autonomy-and-interruption-budget.md)
> 自主行为通道的接线前提
> 更新日期：2026-10-28（立项并跑前冻结）

## 1. 目标

交付**注意力与安全切换的确定性协议层**：宿主能判断「此刻能否抢占」、
能安全地把焦点从一项工作切到另一项、能在切换失败时确定地回滚，
而**不必**自己实现任务状态机、租约纪律或崩溃恢复。

具体交付一个可独立验收的能力增量：

1. **优先级正交化**：`TaskOrigin` × `TaskPriority`，用户任务恒不低于自主任务。
2. **可中断性判定**：一个纯函数、只读的确定性谓词，让调用方在发起切换前预判。
3. **五阶段安全切换协议**：`Prepare → Suspend → Release → Commit → Resume`，
   含每阶段前置条件、失败处置与回滚目标。
4. **焦点作为协调层概念**：只读投影 + 变更事件 + 单一提交点；跨会话。
5. **租约-焦点不变量可验证**：暴露「当前焦点」与「当前 lease 持有者」的对照视图。
6. **检查点对齐**：挂起强制落检查点；追加 `held_resources` 与 `in_progress_step`。
7. **崩溃可识别**：中断的切换可从持久化状态识别，恢复时先重观测/验证。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/attention_contracts.hpp`（新文件）与 `src/attention/`（新目录）。
  两者落在 `core` 模块 roots 内，**policy 零差异**。
- `TaskSpec` 追加 `origin` 与 `priority`（追加式，默认 `UserInitiated` + `Normal`，
  既有调用方行为逐字节不变）。
- `Interruptibility` 判定接口与其状态映射。
- 焦点的只读投影、焦点变更事件与协调层侧的切换协议编排。
- `TaskCheckpoint` 追加 `held_resources` 与 `in_progress_step`。
- 切换过程的持久化标记（用于识别「中断的切换」）。

### 2.2 非目标

- **不做**注意力**策略**：该关注什么、urgency 打分、`min_focus_duration`、
  `urgency_threshold_for_interrupt`、通知合并窗口——全部属宿主。
- **不做**桌面输入租约的实现与持有：租约是宿主能力；本轮只暴露对照视图。
- **不做**「从暂停点执行级续跑」：DEC-071 §5/CP3 明确排除，须另立上位决策。
- **不做**MiraPet / 表现层投影：单向表现属宿主。
- **不做**感知层「焦点」（ROI / 对象引用）：与 L1 认知焦点**不是**同一契约。
- **不做**自主行为：归 [M33](m33-autonomy-and-interruption-budget.md)。
- **不改** `SessionState` 与既有 `TaskState` 取值；焦点是协调层概念。
- **不新建**任务状态机、队列或调度器。

## 3. 设计与决策依据

- [DEC-071](../decisions/DEC-071-attention-focus-and-safe-switch.md)——本里程碑的
  **主决策**：焦点粒度、优先级正交化、可中断性判定、五阶段协议、
  租约-焦点不变量、检查点对齐、恢复与终止。
- [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)——
  协调层是焦点与路由的共同 owner；焦点**不**改写 `SessionState`。
- [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)——
  检查点属保护集合成员；本轮**不**实现容量准入。
- [核心契约与状态机设计](../design/core_contracts_and_state_machine.md)
  §6.3（Task 状态机与终态幂等）、§9（ActionLease）、§8（取消与超时）。
- [Runtime 设计](../design/mira_runtime_design.md) §3.4（单写者状态机）、
  §11（Executor 集成设计）、§11.5（Shutdown 顺序）。
- [事件与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
  §6（副作用提交协议）、§7（崩溃窗口矩阵）、§8（Recovery）。

## 4. 冻结的契约语义（跑前冻结，2026-10-28）

### 4.1 公开类型

```cpp
// include/mira/attention_contracts.hpp
enum class TaskOrigin : std::uint8_t { UserInitiated, Autonomous };
enum class TaskPriority : std::uint8_t { Background, Normal, High };

enum class Interruptibility : std::uint8_t {
    Interruptible,      // 此刻可挂起，无在途副作用
    WaitForSettlement,  // 有在途 operation，需等待其自然结算
    NotInterruptible,   // 处于不可中断区间，拒绝抢占
};

enum class SwitchPhase : std::uint8_t {
    Idle, Preparing, Suspending, Releasing, Committing, Resuming
};

struct FocusView final {
    std::optional<TaskId> task_id;
    std::optional<SessionId> session_id;
    TaskOrigin origin = TaskOrigin::UserInitiated;
    TaskPriority priority = TaskPriority::Normal;
};
```

`TaskSpec` 追加（**追加式**，既有字段语义不变）：

```cpp
struct TaskSpec final {
    std::string goal;
    TaskOrigin origin = TaskOrigin::UserInitiated;
    TaskPriority priority = TaskPriority::Normal;
};
```

### 4.2 状态 → 可中断性映射（冻结）

| `TaskState` | `Interruptibility` | 理由 |
| --- | --- | --- |
| `Idle` / `Observing` / `Reasoning` / `Planning` / `Verifying` / `Recovering` | `Interruptible` | 无在途副作用 |
| `Acting`（无未结算 `OperationKey`） | `Interruptible` | 无在途副作用 |
| `Acting`（有未结算 `OperationKey`） | `WaitForSettlement` | 需等待 operation 结算 |
| `Pausing` | `WaitForSettlement` | 正在收敛 |
| `Paused` | `Interruptible` | 已挂起，无副作用 |
| `TakeoverSettling` / `SuspendedForTakeover` | `NotInterruptible` | 接管收敛中，不得叠加切换 |
| `Cancelling` | `NotInterruptible` | 取消进行中 |
| 终态（`Completed`/`Failed`/`Cancelled`） | `NotInterruptible` | 终态不可抢占（且不得复活） |

判定是**纯函数**：同一 `(TaskState, 在途 operation 数)` → 同一结论；
**只读**，不改变任务状态、不释放 lease、不推进 epoch。

### 4.3 五阶段协议与失败处置

| 阶段 | 前置条件 | 失败处置 |
| --- | --- | --- |
| `Prepare` | 目标存在且非终态；当前焦点可中断性 ∈ {`Interruptible`, `WaitForSettlement`}；调用方给出目标与优先级理由 | 不可中断且优先级不足 → **拒绝**，不进入后续阶段 |
| `Suspend` | 等待当前焦点到达安全点（operation 结算为 Applied 或明确 Failed）；写入挂起检查点 | 等待超时 → 显式失败，**不**强制释放在途 operation |
| `Release` | 焦点任务已挂起；桌面输入 lease 已释放 | 释放失败 → 记诊断并标记「租约-焦点不一致」，**不**静默继续 |
| `Commit` | 原子提交新焦点（唯一写入点）；发布焦点变更事件 | 提交失败 → 显式失败并回滚 |
| `Resume` | 目标可恢复；从检查点恢复；重新申请所需 lease | 恢复失败 → **回滚到原焦点**，目标标记不可恢复 |

- **顺序不可交换**：旧焦点先释放，新焦点后提交。
- **回滚有界**：回滚本身走同一协议，可再失败，但**必须有界**且每次失败可见。
- **崩溃可识别**：切换过程在持久化状态上留下「中断的切换」标记；
  恢复时**先重观测/验证**再决定继续或回滚。

### 4.4 检查点追加字段

```cpp
struct TaskCheckpoint final {
    // ...既有字段...
    // 本任务当前持有的独占资源标识，供恢复后重新申请。
    std::vector<std::string> held_resources;
    // 执行中的步骤及其未决状态；与既有 verification_pending 正交。
    std::optional<InProgressStep> in_progress_step;
};
```

- `Suspend` 阶段**必须**落一份检查点，且崩溃恢复路径能按 `task_id` 取到最近一份。
- 检查点属 DEC-069 §4 保护集合成员。

### 4.5 事件面

新增事件族（**闭集**，自带版本标记）：

| 事件类型 | schema 名 | 载荷要点 |
| --- | --- | --- |
| `FocusSwitchRequested` | `mira.attention.focus-switch-requested.v1` | `from_task_id`、`to_task_id`、优先级、`rationale` |
| `FocusSwitchPhaseChanged` | `mira.attention.focus-switch-phase.v1` | `phase`、`from_task_id`、`to_task_id`、耗时 |
| `FocusSwitchRejected` | `mira.attention.focus-switch-rejected.v1` | `from_task_id`、`to_task_id`、`interruptibility`、拒绝理由 |
| `FocusSwitchRolledBack` | `mira.attention.focus-switch-rolled-back.v1` | `to_task_id`、`failed_phase`、`restored_task_id` |
| `FocusCommitted` | `mira.attention.focus-committed.v1` | `task_id`、`session_id`、`origin`、`priority` |

- 版本标记写在 schema 名内；**不得**依赖 `EventEnvelope::schema_version`（死字段）。
- `rationale` 与拒绝理由**不得**含凭据或敏感正文。

### 4.6 Executor 路由与生命周期（Q7）

| 类别工作 | Executor 路径 | 完成语义 |
| --- | --- | --- |
| 可中断性判定、焦点投影读取 | 同步只读（不提交 Executor） | 直接返回 |
| 切换协议编排 | Runtime 串行控制面 + 既有命令面 | `CommandReceipt`/`CommandOutcome` |
| 检查点写入 | 既有 EventStore/CheckpointStore 通道 | durable ack |
| 焦点变更事件广播 | 既有事件分发通道 | 事件落盘 |

- **不新建 Executor**；复用 Runtime 的 Executor 与 `ExecutionSupervisor`。
- **不新建定时器**：本轮无周期任务。
- 关闭顺序沿 [Runtime 设计 §11.5](../design/mira_runtime_design.md)。
- **超时不是强制终止**：等待安全点超时只产生可观察失败。

### 4.7 决策记录表

| 决策点 | 选择 | 被否备选 | 依据 |
| --- | --- | --- | --- |
| 焦点粒度 | 任务/活动级，跨会话，协调层持有 | 设备级 / 窗口级 / 感知 ROI 级 | DEC-071 §1 |
| 优先级模型 | `origin` × `priority` 两维正交 | 单枚举（含 `Urgent`） | DEC-071 §2 |
| 非侵入性规则 | 用户任务优先级恒不低于自主任务；`High` 只影响同级竞争与抢占资格 | 让 `High` 越过用户任务 | DEC-071 §2/PR2-PR3 |
| 可中断性 | Core 提供纯函数只读判定 | 宿主自行判断 / 用超时强行抢占 | DEC-071 §3 |
| 切换协议 | 五阶段 + 失败回滚 + 崩溃可识别 | 复用既有 `pause_task` 语义 | DEC-071 §4 |
| 租约 | 宿主持有；Core 暴露对照视图使不变量可验证 | Core 管理桌面租约 | DEC-071 §4/SW2 |
| 续跑 | **不支持**执行级续跑；恢复=重新观察→重新规划 | 支持暂停点续跑 | DEC-071 §5/CP3 |
| 检查点 | 追加 `held_resources` + `in_progress_step`；挂起强制落盘 | 不对齐 / 不强制落盘 | DEC-071 §5 |
| 焦点与路由 | 两个独立概念，互不隐式 | 路由即抢占 | DEC-071 §7/X3 |
| 模块归属 | `core` roots 内，不新增架构模块 | 新增 `attention` 架构模块 | DEC-071 影响与风险 |

## 5. 阶段冻结协议必答八问（2026-10-28）

| # | 问题 | 回答 |
| --- | --- | --- |
| 1 | **行为**：本阶段改变或新增哪些行为？对应设计文档哪些冻结小节？ | 新增优先级、可中断性判定、五阶段安全切换、焦点投影与事件、检查点对齐、崩溃可识别。锚点：DEC-071 §1–§7；[核心契约设计 §6.3、§9](../design/core_contracts_and_state_machine.md)；[Runtime 设计 §11](../design/mira_runtime_design.md) |
| 2 | **所有者**：新增可变状态的唯一所有者是谁？谁只读投影？ | 唯一 writer：协调层（焦点指针 + 切换协议状态）。只读投影：`FocusView`、焦点变更事件、租约-焦点对照视图。`MiraRuntime` 仍是 Session/Task 状态唯一 owner；**协调层不得直接改写任务状态字段** |
| 3 | **契约**：最小公共契约面是什么？哪些调用方允许使用？ | `include/mira/attention_contracts.hpp`：`TaskOrigin`/`TaskPriority`/`Interruptibility`/`SwitchPhase`/`FocusView` + `interruptibility_of` + 切换请求入口 + `TaskSpec`/`TaskCheckpoint` 追加字段。允许使用者：宿主（策略实现方） |
| 4 | **层与依赖**：新文件属于哪个模块？include 方向是否已登记？ | `include/mira/attention_contracts.hpp` 与 `src/attention/` 落在 `core` roots 内 → **policy 零差异**；只依赖 `core` + `kairo` 公开头，**不**引用 `src/` 私有头 |
| 5 | **复用**：哪条现有路径已承担部分职责？为何不新增平行路径？ | 复用：Task 状态机与终态幂等、`pause_task`/`resume_task`/takeover 家族、`ActionLease`、epoch 作废语义、`TaskCheckpoint`/`ICheckpointStore`/`RecoveryPlanner`、既有命令三阶段协议。**不新增**：第二套任务状态机、抢占专用命令、桌面租约实现、定时器 |
| 6 | **时序**：事件顺序、幂等键、过期结果规则、取消与 deadline 边界是什么？ | 事件顺序：`FocusSwitchRequested` → `FocusSwitchPhaseChanged`* → （成功）`FocusCommitted` / （失败）`FocusSwitchRejected` 或 `FocusSwitchRolledBack`。过期规则：切换请求不幂等（每次发起是一次新尝试），但**同一目标已在焦点时不重复切换**（NoOp）。取消边界：切换中的取消走既有 Task 取消语义；等待安全点超时只产生可观察失败，**不**强制终止 |
| 7 | **Executor 路由**：每类工作由 Executor 哪种能力承载？句柄谁持有？关闭顺序如何？ | 见 §4.6。句柄由协调层持有并消费；`shutdown(true)` 由非 worker 线程执行 |
| 8 | **验证上下文**：评审者最小需要读哪些契约、spec 与测试才能验收？ | 契约：DEC-071 §1–§7、本文件 §4；设计：核心契约 §6.3/§9、Runtime §11；测试：`tests/m32/`（`M32-G1`–`M32-G8`），label `integration;m32` |

## 6. 测试与退出条件

### 6.1 门禁矩阵

| 门禁 | 内容 | 测试文件（`tests/m32/`） |
| --- | --- | --- |
| `M32-G1` | 优先级正交性：`origin` × `priority` 四组合；用户任务优先级恒不低于自主任务 | `m32_priority_test.cpp` |
| `M32-G2` | 可中断性判定：逐 `TaskState` 断言；判定为只读（调用前后快照与 epoch 不变） | `m32_interruptibility_test.cpp` |
| `M32-G3` | 五阶段顺序：正常切换逐阶段可观察；旧焦点先释放、新焦点后提交 | `m32_switch_protocol_test.cpp` |
| `M32-G4` | 拒绝路径：不可中断且优先级不足 → 明确拒绝，不进入 `Suspend` | `m32_switch_protocol_test.cpp` |
| `M32-G5` | 回滚：`Release`/`Commit`/`Resume` 各阶段失败均回滚到原焦点；回滚有界且失败可见 | `m32_switch_protocol_test.cpp` |
| `M32-G6` | 租约-焦点不变量：切换全程成立；崩溃后可识别「中断的切换」 | `m32_focus_lease_test.cpp` |
| `M32-G7` | 检查点：`Suspend` 落盘；追加字段可往返；恢复不重复执行已派发副作用；不可恢复显式标记 | `m32_checkpoint_alignment_test.cpp` |
| `M32-G8` | 架构门禁与契约四件套同步；无自建线程/定时器 | CI quality 管线 |

### 6.2 退出条件

- [ ] `M32-01`–`M32-08` 全部工作项完成并勾选。
- [ ] `M32-G1`–`M32-G8` 全部通过，测试文件与 label 就位。
- [ ] CI 24/24 全绿（linux gcc/clang × Debug/Release、windows Debug/Release、
      android arm64/x86_64 NDK 交叉、sanitizers ASAN/TSAN/UBSAN、quality）。
- [ ] 公开 API、事件 schema、错误语义与示例已同步更新。
- [ ] 受影响的设计、决策、计划、术语表、API 手册已同步更新。
- [ ] 验证记录（§9）含 commit、环境、命令、结果与限制。

### 6.3 未声明的量

**切换时延与抖动指标不在本阶段门禁内**：具体数值须按目标设备实测后冻结。
本阶段只声明**协议正确性**（阶段顺序、拒绝、回滚、不变量、崩溃可识别）。

## 7. 工作项

- [ ] `M32-01` 立项与跑前冻结：DEC-071 冻结面、本文件 §4 契约语义、
      §5 必答八问、§4.7 决策记录表、§6 门禁矩阵。
- [ ] `M32-02` 公开契约面：`include/mira/attention_contracts.hpp`
      （`TaskOrigin`/`TaskPriority`/`Interruptibility`/`SwitchPhase`/`FocusView`）、
      `TaskSpec` 追加字段（默认零漂移）、`mira.attention` 错误域与 wire schema 键集冻结。
- [ ] `M32-03` 可中断性判定：纯函数实现、状态映射、只读保证、
      在途 operation 结算状态的读取路径。
- [ ] `M32-04` 五阶段切换协议：协调层编排、阶段前置条件与失败处置、
      拒绝路径、回滚目标与有界回滚。
- [ ] `M32-05` 焦点与租约：焦点只读投影、唯一提交点、焦点变更事件、
      租约-焦点对照视图。
- [ ] `M32-06` 检查点对齐：`TaskCheckpoint` 追加 `held_resources` 与 `in_progress_step`；
      `Suspend` 强制落盘；崩溃恢复可取到最近检查点；
      「中断的切换」持久化标记。
- [ ] `M32-07` IVA 测试矩阵：`tests/m32/` 五组用例（§6.1），label `integration;m32`。
- [ ] `M32-08` 文档同步：API 手册新页、术语表、总计划索引、事件面说明、
      DEC-071 状态回填。

## 8. 风险与阻塞

| 风险 | 说明 | 缓解 / 归属 |
| --- | --- | --- |
| `M32-R1` 焦点与租约耦合面过宽 | 协调层可能演变为「第二个 Runtime」 | 协调层只持有焦点指针与协议状态；任务状态全在 `MiraRuntime`；切换不得直接改写任务状态字段。M32-04/M32-05 |
| `M32-R2` 判定与实际不符 | 可中断性判定与真实在途状态脱节 | 判定与状态机同源且为纯函数；`M32-G2` 逐状态断言。M32-03 |
| `M32-R3` 切换中间态持久化缺口 | 崩溃后无法识别中断的切换 | `M32-06` 强制落持久化标记；`M32-G6` 断言可识别。M32-06 |
| `M32-R4` 优先级与 Executor 指标混淆 | `priority_promoted_count` 与 `TaskPriority` 不是同一概念 | DEC-071 §2/PR4 已明确禁止混用；文档同步时标注。M32-08 |
| `M32-R5` 执行级续跑被顺手引入 | 会带来副作用重放边界问题 | CP3 明确**不支持**；须另立上位决策。M32-02 |

**外部阻塞**：无。本阶段为纯 Core 确定性工作，不依赖真实模型、真机或宿主凭据。
切换时延数值待目标设备实测（与 Mirage 路线图 §4.3 的分层时延指标对应），
**不被本立项解锁**。

## 9. 验证记录

> 尚无记录。实现开始后按时间追加：日期、commit、环境、命令、结果、限制与剩余项。
> 未执行项必须保持未勾选并记录原因、负责人与补跑条件。
