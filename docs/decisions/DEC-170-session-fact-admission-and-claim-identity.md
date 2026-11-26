# DEC-170 会话事实准入与陈述身份

> 状态：Accepted
> 日期：2026-11-26
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-115](DEC-115-write-path-decoupling.md)、[DEC-119](DEC-119-six-memory-systems.md)、MIRA-20261126-004～005

宿主可在 consolidation 策略中允许有事件出处的会话环境事实与应用事实以 `Unverified`
写入。默认要求 `Observed`，其他范围和 kind 继续使用各自的验证下限。计划出的 mutation
携带宿主选择，以便经过存储验证及可选审批。模型提案只携带资料，宿主仍负责绑定会话范围。
召回文本显式标注未验证的环境事实与应用事实。

Record 可提供结构化陈述，字段分别为带主体和属性的 `subject` 以及该属性的 `value`。
同一 scope 和 kind 内按该主体属性定位，按值区分证据合并与备选陈述。原始表述供展示和
词法召回。自由文本按首尾句读归一化，既有 `subject=value` 解析保留。旧记录即读即归一化，
沿用既有存储和历史版本路径。
