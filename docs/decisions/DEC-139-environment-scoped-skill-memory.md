# DEC-139：技能记忆的环境范围

> 状态：Accepted
> 日期：2026-11-24
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-119](DEC-119-six-memory-systems.md)、[DEC-136](DEC-136-exact-recovery-lesson-recall.md)、[DEC-138](DEC-138-evidenced-skill-hints.md)

## 决策

宿主可为 Workflow 学习绑定提供 `SkillEnvironmentKey`。地址由租户、工作或社交域及
稳定环境 ID 组成，映射到 `SkillEnvironment` scope。一次绑定的技能环境与事件 scope
必须同属一个租户。`Procedure`、`SkillHint` 和 `RecoveryLesson` 在该范围内独立读写；
`Episode` 使用原学习范围。SQLite 用现有 scope 前导索引和恢复经验索引隔离候选。
缺省绑定沿用原读写行为。旧记录保留原归属，切换至环境范围后没有跨范围回填。

宿主继续负责当前身份与环境授权。技能环境范围通过专用宿主入口使用。运行接纳时记下
环境绑定世代，环境、事件 scope 或后端改变会使已经接纳的运行丧失经验写入和自动
召回资格。发布索引的宿主操作可在新绑定中继续。已经准入的单次持久写入沿原存储事务完成。
同一 WorkflowRuntime 的技能子运行继承父运行的环境世代。跨实例派发无法核验环境世代时，
该子运行跳过经验读写。

Memory 记录增加范围类型，沿用 SQLite 列布局。环境擦除由宿主调用既有按 scope 擦除。
工作环境和社交环境可以使用相同的内部 ID，各自的技能范围仍独立。

## 验收

验证工作与社交共用内部 ID 时的隔离、租户与事件隔离、旧记录边界、按范围擦除、
运行过程中换绑与回绑，以及技能子运行继承父运行绑定。见 M30-34。
