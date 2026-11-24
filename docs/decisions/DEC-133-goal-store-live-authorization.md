# DEC-133：目标存储的当前授权

> 状态：Accepted
> 日期：2026-11-24
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-070](DEC-070-long-term-memory-behavior-contract.md)、[DEC-129](DEC-129-durable-goals-and-commitments.md)、[DEC-130](DEC-130-durable-recurring-goal-plans.md)、[DEC-131](DEC-131-goal-corrections-and-reminder-receipts.md)

## 决策

目标库为宿主提供 `open_authorized`。宿主在认证入口确定租户和内部用户并构造归属。
每次读取、写入、分页或幂等重试进入现有 SQLite worker 后，携带精确归属、资源类型、
操作及单条操作的 ID，向宿主的当前策略查询一次。按归属列出和到期筛选分别检查集合级授权。
探针拒绝或抛异常时返回 `PermissionDenied`，数据库未执行该请求。目标库不持久化策略。

授权决定是该存储操作的准入点。宿主使用有界的本地线程安全探针，在调用期间保持已绑定
的身份和数据有效；探针不得重入同一个目标库。策略在准入后发生变化时，已经获准且正在
执行的单次存储操作可结束。下一次操作、下一页及重试重新查当前策略。动作派发与送达
使用宿主和 M33 的现行授权。

已有 `open` 保留为信任调用方授权的底层入口，供在外层执行等价策略的宿主及离线工具
使用。增加探针不引入 SQLite schema 迁移，也不要求改变既有目标和计划 ID。

## 验收

验证租户、用户、目标与计划操作区分，读取和写入撤权、回执重试、探针异常、缺失探针及
闭库路径。见 M30-28。
