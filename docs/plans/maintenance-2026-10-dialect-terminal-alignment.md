# 维护计划：方言终态分类对齐（2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：M3 方言与模型契约（Completed）、
> [DEC-066](../decisions/DEC-066-conversation-incomplete-output-is-not-an-answer.md)（不完整输出）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-25

## 1. 目标

关闭 `MNT-202610-60` 登记项：让三个方言按[设计 §7.2](../design/llm-api-protocol-design.md)
把供应商明确报告的**过滤与拒绝**信号翻译成规范终态。Responses 方言此前把 content filter
折叠进 `Incomplete(Other)`、把拒绝留成「完成但没有文本」；Messages 方言把
`stop_reason: "refusal"` 归一化成内容过滤。三者都会让循环**重试**一个被过滤或被拒绝的
回复——正是设计 §7.2 要求「不得绕过」的路径
（[DEC-067](../decisions/DEC-067-dialect-terminal-classification-alignment.md)）。

## 2. 范围与非目标

### 2.1 范围

- `src/model/model_responses.cpp`：`incomplete_details.reason == "content_filter"` →
  `ContentFiltered`（并清空 `incomplete_reason`）；`Completed` 且输出携带拒绝 →
  `Refused`。
- `src/model/model_anthropic.cpp`：`stop_reason == "refusal"` → `Refused`，并附固定摘要的
  `RefusalOutput`（不回显供应商原文）。
- `src/model/model_dialect_internal.hpp` / `model_dialect.cpp`：共享助手
  `carries_refusal()`。
- 测试：`tests/m3/m3_dialect_test.cpp`、`tests/m3/m3_anthropic_test.cpp`、
  `tests/m3/m3_conversation_loop_test.cpp` 三个用例；共享夹具
  `content_filtered_response()` 落位 `tests/support/m3_support.hpp`。
- 文档：[DEC-067](../decisions/DEC-067-dialect-terminal-classification-alignment.md)、
  API 手册方言终态节、总计划索引与本记录。

### 2.2 非目标

- 不改模型契约的枚举取值与语义（`ModelCompletionStatus`、`IncompleteReason`、
  `parse_decision()` 映射均不动）。
- 不改 Chat Completions 方言（已符合设计 §7.2）。
- 不改循环的失败策略（`Refused`/`ContentFiltered` 的 fail closed 早已存在）。
- 不改工作流恢复层对 `ContentFiltered`/`Refused` 的分类文案（另一模块的独立事项）。
- 不在 Mira 侧做内容策略判断（过滤与拒绝都是供应商报告的事实）。
- 不涉及 Executor 能力边界 → **不**登记
  [`docs/executor_feedback/ledger.md`](../executor_feedback/ledger.md)。

## 3. 设计与决策依据

- [设计 §7.2](../design/llm-api-protocol-design.md)：本轮的唯一规范来源——「明确
  refusal → `Refused`」「content filter → `ContentFiltered`」是**已冻结**的方言无关映射。
- [DEC-067](../decisions/DEC-067-dialect-terminal-classification-alignment.md)：本轮的
  实现符合性决定、影响与验证方式。
- [DEC-066](../decisions/DEC-066-conversation-incomplete-output-is-not-an-answer.md)：
  不完整输出（`Incomplete`）的处置——本轮**不覆盖**它：供应商说「没说完」的响应保持
  自己的终态，走恢复路径，不被改判成拒绝。

## 4. 工作项

- [x] `MNT-202610-61` Responses 方言：`content_filter` → `ContentFiltered`，
  `incomplete_reason` 清空；验收：方言用例断言终态与理由字段。
- [x] `MNT-202610-62` Responses 方言：明确的拒绝 → `Refused`；验收：拒绝内容 part 与
  拒绝输出项两种形态都断言终态。
- [x] `MNT-202610-63` Messages 方言：`stop_reason == "refusal"` → `Refused` + 固定摘要的
  `RefusalOutput`；验收：Anthropic 用例断言终态与摘要文本。
- [x] `MNT-202610-64` 循环侧行为：被过滤的响应让对话循环 fail closed，不消耗恢复预算、
  不重试、不产生答案；验收：循环用例断言恢复次数为 0 且只发出一次模型请求。
