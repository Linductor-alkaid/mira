# M28：交互协调层首阶段——主体级输入接纳与最小路由闭环

> 状态：Planned（2026-10-28 立项并跑前冻结；实现未开始）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)
> 首阶段；来源需求为
> [Issue #111](https://github.com/Linductor-alkaid/mira/issues/111)）
> 前置：DEC-068 方向冻结（已满足——随本立项以 DEC-068 冻结）；
> M25 供给缝接线形态（已满足，[DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)）；
> DEC-002 公开契约版本化纪律（已满足）；kairo v0.7.0 能力面复核（已满足——
> [DEC-068 §1](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)
> 与 [DEC-069 §9](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md) 引用了
> 2026-10-28 的 kairo 公开头文件/API 文档/集成指南核查结论）
> 建议发布点：非发布物；DEC-068 协调层公开契约与 M29/M30/M31 的接线前提
> 更新日期：2026-10-28（立项并跑前冻结）

## 1. 目标

交付 **Mira 侧统一交互的第一段闭环**：宿主把一次用户输入交给 Mira 的**同一个入口**，
Mira 自己决定它属于哪个内部会话/任务、准备该工作的上下文、并把结果关联回这条输入——
调用方**不**需要预先知道 SessionId，也不需要自己决定「新建还是延续」。

具体交付一个可独立验收的能力增量：

1. **主体级输入入口**：`InputEnvelope` → `InputDisposition`，四阶段可观察。
2. **耐久幂等台账**：同一 `input_id` 在窗口内不产生第二次派发；窗口外明确过期拒绝。
3. **确定性路由裁决**：八种 `RouteIntent`，含「指代不清 → 自然语言澄清」。
4. **结果关联**：回复、追问、后台结果、失败全部可追溯到 `input_id` + `task_id`。
5. **Loop 输入缝**：`ConversationLoop` 获得与 `AgentLoop::enqueue_user_message` 对称的
   有界入队通道，使 continuous 模式可被统一入口驱动。
6. **上下文归属**：插话纠正只更新目标任务，不相关输入不污染其他任务的约束。

**本阶段不宣称**：路由准确率、必要澄清率、真实模型语义质量、跨重启后的完整恢复。
路由准确率与澄清率留待标注场景 + 真实模型评估轮（issue #111 的验证方法要求）；
完整恢复流程归 [M29](m29-interaction-coordinator-concurrency-and-recovery.md)。

## 2. 范围与非目标

### 2.1 范围

- **新公开契约面**：`include/mira/interaction_coordinator.hpp`（新文件）、
  `src/interaction/`（新目录）。二者落在 `core` 模块 roots（`include/mira`、`src`，
  [`tools/architecture-policy.json`](../../tools/architecture-policy.json)）内，
  **无 policy 差异**。
- **既有契约加法扩展**（不改既有字段语义、不删既有方法）：
  - `TaskSpec` 增加用户约束字段（`include/mira/runtime.hpp:24-26`）。
  - `ConversationLoop` 增加对称的有界入队通道
    （`include/mira/conversation_loop.hpp:200-297`）。
  - `AgentLoop` **零改动**。
- **新持久化组件**：`IInputLedger` 窄接口 + 其耐久实现（可由 EventStore 承载）、
  主体注册表投影（`(TenantId, UserId)` → sessions/tasks）。
- **事件面**：新增 `mira.interaction.*.v1` 事件族，自带版本标记。
- **测试矩阵**：`tests/m28/`（契约 + 生命周期 + 幂等 + 路由 + 恢复前置）。
- **文档同步**：API 手册新页、术语表、总计划索引、README、事件面说明。

### 2.2 非目标

- **不做** UI / IPC / 身份认证 / 桌面资源租约 / 环境观察与动作执行
  —— 全部属 Mirage 与 Platform Adapter。
- **不做** 宿主侧会话路由；宿主不得自建平行编排系统（issue #111 的边界主张）。
- **不做** 模型介导路由的自动执行：模型只能产出 `suggested_intent`，
  裁决由确定性函数完成（DEC-068 §5/R1）。
- **不做** 并行工作与指代消解的完整语义（归 M29）。
- **不做** 长期记忆的写入/检索/纠正/删除（归 M30）。
- **不做** 容量准入、归档与遗忘（归 M31）。
- **不做** 完整重启恢复流程（归 M29）；本阶段只保证**台账与注册表耐久**，
  恢复执行路径在 M29 交付。
- **不改** `AgentLoopSpec` 的任务帧三元组（DEC-068 备选方案已记录理由）。
- **不改** `SessionState`；会话退出生命周期归 M31。

## 3. 设计与决策依据

- [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)——
  本阶段的**主决策**：协调层边界、主体身份复用、输入信封、幂等台账、路由裁决、
  结果关联、恢复语义。
- [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)——
  台账与注册表**必须计入**容量预算（M31 落地），本阶段只负责不引入新的无界层。
- [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)——
  供给缝「薄缝 + setter 注入 + 失败降级」先例，本阶段的 Loop 输入缝沿用同一形态。
- [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)——
  协调层位于 Agent Harness 控制平面**之上**，不新建第三执行面。
- [DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)——
  主体身份（`PrincipalContext`）、记忆 ACL 域（`MemoryScope.subject_id`）、
  感知局部标识（`TrackedEntity.entity_key`）三者**不可混用**。
- [DEC-058](../decisions/DEC-058-mutation-ledger-scope-binding.md)——
  既有 `memory_mutations` 台账与本阶段的输入台账**不是同一空间**。
- [DEC-062](../decisions/DEC-062-memory-retention-sweep.md)——
  窄接口（存储所有者级能力）定位先例。
- [DEC-002](../decisions/DEC-002-public-contract-versioning.md)——
  公开契约版本化纪律；本阶段**不**改既有公开契约的 major 版本。

设计锚点：
[核心契约与状态机设计](../design/core_contracts_and_state_machine.md)（§5 命令三阶段协议、
§5.2 幂等与去重、§6.2 Session、§6.3 Task、§9 ActionLease、§10 Shutdown）、
[Runtime 设计](../design/mira_runtime_design.md)（§6 所有权模型、§11 Executor 集成设计、
§11.3 背压、§11.5 Shutdown 顺序）、
[Context 与 Memory 设计](../design/context_and_memory_design.md)（§6.1 MemoryScope、
§9 Context 预算模型、§10 Context 构建算法）、
[事件与崩溃一致性设计](../design/event_artifact_crash_consistency.md)（§4 EventStore、
§7 崩溃窗口矩阵、§8 Recovery）。

## 4. 冻结的契约语义（跑前冻结，2026-10-28）

### 4.1 公开类型（`include/mira/interaction_coordinator.hpp`）

```cpp
namespace mira {

/// 一次主体输入的接纳载荷。
struct InputEnvelope final {
    PrincipalContext principal;             // 主体身份（宿主提供，已认证）
    std::string input_id;                   // 宿主提供的稳定输入标识 = 幂等键
    std::string content;                    // 输入正文
    std::vector<std::string> attachment_refs; // 工件引用（不复制内容）
    std::optional<std::string> reply_to;     // 关联另一次输入（如追问链）
};

/// 输入接纳后的阶段。收到输入 != 任务完成。
enum class InputStage : std::uint8_t {
    Accepted, Routed, Answered, TaskSettled, Rejected
};

/// 路由裁决的闭集意图。
enum class RouteIntent : std::uint8_t {
    ContinueCurrent, SupplementTask, NewTask, ResumeTask,
    PauseTask, CancelTask, Clarify, Reject,
};

struct RoutingDecision final {
    RouteIntent intent = RouteIntent::Reject;
    std::optional<SessionId> session_id;
    std::optional<TaskId> task_id;
    std::string rationale;                  // 可审计的裁决理由
    std::optional<std::string> clarification;
};

struct InputDisposition final {
    ControlIngressId ingress_id;
    std::string input_id;
    InputStage stage = InputStage::Accepted;
    bool duplicate = false;
    std::optional<SessionId> session_id;
    std::optional<TaskId> task_id;
    std::optional<Error> error;
    std::optional<std::string> clarification;
};
```

> **实现注记（冻结）**：`InputStage` 与 `RouteIntent` 是**闭集**，
> 枚举成员必须**互不重复**且与下表**逐一对应**；新增成员需要新的决策记录，
> **不得**在实现期顺手扩展：

| `InputStage` | 语义 | 进入条件 |
| --- | --- | --- |
| `Accepted` | 已接纳并登记 | 台账原子登记成功 |
| `Routed` | 已归属到具体 Session/Task | 路由裁决非 `Reject` 且已提交 |
| `Answered` | 已产出面向用户的回复 | 回复已发出并可追溯 |
| `TaskSettled` | 关联任务进入终态 | 任务终态事件已落 |
| `Rejected` | 明确拒绝 | 过期 / 重复 / 越权 / 预算 / 关闭中 |

| `RouteIntent` | 语义 | 目标 |
| --- | --- | --- |
| `ContinueCurrent` | 延续当前工作 | 已有活动 Task |
| `SupplementTask` | 补充 / 纠正某项工作 | 已有活动或挂起 Task |
| `NewTask` | 新建任务 | 新 Session + 新 Task |
| `ResumeTask` | 恢复旧任务（关联旧摘要的新任务） | 已有终态 Task 的旧摘要 |
| `PauseTask` | 暂停 | 已有活动 Task |
| `CancelTask` | 取消 | 已有非终态 Task |
| `Clarify` | 指代不清，需自然语言澄清 | 多个候选都合理 |
| `Reject` | 越权 / 预算 / 过期 / 关闭中 | 无有效目标 |

### 4.2 台账契约（`IInputLedger`）

```cpp
class IInputLedger {
  public:
    virtual ~IInputLedger() = default;
    virtual Result<LedgerEntry> admit(const InputEnvelope &envelope) = 0;
    virtual Result<LedgerEntry> lookup(std::string_view input_id) const = 0;
};
```

- 台账是**存储所有者级**窄接口，不并入 `IMemory`（DEC-068 §4）。
- `admit` 必须**原子**：窗口内的重复 `input_id` 返回既有条目并置 `duplicate=true`，
  **不产生第二次副作用**。
- 窗口外的重复 `input_id` 返回**明确过期拒绝**（稳定错误码），
  **不得**当作新输入执行，**不得**「清掉台账后当成新动作」。
- 去重键是 `input_id`，**不是内容哈希**：内容相同但 `input_id` 不同的输入
  **不**判为重试。
- `LedgerEntry` 至少记录：`input_id`、`ControlIngressId`、接纳时间、当前 `InputStage`、
  归属的 `session_id`/`task_id`、窗口到期时间。
- 台账**必须**有回收窗口，且其字节**计入** DEC-069 的总量预算；
  本阶段只保证「不引入无界表」，预算数值归 M31。

### 4.3 协调器骨架

```cpp
class InteractionCoordinator final {
  public:
    InteractionCoordinator(CoordinatorConfig config,
                           MiraRuntime &runtime,
                           kairo::Executor &executor);
    Result<void> initialize();
    Result<InputDisposition> submit_input(const InputEnvelope &envelope);
    // 紧急停止：确定性路径，不经模型路由。
    Result<CommandHandle> cancel_work(TaskId task_id);
};
```

- 构造参数**显式**携带 `MiraRuntime&` 与 `kairo::Executor&`
  （`MiraRuntime` 不暴露 Executor 引用，沿用 `WorkflowRuntime` 的注入做法）。
- 协调层**不实现** Session/Task 状态机；所有状态变化经 `MiraRuntime` 既有命令面。
- 协调层**不创建也不运行** Loop；通过 `IUserInputSink` 注册面投递。

### 4.4 Loop 输入缝（`IUserInputSink`）

```cpp
class IUserInputSink {
  public:
    virtual ~IUserInputSink() = default;
    virtual Result<void> deliver(const InputEnvelope &envelope, TaskId task_id) = 0;
};
```

- `AgentLoop` 与 `ConversationLoop` 各提供一个适配实现。
- `ConversationLoop` 必须新增**与 `AgentLoop::enqueue_user_message` 对称**的有界入队通道：
  同样的有界队列、同样的 `ResourceExhausted` 溢出语义、同样的线程安全纪律。
  **不允许**协调层只支持 `AgentLoop` 就宣称交付统一输入口。
- 幂等由台账保证，因此 `deliver` **不需要**携带 input id 的去重语义；
  但适配实现**必须**记录最后投递的 input id 以便事件面关联。
- 投递失败**必须**回传到协调层并转为可见结果，**不得**静默丢弃。

### 4.5 上下文归属与用户约束承载

- `TaskSpec` 追加用户约束字段（追加式），承载「最新用户约束」；
  **不改** `goal` 的既有语义。
- 协调层为每个被路由到的工作项组织上下文：目标、**最新**用户约束、任务状态、
  未决副作用、近期热上下文、已提交 Working Context、需要召回的历史与长期记忆。
- 插话纠正只更新**目标任务**的后续上下文；不相关话题**不**污染其他任务的状态或约束。
- 全局偏好（`MemoryScopeKind::User`）、当前指令与任务局部要求的生效范围**显式区分**，
  **不得**以「拼接所有会话历史」处理。
- Layer 0 仍是**请求装配期**的唯一准入转换
  （[Context 与 Memory 设计 §9](../design/context_and_memory_design.md)）；
  协调层只选择**哪一份已提交快照**，**不**新增第二套准入路径。
  既有供给缝（DEC-045）是 Loop 侧消费该选择的唯一通道。

### 4.6 事件面（`mira.interaction.*.v1`）

新增事件族（**闭集**，每个自带版本标记）：

| 事件类型 | schema 名 | 载荷要点 |
| --- | --- | --- |
| `InputAccepted` | `mira.interaction.input-accepted.v1` | `input_id`、`ingress_id`、`principal_ref`、接纳时间 |
| `InputRouted` | `mira.interaction.input-routed.v1` | `input_id`、`ingress_id`、`intent`、`session_id`、`task_id`、`rationale` |
| `InputClarified` | `mira.interaction.input-clarified.v1` | `input_id`、`ingress_id`、`clarification` |
| `InputRejected` | `mira.interaction.input-rejected.v1` | `input_id`、`ingress_id`、错误码、`reason` |
| `InputAnswered` | `mira.interaction.input-answered.v1` | `input_id`、`ingress_id`、`session_id`、`task_id`、回复摘要引用 |
| `InputTaskSettled` | `mira.interaction.input-task-settled.v1` | `input_id`、`ingress_id`、`task_id`、终态 |

冻结约束：

- **不得**依赖 `EventEnvelope::schema_version`——它是死字段（恒 `{1,0}`、
  不入 canonical 摘要、不落盘）。版本标记写在 schema 名与 payload 内。
- `input_id` 必须落在 `EventPayload::data` 内。
- 新增事件类型名时**必须同步**硬编码事件名的闭集：
  [`src/model/conversation_log.cpp:91-97`](../../src/model/conversation_log.cpp#L91-L97)、
  [`src/storage/checkpoint_store.cpp:554,628`](../../src/storage/checkpoint_store.cpp#L554)。
- `principal_ref` 只放**不可逆的内部标识**（如 `TenantId`/`UserId` 的 hex），
  **不得**放外部账号信息或凭据（[Context 与 Memory 设计 §6.1](../design/context_and_memory_design.md)、
  AGENTS.md 日志脱敏要求）。

### 4.7 权限校验（fail-closed）

- 路由裁决**必须**经主体能力校验后才提交。
- 能力缺失、grant 过期或 `identity_version` 不匹配时一律 `Reject`，
  **不得**默认放行。
- 首阶段的校验**严格限定**在协调层**自己创建**的会话与任务上；
  既有显式 API 路径（`open_session`/`submit_task`）**不受影响**。
- `PolicyEngine` 本身的生产接线（策略集、确认质询、版本化）是**独立决策**，
  本阶段只冻结「校验点存在且 fail-closed」。

### 4.8 Executor 路由与生命周期（Q7，显式冻结）

| 类别工作 | Executor 路径 | 完成语义 |
| --- | --- | --- |
| 输入接纳、路由裁决、派发 | Runtime 串行控制面（既有命令面） | `CommandReceipt`/`CommandOutcome` |
| 台账读写 | 既有 EventStore 通道（单写者） | `AppendReceipt` |
| 上下文准备（选快照、渲染） | `submit_auto()` 普通有限任务 | future 的值或异常 |
| Loop 投递 | 同步 sink 调用（有界队列） | `Result<void>` |
| 后台结果关联 | 既有 completion 消息推进 | 事件落盘 |

- 协调层**不新建 Executor**；复用 Runtime 的 Executor 与 `ExecutionSupervisor`
  （[Runtime 设计 §11.2](../design/mira_runtime_design.md)），
  **不得**直接调用 Executor singleton。
- 所有 future **必须保留并消费**；任务异常**不得**被丢弃。
- 关闭顺序沿 [Runtime 设计 §11.5](../design/mira_runtime_design.md)：
  停止生产者 → 发出取消 → 回收 worker → 等待有限任务 → 非 worker 线程
  `shutdown(true)`。
- **背压可见**：接纳队列满时返回明确错误（`ResourceExhausted`）并计入
  `SchedulingMetricsSnapshot`；**不得**静默丢输入。
- **本阶段不引入周期清扫**（M31 承载），因此不涉及
  `submit_periodic` 的「tick 不受 admission 约束 / tick 拒绝即丢」等限制。

### 4.9 决策记录表

| 决策点 | 选择 | 被否备选 | 依据 |
| --- | --- | --- | --- |
| 协调层位置 | Runtime 之上、宿主之下 | 新建平行 `PrincipalRuntime`（两套 Executor 生命周期） | DEC-068 §1 |
| 主体身份 | 复用 `PrincipalContext`；注册表键 `(TenantId, UserId)` | 新增 `PrincipalId`；复用 `MemoryScope.subject_id` | DEC-068 §2；DEC-046 |
| 入口身份 | `ControlIngressId`（既有预留位） | 新造 ID 类型 | DEC-068 §3；`core_contracts.hpp:138` |
| 幂等键 | 宿主提供的 opaque `input_id`（字符串） | 内容哈希；复用 `CommandId` / `mutation_id` | DEC-068 §4；issue #111 §1 |
| 台账接口 | 独立窄接口 `IInputLedger` | 并入 `IMemory` / `IMemoryRetentionSweep` | DEC-068 §4；DEC-062 |
| 路由裁决 | 确定性函数，闭集 8 意图；模型只建议 | 模型直接决定并执行 | DEC-068 §5/R1 |
| 歧义处理 | `RouteIntent::Clarify` + 自然语言澄清（非错误码） | 返回失败码 | DEC-068 §5/R4；issue #111 §2 |
| 紧急停止 | `cancel_task` 确定性路径 | 经下一次模型路由 | DEC-068 §5/R6 |
| 权限校验 | fail-closed 校验点，首阶段限于协调层自建对象 | 默认放行；全局接线 `PolicyEngine` | DEC-068 §5/R9 |
| Loop 输入缝 | `IUserInputSink` + 两 Loop 各一适配；`ConversationLoop` 补对称入队通道 | 只支持 `AgentLoop`；协调层自建 Loop | DEC-068 §1 |
| 上下文归属 | 协调层选快照，Layer 0 仍是唯一请求期准入 | 新增第二套准入路径 | DEC-068 §6 |
| 事件版本 | 事件自带 schema 名；不信 `EventEnvelope::schema_version` | 用信封 `schema_version` | DEC-068 §7；`event_store.cpp:131-143` |
| 容量归属 | 台账/注册表计入 DEC-069 预算，本阶段只保证不无界 | 独立设一个无界表 | DEC-069 §2 |
| Executor 路由 | 复用 Runtime Executor + `ExecutionSupervisor` | 协调层自建 Executor/线程池 | AGENTS.md；Runtime 设计 §11.2 |

## 5. 阶段冻结协议必答八问（2026-10-28）

| # | 问题 | 回答 |
| --- | --- | --- |
| 1 | **行为**：本阶段改变或新增哪些行为？对应设计文档哪些冻结小节？ | 新增主体级输入接纳、幂等去重、确定性路由、结果关联、Loop 输入缝、上下文归属。锚点：DEC-068 §1–§9；[核心契约设计 §5、§6.2、§6.3](../design/core_contracts_and_state_machine.md)；[Runtime 设计 §11](../design/mira_runtime_design.md)；[Context 与 Memory 设计 §6.1、§9](../design/context_and_memory_design.md) |
| 2 | **所有者**：新增可变状态的唯一所有者是谁？谁只读投影？ | 唯一 writer：协调层（主体注册表 + Loop 注册面）、`IInputLedger` 实现（台账）。只读投影：Runtime（Session/Task 状态，唯一 owner）、EventStore（事实源）。协调层**不得**写 Runtime 的 Session/Task 状态；Runtime **不得**写协调层注册表 |
| 3 | **契约**：最小公共契约面是什么？哪些调用方允许使用？ | `include/mira/interaction_coordinator.hpp`：`InputEnvelope`/`InputStage`/`RouteIntent`/`RoutingDecision`/`InputDisposition`/`IInputLedger`/`IUserInputSink`/`InteractionCoordinator`。允许使用者：宿主（Mirage 等集成方）。既有 `open_session`/`submit_task` 等**保留**给高级集成与调试 |
| 4 | **层与依赖**：新文件属于哪个模块？include 方向是否已登记？ | `include/mira/interaction_coordinator.hpp` 与 `src/interaction/` 落在 `core` roots（`include/mira`、`src`）内 → **policy 零差异**。只依赖 `core` + `kairo` 公开头；**不**引用 `src/` 私有头（`deep-import` 零新增） |
| 5 | **复用**：哪条现有路径已承担部分职责？为何不新增平行路径？ | 复用：Runtime 既有命令面与状态机；`PrincipalContext` 主体身份；`CommandReceipt`/`CommandOutcome` 幂等词汇；EventStore 事实源与 append 级幂等；DEC-045 供给缝；`AgentLoop::enqueue_user_message` 有界队列语义；`MemoryScopeKind::Session` 会话分区。**不新增**：第二套 Session/Task 状态机、第二套 Executor、第二套 Context 准入、第二套台账空间 |
| 6 | **时序**：事件顺序、幂等键、过期结果规则、取消与 deadline 边界是什么？ | 事件顺序：`InputAccepted` → `InputRouted`（或 `InputClarified` / `InputRejected`）→ `InputAnswered` → `InputTaskSettled`。幂等键：宿主 `input_id`（窗口内返回既有条目；窗口外明确过期拒绝）。取消边界：投递与后台结果受 Task epoch 约束；迟到结果按既有 `StaleObservation` 语义丢弃，**不得**复活终态任务 |
| 7 | **Executor 路由**：每类工作由 Executor 哪种能力承载？句柄谁持有？关闭顺序如何？ | 见 §4.8。句柄由协调层持有并消费；关闭顺序沿 [Runtime 设计 §11.5](../design/mira_runtime_design.md)，`shutdown(true)` 由非 worker 线程执行 |
| 8 | **验证上下文**：评审者最小需要读哪些契约、spec 与测试才能验收？ | 契约：DEC-068 §1–§9、本文件 §4；设计：核心契约 §5/§6、Runtime §11、Context/Memory §6.1/§9；测试：`tests/m28/`（`M28-G1`–`M28-G8`），label `integration;m28` |

## 6. 测试与退出条件

### 6.1 门禁矩阵

| 门禁 | 内容 | 测试文件（`tests/m28/`） |
| --- | --- | --- |
| `M28-G1` | 输入接纳与阶段分离：四阶段各自可观察，阶段之间无隐式跳转；拒绝有明确错误码 | `m28_input_admission_test.cpp` |
| `M28-G2` | 幂等：窗口内重复不产生第二次派发；窗口外明确过期拒绝；内容相同但 id 不同**不**判为重试 | `m28_input_ledger_test.cpp` |
| `M28-G3` | 路由裁决：八个 `RouteIntent` 各有可复现用例；终态任务不被复活；歧义走澄清而非错误码；权限缺失 fail-closed | `m28_routing_test.cpp` |
| `M28-G4` | 结果关联：回复/追问/后台结果/失败可追溯到 input_id + task_id；迟到结果不冒认其他输入；新事件自带版本标记 | `m28_result_association_test.cpp` |
| `M28-G5` | 上下文归属：插话纠正只更新目标任务；不相关输入不污染其他任务约束 | `m28_context_ownership_test.cpp` |
| `M28-G6` | Executor 纪律：取消、超时、shutdown、背压、提交拒绝各有结算路径；无未决句柄、无静默丢失败 | `m28_lifecycle_test.cpp` |
| `M28-G7` | 契约四件套：公开头文件、API 手册、术语表、事件面同步 | 文档审查 + `tools/check_architecture.py` |
| `M28-G8` | 架构门禁：`architecture-check` 零新增违规；无自建线程/定时器；`clang-tidy`/`format` 零违例 | CI quality 管线 |

### 6.2 退出条件

- [ ] `M28-01`–`M28-08` 全部工作项完成并勾选。
- [ ] `M28-G1`–`M28-G8` 全部通过，测试文件与 label 就位。
- [ ] CI 24/24 全绿（linux gcc/clang × Debug/Release、windows Debug/Release、
      android arm64/x86_64 NDK 交叉、sanitizers ASAN/TSAN/UBSAN、quality）。
- [ ] 公开 API、事件 schema、错误语义与示例已同步更新。
- [ ] 受影响的设计、决策、计划、术语表、API 手册已同步更新。
- [ ] 验证记录（§9）含 commit、环境、命令、结果与限制。

### 6.3 未执行验证的记录要求

依 [项目管理与文档规范 §5](../project/project_management_and_documentation.md)：
设备、权限、网络、凭据或平台缺失导致的未执行验证**必须保持未勾选**，
并记录原因、所需环境、负责人与可操作的补跑条件。

## 7. 工作项

- [ ] `M28-01` 立项与跑前冻结：DEC-068 冻结面、本文件 §4 契约语义、§5 必答八问、
      §4.9 决策记录表、§6 门禁矩阵；状态转 `Planned`。
- [ ] `M28-02` 公开契约面：`include/mira/interaction_coordinator.hpp`
      （`InputEnvelope`/`InputStage`/`RouteIntent`/`RoutingDecision`/`InputDisposition`/
      `IInputLedger`/`IUserInputSink`/`InteractionCoordinator`/`CoordinatorConfig`）；
      wire schema 键集冻结；`mira.interaction` 错误域冻结。
- [ ] `M28-03` 幂等台账：`IInputLedger` 的耐久实现（EventStore 承载），
      原子 `admit`、窗口内重复返回既有条目、窗口外明确过期拒绝、
      回收窗口接口、事件面写入；**不含**容量预算数值（归 M31）。
- [ ] `M28-04` 协调器核心：主体注册表（`(TenantId, UserId)` → sessions/tasks，
      可由 EventStore 重建）、确定性路由裁决（八个 `RouteIntent`）、
      fail-closed 权限校验点、结果关联与迟到结果处置。
- [ ] `M28-05` Loop 输入缝：`IUserInputSink` 抽象；
      `AgentLoop` 适配实现（复用 `enqueue_user_message` 的有界队列与
      `ResourceExhausted` 语义）；`ConversationLoop` **新增**对称的有界入队通道
      与适配实现。
- [ ] `M28-06` 上下文归属与用户约束承载：`TaskSpec` 追加式用户约束字段；
      协调层按工作组织上下文；插话纠正只更新目标任务；Layer 0 仍是唯一请求期准入。
- [ ] `M28-07` IVA 测试矩阵：`tests/m28/` 六组用例（§6.1），
      label `integration;m28`，CMake 目标与门禁注册。
- [ ] `M28-08` 文档同步：API 手册新页（`docs/api/interaction-coordinator.md`）、
      术语表新词条、总计划索引与 §4.1 入口、README 能力行、
      事件面说明、DEC-068 状态回填；修正
      `docs/api/model-agent-loop.md` 中用户消息 provenance 与实现不符的文档漂移
      （实现为 `mira.agent-loop.context.v1`）。

## 8. 风险与阻塞

| 风险 | 说明 | 缓解 / 归属 |
| --- | --- | --- |
| `M28-R1` 路由错误 | 把输入送进错误任务 | 裁决带 `rationale` 可审计；歧义走澄清；纠正路径与普通路由同权。M28-03 |
| `M28-R2` `ConversationLoop` 公开面扩大 | 补入队通道改变其公开契约 | 镜像既有有界队列语义，不引入新背压模型；追加式变更。M28-05 |
| `M28-R3` 权限校验误拒 | fail-closed 可能拒绝既有合法用法 | 首阶段限于协调层自建对象；既有显式 API 路径不受影响。M28-04 |
| `M28-R4` 台账无界增长 | 新增耐久层可能成为无界表 | 本阶段即实现回收窗口接口；字节计入 DEC-069 预算（M31）。M28-03 / M31 |
| `M28-R5` 事件名硬编码闭集漏同步 | 新事件类型未进入 conversation view / checkpoint 闭集 | `M28-G4` 显式断言闭集已同步。M28-07 |
| `M28-R6` 恢复语义半交付 | 台账耐久但恢复执行路径在 M29 | 文档与 API 手册**明确标注**恢复路径未交付，不得宣称重启可恢复。M28-08 / M29 |
| `M28-R7` 真实模型语义未验证 | 路由准确率与澄清率无证据 | 门禁只声明管线行为指标（RULE-10）；真实模型轮另立。M28-08 |

**外部阻塞**：无。本阶段为纯 Core 确定性工作，不依赖真实模型、真机或宿主凭据。
真实模型语义评估与 Stage E 仍受 `MNT-202609-27` 外部证据约束，**不被本立项解锁**。

## 9. 验证记录

> 尚无记录。实现开始后按时间追加：日期、commit、环境、命令、结果、限制与剩余项。
> 未执行项必须保持未勾选并记录原因、负责人与补跑条件
> （[项目管理与文档规范 §5、§10](../project/project_management_and_documentation.md)）。
