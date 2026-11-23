# DEC-127：世界事实的结构化地址与新鲜度

> 状态：Accepted
> 日期：2026-11-23
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-119](DEC-119-six-memory-systems.md)、[DEC-041](DEC-041-session-world-state-projection.md)、[DEC-059](DEC-059-memory-kind-policy.md)

## 决策

宿主从已授权环境、应用和已提交的观察事件写入结构化世界事实。环境事实的 scope
包含租户和稳定环境 ID，应用事实另含稳定应用 ID。同一 scope 内按稳定实体 ID 和属性
标识符索引。继续使用 `EnvironmentFact` 和 `ApplicationFact`，观察写入走现有
MemoryConsolidator 和 IMemory。每个证据事件形成一条不可变观察，相同观察可幂等重试。
通用模型提取与通用召回不获得结构化世界事实的访问入口。

读取按精确地址选择在指定有效时点前发生、在指定知悉时点前记录的最新观察，先比较
观察时间，再比较记录时间。若最新观察的有效区间已结束，返回重新观察信号且省略正文。
两时点均相同的多条观察返回歧义。按观察时间计算现有类型的保留期，保证更新观察的
保留期限不早于旧观察。保留期和事实有效期独立。宿主在收集新证据后可写入新观察。

SQLite 增加精确地址索引，并复用存储 worker 与擦除路径。当前会话的 WorldState、
AppModel 及动作授权沿各自入口。将重新观察信号连接到现有运行时观察路径由 M30-14
后续增量负责。

## 验收

验证来源、去重、同名环境与应用隔离、双时态、最新观察过期、歧义、刷新、重开和擦除。
见 [M30-22](../plans/m30-long-term-memory-integration.md)。
