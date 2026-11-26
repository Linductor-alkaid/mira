# DEC-166：从已结算终答接纳重复计划

> 状态：Accepted
> 日期：2026-11-26
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-130](DEC-130-durable-recurring-goal-plans.md)、[DEC-132](DEC-132-calendar-goal-plans.md)、
> [DEC-133](DEC-133-goal-store-live-authorization.md)、[DEC-164](DEC-164-evidenced-conversation-commitments.md)

重复计划复用同一终答取证及来源授权。候选用完整终答内的逐字摘录作标题，宿主根据语义
提供首次时间及固定间隔或带时区快照的日历规则。写入入口复核摘录、当前来源访问、取消
和截止。目标库继续校验计划规则及精确 `RecurringPlan/Create` 权限。

本增量接纳一条终答中的单个重复计划。计划 ID 按归属与终答事件在独立域派生，可与同一
终答的一次性承诺共存。SQLite 事务返回首次创建或幂等回执。重放相同定义保留原计划进度
和已处理触发。不同定义走既有换版入口，取消和擦除后的旧创建由原 ID 屏障拒绝。

计划写入沿既有容量和到期索引。M30-38 验收生产提取及自动消费，M30-37 验收到期执行。
