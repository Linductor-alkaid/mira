# 持续会话循环

> 状态：Active
> 版本：1.1
> 更新日期：2026-10-29
> 适用范围：ConversationLoop 的显式 Continuous 模式
> 依据：[DEC-056](../decisions/DEC-056-continuous-conversation-loop.md)、
> [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md) §4.4
> 计划：[持续循环维护](../plans/maintenance-2026-10-continuous-conversation.md)、
> [M28](../plans/m28-interaction-coordinator-first-stage.md)

## 目标与边界

持续推进同一个 Task/Session，不用累计模型或工具次数作为终止条件。每轮仍是
有界工作单元，遵循 `request -> admission -> model -> parse -> tools -> history`。
纯文本回答结束，错误、取消、deadline、准入和实际容量不足明确结算。Finite
模式保持原有限计数与零值语义；本设计不改变设备 AgentLoop。

## 所有权与执行

外部 Runtime owner 初始化 kairo，把 run 放入受管 worker，持有并消费 future。
OperationContext 的取消探针映射 StopToken，模型 context 的 deadline 取宿主期限
与单次 model_call_deadline 的最小值。loop 不拥有线程或并发调度。供给和压缩回调
在同一 worker 同步执行，必须有界、非阻塞；在回调后、响应后、每个工具前复核
取消和期限。关闭顺序仍为停止生产、协作取消、消费结果、外部 shutdown(true)。

## 请求预算与兼容

Continuous 产生 model contract 2.0 请求，附带 Request scope、单 infer 预约及
有限输出预算。旧 reader 拒绝 major 2，不能把请求级预算误作累计预算。
新模型 reader 接受 1.x 和 2.x；有限请求仍用 1.0、Task scope（省略字段），保留
原 canonical digest。该版本是 Mira 内部规范契约，不改变供应商 API 版本。

BudgetLedger 仍只有稳定 TaskId 的一个账户，不为每轮创建伪任务。Request 准入
比较当前 estimate；旧未知用量/价格残余继续保留在账户、settlement 和事件中。
这不是整任务花费约束，也不关闭未知价格拒绝策略。gateway 内部 pre-write 重试
仍按原 RetryBudget 有界；max_requests=1 表示一次逻辑 infer，不是物理网络尝试数。
请求失败只释放本次预约，不能释放旧未决金额。累计数饱和；Task 模式算术溢出
在修改账户前拒绝，有限 loop 的预算乘法/请求计数也在发请求前检查。

## 历史与协议

每次收到工具提案，先校验批次容量，按既有身份/schema/取消纪律逐个执行；
然后追加规范 call/result。签名 thinking 与同轮 calls 保持同一个 Assistant 条目。
一轮工具组或本次截断文本被标记为受保护后缀。连续恢复/截断段分别受原配置
限制；完整工具轮结算后清零段内计数，累计 recoveries/output_continuations 仍以
饱和 uint64 统计。Finite 保留整任务额度，不将独立小故障次数作为持续任务上限。

下一轮调用 HistoryTransformer。Continuous 模式额外验证角色、call/result 配对
完整以及最近原始轮逐字段保留；有效返回值替换私有 history，后续从已提交历史
继续，不重新加载被压缩记录。摘要语义与事实归档由宿主负责；不能用空列表删除
所有事实。失败回退原历史并发诊断，之后照常执行容量检查。

历史容量在压缩后、下一次模型派发前检查。超限明确失败，保持已执行工具计数，
不重放动作。该阈值限制活动窗口，而非累计执行轮次。run 的局部历史不是持久
checkpoint，本轮未提供从任意提案重新派发或持久 step/resume 的入口。

## 用户输入通道

