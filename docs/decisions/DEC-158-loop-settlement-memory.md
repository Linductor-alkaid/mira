# DEC-158：从实际循环结算事件提取事件记忆

> 状态：Accepted
> 日期：2026-11-25
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-070](DEC-070-long-term-memory-behavior-contract.md)、[DEC-119](DEC-119-six-memory-systems.md)、[DEC-156](DEC-156-m30-implementation-freeze.md)、[DEC-157](DEC-157-scoped-durable-record-pages.md)

`Record` 从设备循环的 `LoopSettled/Completed` 和会话循环的 `ConversationSettled/Answered`
提取 `Episode`。两者使用现有的任务事件信封，提取时核对事件类别、外层任务归属和世代字段。
会话回答的观察只表示循环已回答。事件记忆保存固定的结果描述、原发生时间及来源事件 ID，
模型正文和调用结果仍从原事件取证。失败等其他状态保留在事件库。旧的平面 `LoopSettled`
格式仍可提取，旧记录的文本和匹配规则保持兼容。

新的事件记忆主张按来源事件 ID 去重，避免不同会话的相同局部序号合并。缺少可靠的
原始时间时返回可诊断的错误。提取与模型候选并入同一 `MemoryConsolidator`，接受既有
范围、证据、版本及内容校验。原 Record worker、结算触发和 scoped 水位继续承载该路径。

每个管线处置携带其 `MemoryKind`，工具状态和持久 Record 页回执可按实际类型核对。
页回执的 `by_kind` 记录每类生效及拒绝的计数，沿用原稳定事件身份和已确认水位。
既有页回执仍可恢复。待审批或需重试的页沿 DEC-157 停在原水位。统一触发各类专用 owner
以及跨来源收集其候选报告，继续由 M30-03 与 M30-38 跟踪。
