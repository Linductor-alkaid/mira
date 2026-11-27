# 记忆代理与完整请求准入

> 状态：Active
> 更新日期：2026-11-27
> 头文件：`mira/memory_agent.hpp`、`mira/model_context_admission.hpp`
> 契约：[DEC-101](../decisions/DEC-101-memory-agent-context-admission.md)、
> [DEC-107](../decisions/DEC-107-interaction-memory-lifecycle.md)、
> [DEC-108](../decisions/DEC-108-native-role-memory-recall.md)、
> [DEC-109](../decisions/DEC-109-memory-preference-approval.md)、
> [DEC-119](../decisions/DEC-119-six-memory-systems.md)
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
默认把事实 statement 合并为单个带框架头的 user 消息（DEC-108）。逐字会话回放候选
按原始说话人展开为原生 User/Assistant 消息，永不 System/Developer。

`Preference` 在同一召回框中作为 `use=advisory` 的 JSON 行出现。作用域、绑定内槽位和
验证等级随正文一起呈现，原文中的引号和换行经过转义。`current-user` 和 `current-session`
限于与工作绑定实际相同的范围，其他获准范围标记为 `authorized-scope`。包含偏好的框还会
说明这类信息可供 Mira 根据自身目标、已接下的承诺和当前任务权衡。字节额度计入这些说明
和转义后的内容，超过额度的偏好候选报告降级。见
[DEC-185](../decisions/DEC-185-advisory-preference-recall.md)。

### 模型实际使用的记忆版本

`MemoryAgentService::context_supplier()` 在召回候选旁带上记录 ID、版本与精确范围摘要。
每个快照最多带 64 项记录引用，引用通过上下文条目 ID 关联。Gateway 从实际 provider
的 Layer 0 结果中取已进入请求的版本。完成响应或达到输出
token 上限的响应通过当前性复核后，`ModelCallOutcome::memory_use` 提供回执。回执含
逻辑请求 ID、实际尝试 ID、选择摘要、记录版本及时间。候选未装入、调用失败、取消与
迟到拒绝均没有使用回执。

宿主可于使用 Gateway 前配置 `set_memory_use_sink`，通过有界回调接收回执。回调按
既有 Gateway 调用生命周期运行，负责接收和转交回执。回调失败可从调用结果诊断，宿主
仍可读取原回执。回执的去重依据是逻辑请求、精确范围、记录 ID 和版本。SQLite 宿主
在调用返回后使用 `record_memory_use()` 提交回执。已提交的计数跨重启保留，未提交的
回执由宿主负责保留与重投。见 [DEC-186](../decisions/DEC-186-admitted-memory-use-receipts.md)
和 [DEC-187](../decisions/DEC-187-durable-memory-usage.md)。

串行调用 Gateway 的宿主可以在内存中保留最近一张回执。`last_use` 须至少存活到回调注销。

```cpp
std::optional<mira::MemoryUseReceipt> last_use;
auto remember_use = [&last_use] (const mira::MemoryUseReceipt &receipt) -> mira::Result<void> {
    last_use = receipt;
    return {};
};
gateway.set_memory_use_sink(remember_use);
```

宿主消费存储的入账结果，并在存储操作超时等可重试错误后保留原回执。端到端接线见
[宿主示例](../../examples/memory_agent_host_consumer.cpp)。

### 召回分组与记录

