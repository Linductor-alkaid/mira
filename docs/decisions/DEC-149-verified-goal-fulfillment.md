# DEC-149：从已验证的任务终态兑现一次性目标

> 状态：Accepted
> 日期：2026-11-25
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-070](DEC-070-long-term-memory-behavior-contract.md)、[DEC-129](DEC-129-durable-goals-and-commitments.md)、[DEC-145](DEC-145-durable-goal-dispatch-receipts.md)

宿主可让协调层为已真实进入 `Completed` 的 Runtime 任务写入持久完成证据。协调层核对
当前主体授权、会话归属和 Runtime 的无错误完成终态。事件 `mira.task.verified-completion.v1` 保存在
原任务会话，绑定任务、租户、用户、结算控制序号和 Runtime 完成时间。按 Runtime、会话
和任务推导稳定事件 ID。要求 `ProcessCrash` 持久确认后返回。交互输入台账的阶段报告
继续服务原有调用方，单独报告输入结算不会产生这份证据。

目标库使用当前授权读取原派发身份和已提交的任务及会话，再对同一个受信事件库做有界
查询。完成证据必须唯一，归属须与目标匹配，时间不得早于任务提交。证据源需确认能以
`ProcessCrash` 持久化。使用原目标版本和事件作为现有 `settle` 的凭据，重复兑现幂等。
延迟写入的证据使用 Runtime 的完成时间。
取消、修订、擦除及到期策略沿用原有事务规则。失败或取消的任务不会兑现一次性目标。
本增量无需迁移目标库，进程重启后可直接读取证据，继续兑现尚未结算的目标。

M30-46 验收真实 Runtime 主体和终态核验、持久证据重放、错误证据拒绝、版本冲突及
重启兑现。生产自主任务准入和触发后自动关联仍由 M30-37 跟踪。
周期发现并核对已派发的一次性目标见 [DEC-150](DEC-150-goal-fulfillment-polling.md)。

直接集成宿主的送达证据接入见 [DEC-173](DEC-173-host-verified-goal-completion.md)。
