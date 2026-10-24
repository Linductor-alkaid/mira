# DEC-066：对话循环的不完整输出不结算为答案

> 状态：Accepted
> 日期：2026-10-24
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 对话不完整输出维护轮
> 替代/被替代：无
> 修订对象：[DEC-065](DEC-065-conversation-terminal-truncation-reason.md) §决策 1
> （终态截断原因的置位口径）
> 关闭的缺口：不完整的模型输出被当作终态答案结算（上游自查发现，见 §背景与问题）

## 背景与问题

`ConversationLoop` 的文本终态分支只检查「响应里有没有文本」：

```cpp
auto answer = assistant_text(outcome.response);
if (!answer.empty()) { /* Answered */ }
```

而 `parse_decision` 对 `status == "incomplete"` 的响应一律给出
`DecisionParseOutcome::Incomplete`。两个判定之间的空档造成了缺陷：**一个供应商明确
声明「没有说完」的响应，只要带着部分文本，就会被当作完整的终态答案结算**。

上游自查（2026-10-24，随 DEC-065 的评审）确认该路径可达，且不是纯理论：

- **Responses 方言**把 `status == "incomplete"` 映射为
  `IncompleteReason::Other`，只有 `incomplete_details.reason == "max_output_tokens"`
  才映射为 `MaxOutputTokens`（`src/model/model_responses.cpp`）。因此
  `incomplete_details.reason == "content_filter"`（OpenAI Responses 的另一个取值）
  带着部分输出文本到达循环时，会以 `Answered` 结算——**被内容过滤打断的输出被当作
  完成答案交给宿主**。
- 供应商报告 `ContextWindow` 的截断同样如此：上下文窗口被撑爆的回复，以部分文本
  结算为答案。

`MaxOutputTokens` 截断此前不受影响：它在更早的分支被续写接管
（[DEC-065](DEC-065-conversation-terminal-truncation-reason.md) 冻结的续写路径）。
缺陷只在**非** `MaxOutputTokens` 的不完整响应上暴露——也正是供应商把输出标成
`Other`/`ContextWindow` 的那一类。

除了语义错误，这个缺陷还与 DEC-065 的可观测性目标直接冲突：若只做「不完整输出不作为
答案」的修正而不动原因字段，那么被修正后的这条**新的失败路径**（不完整输出耗尽恢复
预算）将没有任何结构化原因——宿主无法区分「上下文窗口耗尽」「被内容过滤」与「工具
预算耗尽」。修正必须与可观测性一起交付。

## 决策

### 1. 不完整输出永远不作为答案

`DecisionParseOutcome::Incomplete` 的响应不再进入文本终态分支，而是走既有的恢复路径
（带显式反馈重试，预算耗尽后 fail closed）：

```cpp
auto answer = parse_outcome == DecisionParseOutcome::Incomplete
                  ? std::string{}
                  : assistant_text(outcome.response);
```

该响应携带的部分文本**不进入 `result.answer`**，也不进入 tool-history 回放：上下文
窗口已满时保留片段会继续挤占上下文，被过滤的内容也不应回到历史里。

### 2. 恢复反馈只说供应商报告过的原因

既有恢复反馈对所有不完整输出都断言「被输出上限截断」（"cut off by the output-token
limit"）。这条断言对 `ContextWindow`/`Other` 是**假的**，会把重试引向错误的问题
（上下文窗口满时再要求「更简短」并不必然有效，被过滤时重试多半同样被过滤）。

反馈改为按供应商报告的原因分派：

| 原因 | 反馈要点 |
| --- | --- |
| `ContextWindow` | 上下文窗口耗尽；只要简洁纯文本、不重复既有内容 |
| `Other` / 无原因 | 只说「回复没有说完」，不指认任何未报告的原因 |

`MaxOutputTokens` 仍然不会到达恢复路径（续写分支先接管），因此不存在「把上限截断
说成别的原因」的情形。

### 3. 终态原因覆盖到恢复预算耗尽这条终态（修订 DEC-065 §决策 1）

DEC-065 冻结的置位口径是「仅续写预算耗尽置位，其余 `Failed` 为空」。本记录把该口径
扩大为：**终态失败由不完整输出造成时，携带供应商报告的原因**：

- 续写预算耗尽 → `MaxOutputTokens`（与 DEC-065 一致）；
- 恢复预算耗尽（不完整输出）→ 供应商报告的 `ContextWindow`/`Other`；
- 供应商**没有**报告原因 → 字段留空。**不臆造**原因。

因此 `ConversationLoopResult::terminal_incomplete_reason` 的含义统一为「**供应商为
导致终态的不完整输出报告的原因**」；`ConversationSettled` 载荷的 `incomplete_reason`
键随之遵循同一口径（键存在即有原因）。

