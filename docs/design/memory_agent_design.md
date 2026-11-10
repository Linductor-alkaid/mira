# 受管记忆代理与请求准入设计

> 状态：Active
> 版本：1.1
> 更新日期：2026-11-10
> 负责人：Mira Maintainers
> 决策：[DEC-101](../decisions/DEC-101-memory-agent-context-admission.md)、
> [DEC-107](../decisions/DEC-107-interaction-memory-lifecycle.md)
> 实施：[M36](../plans/m36-memory-agent-context-integration.md)、
> [M30](../plans/m30-long-term-memory-integration.md)

## 1. 目标与边界

记忆代理负责候选搜集、事件提取与语义压缩；主代理负责交互与委派。宿主保持明确的
生命周期、工作归属和权限 owner。InteractionCoordinator 裁决路由，MemoryConsolidator
裁决写入，StandardContextManager 裁决本次请求装载。记忆模型没有任意数据库写入、
System/Developer 编辑、设备动作、scope 扩张或授权提升能力。

本增量复用 M34 的 worker/continuation 模式，而不延长父任务拥有的 child 生命周期。
MemoryAgentService 的 owner 是宿主，因此任务结算后仍可显式提出记忆候选；它不唤醒
已终态任务。M29 的恢复、M30 的承诺实体和完整自然语言记忆管理、M31 容量清理、
M32/M33 注意力与自主行为仍由各自计划交付。Layer 4 未在本增量实现。

## 2. 数据与请求边界

每工作绑定 PrincipalContext、AgentLoopSpec、environment_epoch 与精确 scope allowlist。
默认 User/Session scope 通过带域分隔的内部 ID 摘要派生；tenant 为独立字段。
bind 是可信宿主 API：宿主先鉴权并确认 allowlist 的所属关系，服务验证非空、tenant
一致性和删除屏障。不能把仅知道 TaskId 当作外部用户授权。

M30 可在初始化前向协调层注入共享服务，自动采用 User + 当前 Session 白名单。
Application 映射有独立摘要域，但不会自动加入。路由提交后 stage_input 原子更新
目标、约束及 epoch 并封闭请求候选；整个输入队列接纳成功后 acknowledge_input
重验 revision，再启动一次 Recall。队列未消费、拒绝或不确定投递不等于记忆可用。
这里的接纳是 Loop 队列接纳，输入内容仍由原 Loop 的轮边界 drain 生效。

每工作可带 request_current 可信探针。协调层探针读取 Runtime 的终态/epoch 与当前
绑定 grant 的过期时间；取消、接管、环境变化和 close 使已捕获快照失效。探针异常
保守封闭。它不进行网络/存储调用，不替代宿主撤权同步；直接修改授权/终态后记录
权限仍须 service.revoke 管理。外部 owner 必须让 Runtime 活到服务全部收敛之后。

首次工作结算关闭请求投影并提出 Session scope 的 Record。只有既有已验证事件才
能产生确定性事实，模型候选仍为 Unverified，偏好仍需审批。未送达输入禁止该触发，
旧 job 尚未退出时可显式返回容量错误，不无限重试。完成作业由协调层的有限维护
调用 reap 回收，服务仍持有和消费其 future；每工作最新回执可按当前主体鉴权查询。
记忆错误不回滚已提交路由/投递或结算；受保护候选不可用时，Gateway 可明确拒绝
请求，宿主不能通过忽略失效来继续使用旧缓存。结果中的 more_pending 留给宿主处理。

每工作有独立修订号、候选缓存、历史捕获和压缩前缀；缓存最大 60 秒，且不得超过
记忆的 expires_at/valid_until。每个 Operation 绑定当时的修订号与调用上下文。
查询结果的 scope/status/sensitivity 再验证一次，失败不得倒出整个 scope。

候选供给只读取缓存，返回带 current 探针的不可变快照。Gateway 在派发前和收到结果后
检查该探针；过期、纠正、撤权、删除或关闭后，旧结果不进入后续动作。已准入的在途
请求无法追溯撤回；因此该协议不承诺撤销已经发出的 provider 内容或已发生的副作用。

