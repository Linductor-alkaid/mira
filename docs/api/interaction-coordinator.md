# 交互协调层

> 头文件：`mira/interaction_coordinator.hpp`
> 里程碑：M28 / M29（[DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)、
> [DEC-102](../decisions/DEC-102-interaction-input-arbitration-and-delivery.md)、
> [DEC-106](../decisions/DEC-106-interaction-durable-recovery.md)）
> 状态：Active（M29 并发、澄清与恢复增量；平台/CI 状态见里程碑验收）

设计依据见[交互输入归属与投递](../design/interaction-input-delivery.md)。

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

同一分区的 `input_id` 绑定接纳时的 `(tenant_id,user_id)`；其他主体复用时返回
`InputConflict`，不返回原回执。重复回执前重验能力；撤销授权后不能通过重试读到旧结果。
`advance()` 在 ProcessCrash ack 后才发布内存状态，追加失败不提前变更阶段或归属。
已绑定输入不能重新绑定其他任务；迟到澄清不能倒退阶段。追加失败可能已提交而丢失
回执，因此参考账本会封锁后续写入，`check_writable()` 返回 LedgerUnavailable；宿主需
停止生产者、关闭并重开协调层。读取仍是最后确认的投影，不能据此盲目重试。
EventStoreInputLedger::rebuild() 成功后解除封锁；恢复存储下的队列也应一并重建。旧 accepted 记录从
`principal_ref` 恢复绑定；缺失绑定时不可通过重复提交获取回执。

去重键是宿主提供的 `input_id`，**不是内容哈希**：内容相同但 `input_id` 不同的
输入**不**判为重试。

`EventStoreInputLedger` 是参考实现：它把 `mira.interaction.*.v1` 事件写入
EventStore 的一个**专用主体级分区**（`interaction_ledger_partition()`，Mira 拥有的
常量分区，使重启后的进程无需持久化 id 即可找到同一条耐久流）。`rebuild()` 从该
流重放重建索引；`reclaim_expired()` 回收窗口已过的记录。台账索引本身有界
（`InputLedgerOptions::max_entries`）；rebuild 超出该界显式失败，不发布半个索引。
字节预算归 DEC-069 / M31。持久化配置要求 EventStore 实际提供 ProcessCrash ack，
`MemoryEventStore` 的 Buffered ack 不满足条件；测试可使用明确标注的模拟回执，生产用
FileEventStore 等有相应保证的后端。账本窗口为默认 24 小时；过期回收后的长期键保留
及历史淘汰仍归 M31，不能据此承诺无限期去重。

## 输入信封与阶段

```cpp
struct InputEnvelope final {
    PrincipalContext principal;              // 主体身份（宿主提供，已认证）
    std::string input_id;                    // 宿主提供的稳定输入标识 = 幂等键
    std::string content;                     // 输入正文
    std::vector<std::string> attachment_refs; // 工件引用（不复制内容）
    std::optional<std::string> reply_to;     // 关联另一次输入（如追问链）
    std::optional<TaskId> target_task;        // 宿主明确的任务引用
    std::optional<RouteIntent> requested_intent; // 明确新建/纠正/取消等意图
    std::optional<std::string> clarification_for; // 选定冻结候选时引用原输入
};
```

`InputStage` 是**闭集**：`Accepted` / `Routed` / `Answered` / `TaskSettled` /
`Rejected`。**收到输入不等于任务完成**——阶段之间没有隐式跳转，迁移都由
`advance()` 显式登记。`InputDisposition` 是调用方看到的结果：它带
`ControlIngressId`（Mira 内部的入口身份，与宿主提供的 `input_id` 各司其职）、
当前阶段、`duplicate` 标记、归属与可选的错误/澄清。澄清还带 `candidates`（有序
TaskId 列表），重复提交返回相同候选。该列表不是授权，后续引用仍须重新校验。
`principal_state` / `input_record` / `cancel_work` / 结果登记是受信宿主 owner 面；尤其
`input_record(input_id)` 没有主体过滤，不能直接暴露为远端按 id 查询接口。

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

仲裁优先级由 DEC-102 冻结：

1. `target_task` / `requested_intent`：宿主已确认的引用；明确 `NewTask` 不调用模型。
   其他可执行 intent 需要 task 或 reply 引用；取消直接走 Runtime 控制面。
2. `reply_to`：绑定原输入所属工作。未知引用或同时提供的 task/reply 相冲突时拒绝，
   不转入其他工作。
3. 无显式引用时才调用 resolver。建议目标必须属于主体，session 必须一致；有效建议
   才进入裁决。resolver 抛异常形成固定脱敏错误。
4. 无有效建议时，唯一活动任务 → ContinueCurrent；无活动任务 → NewTask；多个活动
   任务 → Clarify，并按注册顺序返回候选。

终态任务不会复活；延续/补充终态工作会创建 `resumed_from` 关联的新 Task，保留有界
旧约束并追加本次新要求。取消终态任务交给 Runtime 返回 NoOp；暂停终态任务拒绝。
命令结果只有 Applied/NoOp 视为成功，不能把拿到 CommandOutcome 当作成功。

