# DEC-177：通用 Record 的候选证据处置

> 状态：Accepted
> 日期：2026-11-26
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 替代/被替代：无

## 决策

按 M30-03 和 DEC-156，通用 Record 逐条核对模型和宿主处理器提议的类型、声明和来源引用。
单条校验失败只处置该候选。同页有效候选与确定性记录继续进入 `MemoryConsolidator`。整份
响应违反格式、作业模式、数量或字节契约时，仍通过作业错误报告。授权、取消和擦除继续由
同一个 Memory Agent owner 在写入前复核。

作业结果以事件页边界和从零起算的原始候选行号报告提取拒因。持久页回执记录总数，工具
状态只发布计数和原因码。无效提议是该页的确定处置。待审批、合并容量超限和写入失败
继续使用 DEC-157 的待处理水位语义。已有处理器只返回拒绝总数时仍可使用。

## 取舍与验证

整页拒绝会丢失独立的、证据充分的候选。仅记录总数会掩盖具体证据缺口。按页报告让宿主
可以区分相同行号的不同页，无需在诊断中复制消息或提议正文。验证覆盖跨页、非法引用、
直接提议受类型和验证等级约束、持久回执及进程重开。

关联 [M30-03](../plans/m30-long-term-memory-integration.md#51-待完成实施项)、
[DEC-156](DEC-156-m30-implementation-freeze.md)、[DEC-157](DEC-157-scoped-durable-record-pages.md)
和 [DEC-169](DEC-169-memory-agent-structured-proposal-output.md)。
