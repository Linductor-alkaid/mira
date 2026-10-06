# DEC-049：会话流式预览

> 状态：Accepted；日期：2026-10-05；负责人：Linductor-alkaid。

公开ModelPreviewSink在传输执行上下文投递至多16KiB完整快照。空快照标识新请求/尝试，
调用者只做有界投递，不阻塞或执行业务handler；回调异常计入drop，取消后不再投递。
预览非权威，不进入Memory/历史、不触发工具；最终规范响应覆盖预览。
ChatCompletionsSseParser复用SseFramingParser及现有同步mapper：一个choice，稳定身份，
有界工具片段，finish_reason加[DONE]才可归约成功；不接受截断连接的部分答案。
ConversationLoop透传InferOptions与reasoning_effort，返回last_usage及tool_executions。
既有owner/cancellation/admission不变，无新增调度设施。

验证和限制见[维护计划](../plans/maintenance-2026-10-conversation-streaming.md)。
