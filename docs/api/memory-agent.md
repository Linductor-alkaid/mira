# 记忆代理与完整请求准入

> 状态：Active
> 更新日期：2026-11-10
> 头文件：`mira/memory_agent.hpp`、`mira/model_context_admission.hpp`
> 契约：[DEC-101](../decisions/DEC-101-memory-agent-context-admission.md)、
> [DEC-107](../decisions/DEC-107-interaction-memory-lifecycle.md)、
> [DEC-108](../decisions/DEC-108-native-role-memory-recall.md)
> 设计：[记忆代理](../design/memory_agent_design.md)

## 宿主接线

宿主先认证 PrincipalContext、确定工作归属与 scope allowlist，再 bind。服务验证 tenant
一致性，allowlist 的完整授权仍是可信宿主责任。以下为接线片段，依赖由宿主创建并保持
存活；完整可运行版本见[离线示例](../../examples/memory_agent_host_consumer.cpp)。

```cpp
MemoryAgentConfig config;
config.artifacts = artifacts; // Compress 必须提供，继承工作的访问控制
MemoryAgentService memory_agent(executor, memory, events, runtime_id,
    make_model_memory_agent(executor, memory_model_config), config);
auto initialized = memory_agent.initialize();
// 检查 initialized，再为通过认证的每个工作调用 bind(binding)。
gateway.set_context_supplier(memory_agent.context_supplier());
loop.set_history_transformer(memory_agent.history_transformer(spec.task_id));
loop.set_continuation_source(memory_agent.continuation_source(spec.task_id));
auto sink = memory_agent.input_sink(spec.task_id,
    std::make_shared<ConversationLoopInputSink>(loop));
auto registered = coordinator.register_sink(spec.task_id, sink);
// 检查 registered；将 tools() 注册到主代理的授权工具 registry。
```

ModelGateway 始终执行最终 Layer 0 准入，即使没有设置 supplier；supplier 本身只能读取
有界缓存。每个实际 provider/fallback 单独核算完整请求，结果的 context_admission 含预算
和 per-item 审计，派发携带 context_selection_digest。工具无法装下或保护集合超限时明确
失败；不会静默裁掉 selected tools、用户要求或工具结果。

## 记忆操作

MemoryAgentRequest 指定 Recall/Record/Compress 和目标 TaskId。OperationContext 的
session/task/epoch 必须与 binding 一致。submit 返回 MemoryAgentHandle，必须消费
result；调用 result(operation) 是零等待查询，空 optional 代表 pending。

Recall 的 query 只在绑定 scope 中检索，返回类型化候选，实际装载由 Gateway 再裁决。
选中的事实 statement 合并为单个带框架头的 user 消息（DEC-108）；逐字会话回放候选
按原始说话人展开为原生 User/Assistant 消息，永不 System/Developer。
Record 必须显式 write_scope，来源从 EventStore 读取，不接受模型伪造的事件正文。
结果 consolidation 给出 Applied、PendingApproval、Rejected 等状态；待审批 mutation
只有经过宿主当前权限/删除屏障复核后，才可通过既有 apply_pending 提交。

Compress 指定 history 与 protected_items。history 须为完整 User/Assistant 协议，前缀
也必须完整。压缩前缀和模型引用结果存为 Artifact，返回 projection_artifact；缺少
ArtifactStore、无耐久元数据确认、引用越界或来源不匹配都不会发布压缩投影。

tools() 提供 memory_start、memory_result、memory_cancel。工具参数不接受 tenant/
subject 字符串；Record 的 scope_index 只能选择宿主 allowlist 槽位。工具 Compress 使用
history_transformer 最近捕获的历史，默认保留最近 16 项并后退到协议边界；Loop 仍验证
实际 protected tail，不满足时回退原始历史。精确控制可由宿主直接 submit 指定保护项数。

