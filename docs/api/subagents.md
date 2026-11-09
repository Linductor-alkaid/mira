# 受管子代理

> 头文件：`mira/subagent.hpp`、`mira/conversation_loop.hpp`
> 决策：[DEC-099](../decisions/DEC-099-managed-subagent-delegation.md)
> 实施：[M34](../plans/m34-managed-subagent-delegation.md)

`SubagentCoordinator` 是单个父任务的 Harness owner。它以独立 SessionId/TaskId 创建
无观察 child ConversationLoop，用外部注入的 kairo Executor 管理 parent/child worker，
不初始化或 shutdown Executor，不创建设备 Adapter 或新的执行平面。

## 接线与所有权

先注册所有 profile，再将 `tools()` 返回的注册项加入父 BuiltinToolRegistry；调用
`tools()` 会冻结 profile 目录，并把可选名称枚举与描述放入 spawn 工具 schema。
配置完父 loop 后转移给 `start_parent()`；返回的 shared_future 必须消费。
完整离线可运行代码见[宿主示例](../../examples/subagent_host_consumer.cpp)。

```cpp
mira::SubagentCoordinator owner(executor, parent_spec, events, artifacts, runtime_id);
mira::SubagentProfile reader;
reader.name = "reader";
reader.description = "Read-only investigation";
reader.model = child_model_profile;
reader.conversation.max_turns = 4;
reader.conversation.system_prompt = mira::ConversationSystemPrompt{
    "Return findings to the parent. Do not address the user.", "host.reader.v1"};
reader.create_resources = [make_provider] (const mira::AgentLoopSpec &child,
                                         const mira::OperationContext &context)
    -> mira::Result<mira::SubagentResources> {
    mira::SubagentResources resources;
    resources.provider = make_provider(child, context); // 独占、匹配 reader.model 的 provider
    return resources; // 默认无工具、无输入供给
};
// 检查每次 Result，失败不可当成已接线。
auto configured = owner.register_profile(std::move(reader));
// configured 成功后：
for (auto &tool : owner.tools()) {
    auto registered = parent_registry->register_tool(std::move(tool.spec), std::move(tool.handler));
    // registered 失败时中止启动；配置阶段不要并发修改 registry。
}
parent_loop->set_tool_registry(parent_registry);
auto started = owner.start_parent(std::move(parent_loop), context);
// started 成功后，由外部 owner 消费结果并关闭：
auto result = started.value().get();
auto closed = owner.close();
// 检查 result/closed，然后才能释放 gateway/transport 或 shutdown Executor。
```

父 loop 仍引用宿主 gateway，宿主须保持 gateway、transport、模型回调等依赖存活到
future 结算和 close 成功。factory 同步运行在 child 的受管 I/O worker；创建过程也
须检查 OperationContext、使用可解除阻塞的网络/平台路径。provider 以 unique_ptr
移交给 child；共享 transport、工具 handler 和捕获状态的并发安全由宿主负责。

coordinator 生成的 child 是 Harness 的无观察任务身份，不会调用
MiraRuntime::open_session(environment) 或自动提交设备任务。宿主若还使用 MiraRuntime
管理父任务终态，应在该 future 结算后按已有控制面完成它，不能提前将父 Task 结算。

## 模型工具与宿主控制

| 接口 | 语义 |
| --- | --- |
| `spawn_agent(profile, prompt)` | 只接受冻结目录中的 profile，返回 session_id/task_id/execution_id 和 started；不返回最终答案 |
| `get_agent(handle)` | 读取本 parent 所属准确执行的状态、有限答案和 safe_summary |
| `cancel_agent(handle)` | 请求该执行协作取消；返回状态不代表 worker 已释放 |
| `spawn/get/cancel` | 宿主的类型化对应接口；spawn 校验调用 context 的 parent Session/Task/epoch |
| `request_cancel()` | 封准入并取消整个 root，包括 child 和父等待 |
| `request_human_takeover()` | 封准入、取消自主执行；恢复需新任务，设备宿主仍须重新观察 |
| `close(timeout)` | 外部 owner 请求停止并等待真实收尾，再 join 句柄；超时可重试 |

