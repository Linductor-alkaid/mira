# DEC-068：交互协调层与主体级输入入口

> 状态：Proposed
> 日期：2026-10-28
> 负责人：Mira Maintainers
> 冻结里程碑：M28
> 替代/被替代：无
> 来源需求：[Issue #111（`MIRA-20261027-001`）](https://github.com/Linductor-alkaid/mira/issues/111)
> §1–§3、§5（会话/任务编排、统一输入接纳、上下文组织、持续交互契约）

## 背景与问题

### 现状核对（2026-10-28 静态审计）

Mira 的公开控制面目前以「调用方已经知道要开哪个会话、要提交哪个会话下的任务」为前提。

**Runtime 面**（[`include/mira/runtime.hpp:89-130`](../../include/mira/runtime.hpp#L89-L130)）：

- `open_session(std::shared_ptr<IEnvironment>, SessionConfig)`、
  `submit_task(SessionId, TaskSpec)` 与 pause/resume/cancel/takeover/close
  —— **全部公开方法的寻址键都是 `TaskId` 或 `SessionId`**，
  没有任何方法接受或返回 principal / user / tenant。
- `TaskSpec` 只有一个字段 `goal`
  （[`include/mira/runtime.hpp:24-26`](../../include/mira/runtime.hpp#L24-L26)）——
  没有承载「用户补充的约束」的槽位。
- **没有会话/任务枚举面**：`session_snapshot` / `task_snapshot` 都需要先有 ID；
  没有任何接口能回答「这个主体名下现在有哪些会话和任务」。
- 内部 `sessions` / `tasks` / `operations` 是**纯内存 map**
  （[`src/runtime/runtime.cpp:143-147`](../../src/runtime/runtime.cpp#L143-L147)），
  **进程退出即丢**；恢复路径是**按任务**走 `RecoveryPlanner`
  （[`include/mira/task_recovery.hpp:48-64`](../../include/mira/task_recovery.hpp#L48-L64)），
  **不是**按会话重建。

**Loop 面**：

- `AgentLoop::enqueue_user_message(std::string message)`
  （[`include/mira/agent_loop.hpp:156`](../../include/mira/agent_loop.hpp#L156)）向**调用方已经选定的**
  loop 注入消息；它没有 input id，因此重试与断线重连场景下无法在 Loop 之前拦截重复投递。
  该队列有界（`max_pending_user_messages = 16`，
  [`include/mira/agent_loop.hpp:74`](../../include/mira/agent_loop.hpp#L74)）且溢出返回
  `ResourceExhausted` 而非静默丢弃——这是协调层要复用的背压范式。
- **`ConversationLoop` 完全没有用户消息入队通道**：
  [`include/mira/conversation_loop.hpp:200-297`](../../include/mira/conversation_loop.hpp#L200-L297)
  只暴露 `set_model_input_supplier`（宿主每轮主动供图，带 `AgentLoopSpec`）、
  `set_model_input_identity`、`set_history_transformer` 与 `run`，
  **没有** `enqueue_user_message` 或任何等价入口。而 continuous 模式
  （`ConversationLimitMode::Continuous`）正是长时持续交互的形态——
  **这是统一输入口的硬缺口，不是接线细节**。

**身份面**：

- 主体身份**已经存在**：`PrincipalContext{tenant_id, user_id, host_id, auth_strength,
  authenticated_at, grants, identity_version}`
  （[`include/mira/security.hpp:45-53`](../../include/mira/security.hpp#L45-L53)），
  `UserId`/`TenantId`/`HostInstanceId` 是既有强类型 ID
  （[`include/mira/core_contracts.hpp:135-137`](../../include/mira/core_contracts.hpp#L135-L137)）。
  但它今天的身份是**会话的属性**——由 `SessionConfig` 承载
  （[`include/mira/runtime.hpp:20-22`](../../include/mira/runtime.hpp#L20-L22)），
  在 `open_session` 时绑定，而不是**输入入口的身份**。
- `ControlIngressId`
  （[`include/mira/core_contracts.hpp:138`](../../include/mira/core_contracts.hpp#L138)）
  **已定义但全仓零使用**——它是既有的「控制入口/输入入口」ID 预留位。
- `PrincipalContext` **不可比较、无 `operator==`、无 hash**，不能直接做 map key。
- `PolicyEngine::evaluate` 与 `has_capability` 的**唯一**调用者是 `PolicyEngine`
  自身（[`src/security/security.cpp:49`](../../src/security/security.cpp#L49)），
  而 `PolicyEngine` **在生产路径中无调用方**；`submit_task` 只看
  `SessionState == Autonomous`（[`src/runtime/runtime.cpp:438`](../../src/runtime/runtime.cpp#L438)），
  **Runtime 从不检查 grants**。

**结论与 issue #111 的核对一致**：Mira 现有能力足以支撑「一次已定会话/任务的执行闭环」，
但**不存在**「主体级输入 → 内部会话/任务安排 → 上下文准备 → 统一结果关联」这一层。
把统一输入框做在 UI 层不改变这一点；只增加记忆表、或把所有输入送进固定 loop 也不改变这一点。

### 缺口清单

| # | 缺口 | 性质 |
| --- | --- | --- |
| G1 | 无主体级输入入口；调用方必须先决定 SessionId | 新契约面 |
| G2 | 无「主体 → 其会话/任务」的所有者与可枚举映射 | 新可变状态 |
| G3 | 映射必须**耐久**：Runtime 的会话表是纯内存，重启即丢，无法满足 issue #111 §5 的「重启恢复内部任务/会话映射」 | 新持久化组件 |
| G4 | 无 input id 幂等台账；`CommandId` 去重语义绑定 Task/Session 终态 retention（[核心契约设计 §5.2](../design/core_contracts_and_state_machine.md)），不能承担跨重启的主体级输入去重 | 新持久化组件 |
| G5 | `TaskSpec` 无用户约束槽位，「补充/纠正某项工作」无处落 | 既有契约加法扩展 |
| G6 | `ConversationLoop` 无入队通道，无法承接长时持续交互 | 既有契约加法扩展 |
| G7 | 无「指代不清 → 自然语言澄清」的结果面；现有错误模型只有稳定错误码 | 新契约面 |
| G8 | 无「回复/后台结果 → 原始输入与任务」的可追溯关联；Loop 侧无 input id | 新契约面 |
| G9 | 输入接纳与任务完成之间无阶段区分（[核心契约设计 §5](../design/core_contracts_and_state_machine.md) 的三阶段协议覆盖的是**控制命令**，不是主体输入） | 新契约面 |
| G10 | 权限校验无生产接线（Runtime 从不查 grants） | 既有契约接线 |

### 边界主张

issue #111 明确要求 Mirage 侧不得自建平行编排系统。因此本决策冻结的是**Mira 侧的
公共集成边界**，而不是实现某一版 UI。Mirage 保留现有显式 session/task 模式，
该模式不被视为本需求已交付。

## 决策

### 1. 引入「交互协调层」（Interaction Coordinator），位于 Runtime 之上、宿主之下

新增一个 Core 组件 `InteractionCoordinator`：

```text
宿主（Mirage / 其他集成方）
  │  统一输入口：InputEnvelope
  ▼
InteractionCoordinator  ── 主体注册表、路由裁决、幂等台账、结果关联
  │              │
  │              └── IUserInputSink 注册面（task_id → 已在运行的 Loop）
  ▼
Agent Harness 控制面 ────────── MiraRuntime（Session/Task/Action，唯一状态机 owner）
  │                            AgentLoop / ConversationLoop（模型闭环）
  ▼
Context / Memory / EventStore / ArtifactStore
```

**协调层不实现第二套状态机。** 所有 Session/Task 的状态变化仍然只经
`MiraRuntime` 的既有命令面发生（[`include/mira/runtime.hpp:97-120`](../../include/mira/runtime.hpp#L97-L120)）。
协调层是这些命令的**编排者**，不是替代者。既有命令的三阶段协议
（`Submitted → Accepted/Rejected → Settled`，
[核心契约设计 §5](../design/core_contracts_and_state_machine.md)）与
终态幂等词汇（`Applied`/`NoOp`/`Failed`/`Superseded`）原样复用。

**协调层不创建也不运行 Agent Loop。** 它持有一个显式的注册面
`task_id → IUserInputSink`，由宿主装配时建立绑定；投递经既有
`AgentLoop::enqueue_user_message`（有界队列 + `ResourceExhausted` 溢出语义）。
这一形态沿用 M25/DEC-045 的「薄缝 + setter 注入」先例
（[DEC-045](DEC-045-agent-loop-working-context-seam.md)）。

> **硬前置（新增于本决策）**：`ConversationLoop` 没有任何入队通道
> （见背景核对）。首阶段交付前**必须**先为 `ConversationLoop` 补一条与
> `AgentLoop::enqueue_user_message` **对称**的有界入队通道，或引入两者共用的
> `IUserInputSink` 抽象并让两个 Loop 各实现一次。
> **不允许**协调层只支持 `AgentLoop` 就宣称交付统一输入口——
> continuous 模式是长时交互的主形态。

**协调层不拥有第二套 Executor。** 它的全部异步工作经 Runtime 已有的
`ExecutionSupervisor` 与 Executor 路由（[Runtime 设计 §11.1–§11.2](../design/mira_runtime_design.md)），
句柄由协调层持有、关闭顺序沿 [Runtime 设计 §11.5](../design/mira_runtime_design.md)。
协调层**不得**直接调用 Executor singleton。

> **依赖注入约定**：`MiraRuntime` **不暴露** `kairo::Executor&`
> （[`include/mira/runtime.hpp:89-130`](../../include/mira/runtime.hpp#L89-L130) 只暴露
> `RuntimeConfig`），因此协调层必须**显式注入** Executor 引用。
> 沿用 `WorkflowRuntime` 的既有做法（[`include/mira/workflow_runtime.hpp:228`](../../include/mira/workflow_runtime.hpp#L228)）。

### 2. 主体身份复用 `PrincipalContext`；注册表键为 `(TenantId, UserId)`

协调层持有 `PrincipalContext` 作为主体身份。**不**引入 `PrincipalId` 之类的第五套主体标识：

- `PrincipalContext` 已含 `TenantId`/`UserId`/`HostInstanceId`/`AuthenticationStrength`/
  `CapabilityGrant[]`/`identity_version`，是安全与授权的既有载体。
- 协调层为它创建的每个 Session 派生一份 `SessionConfig{principal}`，沿用
  `open_session` 的既有绑定路径；**协调层不扩大权限**——派生只做裁剪，不做提升。

**注册表键**：`PrincipalContext` 不可比较、无 hash，因此注册表以
`(TenantId, UserId)` 二元组为键；`host_id` 与 `grants` 是**属性**而非身份维度
（同一用户在不同宿主实例上的会话必须能被同一个入口看到）。
`UserId`/`TenantId` 是 Id128 强类型，可直接比较。

**不**复用 `MemoryScope.subject_id`（自由字符串，记忆 ACL 域，
[DEC-046](DEC-046-entity-vocabulary-alignment.md) 决策 3）或
`TrackedEntity.entity_key`（感知实体局部标识，同上决策 4）。
二者与主体身份**不可混用**；`PrincipalContext.user_id` → `MemoryScope.subject_id`
的映射规则由 DEC-046 登记，本决策**不**建立该映射（M30 承载）。

### 3. 统一输入接纳：输入信封与阶段分离

```cpp
// include/mira/interaction_coordinator.hpp
struct InputEnvelope final {
    PrincipalContext principal;            // 主体身份（宿主提供，已认证）
    std::string input_id;                   // 宿主提供的稳定输入标识 = 幂等键
    std::string content;                    // 输入正文
    std::vector<std::string> attachment_refs; // 工件引用（不复制内容）
    std::optional<std::string> reply_to;    // 关联另一次输入（如追问链）
};

enum class InputStage : std::uint8_t {
    Accepted,     // 已接纳并登记；**不等于**任务完成
    Routed,       // 已归属到具体 Session/Task
    Answered,     // 已产出面向用户的回复
    TaskSettled,  // 关联任务进入终态
    Rejected,     // 明确拒绝（过期 / 重复 / 越权 / 预算 / 关闭中）
};

struct InputDisposition final {
    ControlIngressId ingress_id;           // Mira 内部的入口身份（既有预留 ID 位）
    std::string input_id;                   // 宿主提供的幂等键
    InputStage stage = InputStage::Accepted;
    bool duplicate = false;                 // 命中幂等窗口内的既有记录
    std::optional<SessionId> session_id;
    std::optional<TaskId> task_id;
    std::optional<Error> error;
    std::optional<std::string> clarification; // 指代歧义时的自然语言澄清
};
```

**两个标识各司其职**：`input_id` 是**宿主提供的幂等键**（opaque 字符串，
允许宿主用其自有系统的消息 id，不强制 UUID 形态）；`ControlIngressId` 是
**Mira 内部的入口身份**（Id128 强类型，可进入事件面与日志引用）。
协调层在接纳时铸造 `ControlIngressId`，并把它与 `input_id` 一起写入台账。
**不新造 ID 类型**——`ControlIngressId` 是仓库里已为此预留的位。

阶段语义逐条对应 issue #111 §1「明确接纳、理解/路由、执行、回答和任务完成的不同阶段；
收到输入不等于任务已完成」。

### 4. 幂等台账是独立的窄接口 `IInputLedger`

```cpp
class IInputLedger {
  public:
    virtual ~IInputLedger() = default;
    // 原子登记一次输入；窗口内的重复 input id 返回既有记录。
    virtual Result<LedgerEntry> admit(const InputEnvelope &envelope) = 0;
    // 查询一次输入的当前归属与阶段。
    virtual Result<LedgerEntry> lookup(std::string_view input_id) const = 0;
};
```

定位与 `IMemoryRetentionSweep`（[DEC-062](DEC-062-memory-retention-sweep.md)）一致：
**存储所有者级的窄接口**，不并入 `IMemory`。

- 不并入 `IMemory`：`IMemory` 的每个方法都绑定一个 scope
  （[`include/mira/memory_contracts.hpp:381`](../../include/mira/memory_contracts.hpp#L381)），
  而输入台账是**主体级、跨会话、跨重启**的——它回答「这个 input id 之前发生过什么」，
  不是「这个 scope 里有哪些记忆」。
- 不并入 `IMemoryRetentionSweep`：清扫按保留期回收已到期数据；台账按**幂等窗口**保留
  尚未过期的去重事实，两者生命周期与触发条件都不同。
- 不复用 `CommandId` 去重：命令去重记录「至少保留到所属 Task/Session 终态 retention 结束」
  （[核心契约设计 §5.2](../design/core_contracts_and_state_machine.md)），
  主体级输入的幂等窗口必须**跨进程重启**且与任务终态无关。
- 不复用 `memory_mutations` 台账：那张表是**记忆变更**的幂等台账
  （[DEC-058](DEC-058-mutation-ledger-scope-binding.md)），
  key 是 `mutation_id`、scope 绑定到记忆 scope，与主体输入不是同一空间。

**窗口外语义**：同一 input id 在窗口外再次到达，必须返回**明确拒绝/过期**
（`InputStage::Rejected` + 稳定错误码），**不得**被当作新输入静默执行，也不得
「清掉台账后当成新动作」。窗口长度是部署档位参数（暂定默认值见 M28 §4）。

**不同输入即使内容相同也不是重试**——去重键是 `input_id`，不是内容哈希。

**台账自身必须被容量治理**：它是新增的耐久数据层，
**计入** [DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md) 的总量预算，
并与既有 `memory_mutations` 台账一样**必须有回收窗口**——
不得成为新的无界表。

### 5. 路由裁决是确定性函数；模型只能建议

```cpp
enum class RouteIntent : std::uint8_t {
    ContinueCurrent,  // 延续当前工作
    SupplementTask,   // 补充 / 纠正某项工作
    NewTask,          // 新建任务
    ResumeTask,       // 恢复旧任务（关联旧摘要的新任务）
    PauseTask,
    CancelTask,
    Clarify,          // 指代不清，需自然语言澄清
    Reject,           // 越权 / 预算 / 过期 / 关闭中
};

struct RoutingDecision final {
    RouteIntent intent = RouteIntent::Reject;
    std::optional<SessionId> session_id;
    std::optional<TaskId> task_id;
    std::string rationale;                 // 可审计的裁决理由
    std::optional<std::string> clarification;
};
```

冻结规则：

- **R1（确定性）** 路由裁决由协调层内的确定性函数完成，输入是
  `(PrincipalContext, InputEnvelope, PrincipalState)`。模型可以产出 `suggested_intent`，
  协调层**校验其权限与状态后**才采纳或拒绝；模型的建议本身不是裁决。
- **R2（不静默重写）** 路由到已终态任务必须转为 `ResumeTask`（关联旧摘要的新任务）
  或 `NewTask`，**不得复活终态任务**（[核心契约设计 §6.3](../design/core_contracts_and_state_machine.md) 的终态幂等）。
- **R3（不默认归属）** 不把「最近一个 Session」当成所有输入的唯一默认归属。
  归属依据是：当前交互、活动/挂起任务、用户显式引用、授权范围内的历史。
- **R4（澄清不是错误）** 多个候选都合理时返回 `RouteIntent::Clarify` +
  `clarification`，**不**返回失败码；`clarification` 走正常回复通道。
  明确的补充（唯一候选）**不**触发多余澄清。
- **R5（拆分关系保留）** 一个输入关联多个工作项时，协调层记录拆分关系，
  不把输入压平成单一任务归属。
- **R6（紧急停止走确定性路径）** 紧急停止直接经 `MiraRuntime::cancel_task`，
  **不依赖**下一次模型路由成功。模型建议不得成为停机的前置条件。
- **R7（纠正先于副作用）** 插话纠正更新的是**目标任务的后续上下文**；
  接纳纠正**不宣称已撤回**已经发生的副作用；在途动作按既有 Task 状态机安全收敛。
- **R8（能力边界）** 协调层不承载平台资源租约与并发上限的判定——那是宿主能力与
  既有 ActionLease 纪律（[核心契约设计 §9](../design/core_contracts_and_state_machine.md)）。
- **R9（权限 fail-closed）** 路由裁决**必须**经主体能力校验后才提交。
  现状 `PolicyEngine` 无生产调用方、`Runtime` 从不查 grants，因此
  **协调层是第一个生产权限校验点**，其校验**必须 fail-closed**：
  能力缺失、grant 过期或 `identity_version` 不匹配时一律 `Reject`，
  **不得**默认放行。`PolicyEngine` 本身的生产接线（策略集、确认质询、版本化）
  是**独立决策**，本决策只冻结「校验点存在且 fail-closed」。

### 6. 上下文组织按「当前工作」，不拼接全部历史

协调层为每个被路由到的工作项组织模型请求所需的上下文：

- 目标、**最新**用户约束、任务状态、未决副作用、近期热上下文、已提交 Working Context、
  需要召回的历史与长期记忆。
- 插话纠正只更新**目标任务**的后续上下文；不相关话题不得污染其他任务的状态或约束。
- 全局偏好（`MemoryScopeKind::User`）、当前指令与任务局部要求的生效范围必须显式区分
  （[Context 与 Memory 设计 §6.1](../design/context_and_memory_design.md)），
  **不得**以「拼接所有会话历史」处理。
- Layer 0 的预算/authority/epoch/最低执行集准入（[Context 与 Memory 设计 §9](../design/context_and_memory_design.md)）
  继续是**请求装配期**的唯一准入转换；协调层只负责**选择哪一份已提交快照**，
  不新增第二套准入路径。既有供给缝（[DEC-045](DEC-045-agent-loop-working-context-seam.md)）
  是 Loop 侧消费该选择的唯一通道。

> **作用域澄清（新增）**：Layer 0 是**单次请求**的纯函数
> （[`include/mira/context_manager.hpp:105-116`](../../include/mira/context_manager.hpp#L105-L116)
> 明写「pure functions of the request」），**不跨任务、不跨主体、不跨请求**。
> issue #111 §3「复用 Layer 0 的最终准入」成立，但**不等于** Layer 0 已经具备
> 跨任务的预算准入——那属于 DEC-069 的容量预算，二者不可混谈。

### 7. 结果关联：回复与后台结果都带归属

- 协调层产出的每一份回复、每一个追问、每一个后台结果与失败，都携带
  `input_id` + `session_id` + `task_id`（可空）+ `ControlIngressId`。
- **迟到消息保留归属**：后台任务的结果回到它所属的 input/task，
  **不得**看起来像是在回答用户刚发出的另一句话（issue #111 §5）。
- 输入口在执行任务、等待工具或等待某个澄清时**持续可用**；新输入的接纳有界且可观察
  （经 Executor 队列容量与 `SchedulingMetricsSnapshot` 的拒绝计数，
  [`include/mira/runtime.hpp:41-55`](../../include/mira/runtime.hpp#L41-L55)）。

**事件面版本化约定（新增，冻结）**：

- `EventEnvelope::schema_version` 是**死字段**——恒为 `{1,0}`、不进入 canonical 摘要、
  不落盘（[`src/storage/event_store.cpp:131-143`](../../src/storage/event_store.cpp#L131-L143)）。
  新事件**必须自带**版本标记：schema 名（`mira.<family>.<kebab>.v1` 族）或
  payload 内 `schema_version` 键。**不得**依赖信封字段。
- `input_id` 必须落在 `EventPayload::data` 内。
- 新增事件类型名时**必须同步**硬编码事件名的闭集：
  [`src/model/conversation_log.cpp:91-97`](../../src/model/conversation_log.cpp#L91-L97)、
  [`src/storage/checkpoint_store.cpp:554,628`](../../src/storage/checkpoint_store.cpp#L554)。

### 8. 恢复语义

- 协调层的**主体注册表**（主体 ↔ 会话/任务映射）必须是**可从 EventStore 重建的投影**，
  不得只存在于内存（Runtime 的会话表是纯内存，
  [`src/runtime/runtime.cpp:143-147`](../../src/runtime/runtime.cpp#L143-L147)）。
- 重启后：台账恢复 → 已接纳输入的阶段恢复 → 未决输入重新路由；
  **未决副作用必须先重观测/验证**，不得因恢复流程重复执行
  （[事件与崩溃一致性设计 §7–§8](../design/event_artifact_crash_consistency.md)）。
- 历史缺失、已擦除或不可恢复时**明确说明**，不得补造经历。

### 9. 既有显式 API 保留

`open_session`/`submit_task`/暂停/取消/接管等**不删除、不弃用**。它们继续服务高级集成与调试。
协调层是「默认用户交互」的入口，不是唯一入口。

## 备选方案

- **在 `MiraRuntime` 上加 `submit_input(principal, input)`**：否决。把主体级编排塞进
  Runtime 的控制命令三阶段协议，会把「一次用户输入」与「一个控制命令」混为一谈，
  污染 Session/Task 状态机、背压语义与终态幂等。Runtime 保持「唯一状态机 owner」
  是本仓库的既有不变量。
- **建一个平行的 `PrincipalRuntime`（第二套 Runtime）**：否决。AGENTS.md 要求
  Runtime/Task/Session/Controller 不得各自隐藏全局生命周期；两套 Runtime 意味着两套
  Executor 生命周期与两套终态语义。
- **新增 `PrincipalId` 强类型 ID**：否决。主体身份的三要素已是既有强类型 ID；
  新造第四套只会制造需要映射的冗余（DEC-046 已就同类问题立过界）。
- **把幂等台账并入 `IMemory`**：否决。scope 绑定语义不匹配（理由见 §4）。
- **把幂等台账并入 `IMemoryRetentionSweep`**：否决。保留期清扫与幂等窗口的触发条件、
  生命周期与失败语义都不同。
- **复用 `memory_mutations` 表**：否决。key 空间与 scope 绑定都不同（见 §4）。
- **以内容哈希代替 input id 去重**：否决。issue #111 §1 明确「不同输入即使内容相同也不能
  自动当成重试」；内容哈希会把用户的合法重复输入误判为重试，也会漏掉内容相同但
  意图不同的多次输入。
- **让模型直接决定路由并执行**：否决。路由会改变权限边界与工作归属，
  必须由确定性 Runtime 校验状态与权限后提交。
- **协调层直接创建并运行 `ConversationLoop`**：否决（首阶段）。那会把 Provider/模型路由/
  工具装配全部拉进协调层，扩大耦合面，并绕过 M25 已冻结的薄缝形态。
  后续若需要，**须另立上位决策**。
- **把主体标识加进 `AgentLoopSpec`**：**暂不采纳**。`AgentLoopSpec`
  （[`include/mira/agent_loop.hpp:95-101`](../../include/mira/agent_loop.hpp#L95-L101)）
  今天是全仓统一的**任务帧身份三元组**，被三个供给缝与事件面共用。若主体要进入
  身份门控口径，**加在 `AgentLoopSpec` 上比改各 supplier 签名更一致**；
  但这是公开契约的 minor 版本变更（DEC-002），且首阶段的主体门控
  **由协调层在 Loop 之外完成**，因此本轮**不改** `AgentLoopSpec`。
  后续若需要，另行决策。
- **由宿主自行路由、只提供薄工具库**：否决。那正是 issue #111 否掉的形式——
  「即使统一输入框在 UI 层完成，上述核心行为仍没有成为 Mira 的统一高层公开契约」。

## 影响与风险

- **新增公开契约面**：`include/mira/interaction_coordinator.hpp`（新文件）与
  `src/interaction/`（新目录）。二者都落在 `core` 模块 roots（`include/mira`、`src`，
  [`tools/architecture-policy.json`](../../tools/architecture-policy.json)）内，
  **无 policy 差异**；若后续需要独立模块再按架构治理流程登记。
- **既有契约加法扩展**：`TaskSpec` 增加用户约束字段（追加式，不改既有字段语义）；
  `ConversationLoop` 增加对称的有界入队通道（追加式）；
  `AgentLoop` 首阶段**零改动**。
- **新增可变状态 owner**：协调层是「主体注册表（`(TenantId, UserId)` → sessions/tasks）」
  与「Loop 注册面（`task_id` → sink）」的**唯一 writer**；
  `MiraRuntime` 仍是 Session/Task 状态的唯一 owner；台账由 `IInputLedger` 实现持有。
  三者不得互相写对方的状态。
- **新增持久化层**：幂等台账 + 主体注册表投影，是新的耐久组件，其自身字节
  **必须计入**总容量预算（[DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md)）。
- **风险 R-1：路由错误把输入送进错误任务。** 缓解：裁决必须带 `rationale` 可审计；
  纠正路径与普通路由同权；R3 禁止「最近一个 Session」默认归属；歧义走澄清而非猜测。
- **风险 R-2：协调层变成新的「上帝组件」。** 缓解：协调层不含任何决策策略实现，
  路由裁决被限制在闭集 `RouteIntent` 与确定性规则内；模型只提供建议。
- **风险 R-3：幂等台账与注册表成为新的无限增长数据池。** 缓解：二者必须有回收窗口与
  总量上限，且上限由 DEC-069 的容量准入统一裁决。
- **风险 R-4：与既有 `CommandId` / `mutation_id` 去重语义混淆。** 缓解：三者作用域、
  生命周期、键空间完全分离；台账条目必须记录 `input_id`、`ControlIngressId`、
  接纳时间、归属与阶段。
- **风险 R-5：`ConversationLoop` 补入队通道扩大其公开面。** 缓解：镜像
  `AgentLoop::enqueue_user_message` 的有界队列与 `ResourceExhausted` 语义，
  不引入新的背压模型。
- **风险 R-6：权限校验 fail-closed 会拒绝掉既有合法用法。** 缓解：首阶段的能力校验
  严格限定在协调层**自己创建**的会话与任务上；既有显式 API 路径不受影响。
- **本决策不涉及 Executor 能力边界**：协调层的接纳、路由、投递全部落在既有
  `submit_auto()`/timer/控制面能力内；**不登记** Executor 反馈台账。
  按主体的容量份额准入属于并发容量治理，其 Executor 缺口另见
  [DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md) 与
  [`EXE-20261028-001`](../executor_feedback/ledger.md)。

## 验证方式

契约与行为验收由 [M28 计划](../plans/m28-interaction-coordinator-first-stage.md)
承载，门禁矩阵 `M28-G1`–`M28-G8`：

- `M28-G1` 输入接纳与阶段分离：接纳 → 路由 → 回答 → 任务终态四阶段各自可观察，
  阶段之间无隐式跳转。
- `M28-G2` 幂等：窗口内重复 input id 不产生第二次派发；窗口外重复返回明确过期拒绝；
  不同 input id 且内容相同**不**被判为重试。
- `M28-G3` 路由裁决：延续/补充/新建/恢复/暂停/取消/澄清/拒绝八个意图各有可复现用例；
  终态任务不被复活；歧义走澄清而非错误码；权限缺失时 fail-closed。
- `M28-G4` 结果关联：回复、追问、后台结果、失败均可追溯到 input_id + task_id；
  迟到结果不冒认其他输入；新事件自带版本标记。
- `M28-G5` 上下文归属：插话纠正只更新目标任务；不相关输入不污染其他任务约束。
- `M28-G6` Executor 纪律：取消、超时、shutdown、背压、提交拒绝各有结算路径，
  无未决句柄、无静默丢失败。
- `M28-G7` 契约四件套：公开头文件、API 手册、术语表、事件面同步。
- `M28-G8` 架构门禁：`architecture-check` 零新增违规；无自建线程/定时器。

真实模型语义质量（路由准确率、必要澄清率）不在 M28 门禁内，按 issue #111 的要求留待
标注场景与真实模型评估轮；M28 只声明管线行为指标（RULE-10）。

## 关联文档和工作项

- 工作项：[`M28：交互协调层首阶段——主体级输入接纳与最小路由闭环`](../plans/m28-interaction-coordinator-first-stage.md)
- 来源需求：[Issue #111](https://github.com/Linductor-alkaid/mira/issues/111)
- 相关决策：[DEC-002](DEC-002-public-contract-versioning.md)（公开契约版本化）、
  [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)（双路径与控制平面）、
  [DEC-045](DEC-045-agent-loop-working-context-seam.md)（薄缝注入先例）、
  [DEC-046](DEC-046-entity-vocabulary-alignment.md)（身份词表划界）、
  [DEC-058](DEC-058-mutation-ledger-scope-binding.md)（台账 scope 绑定）、
  [DEC-062](DEC-062-memory-retention-sweep.md)（窄接口定位先例）、
  [DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md)（容量预算与归档）
- 设计：[核心契约与状态机设计](../design/core_contracts_and_state_machine.md)、
  [Runtime 设计](../design/mira_runtime_design.md)、
  [Context 与 Memory 设计](../design/context_and_memory_design.md)、
  [事件与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
- 术语：[公共术语表](../project/glossary.md)
