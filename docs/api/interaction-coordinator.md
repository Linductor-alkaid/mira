# 交互协调层

> 头文件：`mira/interaction_coordinator.hpp`
> 里程碑：M28（[DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)）
> 状态：Active（M28 首阶段交付；并发归属、完整恢复与容量准入分别归 M29 / M31）

交互协调层是 Mira 的**主体级输入入口**。宿主把一次用户输入交给
`InteractionCoordinator::submit_input()`，协调层自己决定它属于哪个内部
Session/Task、准备该工作的上下文，并把结果关联回这条输入。调用方**不需要**
预先知道 `SessionId`，也**不需要**自己决定「新建还是延续」。

协调层位于 `MiraRuntime` **之上**、宿主**之下**（DEC-068 §1）。它**不实现第二套
Session/Task 状态机**：所有状态变化仍只经 `MiraRuntime` 的既有命令面发生；它也
**不创建也不运行** Agent Loop：宿主通过 `IUserInputSink` 注册面绑定已经在运行的
Loop。

```text
宿主（Mirage / 其他集成方）
  │  统一输入口：InputEnvelope
  ▼
InteractionCoordinator  ── 主体注册表、路由裁决、幂等台账、结果关联
  │              │
  │              └── IUserInputSink 注册面（task_id → 已在运行的 Loop）
  ▼
MiraRuntime（Session/Task/Action，唯一状态机 owner）
```

## 幂等台账（`IInputLedger`）

台账是**存储所有者级的窄接口**，不并入 `IMemory`，也不并入
`IMemoryRetentionSweep`（DEC-068 §4）：`IMemory` 的每个方法都绑定一个 scope，
而输入台账是主体级、跨会话、跨重启的。

- `admit(envelope)`：**原子**登记一次输入。窗口内的重复 `input_id` 返回既有条目
  并置 `duplicate=true`，**不产生第二次副作用**；窗口外的重复 `input_id` 返回
  **明确过期拒绝**（`InteractionDomainCode::InputExpired`），**不得**被当作新输入
  执行。
- `lookup(input_id)`：读取一条输入的当前记录。
- `advance(input_id, transition)`：登记一次生命周期迁移（阶段、归属、澄清、
  错误）。迁移是耐久的，阶段**永不回退**。

去重键是宿主提供的 `input_id`，**不是内容哈希**：内容相同但 `input_id` 不同的
输入**不**判为重试。

`EventStoreInputLedger` 是参考实现：它把 `mira.interaction.*.v1` 事件写入
EventStore 的一个**专用主体级分区**（`interaction_ledger_partition()`，Mira 拥有的
常量分区，使重启后的进程无需持久化 id 即可找到同一条耐久流）。`rebuild()` 从该
流重放重建索引；`reclaim_expired()` 回收窗口已过的记录。台账索引本身有界
（`InputLedgerOptions::max_entries`），字节预算归 DEC-069 / M31。

## 输入信封与阶段

```cpp
struct InputEnvelope final {
    PrincipalContext principal;              // 主体身份（宿主提供，已认证）
    std::string input_id;                    // 宿主提供的稳定输入标识 = 幂等键
    std::string content;                     // 输入正文
    std::vector<std::string> attachment_refs; // 工件引用（不复制内容）
    std::optional<std::string> reply_to;     // 关联另一次输入（如追问链）
};
```

`InputStage` 是**闭集**：`Accepted` / `Routed` / `Answered` / `TaskSettled` /
`Rejected`。**收到输入不等于任务完成**——阶段之间没有隐式跳转，迁移都由
`advance()` 显式登记。`InputDisposition` 是调用方看到的结果：它带
`ControlIngressId`（Mira 内部的入口身份，与宿主提供的 `input_id` 各司其职）、
当前阶段、`duplicate` 标记、归属与可选的错误/澄清。

**语义拒绝**（过期、越权、路由拒绝、投递失败）返回**成功的** `Result`，其
disposition 的阶段是 `Rejected` 并带 `error`；**操作失败**（未初始化、协调器关闭中）
直接返回 `Result` 的错误。

## 路由裁决

`RouteIntent` 是**闭集**：`ContinueCurrent` / `SupplementTask` / `NewTask` /
`ResumeTask` / `PauseTask` / `CancelTask` / `Clarify` / `Reject`。裁决是
**确定性函数**，输入是 `(PrincipalContext, InputEnvelope, PrincipalState)`。

宿主可以接一条**建议缝** `set_route_resolver()`（模型只能产出建议，见 DEC-068 R1）：

```cpp
using RouteResolver =
    std::function<std::optional<RouteSuggestion>(const InputEnvelope &,
                                                  const PrincipalState &)>;
```

建议**必须先通过校验**才被采纳：目标必须是该主体的任务；目标任务已终态时
「延续/补充」转为 `ResumeTask`（关联旧摘要的**新**任务），「暂停/取消」无效；
建议缺目标或目标不存在时回落到结构规则。建议被拒绝时，裁决理由里写明原因。

> **回调纪律**：建议回调在协调层**串行化自身提交**的持有期运行，因此它
> **不得**回调协调层的写接口（那会等待回调自己持有的提交锁）。与既有宿主缝
> 一样，建议回调应当有界且不阻塞。

结构规则（无建议或建议未通过校验时）：

1. `reply_to` 指向一次已知输入 → 延续该输入所属的**存活**任务（已终态则转
   `ResumeTask`）。
2. 该主体**恰好有一个**存活任务 → `ContinueCurrent`。
3. 该主体**没有**存活任务 → `NewTask`。
4. 该主体**多个**存活任务 → `Clarify`：返回一句自然语言澄清，**不是错误码**
   （DEC-068 R4）。

