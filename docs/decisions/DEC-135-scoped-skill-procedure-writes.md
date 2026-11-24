# DEC-135：按学习范围写入技能发布投影

> 状态：Accepted
> 日期：2026-11-24
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-058](DEC-058-mutation-ledger-scope-binding.md)、[DEC-119](DEC-119-six-memory-systems.md)、[DEC-134](DEC-134-current-skill-procedure-recall.md)

## 决策

技能发布投影的记录和 mutation 身份包含规范化的完整学习 scope 与原发布版本身份。
宿主可将同一发布同步到多个已授权 scope，各范围独立读取、重试和擦除。查当前发布时
先查新身份，缺失时在同一授权范围查旧身份。已写入的旧版投影保留原 ID 和回执。

SQLite 在现有 `Add` 事务内核对旧 mutation 回执。同 scope 已完成的旧写入按幂等处理，
即使正文已经擦除。归属明确的其他 scope 回执不限制本 scope 的新身份写入。归属未知
的旧回执需要修复归属后才能继续。回执检查只返回本次操作的结果，符合 DEC-058。

首次写入新身份时，事务内为未使用的旧 mutation ID 保留一个 `Noop` 回执。旧写入方
随后重试旧身份时无法另写一份投影。新写入方识别保留回执，使用自己的范围化身份。
无需修改 SQLite schema。同一发布范围已擦除后，重复同步继续尊重原有幂等回执。

现有 `IMemory` 后端通过默认实现继续写原身份。支持跨 scope 独立发布的后端实现
`apply_add_with_legacy_identity` 并承担旧回执检查和写入的事务语义。

## 验收

验收新旧身份共存、多范围共享一个库、旧写入重试、回执归属缺失、擦除后重开及并发同步。
见 M30-30。