宿主可在构造服务前配置召回组。每组独立按 kinds 查询并形成一个候选，组序即供给顺序。
kind 在全部组内唯一，单组 max_records 为正、max_bytes 至少 64。各组配额之和分别
受 max_records 与 max_result_bytes 限制。配额覆盖初始及模型补充查询，max_bytes
包含最终框架。每个查询文本的组调用共享最多 250ms 的存储软时限，返回的部分结果或
文本裁剪通过 degraded 可见。显式存储错误与硬截止通过 future 返回。全部使用工作既有
scope、标签和撤权探针。组内所有 kind 允许语义检索时才使用该文本的查询向量；关键词
召回沿现有策略。空配置保留默认行为。
`SkillEnvironment` 范围从 Workflow 的宿主授权入口读取。见
[按环境隔离技能记忆](workflow-contracts.md#按环境隔离技能记忆)。

```cpp
config.recall_groups = {
    {{MemoryKind::Episode}, 4, 4096},
    {{MemoryKind::EnvironmentFact, MemoryKind::ApplicationFact}, 6, 8192},
    {{MemoryKind::Procedure, MemoryKind::SkillHint, MemoryKind::RecoveryLesson}, 6, 8192},
};
```

Record 必须显式 write_scope，来源从 EventStore 读取，不接受模型伪造的事件正文。
`record_page_limit` 默认 1，最大为 `kMaxRecordPagesPerJob`。协调层在结算后使用上限 8。
多页共用同一 EventStore 快照、截止时间和作业回执。每页提交后更新在途工作水位，结果
汇总各页处置。`more_pending` 为真时可以继续提交 Record。后续页失败时，已成功页仍提交，
重试从其水位续跑。只提供旧分页接口的事件适配器每次仍处理一页。见
[DEC-141](../decisions/DEC-141-automatic-record-pages.md)。
支持事件切片时按绑定 Session/Task 读取，每个精确写入 scope 独立保存同轮扫描水位
和上界，跳过记忆服务自身的审计事件。默认在进程内维持水位，旧 EventStore 后端可
通过 `UnsupportedCapability` 回退到会话分页。见[事件切片](events-recovery.md#有界事件切片)。

以 `MemoryAgentConfig::durable_record_pages = true` 启用持久页回执。宿主须提供支持
索引读取和 `ProcessCrash` 确认的事件库，并对同一工作维持唯一写入 owner。Record 在
完成整页处置后确认回执，再推进水位。重开后在相同的主体、Session、Task 及 scope
绑定下可继续先前的固定快照。待审批、候选截断或管线存取失败的页仍待处理，结果保留
处置报告并设置 `more_pending`，`through_event_sequence` 指向最近已确认的水位。回执
存储错误和不一致的水位通过失败结果返回。已擦除 scope 的屏障继续拒绝 Record。
见 [DEC-157](../decisions/DEC-157-scoped-durable-record-pages.md)。
结果 consolidation 给出 Applied、PendingApproval、Rejected 等状态；待审批 mutation
只有经过宿主当前权限/删除屏障复核后，才可通过既有 apply_pending 提交。

模型和宿主处理器的 Record 提议按行核对类型、陈述、结构化主张和来源引用。同页的有效
提议继续经 `MemoryConsolidator`。`MemoryAgentResult::record_pages` 用 `after`（不含）和
`through`（含）定界已检查的事件，`rejected` 给出该页原提议的零起算行号和原因。
待处理页的作业水位 `through_event_sequence` 仍表示上次已提交处。拒绝总数由
`rejected_record_rows` 汇总，也写入持久回执的 `rejected_model_rows`。工具结果中的
`record_rejections` 和 `truncated_candidates` 分别说明提取拒因和合并容量。诊断省略
候选正文。整份响应的格式、模式或预算错误仍作为作业错误返回。
见 [DEC-177](../decisions/DEC-177-record-candidate-evidence-report.md)。

结算后的 Record 可直接从已提交的设备循环完成事件、会话回答事件提取 `Episode`，无需
配置提取模型。新事件记忆保留原事件 ID 与发生时间。候选处置及工具状态带 `kind`，
持久页回执的 `by_kind` 按类型汇总生效及拒绝数量。见 [DEC-158](../decisions/DEC-158-loop-settlement-memory.md)。

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

## 汇总六类写入结果

授权宿主可用 `mira/memory_recording_report.hpp` 在完成各所有者的操作后汇总回执。

```cpp
MemoryRecordingReport recording;
recording.collect(record_handle.result.get());
recording.collect(social_attribute_result);
recording.collect_world_fact(world_fact_result);
for (const auto &attempt : recording.attempts()) {
    publish_recording_status(attempt);
}
```

`attempts()` 按收集顺序提供六类入口结果，`totals(system)` 按类别汇总候选处置。
`counts` 区分实际生效 `applied`、无变更
`unchanged`、待审批、拒绝、未解析及截断。`replayed` 是已经解释为无变更或待审批的
幂等重放子集。人格建议的待审回执计为待审批。事件入口还保留 Record 的 `more_pending`。
入口报错保留 `ErrorCode` 和原错误的 `retryable` 标记，供宿主一起查看已完成的其他结果。
类型化回执中的证据、具体记录和版本仍由原入口提供。`memory_result` 与宿主接口共用
Record 的计数语义，发布在 `recording_counts`。见 [DEC-178](../decisions/DEC-178-memory-recording-receipts.md)。

### 按批处理六类候选

`mira/memory_recording_batch.hpp` 为宿主提供已归类候选的处理入口。安装包消费者链接
`Mira::core`、`Mira::state_store` 和 `Mira::workflow`。宿主在交互结算及输入对账后提供
Record 的当前工作和范围，其他类提供各自的证据定位及当前授权。包含 Record 的批次运行在
宿主已有的 Executor 阻塞 worker 上，以便提取处理器使用有限任务池。下例在该 worker
内部处理一个 Record 和一组已索引消息的画像候选。

```cpp
MemoryRecordingOwners owners{*events};
owners.event_memory = &memory_agent;
owners.social = directory.get();
owners.authorize_social = authorize_indexed_message;
const std::array<MemoryRecordingCandidate, 2> candidates{
    EventRecordingCandidate{session_scope, kMaxRecordPagesPerJob},
    SocialAttributeRecordingCandidate{environment, message, validated_attributes},
};
const auto batch = record_memory_batch(owners, candidates, operation);
```

同一批次复用当前会话及操作，也保留 Record 的可选版本约束。社交授权回调按具体环境和
消息检查宿主的当前权限；宿主维持所有者及授权依赖直到结果返回。批次上限为 32 个条目，
超限直接返回 `ResourceExhausted`。`outcomes` 按原顺序保留已处理条目的类型化回执或
错误，`report` 提供同顺序汇总。某条出现错误时仍可处理后续条目。操作取消或到期时停止
发出后续写入，`interruption` 说明原因，`outcomes.size()` 指向第一个尚未处理的位置。
候选的证据校验、幂等和待处理状态沿各所有者契约。默认触发及重启续办归 M30-38。
见 [DEC-179](../decisions/DEC-179-memory-recording-batches.md)。

## 修订、删除与关闭

current 失效但 refreshable 为真、且 Runtime 仍接纳同一 task/epoch 时，Gateway 报告
context_refreshable。两个 Loop 在原恢复预算内重建请求，设备 Loop 重新观察；取消、
接管、终态、删除或投递未完成均不可刷新。schema repair 也遵守此检查。
schema repair 保留原来的 policy、goal、观察与未消费工具结果，再附加有界校验摘要，
不引入原始无效模型输出，也不剥掉任务框架来逃避预算。

revise 更新 task/environment epoch 并失效缓存；input_sink 会在成功入队用户纠正前
自动失效旧操作。投递失败保持暂停请求准入。retire 封闭请求装配但保留显式 Record；
revoke 用于接管/撤权，关闭全部模式。宿主必须同步 Runtime 的生命周期变化。

外部 owner 在 terminal Record 结算后调用 `release_work(task)` 回收绑定槽。
活跃绑定或仍有在途作业时返回 InvalidState。保留的作业回执已结算，后续通过服务查询
会报告绑定已失效，宿主仍按既有流程消费 future 并 `reap()`。Task ID 由 Runtime
唯一分配，宿主释放后不得再次绑定该 ID。已擦除 scope 的持久屏障继续生效。

`ModelContextSnapshot::prepend_history` 允许把保留原生角色的召回轮次装入 System /
Developer 前缀与当前首个 User 之间，默认关闭。普通记忆候选继续按现有位置追加，
宿主须保持每组历史的序号及角色顺序。最终预算、审计和 digest 由 Gateway 按实际
wire 请求结算。预算只容纳部分原生历史时，整组省略并重新分配给其它可选候选。
启用时，原请求必须以 User 开始对话，并保留可选的宿主指令前缀。

erase_scope 是已鉴权宿主 API：持久屏障、派生 Artifact 删除、既有 IMemory::erase
共用提交串行化边界。失败不表示已经删除。受影响工作封闭，删除 scope 在重启后仍
不可重新绑定；同 scope 重新接收新知识的世代协议留待 M30/M31，不得手工抹掉屏障。
out-of-service mutation 必须先 invalidate_scope；ArtifactStore 不能对未授权主体开放。

启用 `MemoryAgentConfig::conversation_index` 时，Session scope 的擦除还清理持久
会话片段并提交该索引的重建屏障。初始化会重放持久意图。宿主在回放成功后才开放
片段检索，并在关闭 Executor 前关闭索引。见[持久片段接线](durable-memory-host.md#持久会话片段索引)。

外部 owner 定期 reap，保留并消费操作 future；close 后才关闭存储/Executor。close 超时
没有强杀语义，资源须保留并重试。服务不负责启动/关闭 Executor，也不自动恢复旧模型
请求、worker 或副作用。max_works/max_jobs/max_projection_artifacts 达限均明确拒绝。

宿主交给 `ModelContextSupplier` 的每个 `ContextItem` 使用请求内唯一的 ID。源 `asset_id`
可标识同一资产的多条片段，拆分时按资产及片段位置派生各自的 `ContextItemId`。准入错误
附带从零开始的候选序号和拒绝原因，便于在宿主原始候选数组中定位。

## 模型候选策略与兼容性

`make_model_memory_agent` 以独立任务身份经 Gateway 发起单次无工具请求。Recall、Record
和 Compress 使用各自的 strict JSON schema，所选模型 profile 需要声明该能力。Record 按
当前事件页的局部序号引用来源，未知 kind 和无效引用的候选逐项丢弃，结果和诊断报告数量。

Record 提议可带 `subject` 和 `value`。前者标识陈述所指的主体和属性，后者是该属性的
取值。内置模型需要为这两个字段各给出字符串，缺少稳定身份时两个字段都留空。宿主自定义
处理器可通过 `MemoryAgentMemory::claim` 提供同样的信息。同一作用域和类别中的同值陈述
合并证据，原句继续用于显示和词法召回。

宿主可通过 `MemoryAgentConfig::consolidation.allow_unverified_session_facts` 启用
有事件出处的会话 `EnvironmentFact` 和 `ApplicationFact` 候选。写入保持 `Unverified`，
候选仍经过既有审批策略。默认验证下限拒绝此类候选，报告中的原因是 `kind-policy`。
召回文本标注未验证事实。事实和偏好可配置不同的召回组。见
[DEC-170](../decisions/DEC-170-session-fact-admission-and-claim-identity.md)。

省略模型处理器时，Record 继续使用确定性规则。宿主可通过结果和完成诊断中的
`deterministic_record_only` 识别这一模式。它覆盖已有确定性事件规则，自然语言抽取可由
内置或宿主自定义的处理器提供。见 [DEC-169](../decisions/DEC-169-memory-agent-structured-proposal-output.md)。

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

Work 绑定可携带 `labels`（any-of 预过滤，DEC-112）：该 work 的每个 Recall 查询只召回携带至少一个所列标签的记录；边界在 bind 时拒绝。

可选 request_current 是可信宿主的有界、非重入、无 I/O 探针。false/异常会封闭
Recall/Compress 与缓存请求快照；Record 在 terminal 后沿用显式权限，因此必须用
revoke 彻底撤销写入。探针捕获对象须活到服务关闭，不能捕获已销毁的宿主裸指针。
统一交互接线见[协调层 API](interaction-coordinator.md)。

## M30：长期偏好提案、预览与决定（DEC-109）

这些是有界同步 I/O API。宿主在注入 Executor 的有限任务中调用并消费 future，先停止
生产者并消费这些有限任务，再 close 服务和存储。不能在持有服务锁的回调中重入，
不能从服务自己的 worker 中做 owner teardown。I/O 依赖 Adapter 的有界返回，deadline
是合作检查，不是抢占或撤销已提交写入的承诺。

```cpp
// 规范接口片段。context 含绑定的 task/session/epoch 与有效 deadline。
PreferenceProposalRequest request;
request.id = MutationId::generate(); // 保留到本次提案结算；同 id 不能换正文/主体。
request.statement = "answer.style=brief";
auto prepared = executor.submit_auto([&] {
    return service.propose_preference(request, context);
});
auto proposal = prepared.get(); // 检查 Result；此时没有写入 MemoryRecord。
// 将 proposal 的 statement / previous / digest 交给已认证用户预览。
// 只有宿主收到明确的用户决定后才执行以下阶段；模型输出不是用户确认。
auto confirmation_context = make_control_context();
confirmation_context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
// 使用此次 UI 操作的取消信号和最新 principal，不复用原 Task 已过期/已取消的 context。
auto decided = executor.submit_auto([&] {
    return service.decide_preference(current_principal, proposal.value().id,
        proposal.value().digest, ConfirmationDecision::Approve, confirmation_context);
});
auto receipt = decided.get(); // 检查 Result 和 state，不能将提交/超时当作未写入。
```

提案要求活动 work 的精确 User scope 白名单；固定 kind=Preference、verification=
Unverified，正文为至多 4096 字节的词法 key=value。自由语义规范化由主代理/宿主完成。
同 key 不同值形成待确认 Supersede，previous 与 mutation 一起冻结，UI 必须展示修改
目标和版本；可选 request.target + expected_version 必须成对且匹配当前记录。

`preference_proposal(principal, id, context)` 用于重开后的预览，授权与决定一致：
非 Anonymous 的当前身份、tenant/user/identity_version 与提案一致，持有有效
`mira.memory.manage` grant，grant.version 与身份版本相等。resource selector 可为空
（主体自己的偏好），或精确 `{type="memory", scope=<User subject_id>, pattern=""}`。
其他/wildcard selector 不被隐式接受。宿主应重新读取当前认证结果，不能复用原任务
保存的旧 grant 或把记忆里的授权文字当成 PrincipalContext。

若配置 ArtifactStore，`tools()` 在旧三个工具之后新增 `memory_propose_preference`：
参数为 statement，以及可选 request_id/target/expected_version。省略 request_id 时
从本次 tool operation 派生；工具仅返回 proposal_id、digest、状态和需人工审批标志，
不提供确认/拒绝工具。普通前台发言、模型 done 或模型的“用户已同意”不能调用确认面。
User/Assistant 原生召回与 `[recalled memory v1|count=N]` 合并块沿用 DEC-108。

状态与结果：

| 状态 | 含义 |
| --- | --- |
| Pending | mutation 已冻结、引用已耐久登记，尚未写入 MemoryRecord |
| Approving | 审批意图已登记；写入/完成回执可能未结算，不能宣称未写入 |
| Applied | 冻结 mutation 经 apply_pending 提交，applied 给出 record/version；这是历史回执 |
| NoChange | 同 claim 已有验证等级不低的记录，本次未修改，也未提高验证等级 |
| Rejected / Stale | 用户拒绝或 scope 外部变更使提案关闭，清理提案正文 |
| Erased | scope 删除屏障已使该提案不可用，不能从它重新提取内容 |
| Expired | 待确认窗口已过期，不能再批准；只返回元数据 |

批准是授权保存，不将模型提案升级为 HumanConfirmed。纠正 mutation 的 reason 是
HumanCorrection，但仍先经过强制审批（DEC-115 后该 API 的强制审批语义不变——变的只是
Consolidator 管道的 kind 审批默认）；原记录保留 Superseded 版本链。冲突不改写目标，
必须重新提案。拒绝未结算 Approving 不被接受，先由当前授权宿主核对/幂等重试。

默认保留 256 个回执（含已完成），确认窗口 24 小时；窗口不晚于 previous 的到期/
valid_until。过期后不返回冻结正文，即使终态回执仍可查看。物理回收与回执 GC 归 M31，
当前不声称凭 TTL 已释放所有 Artifact 字节。已完成结果也不代表该偏好仍是当前值。

正文和完整 mutation 仅存在 Sensitive Artifact。控制分区的版本化
`preference-proposed` / `preference-approval-intent` / `preference-applied` /
`preference-rejected` / `preference-invalidated` 事件只有身份、摘要、引用和结果元数据，
均要求 ProcessCrash ack。失败/弱 ack 封锁偏好管理面；关闭并重开相关存储及服务后
显式重试。原 mutation id 保证 SQL 幂等，不自动恢复 worker 或自动批准。过期的
Approving 需宿主核对，不能以过期为由声称先前没有写入。

同 scope 的请求快照在批准写入前失效。服务外写入必须先成功 invalidate_scope，并
收敛相关并发写入；它会耐久作废待确认提案。resolve Approving 后才允许该操作。
scope erase 与重开会清理关联提案 Artifact，包括已批准回执的正文；已被调用方持有
的预览副本不能再次作为准入或审批凭据。scope 删除后仍不可重新绑定，M30-09 未开放。
承诺调度和前端接入各自由 M30 的对应工作项验收。

### 从当前用户输入准备偏好意图

输入成功路由且已被当前工作的输入队列接纳后，宿主可调用
`InteractionCoordinator::prepare_memory_intent(principal, ingress_id, input_text, operation)`。
使用提交输入得到的 `ingress_id` 和完整原文。此入口在 Executor 有限任务中执行，
`OperationContext` 包含当前工作和截止时间。

| 意图 | 明确请求示例 | 结果 |
| --- | --- | --- |
| 记住 | `请记住偏好：answer.style=简洁` | `PreferenceIntentRequest`，走既有审批 |
| 纠正 | `Please correct preference answer.style=详细` | `PreferenceIntentRequest`，含准确记录 ID 和版本 |
| 忘掉 | `忘掉偏好 answer.style` | `MemoryForgetRequest`，含准确记录 ID 和版本 |

也支持 `remember preference`、`correct preference`、`forget preference`，中文前缀可带「请」。
命令须独立成一行，偏好键使用字母、数字、点、下划线或连字符。普通文本与完整引用返回
空结果。识别到命令但格式不完整时返回 `InvalidArgument`。纠正或忘掉找不到当前记录时
返回 `NotFound`，多个匹配时返回 `VersionConflict`，宿主可以要求澄清。

准备不会提交写入。按返回类型交给 `submit_preference_intent` 或 `submit_forget_intent`。
首次提交要求来源仍是当前已接纳的输入。保存返回的结构化请求以便重试，尤其是逐记录遗忘
所用的操作 ID 和版本。原文摘要只在当前工作保留，持久回执沿原有确认及授权契约。见
[DEC-184](../decisions/DEC-184-evidenced-preference-input-intents.md)。

### 显式的记住与纠正

宿主从已认证的当前工作构造 `PreferenceIntentRequest`，在 Executor 有限作业内调用
`InteractionCoordinator::submit_preference_intent(principal, request, operation)`。入口从
工作绑定得出用户范围，复用该范围的 `mira.memory.manage` 授权和 DEC-109 的审批回执。

```cpp
PreferenceIntentRequest intent{PreferenceIntent::Remember,
    {proposal_id, "answer.style=brief", {}, {}}};
const auto staged = executor.submit_auto([&] {
    return coordinator.submit_preference_intent(principal, intent, operation);
}).get();
```

`Remember` 接纳新偏好，已知同值返回 `NoChange`。现有值不同或匹配存在歧义时返回
`VersionConflict`。`Correct` 使用确切的记录 ID 和当前版本，替换同一偏好键的值。
相同意图和 ID 可恢复原提案，重复值保留原记录版本和出处。新的或更正后的偏好须由宿主
使用已有 `decide_preference` 结算。见 [DEC-180](../decisions/DEC-180-explicit-preference-intent.md)。

### 查找纠正或遗忘的目标

宿主从用户请求提取主张键后，可在 Executor 有限任务内调用
`InteractionCoordinator::resolve_memory_target`。例如「把回复风格改成详细」的目标选择器为
`MemoryTargetSelector{MemoryScopeKind::User, MemoryKind::Preference, "answer.style"}`。
`OperationContext` 必须含当前工作和截止时间。服务复核 `mira.memory.manage`，从绑定推导
精确的 User 或当前 Session 范围，只查询该范围和类型的当前记录。

唯一匹配返回 ID 和版本。空结果表示当前范围和类型无匹配，`VersionConflict` 表示多个 ID
匹配，宿主应澄清目标。精确解析需要存储适配器支持，即使正文相同的记录也分别计数。已有
结构化偏好入口继续支持旧适配器。解析后将得到的 ID 和版本交给对应的纠正或遗忘入口，
这些入口会复核授权和版本。遗忘续办使用原操作 ID 与原目标。见
[DEC-183](../decisions/DEC-183-exact-memory-intent-target.md)。

### 显式忘掉一条记录

当前工作已解析出目标记录后，宿主在 Executor 有限任务中调用
`InteractionCoordinator::submit_forget_intent`。服务从工作绑定推导 User 或 Session
范围，要求现行 `mira.memory.manage` 授权及带截止时间的 `OperationContext`。
在同一操作 ID 下提交相同目标可以续办。

```cpp
const MemoryForgetRequest request{forget_id, MemoryScopeKind::User, record_id, expected_version};
const auto receipt = executor.submit_auto([&] {
    return coordinator.submit_forget_intent(principal, request, operation);
}).get();
```

服务失效受影响的回忆及压缩缓存，清理引用目标 ID 的偏好提案正文，并清理只标记了范围
依赖的压缩工件。待处理期间，相关工作的记忆作业暂停。完成时返回存储的擦除状态、计数
和重放标记。提交或控制回执失败时用同一 `forget_id` 核对，重开后由当前授权宿主续办。
原始事件和会话发言索引保留各自的删除入口。见
[DEC-182](../decisions/DEC-182-host-record-forget-intent.md)。
