# DEC-129：一次性目标与承诺的持久状态

> 状态：Accepted
> 日期：2026-11-23
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-070](DEC-070-long-term-memory-behavior-contract.md)、[DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md)、[DEC-119](DEC-119-six-memory-systems.md)

## 决策

`SqliteGoalStore` 保存一次性任务和承诺。宿主按当前授权提供精确租户、用户与稳定目标 ID，
来源事件与内容一同写入。目标按 Pending、Fulfilled、Cancelled、Expired、Erased 解释；
结算带来源和时间，通过期望版本防止覆盖。相同创建或结算可幂等重试，终态禁止回到 Pending。

宿主可按归属与状态列出目标，包括无到期时间的任务。到期查询按归属、`due_at` 和 ID
排序并返回有界分页。截止时间用于发现应处理事项；
显式 `expires_at` 决定何时不再列入到期页，并约束 Expired 结算。承诺须有截止时间。
目标正文和证据经显式擦除清理，保留稳定 ID 屏障，迟到创建无法恢复。每主体的待完成目标
与全库实体分别受限，待完成项及屏障不参与普通记忆淘汰。库容量整合和回收由 M31 承接。

读写使用宿主拥有的 SQLite 和现有 Executor StoreChannel。到期页由宿主或显式调用方读取，
其中的记录或文本本身不授予动作授权。重复计划、纠正、提醒计数与主动派发在后续工作项处理。

## 验收

验证状态、证据、归属隔离、到期分页、过期、额度、擦除后重开和幂等。见 M30-24。
