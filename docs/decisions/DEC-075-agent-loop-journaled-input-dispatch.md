# DEC-075：AgentLoop 输入经宿主租约与 Journal 派发

> 状态：Accepted
> 日期：2026-11-05
> 负责人：Mira Maintainers
> 冻结里程碑：2026-11 运行时接线维护轮
> 替代/被替代：无；落实 DEC-003，延续 DEC-018、DEC-074

## 背景与问题

AgentLoop 原先发送尽力而为的 ActionDispatched 后直接 execute，无法保证副作用前
有 durable start。Runtime 的 begin_operation 是控制面操作登记，不是覆盖输入全过程
的 ActionLease；不能把它当作设备资源租约。模型 admission 也不能替代动作派发准入。

## 决策

### 1. 所有权与边界

新增公共 `JournaledActionDispatcher`，由宿主在 run 前经
`AgentLoop::set_action_dispatcher` 注入。Dispatcher 持有宿主提供的 IEventStore，
每次 dispatch 以同 runtime/session 建立仅含一个动作的 ActionJournal，
不创建 Executor、线程或存储实现。
每次有界派发产生一个 ActionId，使用编译后 InputSequence（display、顺序、kind、
payload、duration）的摘要；不持久化输入原文或供应商/平台回显文本。

宿主提供 ActionLeaseAcquirer，返回唯一拥有的 IActionDispatchLease。宿主负责租约
资源互斥、任务与环境 epoch、权限/确认、有效任务生命周期；租约析构无异常地释放
预留。`validate` 在 durable ack 后执行，重新拒绝已取消/过期/被接管/epoch 变化的
许可。租约预留到执行结果完成记录才释放。准入撤回必须同时取消 OperationContext
并调用环境 interrupt；Adapter 在最终输入路径仍须检查取消，不能将一次 validate
描述为对任意外部宿主的抢占/原子平台输入保证。

主循环使用 spec 的 session/task/task_epoch 绑定动作 context，保留原 context 的
取消与 deadline。Dispatcher 检查 session 绑定及非 nil 身份，lease 校验任务和
观察时 environment_epoch。观察后 epoch 变化由宿主 lease 识别，不额外猜测设备状态。

### 2. 提交时序与失败

```text
compile input -> prepare -> acquire host lease -> durable start ack
 -> cancellation/deadline + lease revalidation -> execute
 -> durable receipt / uncertain -> release lease -> Observe/Verify
```

存储/准入缺失、弱 ack、拒绝、取消、超时或租约失效均不可调用 Environment。
start 后失败保留记录，重建时按可能执行处理。Environment 错误或异常记录 uncertain；
结果记录失败向调用方返回结构化错误，尝试补 uncertain，但即使补写失败也保留 start。
所有前置/后置日志提交的失败都不能被普通 void emit 吞掉。

Unknown、Dispatched、side_effect_may_have_occurred（包括 Rejected+true）均写
uncertain。Completed 和无副作用的 Rejected 才写 receipt。结果未知/仍在派发、执行
错误或异常时，Dispatcher 在 lease 释放前调用环境 interrupt；释放失败或异常也向
调用方显式报告，start/uncertain 仍保留用于恢复。副作用后取消/到期不能
因迟到结果把 Loop 完成；未决动作在一次 Verify 未证实目标后结束本轮交由宿主恢复，
不自动进入下一次动作。明确的无副作用 Rejected 保留现有有界修复预算。

### 3. 兼容性与事件

接口加法，但输入行为有意收紧：没有 dispatcher 的 AgentLoop 在遇到输入动作时
返回 Failed，环境调用次数为零；纯对话、只读工具和 done/fail 不强制装配输入派发器。
set_event_store 继续表示诊断/Loop 事件，不能被视为准入或 durable gate 的替代品。
有 Loop 事件存储时，宿主必须让 Dispatcher 使用同一 runtime/session/store。

新路径不再发旧 ActionDispatched，避免 reducer 重复计入同一次动作。
Journal 保持现有八字段 pipe 格式；本接线只产生安全的 input/UUID/primary/session
目标与固定状态摘要，不注入含分隔符的自由文本。旧 ActionDispatched/VerificationResult/
LoopSettled JSON 事件仍读回；不把旧 JSON 事件视为 durable start 的替代品。
Checkpoint reducer 同时消费旧 flat JSON 与实际 Loop 的 task_id/task_epoch/detail
envelope；嵌套身份错误、负 epoch 或非对象 detail 拒绝应用，旧 epoch 事件不覆盖新状态。
这修复了原先 LoopSettled envelope 被按 flat JSON 处理而遗漏终态的接线偏差。
对结果未知且 Verify 未证实的动作不发送会清除 pin 的 VerificationResult。
ActionJournal 通用恢复扫描的分页与任意宿主 target/intent 的全字段校验继续由维护项
03 补齐；本批不声称旧存量事件天然具有完整日志保护。

### 4. 入口覆盖清单

| 入口 | 本批前后与后续 |
| --- | --- |
| AgentLoop 编译后 IEnvironment::execute | 本批强制走 Dispatcher；离散输入与有界 swipe 使用同一门槛 |
| AgentLoop BuiltinToolRegistry handler | 原有工具身份/schema/调用纪律保留；可写工具 journal 接线仍未完成，不能据此宣称安全闭环 |
| Workflow dispatch_tool_invocation / Navigate | 仍由 kairo 任务经 registry 调用；后续在工具派发边界统一接日志，不在 Workflow 复制协议 |
| 连续 Native Controller / 平台外部调用 | 不在本批接线；连续控制需整段 start、输入释放/watchdog 与 realtime 门禁的专项设计 |
| OfflineReplayEnvironment | 仍无真实输入能力；测试显式装配进程内 journal，消费录制 receipt，不创建网络路径 |

## 备选方案

- 从 set_event_store 自动构造默认“允许”策略：无法提供 lease/权限语义，不采用。
- 未装配时沿用直接 execute：保留默认旁路，不采用。
- 在 Core 创建全局 Executor 或私有输入线程：违反所有权与生命周期约束，不采用。

## 影响与风险

下游必须为输入场景迁移装配；只传 event store 不足。宿主的 lease 与 Adapter 实现是
最终准入和取消保证的组成部分，本接口不能证明任意宿主实现正确。内存替身只用于
离线 fixture，进程崩溃验证必须使用耐久 store。Journal 驻留容量与保留沿用 M31，
不能自行淘汰未决动作。完整可写工具/连续控制与真实平台仍为开放验证范围。

## 验证方式

派发故障矩阵逐项断言实际输入次数、lease 生命周期、日志顺序、恢复状态：
正常、prepare/start/receipt 拒绝和弱 ack、执行错误/异常、准入撤回/接管、epoch 变化、
deadline、取消与迟到结果、未知状态、重启无 receipt、终态不复活。
新增路径同步有界，不新增并发；集成运行仍由外部 Executor owner 管理。

## 关联文档和工作项

- [维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)：01–03。
- [DEC-003](DEC-003-event-sourced-persistence.md)、[DEC-018](DEC-018-takeover-input-release-and-operation-admission.md)。
- [输入派发设计](../design/journaled_input_dispatch_design.md)。
