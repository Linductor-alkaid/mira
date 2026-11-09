# 交互输入归属与投递

> 状态：Active
> 版本：1.0
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 范围：M29 首批并发与执行中插话增量；恢复部分尚未交付
> 决策：[DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)、
> [DEC-102](../decisions/DEC-102-interaction-input-arbitration-and-delivery.md)

## 目标与边界

同一主体可以同时有多个工作。用户的明确引用决定输入归属，模型建议只参与无显式引用的
路由。输入接收、上下文更新、工具执行和任务结算分别有可观察边界。协调层位于 Runtime
之上，不创建另一套 Task 状态机或副作用提交队列。完整请求仍由 Layer 0 决定准入。

这份设计解释当前实现；完整澄清回收、跨进程注册表恢复、断线回补和未决副作用恢复
由 [M29-03/05/06](../plans/m29-interaction-coordinator-concurrency-and-recovery.md) 继续跟踪。
不能用账本 rebuild 推导出这些能力已完成。

## 归属与提交顺序

协调层的 submit_mutex 串行化提交、汇绑定/解绑与关闭；registry mutex 只保护只读投影
复制和短时发布，调用宿主缝不持有它。宿主缝不能重入写接口。

`InputEnvelope.target_task/requested_intent` 与 reply_to 先经过主体归属校验；冲突或未知
引用拒绝。没有显式引用才调用 resolver，随后是唯一活动任务/新任务/有序候选澄清。
权限在重复回执返回前重验。input_id 在一个账本分区内绑定 tenant/user；其他主体复用
不能得到原回执。控制 intent 直接使用 Runtime，检查 Applied/NoOp。

普通输入的顺序为：接纳落账 → 裁决 → 有界上下文纯计算 → 必要的 Runtime 创建 →
路由落账 → 注册表发布 → 有界排队/投递。Routed 不表示已读。追加失败不发布内存账本
状态；已有工作不发布失败纠正；新 Task 路由提交失败时请求取消，取消失败明确返回
清理不确定及内部 TaskId。Runtime 创建命令的跨崩溃意图与投影恢复仍是 M29-05。

Task/Session 数量在创建前检查。默认每 Task 待投递 256 项、关联输入 4096 项，退役
上下文计算 16 个；上限触发返回显式错误。本增量没有建立跨主体字节容量或长期淘汰
机制，这部分继续由 M31 约束。

## 投递失败与插话

汇返回失败必须保证没有接收输入。直接拒收的输入登记 Rejected，不进入有效约束。
绑定汇补投时只删除成功前缀；失败项与后缀保留，新输入不能越过它们。宿主显式再次
register 才尝试补投。汇抛异常时无法判断是否接收，记录 DeliveryUncertain、封闭该
任务后续投递；登记诊断失败也显式返回，不自动重试可能已经接收的输入。

Loop 在轮边界 drain，派发模型前、接收完成后、repair 后及下一次动作/答案准入前检测
待处理输入。在既有恢复预算内重建，设备侧先 Observe。预算耗尽不接受旧完成。已经
进入的单个动作/工具遵循原取消/deadline/journal，不能追溯撤回；工具组中的未启动项
使用失败结果关闭 call/result 协议组，不调用处理器、不计执行数，发出 ToolSkipped。
动作执行后的 Verify 仍进行；插话使旧目标的成功验证不能直接结算更新后的工作。

最后一次队列检查是当前 action/tool 的输入准入边界。之后到来的纠正影响下一单元，
不会把已进入动作伪装成未执行。Human Takeover 与取消继续由 Runtime epoch、宿主
OperationContext 探针和 IActionDispatchLease 关闭动作准入；输入队列不是接管协议。

## Executor 与生命周期

上下文文本准备由注入 Executor 的 submit_auto 执行，只捕获候选工作项的值。成功
返回后由 owner 发布；超时 future 被保留，但其迟到完成不能写回 registry 或引用
已销毁 owner。close 消费全部有限任务并报告迟到错误；析构与旧 shutdown 为尽力清理。
宿主先停输入生产者及 Loop，再 close 协调层，随后关闭 Runtime、存储和 Executor。

submit_input 是同步且可能等待命令与有限计算的宿主操作。调用方使用外部 owner 或
Executor 的阻塞 I/O 边界，不要让同一个有限池的全部 worker 等待协调层在该池提交的
上下文任务。回调有界、无阻塞且异常隔离；错误使用固定文本，不持久化 exception.what。

## 耐久、安全与恢复限制

参考账本要求 EventStore 的 ProcessCrash ack。MemoryEventStore 的 Buffered ack
不符合生产耐久要求；测试回执模拟不能作为进程崩溃证据。FileEventStore 的实际关闭/
重开测试证明输入绑定、归属与幂等记录可重建。

既有 v1 事件追加可选引用、澄清候选和不确定投递诊断；不写输入正文、grants 或模型异常
原文。宿主仍需在信封进入系统前脱敏正文，Loop 既有文本事件纪律沿用 DEC-016。
按 id 查询账本以及结果登记是受信 owner 面，不能直接作为远端用户接口。

rebuild 是索引投影恢复，不重建活动 Runtime Task，也不重投 Routed 输入。完整恢复还
需受控 Artifact 正文引用、主体注册表投影和已消费/不确定状态协调。未决副作用只能复用
RecoveryPlanner + journal，先 Observe/Verify。缺失或擦除历史不能补造；这些接线的
未完成状态保留在 M29 计划中。

## 验证与后续

`tests/m29/` 覆盖多工作归属、实际并发重复提交、权限撤销、账本失败原子性、文件重开、
补投后缀保留、汇异常、队列背压、有限任务超时/关闭、无记忆代理的两类 Loop 插话、
repair、工具组中断、已执行动作 Verify，以及 Runtime 取消/接管的迟到完成。
M28/M36 全量回归与 Sanitizer 是兼容性证据。目标平台、真实模型与完整恢复均须另行
验收，具体结果和补跑条件见 M29 验证记录。