回调运行在提交串行化边界内，必须有界、无阻塞、不重入写接口。模型建议不能覆盖
用户显式归属。澄清答案提供 clarification_for 和候选中的 target_task，协调层保留
原输入并追加答案，以原 input_id 投递；resolved_by 唯一认领答案，答案本身不再次入队。
宿主/主代理可把自然语言答案解析为候选引用；Mira 重新验证主体、窗口、候选及当前状态，
不据此声明模型语义准确率。两条归属记录之间崩溃时，重试可从认领修复答案回执，
不会自动补投原输入。

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
- 输入先提交 Routed 耐久记录，再进入汇。成功只表示接收进队列，不表示已读或执行。
- 返回失败必须保证未接收该输入；直接投递失败登记 Rejected，失败纠正不发布到有效约束。
  汇不得抛异常；抛异常视为不确定接收，返回 DeliveryUncertain，写入耐久诊断并封闭
  该任务后续自动投递。宿主需核对、取消/新建工作，不可盲目换汇重试。
- 协调层为每个工作项持有一个**待投递队列**：任务还没有绑定 Loop 时，输入等待
  绑定；`register_sink()` 时补投，只移除成功前缀。失败项及整个后缀原序保留；
  新输入不能通过已绑定汇越过它们，需宿主显式再次 register 补投。
- sink 只可绑定注册表内的活动任务；关闭、未知任务或终态任务的迟到注册拒绝。
  队列上限默认每任务 256，任务关联输入上限默认 4096，满时显式拒绝。控制输入的
  耐久关联可用于后续 reply_to，不因 Hot 输入 ID 列表达到上限而丢失引用。

## 持久化、回补与恢复

初始化前调用 `set_recovery_stores(artifacts, checkpoints)` 开启恢复。两个 store 都必须
提供，同时要有 EventStore。未配置时保留原显式运行模式，进程内澄清可回收，跨重启
正文恢复明确不可用；只绑定一个 store 不会静默降级。建议消费
[完整离线宿主示例](../../examples/interaction_recovery_host_consumer.cpp)。

正文和工作快照为 Sensitive、版本化 JSON Artifact，账本只保存引用、摘要与关联。
默认单个 Artifact 128 KiB、输入 ID/回复引用 256 字节、附件引用至多 64 个；恢复扫描
还受 InputLedgerOptions::max_events_scanned 和 RecoveryPolicy 限制。快照保存有界
来源闭包，任一来源正文缺失/擦除时不从摘要补造。接纳结果不确定时保留正文供重建；
已确认未接纳的参数/配额拒绝才立即清理。长期孤立产物清理及跨主体容量归 M31。

`initialize()` 枚举账本重建主体映射和必要上下文，旧工作标记 historical。上下文可用性
为 Available / MissingOrErased / Expired / Incompatible；这是上下文可用性，不能作为
动作许可。旧 Task 不注册为新 Runtime 的活动 Task，也不自动绑定汇。旧记录没有正文/
快照引用时明确不可恢复。当前活动状态仍由 Runtime 掌握。

投递状态独立于 InputStage：

| InputDeliveryState | 含义及恢复行为 |
| --- | --- |
| NotDispatched | 尚未调用汇；当前 Runtime 内可显式 resume_input 补投 |
| Dispatching | 调用汇之前已耐久记录；崩溃后不能判断是否入队，不自动重投 |
| Delivered | 汇已接收，尚不证明已读/已执行；不重投 |
| Uncertain | 异常或回执不确定；封闭进一步投递，交给宿主核对 |
| NotRequired | 控制输入或澄清答案，无第二次数据投递 |

`replay_inputs(principal, after_sequence, limit)` 是有界只读当前投影，按稳定接纳序号
翻页，默认页上限 64；返回 next_sequence/has_more/window_start/expired_gap。每页
重新校验权限与主体，只返回该主体记录，不调用 resolver、模型、Runtime 或汇。
它不是跨页事务快照，也不承诺超过输入窗口的历史回补。

`resume_input(principal, input_id)` 只显式恢复有正文的 Accepted 输入，或当前 Runtime
中 NotDispatched 的已路由输入。等待澄清时返回原候选；已投递/派发中/不确定/已结算
输入不重投。历史工作需要下述恢复许可，随后由新的用户输入关联新 Task。

`recover_work(principal, task, environment, context, verifier)` 对历史或当前已结算工作
执行 RecoveryPlanner → 新 Observe → 可选宿主 Verify。context 必须有 deadline，
取消探针应覆盖宿主停止/接管；环境必须是该工作的真实资源映射。宿主先停止旧生产者、
安全收敛在途控制。verifier 返回 true 是宿主对全部未决副作用及不明投递的核验声明，
模型文本不能直接授予它。没有核验时 report.ready_for_new_task=false，pin 保留。