模型工具通过同一 BuiltinToolRegistry/schema/请求暴露边界调用，可与 DEC-097 工具选择
组合。工具 handler 只捕获 weak owner，owner 关闭后不可再执行。宿主可在 close 后用
`get()` 检查保留记录。不存在名称到权限的隐式转换，也没有任意 Session 路由。

`SubagentHandle` 绑定三个稳定 ID，不允许只用 SessionId 取消；错误或其他 owner 的
执行返回 NotFound。首期一个 child 只有一次执行，无递归、跟进消息或自动重启。

## Profile、工具和结果

profile 的 `model` 在注册时复制。factory 的 provider 必须匹配该 profile digest，
否则零 infer 失败。每个 child 构造自己的 router/gateway/账本/admission，只注册一个
模型，gateway attempt 上限固定为 1，loop 恢复消耗其有限轮数。

`conversation` 必须为 Finite，max_turns 为 1..64、每轮输出 1..16384、recoveries/
continuations 最大 8；timeout 为正且不超过 10 分钟。子角色使用完整 system 模板，
不会从父复制未结束工具轮，也不假设共享模型 KV cache。

默认 `tool_access=StaticReadOnly`：`allowed_tools` 为空表示零工具，每个 ID 必须存在、
唯一且声明 has_side_effects=false。模型不能修改 host profile、endpoint 或凭据。

`tool_access=Discover`（[DEC-100](../decisions/DEC-100-subagent-semantic-tools.md)）：
`allowed_tools` 留空，factory 不设置 `resources.tools`，而供应 `tool_catalog`。父代理
只选角色、委派目标，无需了解目录或逐项授权。child 自己根据任务含义选择工具，
目录可以含写入、发布、任务执行等副作用。目录由可信 factory 按 child goal/context
提供受限执行闭包，属于宿主能力边界；模型选择不创建新的宿主身份或资源权限。

子模型初始只看到 `discover_tools` / `load_tools`。发现结果每页最多 16 个名称、
描述和副作用标志，不带 schema；`offset` 翻页，`query` 可接 factory 的 `search_tools`
语义检索回调。未接检索时 `search_applied=false`，返回目录分页供模型按含义选择，
不会使用关键字匹配冒充语义授权。检索返回最多 256 个目录内唯一 ID。
`load_tools({names:[...]})` 替换活动集，下一次请求暴露所选原生工具的真实 schema。
已卸载工具不可在新请求调用，同批先卸载再调用也被拒绝。父上下文不接收目录、schema
和执行历史；最终摘要仍可能包含模型主动总结的内容，不是信息流隔离保证。

目录最多 4096 项、编码元数据总计 8 MiB，名称最多 128 字节、描述 2048 字节、单项
schema 32 KiB。禁止重复身份/名称和 spawn_agent/get_agent/cancel_agent/directory 控制名。
`max_active_tools` 为 1..64，实际上限还取模型工具上限减 2（目录工具占两个名额）；
Discover profile 要求模型工具上限至少 3。输入估算只计算所选 schema 和真实历史。

可选 `resources.authorize_tool(child,spec,args,context)` 在每次原生调用前运行自动策略，
可据目标语义、资源和参数判定，拒绝/异常不会执行 handler；不要求用户或父代理逐项
确认。缺省允许目录内的所选调用。需要既有 PolicyEngine/能力 grant 的宿主应在此接线，
模型文本不替代 host grant。工具须准确声明副作用，工厂负责外部资源互斥与适配。

每个副作用目录项必须提供 `SubagentTool::verify`：在 handler 返回/抛异常后，独立检查
外部后置条件，返回 Applied、NotApplied 或 Uncertain。回调收到精确参数、原始
`Result<JsonValue>` receipt，以及同一个执行 context；不能在 verifier 中重发动作。
Applied 记录已验证效果；NotApplied 给模型失败工具结果，允许重新规划；Uncertain、
验证错误/异常或记录失败封住后续工具并使 child 失败。handler 报错但 Verify 确认
Applied 时也停止并要求对账，避免模型盲目重发。未知结果不是“没有执行”。

