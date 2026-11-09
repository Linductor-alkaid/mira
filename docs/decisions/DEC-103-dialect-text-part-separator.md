# DEC-103：Chat Completions 纯文本消息的 part 分隔契约

> 状态：Accepted
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-BR-01

## 背景与问题

`ConversationLoop` 把一次请求的 user 内容装进多个 `TextPart`（goal、每条运行中
注入指令、recovery feedback、DEC-053 供给内容，见 `ConversationLoop::build_request`）。
`ChatCompletionsV1Mapper` 渲染无图纯文本消息时把这些 part 裸累加成一个字符串，
part 边界完全丢失。mira-bridge 用捕获代理逐字节对比 wire 格式时发现 goal、信封行、
结算记录在 wire 里粘连成一行；对结构敏感的小模型，这同时是纪律噪声（信封行边界
是桥与模型的渲染契约）。Responses 与 Messages 方言把每个 part 渲染为独立
content 块，不存在此问题。

part 是逻辑上的独立块，方言渲染层却把它们当作同一段流文本。DEC-053 的折叠语义
只约定"合并进一条 user 消息"，没有约定分隔符；宿主为每段内容手工补 `\n` 是把
渲染责任错放到宿主层。

## 决策

`ChatCompletionsV1Mapper` 的无图纯文本分支在**非空** `TextPart` 之间以 `\n`
连接；空 part 不产生任何输出，也不产生孤立分隔符。该分支输出仍是单一 JSON
字符串（wire 形状不变），part 边界以换行保留。带 image 的 multipart 分支继续
把每个 `TextPart` 渲染为独立 content part，不变。Responses 与 Messages 方言
不变。

契约表述：Chat 方言下，同一 input item 的多个 `TextPart` 渲染为一个字符串，
相邻非空 part 之间恰有一个 `\n`；宿主不需要（也不应该）为逻辑块手工补尾
分隔符。此契约由方言测试固定。

## 备选方案

- 在文档里把"part 边界不保留"定为契约、由宿主自理：被否决，因为宿主无法在
  不知道方言渲染规则的情况下可靠地自行拼接，且多 part 是 loop 自己的请求形状，
  渲染责任在方言层。
- 用 `\n\n` 或其他分隔符：单个 `\n` 足以构成可分标的边界，且与
  `ConversationLoop` 内部 `assistant_text` 的多段连接方式一致。
- 改为 multipart content 数组：改变 wire 形状，对无图请求是纯粹兼容性破坏。

## 影响与风险

- 行为变化：多 `TextPart` 的 Chat wire 内容从裸拼接变为 `\n` 连接；单 part
  与空 part 的输出字节不变。既有依赖裸拼接的消费者（如有）需改为自行合并。
  这是方言渲染契约变更，二进制消费者无需重编译（公开头无变化）。
- `ConversationLoop` 是主要受益者：goal/指令/feedback/供给内容的边界恢复可读。
  宿主此前为绕过此问题补的尾换行可在升级后撤除。
- 不改变 canonical 请求、digest 输入或 Replay 语义；digest 覆盖 canonical
  请求，wire 渲染差异在 mapper 层。
- 本轮只承诺本地编码与方言测试；真实端点行为按部署验证。

## 验证方式

方言测试覆盖：多 part 顺序与 `\n` 边界、空 part 不产生孤立分隔符、单 part
字节不变、带图 multipart 分支不受影响、ConversationLoop 真实多 part 请求的
wire 内容包含完整独立行。

## 关联文档和工作项

- [维护计划](../plans/maintenance-2026-11-bridge-host-feedback.md)
- [LLM API 设计](../design/llm-api-protocol-design.md)
- [DEC-053](DEC-053-conversation-image-supply-seam.md)（供给内容折叠进 user 消息）
- [DEC-047](DEC-047-conversational-loop-and-canonical-tool-parts.md)（会话循环请求形状）
