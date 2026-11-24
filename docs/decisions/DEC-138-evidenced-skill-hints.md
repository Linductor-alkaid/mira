# DEC-138：以已结算技能调用为证据记录 SkillHint

> 状态：Accepted
> 日期：2026-11-24
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-040](DEC-040-tool-reference-and-skill-layer.md)、[DEC-119](DEC-119-six-memory-systems.md)、[DEC-134](DEC-134-current-skill-procedure-recall.md)

## 决策

宿主可为当前仍发布的技能记录一条有界提示。运行时只接纳同一学习绑定内、工具调用已成功
返回且有结算事件的非 DryRun skill 子 run。提示作为 `Observed` 数据存入既有学习 scope，
结算事件证明来源。提示文字由宿主维护，经保留和指令形态检查。

写入沿 `MemoryConsolidator` 的宿主候选 `Add`，按学习 scope、发布摘要和源 run 派生
记录与 mutation ID。同一来源首次成功的提示生效，重试返回幂等结果。擦除后重试使用
原持久回执。授权在写入准入点核验，已准入的单次写入沿现有存储事务完成。

宿主按当前技能及可选的预期发布摘要读取。使用已授权 scope、`SkillHint` 类型及描述符
摘要标签过滤候选，结果按结构、来源、当前版本和状态复核。访问和学习绑定在返回前再核验。
旧发布的提示保留在历史记录中。技能执行仍沿已授权的工具注册表及现有准入。

## 验收

验收工具调用证据、未通过的写入、归属与版本隔离、旧库重开、重复登记、撤权和擦除后重试。
见 M30-33。