M28/DEC-068 §4.4 为 `ConversationLoop` 补齐了与 `AgentLoop::enqueue_user_message`
**对称**的有界入队通道：同样的有界队列、同样的 `ResourceExhausted` 溢出语义、
同样的线程安全纪律。消息在**轮边界**抽取，记录 `UserMessageInjected` 事件，并作为
常驻指令进入本轮及后续每轮请求。容量由
`ConversationLoopConfig::max_pending_user_messages` 界定。

这条通道是**交互协调层**的 Loop 输入缝之一：协调层把已路由的输入投递给已在运行
的 Loop，**不**创建也不运行 Loop，也不改变本设计的请求预算、历史或工具派发语义。
没有这条通道时，continuous 模式——长时持续交互的主形态——无法被统一输入口驱动。

## 工具派发的存量

公开 BuiltinToolRegistry::execute 继续维护 OperationId 去重集合。Continuous
路径仅向 ConversationLoop 开放私有入口，仍复用同一身份/schema/handler/取消
检查。其安全前提是：每次 gateway infer 创建新请求身份、桥接按 call ID 去重、
run 同步向前遍历一次并销毁提案，不接收外部提案、不重放旧响应、不在回调续派。
因此这个路径无需把所有历史 operation 永久登记到注册表。

未来若增加持久恢复、外部提案或异步派发，必须改用有可验证水位/准入的协议，
不能直接复用这一私有入口。直接执行接口本身的去重不作回收，兼容旧调用者。

## 容量、诊断与限制

| 存量 | 默认边界与释放点 |
| --- | --- |
| 请求前规范历史 | 128 条目、2 MiB canonical input 序列化字节；提交压缩释放旧版本 |
| 单轮提案 | 32 个；批次超限在首次工具前拒绝 |
| 逐轮结果记录 | 最近 64 条；删除已结算记录，累计 dropped 计数，终态结果独立 |
| 任务账本 | 每 Task 一个定长账户；未知用量不分配逐轮账户 |
| Continuous 工具 operation 集合 | 无历史集合；仅当前请求/提案随轮销毁 |
| 持久事件和 artifact | 宿主配置现有 EventStore/ArtifactStore 的保留策略 |

历史的临时上界还包括一轮新增响应/工具结果、compactor 返回值、请求装配与
canonical 度量的临时副本；响应与工具桥接本身有单次容量。配置字节是逻辑
序列化规模，不是 RSS/GPU 或任意宿主回调分配保证。

结果的 total_turns/tool/supply/history 累计数与逐轮窗口独立，采用饱和 uint64；
事件中的计数在 int64 上限饱和。ConversationTurnRecorded 在窗口淘汰前发布，
ConversationSettled 保留终态和累计数（原 turns 仍为保留记录数，新增 total_turns
为推进尝试数），retained_history_items/bytes 报告最近一次
请求前存量。事件写入延续既有 best-effort 契约，不能单凭发出事件宣称归档成功。

终态截断原因是结构化事实（DEC-065）：续写额度耗尽时结果对象携带
`terminal_incomplete_reason`，ConversationSettled 载荷携带 `output_continuations`
（整 run 累计）与仅在终态截断时出现的 `incomplete_reason`。持续模式的段内额度重置
只影响段内可用额度，不改变终态字段与累计计数的口径——两种 limit mode 语义一致。
DEC-066 把该字段的口径统一为「供应商为导致终态的不完整输出报告的原因」：恢复额度
耗尽同样携带该原因，供应商未报告原因时留空。不完整输出在任何 limit mode 下都不作为
答案（片段不进答案、不进历史），统一走恢复路径。

## 验证与后续

验证有限/零值、1,024 工具脚本、提交式压缩、签名多工具组、容量拒绝、错误/
异常降级、输出续写上限、取消、deadline、准入和外部 shutdown。测试用观察器/
script provider 自身可能保存完整请求，因此该 fixture 的进程 RSS 不代表 loop
的生产内存曲线；本轮验证私有历史和派发集合的存量。真实模型及 Mirage 主机
接线后的长期资源和完成率由下游复验，不以 fixture 结论代替。