恢复观察与验证使用任务流中的 RecoveryObservationRecorded / RecoveryVerificationResult，
各有 v1 schema 和 expected_through_event_sequence。CheckpointBuilder 在推进水位前
机械校验源前缀；验证还需引用当前观察及所有未决 ActionId。过期提案不能清掉后来出现的
不确定动作。TaskCheckpoint.recovery_verification_sequence 是接受见证；默认 0 兼容旧
JSON/digest，旧 reducer 保守忽略新事件。观察本身不会清除 pin。

成功许可绑定主体、上下文摘要、账本修订及源事件水位，默认 60 秒并单次消费，进程重启
不恢复该许可。续办重新验证来源、打开对应的新 session/task，resumed_from 关联旧工作；
同进程终态续办也不能绕过恢复门。动作仍受后续新观察、Runtime epoch 和资源 lease 约束。

`recovered_context_items(principal, task)` 只读有界缓存，提供不 pinned 的 P3
CheckpointSummary / UntrustedExternalData 候选。宿主将其与 MemoryAgentService 的候选
融合后交给原 ModelContextSupplier/Layer 0，不能把 checkpoint 粘到受保护的主 User/System
项。候选保留至多 16 个近期步骤、事实和动作，记录总数；默认 64 KiB，超额明确要求进一步
压缩。原目标及有界用户约束留在执行集，历史 checkpoint 可由 Layer 0 丢弃。

```cpp
coordinator.set_event_store(events, runtime_id);
coordinator.set_recovery_stores(artifacts, checkpoints); // initialize 前
const auto started = coordinator.initialize();
const auto page = coordinator.replay_inputs(principal, 0, 32);
const auto recovered = coordinator.recover_work(principal, old_task, environment, context, verifier);
// ready 后，新的 InputEnvelope.target_task=old_task 将创建关联的新 Task。
// Gateway supplier 合并 recovered_context_items 与既有记忆候选，不自行准入。
```

## 执行中插话

纠正从投递进入 Loop 队列后生效；Accepted/Routed 不表示本轮已经采用。需要立即停止
自主执行时，宿主使用确定性 cancel_work/Runtime 控制并等待安全收敛。
两类 Loop 无需 MemoryAgentService 也会检测新输入：轮边界 drain；请求派发、模型完成、
schema repair 完成及下一次 action/tool/答案准入前检查队列。旧完成被丢弃，按既有恢复
预算重建；设备循环先重新 Observe，预算耗尽显式失败，不发布旧答案或旧动作。

已经准入的单个 action/tool 沿既有取消/deadline/journal 收敛，无法追溯撤销。工具批次
中途插话时保留已执行结果，未开始项产生 `superseded_by_user_input` 的失败结果，
`executed=false`，发 ToolSkipped；不算入执行计数。原生 call/result 配对完整保留。
设备动作完成后仍 Verify；若有插话，即便旧目标已满足也不能直接宣布新任务完成。

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
| `InputAccepted` | `mira.interaction.input-accepted.v1` | `input_id`、`ingress_id`、`principal_ref`、接纳时间、窗口到期时间；可选 reply_to/target_task/requested_intent |
| `InputRouted` | `mira.interaction.input-routed.v1` | `input_id`、`ingress_id`、`intent`、`session_id`、`task_id`、`rationale`；可选 error_code/error（投递不确定诊断） |
| `InputClarified` | `mira.interaction.input-clarified.v1` | `input_id`、`ingress_id`、`clarification`、`candidates`、`rationale` |
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
`NotInitialized`、`InputConflict`、`DeliveryUncertain`、`RecoveryUnavailable`、
`RecoveryRequired`、`RecoveryStale`；新增值依次追加至 17）。

## Executor 与生命周期

协调层**不新建 Executor**：它复用宿主注入的 `kairo::Executor&`（与
`WorkflowRuntime` 的注入做法一致），上下文准备经 `submit_auto()` 提交为普通有限
任务，只捕获待装配工作项的值。结果在有界等待内成功才提交；超时不会迟写注册表，
退役 future 默认最多 16 个，满时拒绝。外部 owner 调用 `close()` 消费所有有限任务并
检查迟到错误，再关闭 Executor。旧 `shutdown()` 与析构保留尽力清理；需要错误回执的
宿主使用 `close()`。接纳队列满、提交被拒、投递失败、超时与关闭中都有明确的结算路径。

```cpp
InteractionCoordinator coordinator(config, runtime, executor);
coordinator.set_event_store(events, runtime_id);
coordinator.set_environment_provider([] { return environment; });
coordinator.set_route_resolver(resolver);
const auto started = coordinator.initialize();      // 绑定并重建耐久台账
const auto disposition = coordinator.submit_input(envelope);
// 启动 Loop 后 register_sink(task, sink)，退出时先停止输入生产者与 Loop。
const auto closed = coordinator.close();            // 消费 finite futures，检查错误
```

## 验证边界

M29 的本地验收与剩余 CI/平台条件见[里程碑计划](../plans/m29-interaction-coordinator-concurrency-and-recovery.md)。
本接口不承诺跨崩溃 exactly-once 消费，也不重放外部动作。Artifact 长期保留/擦除、跨主体
容量与遗忘归 M31；长期记忆自动生命周期归 M30；真实模型语义质量和真实设备验证单独取证。