快照另有 refreshable 探针：同一 task/epoch 仍活动且纠正已送达时，旧响应仅请求重建，
两个 Loop 在原恢复预算内继续。AgentLoop 先重新 Observe，schema repair 中的失效也
同样处理；未消费 tool result 保留到有效推理完成。接管、终态、删除、投递未完成或
Runtime admission 拒绝都不可自动刷新，不复活任务或绕过预算。

## 3. 完整请求的 Layer 0 接线

Gateway 在两个 Loop 的动态工具选择之后执行准入。每个候选 provider 都按实际 profile
的 context/output/tool/item 限额构建 ContextRequest，并调用 StandardContextManager。
原始消息为保护集合：保留其完整角色、顺序、provenance、thinking 和原生工具往返。
记忆只允许 RetrievedMemory/RetrievedMemory，摘要只允许
CheckpointSummary/UntrustedExternalData；二者不得 pinned，不允许携带非文本协议项。

召回条目的角色语义见 DEC-108：逐字会话回放按连续同说话人轮次展开为原生
User/Assistant 消息（`ContextItem::wire_role` 携带说话人，永不 System/Developer），
事实召回合并为单个带框架头的 user 消息。文本前缀不再是说话人的承载层，
`entry_line` 只是文本消费者的渲染器。

输出 schema、provider extra_body、请求 framing、输出预留、安全 margin、图像和工具
schema 都参与计数。Layer 0 若裁掉宿主选择的工具，Gateway 显式失败；不会静默改变
Named/Required tool 语义。每个 fallback 独立重算，实际派发带 selection_digest，结果
携带 PreparedModelContext 审计。没有记忆 supplier 时也执行完整请求准入。

默认估算为一字节/token 加现有图像/文件估计；宿主可注入校准计数器。估算质量显式
报告，不能当作所有 provider 的精确 tokenizer/vision 保证。现有完整历史若装不下，
会明确失败；本设计不会以删除保护尾部来维持表面可用。

## 4. 三类记忆工作与模型

Recall 在绑定 allowlist 中查询，模型可选择提供的记录或提出最多四个扩展词法查询；
新查询仍由服务设置 scope。返回的类型化候选最终经 Layer 0，服务缓存本身不具准入权。
选中的事实 statement 合并为单个带框架头的 user 消息（`[recalled memory v1|count=N]`），
不逐条注入裸 user 消息（DEC-108）。

Record 从该 Session 的 EventStore 有界读取增量，只消费所属 Task 的事件。模型只返回
带事件索引引用的 statement/kind。服务填充 provenance、scope 和 Unverified 等字段，
通过既有 MemoryConsolidator 提交；Preference 可能待审批，事实类验证不足会拒绝。
状态明确区分 accepted、pending、completed、failed 和待审批，不虚报“已记住”。

既有 DEC-059 对 namespaced promotion/import 的兼容行为保留；新增
ConsolidationPolicy::allow_model_import_exemption 默认为 true 以兼容旧调用方，
MemoryAgentService **强制使用 false**。严格模式保留来源标签，但不能借标签豁免
kind 验证/scope 门，也不能自称高于 Unverified。未批准修改旧 promotion 契约。

Compress 捕获一个完整历史前缀，排除调用者指定的 protected tail，检查切分处无悬空
tool call/result。模型每条摘要须引用范围内的来源；存在旧记忆 checkpoint 时必须引用
它，避免静默清空。该校验不证明语义蕴含；模型质量仍需真实场景评估。

make_model_memory_agent 为每次调用创建独占 provider、Gateway 与无工具 ConversationLoop，
独立 Session/Task，最多一个模型请求，不递归委派或执行用户副作用。模型输出经过 JSON、
字段、引用、数量与字节校验；factory/provider 必须支持协作取消和超时。

## 5. 压缩保存、回放与删除