foreground=true 的操作可以通过 continuation_source 自动回流有界状态报告。父 Loop
等待必须运行在受管 I/O worker，不能把长等待塞到有限任务池。非 foreground 的后台
提取不会拖住父任务终态。

## 修订、删除与关闭

current 失效但 refreshable 为真、且 Runtime 仍接纳同一 task/epoch 时，Gateway 报告
context_refreshable。两个 Loop 在原恢复预算内重建请求，设备 Loop 重新观察；取消、
接管、终态、删除或投递未完成均不可刷新。schema repair 也遵守此检查。
schema repair 保留原来的 policy、goal、观察与未消费工具结果，再附加有界校验摘要，
不引入原始无效模型输出，也不剥掉任务框架来逃避预算。

revise 更新 task/environment epoch 并失效缓存；input_sink 会在成功入队用户纠正前
自动失效旧操作。投递失败保持暂停请求准入。retire 封闭请求装配但保留显式 Record；
revoke 用于接管/撤权，关闭全部模式。宿主必须同步 Runtime 的生命周期变化。

erase_scope 是已鉴权宿主 API：持久屏障、派生 Artifact 删除、既有 IMemory::erase
共用提交串行化边界。失败不表示已经删除。受影响工作封闭，删除 scope 在重启后仍
不可重新绑定；同 scope 重新接收新知识的世代协议留待 M30/M31，不得手工抹掉屏障。
out-of-service mutation 必须先 invalidate_scope；ArtifactStore 不能对未授权主体开放。

外部 owner 定期 reap，保留并消费操作 future；close 后才关闭存储/Executor。close 超时
没有强杀语义，资源须保留并重试。服务不负责启动/关闭 Executor，也不自动恢复旧模型
请求、worker 或副作用。max_works/max_jobs/max_projection_artifacts 达限均明确拒绝。

## 模型候选策略与兼容性

make_model_memory_agent 创建独立无工具 ConversationLoop，每次独占 provider，一个
请求上限、合作取消和 JSON 提案校验。它只管理资料，不拥有设备或写库工具。

MemoryAgentService 强制关闭 allow_model_import_exemption：即便提案携带 namespace，
也先降到 Unverified 并检查 kind floor/scope。旧 ConsolidationPolicy 调用默认维持
DEC-059 的 namespaced import/promotion 兼容行为。该开关只给可信宿主配置，模型不能
更改；不将使用频率、引用存在或模型自评当成事实验证。

默认 token 估算不是精确 provider count。Linux fixture 之外的平台、模型质量和长期性能
证据见[M36](../plans/m36-memory-agent-context-integration.md)，不能从本 API 推定。

## M30 输入帧与内部 scope

`memory_user_scope`、`memory_session_scope` 和 `memory_application_scope` 分别使用
版本化摘要域，tenant 为独立字段。Application 参数必须是宿主稳定内部标识，不是
模型提供的任意 scope，也不会自动赋予访问权。这些映射函数不负责鉴权或验证 ID；
调用方必须先验证 principal/Session/Application 归属，再把精确白名单交给 bind。

`stage_input(MemoryWorkBinding)` 返回新 revision，封闭当前候选并取消旧作业；可创建
绑定或更新同主体、同 Session、相同 scope 集的活动工作。不得回退身份版本、任务/
环境 epoch，不能复活已 retire/revoke 的工作。user_constraints 有界并传入语义工作
的独立字段，仍为待解释的数据；不会提高其 authority 或 verification。
`acknowledge_input(task, revision)` 只开放对应的当前帧，旧 revision 明确失败。

可选 request_current 是可信宿主的有界、非重入、无 I/O 探针。false/异常会封闭
Recall/Compress 与缓存请求快照；Record 在 terminal 后沿用显式权限，因此必须用
revoke 彻底撤销写入。探针捕获对象须活到服务关闭，不能捕获已销毁的宿主裸指针。
统一交互接线见[协调层 API](interaction-coordinator.md)。
