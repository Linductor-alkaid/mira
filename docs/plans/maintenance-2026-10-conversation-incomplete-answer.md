# 维护计划：对话不完整输出不作为答案（2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：M3 对话循环（Completed）、[DEC-065](../decisions/DEC-065-conversation-terminal-truncation-reason.md)
> （终态截断原因，2026-10-24 交付）、[DEC-066](../decisions/DEC-066-conversation-incomplete-output-is-not-an-answer.md)
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-24

## 1. 目标

修正一个上游自查发现的语义缺陷：**供应商声明「没有说完」的响应，带着部分文本时会被
当作完整的终态答案结算**（`Answered`）。修正后，不完整输出不再成为答案，而是走既有
的恢复路径并在预算耗尽后 fail closed；同时把该失败路径纳入结构化终态原因（修订
[DEC-065](../decisions/DEC-065-conversation-terminal-truncation-reason.md) 的置位口径，
见 [DEC-066](../decisions/DEC-066-conversation-incomplete-output-is-not-an-answer.md)）。

## 2. 范围与非目标

### 2.1 范围

- `src/model/conversation_loop.cpp`：
  - `DecisionParseOutcome::Incomplete` 的响应不再进入文本终态分支（片段不进入
    `result.answer`，也不进入 tool-history 回放）；
  - 恢复反馈按供应商报告的原因分派（`ContextWindow` 专用指引；`Other`/无原因只说
    「没有说完」，不再断言输出上限截断）；
  - 恢复预算耗尽这一终态携带供应商报告的原因（DEC-065 口径修订）。
- `include/mira/conversation_loop.hpp`：`terminal_incomplete_reason` 的口径注释更新。
- 测试：`tests/m3/m3_conversation_truncation_test.cpp` 新增三个用例；既有
  `truncated_reply_recovers_with_short_answer_guidance` 的反馈文案断言随修正更新。
- 文档：[DEC-066](../decisions/DEC-066-conversation-incomplete-output-is-not-an-answer.md)、
  DEC-065 的修订注记、API 手册 ConversationLoop 节、持续循环设计的终态口径、总计划索引。

### 2.2 非目标

- 不改模型契约的 `IncompleteReason` 词表，也不改任何方言的状态映射（见 §5 的后续项）。
- 不改 `MaxOutputTokens` 的续写路径（DEC-065 冻结，行为不变）。
- 不改工具执行、预算、取消、准入语义。
- 不改 `safe_summary` 既有文案。
- 不涉及 Executor 能力边界 → **不**登记
  [`docs/executor_feedback/ledger.md`](../executor_feedback/ledger.md)。

## 3. 设计与决策依据

- [DEC-066](../decisions/DEC-066-conversation-incomplete-output-is-not-an-answer.md)：
  本轮行为修正与终态原因口径修订。
- [DEC-065](../decisions/DEC-065-conversation-terminal-truncation-reason.md)：终态截断
  原因的字段、事件增量键与「计数看过程、原因看终态」的口径。
- [DEC-047](../decisions/DEC-047-conversational-loop-and-canonical-tool-parts.md)：
  对话循环的终态与既有恢复路径（空回复/不可用回复进入有界恢复）不变，本轮只是让
  「带文本的不完整输出」也走这条既有路径。

## 4. 工作项

- [x] `MNT-202610-54` 不完整输出不作为答案：`DecisionParseOutcome::Incomplete` 的
  响应不进入文本终态分支；片段不进入 `result.answer` 与 tool-history。验收：
  带文本的 `ContextWindow` 截断不再以 `Answered` 结算，片段不出现在答案中。
- [x] `MNT-202610-55` 恢复反馈按供应商报告的原因分派：`ContextWindow` 专用指引；
  `Other`/无原因不指认未报告的原因。验收：反馈文案不含供应商未报告的「output-token
  limit」断言；`ContextWindow` 反馈含上下文窗口指引。
- [x] `MNT-202610-56` 终态原因口径修订（DEC-066 修订 DEC-065 §决策 1）：终态失败由
  不完整输出造成时携带供应商报告的原因；供应商未报告原因则留空。验收：载荷
  `incomplete_reason` 与结果字段同口径；按值判别的消费方行为不变。
- [x] `MNT-202610-57` 测试矩阵与变异验证：三个新用例（不作为答案 / 恢复耗尽带原因 /
  无原因不带原因）+ 既有文案断言更新；三条变异全部被捕获（见 §7）。