存在 Discover profile 时，父的 `spawn_agent` 保守声明 has_side_effects=true；只含默认
只读 profile 时保持 false。委派本身可以启动写任务，不能用只读标记隐藏这一事实。

派发前要求 EventStore 的 ProcessCrash/PowerLoss 确认，Buffered store 不可执行写工具。
派发和 Verify 事件使用同一个独立 operation ID，handler 可用作幂等键。只有模型已解析
的有效调用才进入 handler；框架不自动重试或 replay。资源工厂、搜索、策略、handler
和 verifier 均在既有 child worker 内同步运行，必须合作取消、有界等待。取消不是回滚，
`snapshot.verified_effects` 和 `snapshot.effects_uncertain` 在取消/超时后仍回流给父代理。
若取消阻止外部验证，应返回 Uncertain；不得把本地取消伪装成外部未应用。


`SubagentResources::input_supplier` 可显式提供 M24 快照的渲染内容或其他任务输入，
仍遵循 DEC-053 的 User 输入纪律和供给容量；不会自动 fork/merge Working Context。
`output_schema` 可选，注册时校验 schema subset，答案须可解析并匹配该 schema。
错误结构返回 Failed，验证成功的值通过 snapshot.structured 和结果回流提供。

状态包括 Starting、Running、Answered、Failed、Cancelled、TimedOut。只有 Answered
携带 answer/structured；取消、超时或接管期间返回的迟到成功不会变成有效答案。
异常和资源工厂失败只报告稳定摘要，不回显原始异常。结果超出原始字节限制或 JSON
编码后 32 KiB 限制会明确失败，不截断成貌似完整的答案。

## 父循环等待与继续

coordinator 接管父 loop 的 continuation source 与事件存储接线，替换原有设置。
需要保留的其他宿主数据应使用独立的 ModelInputSupplier/用户输入通道接线。
每次请求前收取已完成
child，父模型拟给出无工具文本时，如果仍有 child 则等待一个结果并继续同一 run。
等待在独立 I/O worker 上，不占用默认有限任务池；20 ms 条件变量等待片段用于
观察无主动唤醒的取消探针，不是另建任务调度器。

拟终态的 Assistant 文本进入历史；可回放方言的签名 thinking 保留，规范工具历史
不被删除。结果以独立 User 项回流，来源固定为
`mira.conversation-loop.continuation.v1`，authority=Internal、文本 sensitivity=Sensitive，
明确属于任务数据；它不是用户授权或 System 指令。结构化结果不会再复制一份文本答案
到同一回流项。全部 child 已收取后才接受最终文本、原子封住新 spawn 准入。

`Answered`、run 预算耗尽、模型失败或取消结束 parent 后不重新启动旧 run/Task。
有限父轮数仍生效，宿主须给结果汇总留轮数；Continuous 模式也可作为父 loop。
先前文本终态/原始历史语义在未设置 source 时不变。

通用 `set_continuation_source()` 的回调在 request 前以 wait=false 调用，拟终态时以
wait=true 调用。后者允许协作等待；回调须有可解除阻塞的路径。每批最多 16 个非空
字符串、合计 64 KiB，错误/异常/越界 fail closed。启用 source 后有限和持续模式均
检查 max_history_items/bytes。继续本身不豁免 max_turns，不保证跨进程恢复。

## 额度、背压和关闭

SubagentConfig 默认：4 活跃执行、16 累计 child、16 活跃/未 claim 结果槽、16 KiB
prompt、8 KiB answer、128 累计模型轮次、2 Mi 输入估算 tokens、128 Ki 输出 tokens。
配置中的 0 不代表不限。max_active 不超过 16、max_children/pending 不超过 64；
结果每批按 48 KiB 编码字节限制组装。每个活跃 child 预占结果槽，完成风暴不会
临时突破容量；工具失败或启动拒绝也不会返还累计创建/预留额度。

