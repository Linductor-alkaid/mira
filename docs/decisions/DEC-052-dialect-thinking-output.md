# DEC-052：Chat Completions / Responses 方言的思考输出映射

> 状态：Accepted
> 日期：2026-10-08
> 负责人：Linductor-alkaid
> 冻结里程碑：M3
> 替代/被替代：无；细化 [DEC-051](DEC-051-messages-adaptive-thinking.md) 在其余两方言上的输出语义
> 依据：Mirage 依赖反馈 MIRA-20261008-001（维护者已授权本轮修复）

## 背景与问题

DEC-051 为 Anthropic Messages 方言引入了 `ThinkingPart`，但 Chat Completions 与
Responses 方言的思考输出仍被丢弃：Chat Completions 非流式把 `message.reasoning_content`
降级为 `UnknownOutput` 摘要（仅 payload digest），流式解析器不读取
`delta.reasoning_content`；Responses 终态把 `reasoning` output item 落入 UnknownOutput
回退，SSE 解析器对 reasoning 事件族 fail closed。仅在两方言上提供思考输出的模型
（DeepSeek-R1 系、经 Responses 代理的 reasoning 模型等）无法向会话展示思考过程，
宿主感知为"开启思考无效果"。

## 决策

两方言把 reasoning 输出映射为与 Messages 同语义的有界 `ThinkingPart`：

1. Chat Completions 非流式：`message.reasoning_content`（非空 string）映射为
   `ThinkingPart{text, signature="", redacted=false}`，位于 MessageOutput 之前；
   超过思考预算（4 MiB，与 Messages 非流式一致）按 ProtocolViolation 失败闭合，
   不静默截断。
2. Chat Completions 流式：`delta.reasoning_content` 按 `SseStreamLimits` 的累积文本
   预算增量累积；思考增量不进入正文预览、不计入 `text_deltas`；终态归约重建
   `reasoning_content` 并复用非流式解码，保证两路径产出一致。
3. Responses 终态（同步与 SSE 共用 `decode_responses_terminal_body`）：`reasoning`
   item 的 `content[]` 中 `reasoning_text` 视为原始思考（`redacted=false`）；无原始
   内容时 `summary[]` 中 `summary_text` 按 redacted 语义保留摘要（`redacted=true`）。
   两者皆无（如仅 encrypted_content 的不透明载荷）保留 UnknownOutput 摘要回退。
   malformed 的数组/分片按 ProtocolViolation 失败闭合。
4. Responses 流式：解析器接受 `reasoning_summary_part.added/done`、
   `reasoning_summary_text.delta/done`、`reasoning_text.delta/done`，只做 item 身份
   校验与共享 reasoning 字节预算 enforcement；权威思考内容一律来自终态 body 的
   reasoning item，不单独信任流式增量。
5. 回放门控：DEC-051 的"其他方言显式拒绝 thinking 回填"不变。工具循环以
   `dialect_accepts_thinking_replay(profile.dialect)`（仅 Messages 为 true）判定是否
   回填完整 assistant 内容；两方言的思考输出仅作可观测输出，不做权威历史输入，
   工具轮沿用 ToolCallPart + tool result 回放。

## 备选方案

- 继续保持 UnknownOutput 摘要：被否决，思考文本对宿主不可见，等于功能缺失。
- 在 Chat/Responses 编码器中放行 ThinkingPart 回填：被否决，违反 DEC-051 的显式
  拒绝约束，且 DeepSeek 等供应商不要求也不建议回传 reasoning。
- 把 Responses summary 永远映射为 redacted=false 正文：被否决，summary 不是原始
  思考；redacted 标志让宿主能区分"完整思考"与"供应商摘要"。
- 把流式 reasoning 增量本身作为权威输出：被否决，终态 body 才是两路径共享的
  单一事实来源，与既有 output_text/refusal 的终态归约纪律一致。

## 影响与风险

- 思考文本进入 `ModelResponse.output`，规范 JSON/摘要/事件引用按既有 ThinkingPart
  路径版本化；文本受 4 MiB 方言预算（纵深防御，与 Messages 一致）、JSON 解析层
  1 MiB 字符串上限与 1 MiB 契约校验约束——公开解析路径上超限由 JSON 层先行失败
  闭合，方言层预算的专用错误消息不可观测。
- 签名字段恒为空；redacted ThinkingPart 的签名约束（redacted ⇒ 无签名）由契约
  校验保证。
- 网关侧 Responses 流式不再因 reasoning 事件族 fail closed；事件身份与预算仍严格
  校验。
- 旧契约读取器遇到新增 thinking 输出按既有规则失败闭合（DEC-051 已声明）。

## 验证方式

两方言 fixture 覆盖增量、终态、越界失败闭合与 redacted 语义；ConversationLoop 验证
Chat 方言思考输出不进入回放且工具循环不回退；Messages 套件回归不回退。真实
reasoning 模型的在线增量可见性由宿主（Mirage）在升级 pin 后复验，Mira 侧不声明
未执行的在线证据。

## 关联文档和工作项

- [DEC-051](DEC-051-messages-adaptive-thinking.md)
- [维护计划](../plans/maintenance-2026-10-dialect-thinking-output.md)
- M3 计划工作项 `M3-22`
- 反馈来源：Mirage 台账 MIRA-20261008-001