下游影响：按 `== IncompleteReason::MaxOutputTokens` 判别的消费方行为不变；把「字段
存在」当作「输出上限耗尽」的消费方需要改为按值判别。该修订已向下游公开通报。

### 4. 其余终态不受影响

`Answered` / `MaxTurns` / `Cancelled` 与其它 `Failed`（工具预算、拒绝、准入拒绝、
请求装配失败、取消、deadline）仍一律留空。

## 备选方案

- **把不完整响应的部分文本作为答案、但加一个标志**：拒绝。宿主拿到的仍然是一个
  未完成的答案；标志位只是把「答案不可信」这件事推给每个消费方各自处理。
- **不完整输出直接 fail closed、不做恢复**：拒绝。既有设计（DEC-047 起的恢复路径）
  已经把「空回复/不可用回复」纳入有界恢复；上下文窗口满或一次过滤未必不可重试，
  直接放弃会丢掉本来能答出来的运行。恢复预算有界，代价可控。
- **把 `Other` 一律当作 `ContextWindow`**：拒绝。供应商没报告的原因不能由 Mira 猜。
- **在模型契约里新增 `IncompleteReason::ContentFiltered` 并修正 Responses 方言映射**：
  拒绝（**本轮不做，登记为后续项**）。Chat Completions 方言把
  `finish_reason == "content_filter"` 映射为 `ModelCompletionStatus::ContentFiltered`
  （独立终态，循环 fail closed），而 Responses 方言把 `content_filter` 折叠进
  `Other`——两者不一致。这属于**方言映射层**的独立缺陷：改它要动模型契约的状态映射
  并单独通知下游。本轮的行为修正已经让「被过滤的输出被当成答案」不再可能（它不再
  结算为答案，而是走恢复并以带原因的失败收敛），方言对齐留作后续项。
- **在事件面新增一个「输出不完整」事件类型**：拒绝。终态原因已在 `ConversationSettled`
  载荷里，过程信号由 `output_continuations` 覆盖，不需要新事件名。

## 影响与风险

- **行为变更**：供应商报告 `Other`/`ContextWindow` 的响应，其带文本的部分内容不再
  作为终态答案。宿主会看到 `Failed`（恢复预算耗尽）或一次恢复后的 `Answered`，
  而不是一段未完成的文本。这是**修正**，但对依赖旧行为的宿主是可见变化。
- **恢复预算被消耗**：不完整输出现在会消耗恢复额度（此前直接 `Answered`）。若供应商
  持续返回不完整响应，运行会比此前多花 `max_recoveries` 轮后失败。有界，可预期。
- **反馈文案变化**：恢复反馈不再断言输出上限截断。既有下游若匹配该文案需复核。
- **契约修订**：DEC-065 的置位口径被本记录扩大，已向下游通报（issue #100 评论）。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

`tests/m3/m3_conversation_truncation_test.cpp` 新增三个用例：

- `an_incomplete_output_is_never_the_answer`：带文本的 `ContextWindow` 截断不再结算为
  答案（`result.answer` 是重试后的完整答案，片段不出现），恢复反馈指认上下文窗口，
  终态原因为空（重试成功）。
- `an_incomplete_output_fails_closed_with_its_reason`：恢复预算耗尽 → `Failed`，
  `answer` 为空，终态原因为 `ContextWindow`，载荷 `incomplete_reason ==
  "context_window"`。
- `an_incomplete_output_without_a_reason_reports_none`：供应商未报告原因 → `Failed`，
  终态原因为空，载荷无 `incomplete_reason` 键。

既有 `truncated_reply_recovers_with_short_answer_guidance` 的断言随反馈文案修正更新。

**变异验证**：去掉「不完整输出不作答案」的守卫 → `an_incomplete_output_is_never_the_
answer` 失败；去掉恢复耗尽终态的原因赋值 → `an_incomplete_output_fails_closed_with_
its_reason` 失败；把反馈改回指认输出上限 → 「context window」断言失败。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-conversation-incomplete-answer`](../plans/maintenance-2026-10-conversation-incomplete-answer.md)
- 上游决策：[DEC-065](DEC-065-conversation-terminal-truncation-reason.md)（终态截断原因
  的原始冻结口径）、[DEC-047](DEC-047-conversational-loop-and-canonical-tool-parts.md)
  （对话循环与恢复路径）
- API：[`docs/api/model-agent-loop.md`](../api/model-agent-loop.md) 的 ConversationLoop 节
- 下游反馈：Mirage `MIRA-20261022-001`（[mira#100](https://github.com/Linductor-alkaid/mira/issues/100)），
  本记录修订其已发布的终态原因口径