压缩来源前缀和带引用的结果以版本化 JSON Artifact 保存，事件只记身份、计数、digest
和 artifact 引用。artifact 元数据在服务控制分区收到 ProcessCrash ack 后才发布投影。
宿主必须把 ArtifactStore 的访问限制在相同主体/scope 边界，不直接向其他用户暴露引用。

HistoryTransformer 比较完整序列化前缀，匹配才用摘要替换；其余历史原样追加。Loop
独立验证真实 protected tail，失败退回 raw replay；Gateway 仍做最终容量准入。
所有压缩数据保持 User/Public 权限，原始系统文本不会被摘要替换。

erase_scope 是可信宿主控制 API，与提交共享 commit mutex：先失效工作并持久记录 scope
摘要屏障，再清除相关投影 Artifact，最后调用既有 IMemory::erase。任何失败都显式返回，
不会报告删除完成；重建服务读取控制分区，再执行未清的投影删除。屏障保留而不带正文。
恢复不调用模型、不重新执行动作。已删除来源不得由摘要猜测重建。

本阶段对删除采用保守策略：受影响工作封闭，已删除 scope 不再允许 bind。重新在同一
scope 接收新知识所需的世代/来源水位协议尚未交付，需由 M30/M31 后续冻结；不得偷偷
清掉屏障来恢复可用性。scope 外的既有写入/擦除者必须先调用 invalidate_scope；完全
绕过服务的任意宿主写入不在这条一致性协议的保护范围。

该后续工作由 M30-09 跟踪。服务本身不把 memory scope 删除等同于原始 EventStore
历史删除；跨存储的隐私删除仍走原有治理路径，不能声称本 API 已擦除所有事实源。

## 6. 插话、任务结束与接管

input_sink 装饰现有 IUserInputSink，不另建路由器。Coordinator 投递前校验主体；装饰器
再检查绑定身份并先增加修订号、取消旧操作，再进入 Loop 队列。队列满或投递异常时
保持 input_pending，请求准入暂停，直到重试成功，避免新要求未送达却继续旧上下文。

retire 关闭 Recall/Compress 和请求投影，但允许显式 Record；revoke 同时关闭 Record，
用于接管/撤权。宿主须把 Task 终态、runtime epoch 和 Human Takeover 接到这些入口；
不会通过查记忆推测 runtime 状态。路由仍用 Coordinator 的 RouteResolver 和既有仲裁，
模糊目标仍需澄清，终态恢复仍关联新任务。

## 7. Executor、背压、错误与关闭

模型和存储等待由注入的 Executor blocking-I/O worker 承载。每工作最多一个在途操作，
work/job/记录/事件/来源字节/结果字节/投影 Artifact 数均有上限，队列满明确拒绝。
服务不维护第二套 timer/线程池；普通 query/模型超时继续使用既有 deadline。

主代理通过 memory_start/result/cancel 操作，start 不代表完成。foreground 操作可通过
continuation_source 返回有界状态报告；父 Loop 拟结束时在自身受管 I/O worker 上合作
等待，后台 Record 不阻止父任务结算。等待不占用有限池槽位。

外部 owner 消费 shared_future、定期 reap 回收已完成 worker。close 停生产者、合作取消，
等待真实执行收尾，再 join 和消费结果；超时只返回 DeadlineExceeded，owner 必须保留
服务及资源并重试，不能按超时视作强杀。最终存储和 Executor 关闭仍由外部 owner 完成。

## 8. 兼容性、验证与后续

新增公开头文件和结构成员要求 C++ 消费者重编译，无数据库表迁移或 third_party 改动。
新增控制分区事件为 mira.memory-agent.scope-erased.v1 / projection-stored.v1；普通诊断
为 started/completed/failed.v1。字段携带版本化名称，不把诊断文本当恢复事实。

验证入口是 M36 测试与 memory_agent_host_consumer：两个工作、协调层路由与纠正、
最终请求准入、真实 SQLite、独立记忆模型循环、引用造假、取消/超时/拒绝/接管/关闭。
目标平台、真实模型语义质量、长期资源增长与同 scope 删除后重新接纳仍需后续验证。
