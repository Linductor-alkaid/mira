# DEC-163：返回技能提示的候选处置报告

> 状态：Accepted
> 日期：2026-11-26
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-138](DEC-138-evidenced-skill-hints.md)、[DEC-156](DEC-156-m30-implementation-freeze.md)

M30-03 在现有技能提示写入管线上提供 `record_skill_hint_candidate`。宿主传入当前操作
上下文，获得稳定记录 ID 和原始 `ConsolidationReport`。报告区分首次生效、幂等重放和
策略拒绝，保留候选种类、处置原因和回执。输入不合法、当前授权不足、证据失效、取消或
截止均通过 `Result` 返回错误。操作在进入授权检查前及提交前检查取消和截止。

候选沿用成功技能调用、当前发布和学习绑定的证据检查。技能提示保持单条提交上限，沿用
稳定记录和变更 ID、首次写入生效及擦除后旧回执的规则。现有 `record_skill_hint` 复用
同一入口，再将候选处置映射为原有的变更回执或错误。生产事件触发和跨重启消费由
M30-38 验收。
