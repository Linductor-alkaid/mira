# DEC-153：按操作 ID 保存已确认的任务准入回执

> 状态：Accepted
> 日期：2026-11-25
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-072](DEC-072-autonomy-and-interruption-budget.md)、[DEC-149](DEC-149-verified-goal-fulfillment.md)、[DEC-152](DEC-152-goal-submission-reconciliation.md)

宿主可将稳定派发操作 ID 随 `TaskSpec` 传入现有 Runtime 命令。Runtime 在快照中保留该
标识。协调层接收已完成的 `SubmitTask` 命令句柄，核对接纳结果、操作 ID、任务、会话和
当前授权，然后在受信事件库保存原会话、任务和 Runtime 接纳时间。记录使用按租户、用户
和操作 ID 派生的独立台账分区，读取有事件数量与字节上限。此类事件与会话及任务序号隔离。

写入和回查均核对当前宿主授权与 `ProcessCrash` 持久性。相同记录返回原回执，冲突归属
或关联拒绝。查询只需当前主体和操作 ID，可在原 Runtime 丢失后由新的协调层执行，并用于
DEC-152 的只读对账回调。两个入口尝试获取协调层锁，有争用可重试，无需等待 Executor
同池工作或 Runtime 命令完成。

本增量由 M30-50 验收。任务仍由现有准入路径提交。崩溃前未登记的 Runtime 准入仍属结果
不明，后续生产准入与任务恢复由 M30-37 跟踪。