裁决结果带可审计的 `rationale`；终态任务**永不被复活**（R2）；紧急停止直接经
`MiraRuntime::cancel_task` / `pause_task` 的确定性控制路径，**不依赖**下一次模型
路由（R6）。

## 权限校验（fail-closed）

路由裁决**必须**经主体能力校验后才提交：缺少
`CoordinatorConfig::required_capability`、grant 过期、或 `identity_version` 与
`CoordinatorConfig::required_identity_version` 不匹配时一律拒绝，**不默认放行**
（R9）。首阶段的校验**严格限定**在协调层自己创建的会话与任务上；既有显式 API 路径
（`open_session` / `submit_task`）**不受影响**。`PolicyEngine` 本身的生产接线是
独立决策。

## Loop 输入汇（`IUserInputSink`）

```cpp
class IUserInputSink {
  public:
    virtual ~IUserInputSink() = default;
    virtual Result<void> deliver(const InputEnvelope &envelope, TaskId task_id) = 0;
};
```

- `AgentLoopInputSink` 复用 `AgentLoop::enqueue_user_message` 的有界队列与
  `ResourceExhausted` 溢出语义。
- `ConversationLoopInputSink` 复用 `ConversationLoop::enqueue_user_message`——M28 为
  `ConversationLoop` **新增**的、与 `AgentLoop` 对称的有界入队通道（continuous 模式
  是长时持续交互的主形态，只支持 `AgentLoop` 不算交付统一输入口）。
- 投递失败**必须**回传协调层并转为可见结果，**不得**静默丢弃。
- 协调层为每个工作项持有一个**待投递队列**：任务还没有绑定 Loop 时，输入等待
  绑定；`register_sink()` 时立即补投，宿主不会丢首条输入。

## 上下文归属

协调层为每个被路由到的工作项组织上下文：目标、**最新**用户约束、任务状态。
插话纠正只更新**目标任务**的后续上下文；不相关话题**不**污染其他任务的状态或
约束。`TaskSpec` 与 `TaskSnapshot` 追加了 `user_constraints` 字段（追加式，不改
`goal` 的既有语义）；协调层另有每任务的约束视图与 `context_block`。Layer 0 仍是
**请求装配期**的唯一准入转换，协调层只选择哪一份已提交快照，**不**新增第二套准入
路径。

## 事件面（`mira.interaction.*.v1`）

新增事件族是**闭集**，每个事件**自带版本标记**（`EventEnvelope::schema_version`
是死字段，不得依赖）：

| 事件类型 | schema 名 | 载荷要点 |
| --- | --- | --- |
| `InputAccepted` | `mira.interaction.input-accepted.v1` | `input_id`、`ingress_id`、`principal_ref`、接纳时间、窗口到期时间 |
| `InputRouted` | `mira.interaction.input-routed.v1` | `input_id`、`ingress_id`、`intent`、`session_id`、`task_id`、`rationale` |
| `InputClarified` | `mira.interaction.input-clarified.v1` | `input_id`、`ingress_id`、`clarification` |
| `InputRejected` | `mira.interaction.input-rejected.v1` | `input_id`、`ingress_id`、`reason_code`、`reason` |
| `InputAnswered` | `mira.interaction.input-answered.v1` | `input_id`、`ingress_id`、`session_id`、`task_id`、回复摘要 |
| `InputTaskSettled` | `mira.interaction.input-task-settled.v1` | `input_id`、`ingress_id`、`task_id`、终态 |

`principal_ref` 只放**不可逆的内部标识**（`TenantId`/`UserId` 的 hex），**不得**
放外部账号信息或凭据。事件写在**主体级专用分区**，不混入 Session/Task 流，因此
会话对话投影与 checkpoint 投影不受影响。

## 错误域

`Error::domain == "mira.interaction"`，`domain_code` 是稳定枚举
`InteractionDomainCode`（`InputInvalid`、`InputExpired`、`LedgerUnavailable`、
`LedgerFull`、`PermissionDenied`、`NoSessionAvailable`、`RuntimeCommandFailed`、
`RuntimeCommandTimeout`、`RouteRejected`、`SinkUnavailable`、`ExecutorRejected`、
`NotInitialized`）。

## Executor 与生命周期

协调层**不新建 Executor**：它复用宿主注入的 `kairo::Executor&`（与
`WorkflowRuntime` 的注入做法一致），上下文准备经 `submit_auto()` 提交为普通有限
任务，句柄由协调层**持有并消费**（超时未结算的 future 会被退役并在关闭时消费，
不丢弃）。接纳队列满、提交被拒、投递失败、超时与关闭中都有明确的结算路径。

```cpp
InteractionCoordinator coordinator(config, runtime, executor);
coordinator.set_event_store(events, runtime_id);
coordinator.set_environment_provider([] { return environment; });
coordinator.set_route_resolver(resolver);
const auto started = coordinator.initialize();      // 绑定并重建耐久台账
const auto disposition = coordinator.submit_input(envelope);
```

## 本阶段不交付

- **并发归属、指代消解与澄清回路的完整语义**（M29）。
- **跨重启的恢复执行路径**（M29）：M28 只保证台账与注册表**耐久**且**可重建**，
  重启后的重新路由与未决副作用的重观测/验证归 M29。
- **容量准入、归档与遗忘**（M31）：M28 只保证台账索引有界且**不引入新的无界层**。
- **真实模型的路由准确率与澄清率**：M28 只声明管线行为指标，真实模型评估轮另立。
