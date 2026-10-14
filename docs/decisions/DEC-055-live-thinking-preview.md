# DEC-055：独立、可选的实时思考预览通道

> 状态：Accepted
> 日期：2026-10-14
> 负责人：Linductor-alkaid
> 冻结工作项：MNT-THINK-PREVIEW-01
> 依据：Mirage MIRA-20261014-004；维护者授权修复上游并提交 PR

## 背景与问题

现有 ModelPreviewSink 仅输出回答正文，ThinkingPart 在规范终态返回。宿主需要根据当前
供应商公开的思考流呈现进行状态，不能用历史终态回放模拟实时内容。

## 决策

新增 `ModelThinkingPreviewSink` 与 `UnvalidatedModelThinkingPreview`，由
`InferOptions`/`ProviderInferOptions::thinking_preview_sink` 显式选择。原正文 sink
调用时机、内容和预算不变；ConversationLoop 沿现有 inference 参数透传。

解析器新增 `take_thinking_preview()`，返回待消费片段；Provider 累积为最近至多
16KiB 的完整 UTF-8 快照，裁掉完整前部字符并报告 truncated/dropped_updates。
每次 stream 请求/尝试先投递空快照，取消后停止，回调异常隔离并计入独立
`SseStreamStats::thinking_preview_drops`。不增加线程、任务或调度器。

Chat Completions 提供 reasoning_content，Messages 只提供 thinking_delta，Responses
只提供 reasoning item 的 reasoning_text.delta。Responses 的 summary-only 内容在
既有规范 mapper 中是 redacted ThinkingPart，本通道保守不输出；签名、redacted data、
encrypted_content 和工具参数始终不输出。供应商不提供公开文本时只有初始清空，不
推测私有思考。预览无权触发工具或写入历史、Memory；规范结算与预算不变。

## 备选方案

将思考混入现有正文 sink 会改变调用方和历史语义；在宿主解析 SSE 复制方言设施。
二者均未采用。通过独立可选回调保持旧调用方兼容，并使源类别明确。

## 影响与风险

每个流最多增加一个 16KiB 快照及解析器有界待消费片段；回调仍运行于传输上下文，
消费方必须仅投递 Executor 通信原语，不运行 UI/业务 handler。预览可能来自后来失败的
请求，调用方须按请求/尝试及宿主终态清除，不能把它当规范响应。

## 验证方式

三方言分片、正文隔离、UTF-8 和零/小预算、签名/redacted 拒绝、Provider 返回前投递、
新尝试清空、回调异常、取消、断流失败及 gateway/ConversationLoop 回归；Debug、
ASAN/UBSAN 与架构检查。未运行的平台不宣称完成。

## 关联文档

[维护计划](../plans/maintenance-2026-10-thinking-preview.md)、
[模型 API](../api/model-agent-loop.md)、DEC-049/051/052。