- [x] `MNT-202610-58` 文档同步：DEC-066、DEC-065 修订注记、API 手册、持续循环设计、
  总计划索引与本记录。
- [ ] `MNT-202610-59` 下游通报：issue #100 的契约修订评论（**未发布**：PR 尚未开出；
  负责人：Mira Maintainers；补跑条件：PR 开出后随 PR 链接一并发布）。
- [ ] `MNT-202610-60` 后续项（**本轮不修**）：Responses 方言把
  `incomplete_details.reason == "content_filter"` 折叠进 `IncompleteReason::Other`，
  而 Chat Completions 方言把 `finish_reason == "content_filter"` 映射为
  `ModelCompletionStatus::ContentFiltered`——两个方言对「被内容过滤」的处理不一致。
  修正它要动模型契约的状态映射并单独通知下游，属独立一轮（见 §5）。

## 5. 风险与阻塞

- **行为变更可见**：供应商报告 `Other`/`ContextWindow` 的响应，其带文本的部分内容
  不再作为终态答案；宿主会看到恢复后的 `Answered` 或带原因的 `Failed`。这是修正，
  但对依赖旧行为的宿主是可见变化 → 以 DEC-066 与下游通报明示。
- **恢复预算被消耗**：不完整输出现在消耗恢复额度（此前直接 `Answered`）。供应商持续
  返回不完整响应时，运行会比此前多花 `max_recoveries` 轮后失败。有界，可预期。
- **方言映射缺口（`MNT-202610-60`）**：Responses 方言的 `content_filter` 不映射为
  独立的 `ContentFiltered` 终态，因此被过滤的输出在本轮之后走「恢复 → 带原因的
  失败」，而不是像 Chat 方言那样立即 fail closed。本轮的行为修正已消除「被过滤的
  输出被当成答案」这一危害；方言对齐留作后续项，不在本轮扩大契约面。
- **终态原因口径修订**：DEC-065 已发布的「恢复耗尽 → 一律为空」被修订；按值判别的
  下游行为不变，按「字段存在」判别的下游需要改为按值判别 → 下游通报（`MNT-202610-59`）。

## 6. 测试与退出条件

- [x] `MNT-202610-54`：带文本的不完整输出不结算为答案；片段不进答案、不进历史。
- [x] `MNT-202610-55`：恢复反馈按原因分派，且不指认未报告的原因。
- [x] `MNT-202610-56`：终态原因覆盖恢复耗尽终态；无原因时留空；载荷同口径。
- [x] `MNT-202610-57`：三个新用例 + 既有文案断言更新；三条变异全部被捕获（§7）。
- [x] 全量 ctest 通过（Linux x86_64 GCC Debug）与 `architecture-check`、`docs-check`、
  `format-check` 等质量门禁通过（见 §7）。
- [ ] 下游通报发布（`MNT-202610-59`，随 PR 一并发布）。
- [ ] Responses 方言 `content_filter` 映射对齐（`MNT-202610-60`，后续轮）。

## 7. 验证记录

2026-10-24：实现轮（工作树 = master `f7a0d25` + 本轮变更，未提交）。环境：Linux
x86_64，GCC 13.3.0，CMake debug 预设，clang-format 18.1.8。

- 实现：`src/model/conversation_loop.cpp`（文本终态分支排除不完整响应；恢复反馈按原因
  分派；恢复耗尽终态携带供应商报告的原因）、`include/mira/conversation_loop.hpp`
  （字段口径注释）。
- 测试：`tests/m3/m3_conversation_truncation_test.cpp` 新增 3 个用例；既有
  `truncated_reply_recovers_with_short_answer_guidance` 的反馈断言更新（不再断言
  「output-token limit」，改为断言「did not complete」且不含未报告的原因）。
- 变异验证（逐条执行并复原）：
  1. 去掉「不完整输出不作答案」的守卫 → `an_incomplete_output_is_never_the_answer`
     失败（答案变成片段）。
  2. 去掉恢复耗尽终态的原因赋值 → `an_incomplete_output_fails_closed_with_its_reason`
     失败（终态原因为空）。
  3. 把反馈改回指认输出上限 → 「context window」断言失败（反馈不再指认真实原因）。
- 全量回归与质量门禁：见本记录提交时的最终计数与命令输出。
- 限制与补跑：Android / Windows 编译路径与真实模型端到端未在本机执行（原因：无对应
  工具链与外部模型凭据；负责人：Mira Maintainers；补跑条件：PR CI matrix 与下游
  端到端复验）。
