# DEC-067：方言的过滤与拒绝终态对齐

> 状态：Accepted
> 日期：2026-10-25
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 方言终态对齐维护轮
> 替代/被替代：无
> 关闭的缺口：Responses 与 Messages 方言未按设计 §7.2 归类供应商的过滤/拒绝信号
> （`MNT-202610-60` 登记项）
> 依据：[设计 §7.2 Terminal 分类](../design/llm-api-protocol-design.md)

## 背景与问题

[设计 §7.2](../design/llm-api-protocol-design.md) 已经冻结了供应商结果到规范终态的
映射，且**不区分方言**：

| Provider 结果 | Canonical 结果 | 是否解析 Decision | 是否 schema repair |
| --- | --- | --- | --- |
| 明确 refusal | `Refused` | 否 | 否 |
| 达到输出 token 上限 | `Incomplete(MaxOutputTokens)` | 否 | 否 |
| content filter | `ContentFiltered` | 否 | 否；不得换供应商绕过 |

三个方言里，**只有 Chat Completions 完全符合**。另两个方言把供应商明确报告的过滤/
拒绝信号丢掉了：

| 方言 | 供应商信号 | 修正前 | 修正后 |
| --- | --- | --- | --- |
| Responses | `incomplete_details.reason == "content_filter"` | `Incomplete(Other)` | `ContentFiltered` |
| Responses | 拒绝内容 part / 拒绝输出项 | `Completed`（看起来像「完成但没有文本」） | `Refused` |
| Messages | `stop_reason == "refusal"` | `ContentFiltered` | `Refused` |

后果不是分类标签不好看，而是**循环对被过滤/被拒绝的回复做出错误的动作**：

- **Responses 的 content_filter** 折叠成 `Incomplete(Other)` 后，回复走的是 DEC-066 修好的
  恢复路径：循环拿着同一条被过滤的提示再问一次模型，烧掉恢复预算后才以一个**与过滤无关
  的原因**失败（`terminal_incomplete_reason` 报 `other`）。设计 §7.2 对 content filter 的
  要求是「否；不得换供应商绕过」—— 恢复路径恰好是「再问一次」。
- **Responses 的拒绝**没有任何终态信号时，响应看起来像「完成但没有文本」，同样走恢复
  路径。**重试一个拒绝**正是 `Refused` 终态存在的理由：模型已经表态拒绝，循环再问一次
  就是绕过策略。Chat 方言把 `message.refusal` 映射成 `Refused` 并 fail closed，Responses
  方言却没有——同一个供应商的同一个行为，两种方言给出相反的处置。
- **Messages 的 `stop_reason: "refusal"`** 被归一化成 Chat 的 `content_filter` 形状，于是
  一个明确的拒绝被报告成「被内容过滤」。Anthropic 的拒绝语义与 provider 侧的内容过滤
  不是一回事，宿主据此分类会得到错误的类别。

顺带确认了一处**已经正确**的地方：Chat 方言的 `finish_reason == "content_filter"` →
`ContentFiltered`、`message.refusal` → `Refused`（并附一个固定摘要的 `RefusalOutput`），
本轮不改。

## 决策

### 1. Responses：`content_filter` → `ContentFiltered`

`status == "incomplete"` 且 `incomplete_details.reason == "content_filter"` 时，终态为
`ContentFiltered`，并清空 `incomplete_reason`（模型契约要求该字段只与 `Incomplete` 终态
共存，`validate_model_response` 会拒绝其它组合）。

`max_output_tokens` 仍是 `Incomplete(MaxOutputTokens)`，其余未知原因仍是
`Incomplete(Other)`——**不臆造**供应商没说出口的原因。

### 2. Responses：明确的拒绝 → `Refused`

响应输出里带有拒绝（拒绝内容 part 或拒绝输出项）且终态为 `Completed` 时，终态改为
`Refused`。

- 判定复用共享助手 `carries_refusal()`（`model_dialect_internal.hpp`），与 Chat 方言的
  拒绝语义同源。
- **只覆盖 `Completed`**：供应商说「没说完」的响应保持它自己的终态——那是不完整输出，
  DEC-066 已经让它走恢复路径并在预算耗尽后 fail closed，片段不进答案。把不完整输出
  改判成拒绝会丢掉「被截断」这个事实。
- 拒绝内容仍按原样留在响应输出里（供应商的原始措辞），但宿主看到的是终态分类而不是
  一段可被当成答案的文本。

### 3. Messages：`stop_reason == "refusal"` → `Refused`

归一化仍把 `refusal` 映射成 Chat 的 `content_filter` 形状（线格式不变），但在解码之后
把终态修正为 `Refused`，并附一个**固定摘要**的 `RefusalOutput`
（`"provider returned a refusal"`）——与 Chat 方言同一纪律：**不回显供应商的原始措辞**，
拒绝原因进事件面的是有界摘要。