- [x] `MNT-202610-65` 测试与变异验证：三个用例 + 三条变异（见 §7）。
- [x] `MNT-202610-66` 文档同步：DEC-067、API 手册方言终态节、总计划索引与本记录。
- [x] `MNT-202610-67` 下游通报：issue #100 下的行为变化通报（问题、修正后行为、下游需
  复核的两点、验证证据与已知遗留）。
- [ ] `MNT-202610-68` 后续项（**本轮不修**）：Responses 方言把供应商的拒绝原文回显进
  `safe_summary`，而 Chat 方言用固定摘要。`safe_summary` 是有界摘要字段，回显模型原文
  与该字段的语义纪律不符。修正要统一两个方言的拒绝摘要策略，属独立一轮。

## 5. 风险与阻塞

- **可观察行为变化**：三类响应（Responses content_filter、Responses 拒绝、Messages
  refusal）此前触发循环恢复，现在直接 fail closed。宿主会看到不同的终态与不同的恢复
  次数 → 以 DEC-067 与下游通报明示。
- **恢复预算不再被消耗**：依赖恢复次数做配额推算的宿主需要复核。
- **终态分类变化**：Messages 的拒绝从 `ContentFiltered` 变为 `Refused`；按终态分类的
  消费方（如下游的 `error_kind` 映射）需要复核。
- **既有偏差未修**（`MNT-202610-68`）：Responses 的拒绝原文回显。本轮只修终态分类，
  不扩大摘要策略的改动面。

## 6. 测试与退出条件

- [x] `MNT-202610-61`…`MNT-202610-64`：四个行为断言全部落地（两个方言用例 + 一个循环
  用例）。
- [x] `MNT-202610-65`：三条变异全部被捕获（§7）。
- [x] 全量 ctest 通过（Linux x86_64 GCC Debug）与 `architecture-check`、`docs-check`、
  `format-check` 等质量门禁通过（见 §7）。
- [x] 下游通报发布（`MNT-202610-67`）。

## 7. 验证记录

2026-10-25：实现轮（工作树 = master `28bbc4d` + 本轮变更，未提交）。环境：Linux
x86_64，GCC 13.3.0，CMake debug 预设，clang-format 18.1.8。

- 实现：`src/model/model_responses.cpp`（content_filter → `ContentFiltered`；拒绝 →
  `Refused`）、`src/model/model_anthropic.cpp`（`refusal` → `Refused` + 固定摘要）、
  `src/model/model_dialect_internal.hpp` + `src/model/model_dialect.cpp`
  （共享助手 `carries_refusal()`）。
- 测试：`tests/m3/m3_dialect_test.cpp` 新增
  `responses_filter_and_refusal_are_their_own_terminals`；`tests/m3/m3_anthropic_test.cpp`
  新增 `refusal_is_the_refused_terminal`；`tests/m3/m3_conversation_loop_test.cpp` 新增
  `content_filtered_response_fails_closed_without_a_retry`；共享夹具
  `content_filtered_response()` 落位 `tests/support/m3_support.hpp`。
- 变异验证（逐条执行并复原）：
  1. 去掉 Responses 的 content_filter 映射 → `m3_dialect_test.cpp:389` 失败（终态回到
     `Incomplete`）。
  2. 去掉 Responses 的拒绝判定 → `m3_dialect_test.cpp:403` 失败（终态回到 `Completed`）。
  3. 把 Messages 的拒绝改回 `ContentFiltered` → `m3_anthropic_test.cpp:167` 失败。
- 全量回归：`ctest --test-dir build/debug --output-on-failure` **113/113 通过**
  （Linux x86_64，GCC 13.3.0，CMake debug 预设），ctest 退出码 0。
- 质量门禁：`format-check`（270 个文件）、`docs-check`、`sbom-check`、
  `platform-boundary-check`、`architecture-check` 全部通过（行数预算 1200：
  `model_responses.cpp` 494 行、`model_anthropic.cpp` 408 行、`model_dialect.cpp` 793 行、
  `m3_dialect_test.cpp` 768 行、`m3_anthropic_test.cpp` 569 行、
  `m3_conversation_loop_test.cpp` 1066 行、`m3_support.hpp` 505 行，均在预算内）。
- 限制与补跑：Android / Windows 编译路径与真实模型端到端未在本机执行（原因：无对应
  工具链与外部模型凭据；负责人：Mira Maintainers；补跑条件：PR CI matrix 与下游
  端到端复验）。
