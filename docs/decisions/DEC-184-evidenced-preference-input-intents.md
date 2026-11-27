# DEC-184：从已接纳输入准备偏好意图

> 状态：Accepted
> 日期：2026-11-27
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-156](DEC-156-m30-implementation-freeze.md)、[DEC-180](DEC-180-explicit-preference-intent.md)、[DEC-182](DEC-182-host-record-forget-intent.md)、[DEC-183](DEC-183-exact-memory-intent-target.md)

## 决策

`InteractionCoordinator::prepare_memory_intent` 从当前工作中已接纳的原始输入准备偏好
记住、纠正或忘掉请求。宿主传入 ingress ID 和原文。协调层用已接纳输入的摘要校验原文，
使用明确的中文或英文命令及主张键。普通对话与整段引用返回空结果。纠正和忘掉按
DEC-183 解析当前 User 偏好，目标缺失或歧义由宿主报告。

准备不写入记忆。已准备请求使用从 ingress ID 派生的稳定操作 ID，携带输入来源。首次
提交时，记忆所有者在现有提交序列中校验来源仍是当前已接纳输入。收到新的输入后，旧准备
请求不能启动新的写入。已提交的请求沿原有授权、确认和回执机制重放，遗忘使用原来的目标
ID、版本和操作 ID 续办。宿主仍可使用既有结构化接口。

本增量支持使用稳定偏好键的单条显式请求。自然语言释义、代词解析、其他类别和范围级
操作继续由 M30-05 跟踪。