### 4. 三个方言的一致口径

| 供应商结果 | Chat Completions | Responses | Messages |
| --- | --- | --- | --- |
| 明确 refusal | `Refused` | `Refused` | `Refused` |
| content filter | `ContentFiltered` | `ContentFiltered` | （无此信号） |
| 输出 token 上限 | `Incomplete(MaxOutputTokens)` | 同 | 同 |
| 其它 incomplete | `Incomplete` | `Incomplete(Other)` | 同 |

### 5. 循环侧无需改动

`Refused` 与 `ContentFiltered` 在 `parse_decision()` 里已经是两个独立终态，`ConversationLoop`
与 `AgentLoop` 都已 fail closed（`"model refused or was filtered; no policy bypass"`），
不重试。本轮修的是**方言到终态的翻译**，不是循环策略；循环因此不再对被过滤/被拒绝的
回复发起恢复。

## 备选方案

- **在循环里按「响应文本为空且状态为 Completed」推断拒绝**：拒绝。猜测终态会把正常的
  空回复也变成拒绝，且把供应商语义的决定权从方言层挪到调用方。
- **给 `IncompleteReason` 新增一个 `ContentFiltered` 值**：拒绝。内容过滤不是「不完整
  输出」——设计 §7.2 给它独立终态，且 `ContentFiltered` 已经在契约里。塞进
  `IncompleteReason` 会让「不完整」与「被过滤」在同一个枚举里互相污染。
- **让 Responses 的拒绝也走 `Incomplete`**：拒绝。拒绝不是不完整；它必须与不完整输出
  区分，否则 #109 的终态原因口径会重新变得含糊。
- **把 Messages 的拒绝改判为 `ContentFiltered`（保持现状）**：拒绝。这正是本轮要修的
  偏差——供应商明确说了「拒绝」，宿主就不该被告知「被过滤」。
- **回显供应商的拒绝文本到 `safe_summary`**：拒绝。Chat 方言已经用固定摘要表达拒绝，
  Responses 现状回显原文属于既有偏差，本轮不扩大它（登记为后续项）。

## 影响与风险

- **可观察行为变化**：此前「Responses + content_filter」「Responses + 拒绝」「Messages +
  refusal」这三类响应会触发循环恢复（再问模型一次）；现在直接 fail closed，不重试。
  宿主会看到 `Failed` + `"model refused or was filtered; no policy bypass"`，而不是一次
  恢复后的结果。**这是修正**，但对依赖旧行为的宿主可见。
- **恢复预算不再被过滤/拒绝消耗**：此前这些响应会烧掉恢复预算；现在不消耗。依赖恢复
  次数做配额推算的宿主需要复核。
- **终态分类变化**：Messages 的拒绝从 `ContentFiltered` 变为 `Refused`；宿主按终态分类
  的代码（例如工作流恢复层对二者的区分文案）会看到不同类别。
- **契约未变**：`ModelCompletionStatus` 的取值与语义、`IncompleteReason` 词表、
  `parse_decision()` 的映射都未改动；本轮修的是方言实现对已冻结设计的符合性。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

- `tests/m3/m3_dialect_test.cpp` 新增 `responses_filter_and_refusal_are_their_own_terminals`：
  Responses 的 `content_filter` → `ContentFiltered` 且无 `incomplete_reason`；拒绝内容
  part 与拒绝输出项 → `Refused`；`max_output_tokens` 仍是 `Incomplete(MaxOutputTokens)`。
- `tests/m3/m3_anthropic_test.cpp` 新增 `refusal_is_the_refused_terminal`：Messages 的
  `stop_reason: "refusal"` → `Refused`，并附固定摘要的 `RefusalOutput`。
- `tests/m3/m3_conversation_loop_test.cpp` 新增
  `content_filtered_response_fails_closed_without_a_retry`：被过滤的响应让循环 fail
  closed，**不消耗恢复预算、不重试、不产生答案**。

**变异验证**：去掉 Responses 的 content_filter 映射 → 方言用例失败；去掉 Responses 的
拒绝判定 → 方言用例失败；把 Messages 的拒绝改回 `ContentFiltered` → Anthropic 用例失败。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-dialect-terminal-alignment`](../plans/maintenance-2026-10-dialect-terminal-alignment.md)
- 设计：[`docs/design/llm-api-protocol-design.md`](../design/llm-api-protocol-design.md) §7.2
- API：[`docs/api/model-agent-loop.md`](../api/model-agent-loop.md) 的方言终态节
- 相关：[DEC-066](../decisions/DEC-066-conversation-incomplete-output-is-not-an-answer.md)
  （不完整输出的终态）、[DEC-065](../decisions/DEC-065-conversation-terminal-truncation-reason.md)
  （终态截断原因）