启动时预留 `max_turns × max_input_tokens_per_request` 和
`max_turns × max_output_tokens_per_turn`；每请求核对输入估算。额度只约束委派部分，
不代表父本身的费用、真实 tokenizer 计数或供应商账单硬上限。真实币种核算仍遵循
gateway 既有 unknown/partial usage 语义，不把缺失 usage 当成零成本。

关闭顺序为封准入 → 请求 parent/children 停止 → 等待 factory/provider/loop 及产物
收尾 → 外部 owner join worker → 释放依赖 → shutdown(true)。父结果 future 要等
child 真正收尾；收到 abort 不代表释放。close 超时返回 DeadlineExceeded，依赖必须
继续存活并补跑 close，不能强杀或销毁正在访问的宿主资源。worker 内 close 返回
InvalidState，避免自等待。析构只作兜底，宿主必须显式检查 close 的结果。

每个 coordinator 最多保留 max_children+1 个 worker 注册/句柄；Executor 持有注册
直到其自身释放，coordinator 不操作内部注册表。只读 snapshot/事件/产物不重发任何
模型或平台动作。多 root 的全局容量仍由外部宿主规划，不从单 root 上限推导全局上限。

## 事件、产物与恢复边界

`SubagentParentStarted/Rejected/Waiting/Stopping/Settled`、`SubagentSpawned/Settled`、
`SubagentCancellationRequested`、`SubagentResultClaimed` 都带 schema_version=1 和
parent/execution 身份；child 事件另带 child Session/Task/execution。
`parent_task_epoch` 为保留完整 uint64 的十进制字符串。parent 事件写在
parent 会话，child loop/model 事件在 child 会话。事件不复制 prompt、profile 描述或
答案原文；成功结果以 Sensitive artifact 保存，事件记录 artifact ID/digest/字节。

创建事件写入失败阻止 child 执行；终态/claim 存储失败通过 parent future 和 close
报告 DataLoss。artifact 写失败产生明确 Failed 子结果。claim 表示已领取入父历史，
不是模型已消费或副作用恰好一次保证；后续请求可能因取消/预算而未派发。

首期不加载历史 owner、不恢复正在执行的 worker，也不自动重投已 claim 结果。
持久部署应注入现有 FileEventStore/FileArtifactStore 并按其耐久等级理解证据；
Memory store 仅适用于离线 fixture。跨进程继续、通知 outbox 和长期 child 复用归
后续扩展，不以本 API 宣称已交付。

## Discover 工厂接线示意

```cpp
profile.tool_access = mira::SubagentToolAccess::Discover;
profile.create_resources = [model] (const mira::AgentLoopSpec &child,
                                   const mira::OperationContext &context)
    -> mira::Result<mira::SubagentResources> {
    mira::SubagentResources resources;
    resources.provider = make_provider(model);
    // 按目标提供工作区/发布端点内的目录，不向父模型发送目录。
    resources.tool_catalog = workspace_directory(child.goal, context);
    // 每个写工具在目录中绑定真实外部后置条件检查 verify。
    resources.search_tools = workspace_semantic_search(child); // 可选
    resources.authorize_tool = workspace_execution_policy(child); // 可选自动策略
    return resources;
};
```

事件 `SubagentToolDenied`、`SubagentEffectDispatchStarted/Verified/NotApplied/Uncertain`
存入 child Session，schema_version=1，仅记录 operation/tool/version/spec_digest/参数摘要。
缺少终结记录的 DispatchStarted 需要宿主对账；恢复只读检查，不自动派发。若效果已经
发生但终态日志写失败，父收到 effects_uncertain=true，不能据 Failed/Cancelled 重发。

完整可运行示例：[自主发布宿主](../../examples/subagent_tool_host_consumer.cpp)，通过独立文件读取验证写入，使用 FileEventStore 保存派发证据。
