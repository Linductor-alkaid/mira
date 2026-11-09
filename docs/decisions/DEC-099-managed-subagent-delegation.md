# DEC-099：受管子代理委派与父循环继续推进

> 状态：Accepted
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 冻结里程碑：M34
> 替代/被替代：补充并修订 DEC-044 第 1 条的编排范围，投影契约不变

后续修订：[DEC-100](DEC-100-subagent-semantic-tools.md)增加自主工具发现和经过验证的
副作用执行；下述只读限制保留为 M34 默认模式，不再是全部子代理的能力上限。

## 背景与问题

用户在完成 [DSH/ZCode 调研](../design/subagent-reference-study.md)后明确要求补齐缺口。
M24 只有上下文投影，M25 为手动宿主接线；模型缺少有 owner 的委派、结果回流和关闭。

## 决策

M34 提供 Agent Harness 层的 `SubagentCoordinator`，子代理仍为独立 Session/Task 的
无观察 ConversationLoop，不新增设备执行平面。DEC-044 原先排除通用子代理编排的
范围扩展到本决策的单层推理委派；M24 fork/delta/merge、动作租约和双平面保持原契约。

外部 owner 注入 kairo Executor、父任务身份、EventStore 和 ArtifactStore。每个 child
用 start_worker 管理有网络/模型等待的运行，由 coordinator 持有 WorkerHandle；parent
也在受管 worker 上运行，返回调用者必须消费的 shared_future。不在默认有限池内
阻塞等待 child，不引入线程池/私有调度器。owner 在 Executor shutdown 前 close。

模型通过 spawn_agent/get_agent/cancel_agent 操作准确执行身份。固定宿主 profile 配置
独立模型 provider 工厂、system 和工具白名单；child 只开放声明无副作用的授权工具，
无递归委派工具。工厂按执行创建 provider；gateway/账本/路由不跨 child 共享。gateway
只注册指定 profile，内部 retry 固定一次，恢复计入 child 有限模型轮数。

限制活跃数、累计创建、待回流结果槽位、提示/结果字节、单 child 模型轮数与 deadline。
按 child 全部轮次上限预留累计请求数、输入估算额度和输出生成额度；额度本 owner
生命周期不返还，失败也不能通过重试创建绕过总上限。这些是委派资源额度，不是整个
父任务的真实币种账单硬限额；父本身继续使用已有 gateway 预算。

ConversationLoop 增加显式 continuation source：请求前非阻塞收取完成输入；拟接受
终态文本时可等待已接收 child 的结果，再继续同一 active run。无 source 默认不变。
回流仅接受有界 User/Text 输入，来源和 authority 由 loop 固定，不授予 System 权限；
失败 fail closed。拟终态文本在继续时作为 Assistant 历史保存，工具历史不丢失。
这里的等待表示父模型当前无工作，父 Task 尚未终态，不复活已完成 Task。

child 结果经 ArtifactStore 保存，事件只记身份、状态、摘要/字节和 artifact 引用，
不复制提示或输出。结果单次 claim，不承诺恰好一次被模型消费；父失败/取消后保留
可查询结果并停止自动回流。EventStore/ArtifactStore 错误显式返回或结算为失败。
首期不恢复进程内 worker、不自动重放模型/工具；持久事件与产物只供只读检查。

父轮拟结束不取消 child；父 run 失败/终态、取消、deadline、接管和 close 则封准入、
请求停止全部 child，并等待真实运行收尾。停止请求和完成分开。close 超时返回明确
结果、允许重试，不把超时当强杀；析构和最终关闭必须由外部 owner 完成。

## 备选方案与后果

只注册 spawn 后向用户队列塞文本无法解决已结束 run 的历史与唤醒；持久 Session 的
跨进程继续、任意层级消息/审批、全历史 fork、真实设备共享与写操作留待独立扩展。
首期新上下文按宿主工厂供应任务所需数据，M24 快照可经已有输入供给显式接线，
不自动合并 child delta。公开布局/接口扩展要求 C++ 消费者重编译。

## 验证与关联

M34 覆盖真实 loop 工具往返、父等待后自动继续、失败/异常、权限/容量/预算拒绝、
取消/deadline/接管、提交拒绝、关闭竞态与小型默认池进展；只验收 Linux fixture，
目标平台和真实模型另保留补跑条件。见 [M34](../plans/m34-managed-subagent-delegation.md)。
