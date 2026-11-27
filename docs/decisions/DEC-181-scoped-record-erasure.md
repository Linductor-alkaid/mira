# DEC-181：逐记录擦除的范围、版本和回执

> 状态：Accepted
> 日期：2026-11-27
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 替代/被替代：无

## 决策

M30-05 增加 User/Session 记录的存储层擦除入口。调用者提供当前授权下的精确 scope、
记录 ID、预期版本和稳定操作 ID。SQLite 在写事务内核对位置与版本。范围外和不存在的
记录返回同一拒绝。默认后端返回不支持，显式宿主入口不能降级为仅按 ID 擦除。

先持久登记 Pending，再执行既有物理擦除管线。Pending 使所属 scope 暂停读写，容量和
保留期路径无法越过这道屏障。同一操作可在重开后续办。与记录清理在同一事务提交的完成
回执只保存范围、ID、版本和计数。重放返回原计数，完成后禁止以相同记录 ID 新建。
已有范围擦除的 Pending 独立保留。

此入口覆盖存储行、历史版本、索引、出处和原擦除管线管理的当前证据工件。协调入口、
跨宿主派生数据清理和阻止旧事件重新提取为全新记录继续由 M30-05、M30-09、M30-38 验收。

关联 [M30](../plans/m30-long-term-memory-integration.md#5-工作项)、
[DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md) 和
[DEC-156](DEC-156-m30-implementation-freeze.md)。
