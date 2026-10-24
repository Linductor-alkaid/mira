# 维护计划：对话终态截断原因（Mirage 反馈 #100 消化，2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：M25（Completed）、M3 对话循环（MNT-202610-03/04，Completed）、DEC-056 持续模式
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-24

## 1. 目标

消化 Mirage 第五轮依赖反馈 [mira#100](https://github.com/Linductor-alkaid/mira/issues/100)
（`MIRA-20261022-001`，P2，可观测性能力缺口）：让 `ConversationLoop` 的**终态截断原因
成为结构化事实**——续写预算耗尽导致的 `Failed` 在结果对象上带可选终态原因，在
`ConversationSettled` 事件载荷上带续写计数与原因名；其余所有终态保持空/缺省，不误报
（[DEC-065](../decisions/DEC-065-conversation-terminal-truncation-reason.md)）。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/conversation_loop.hpp` / `src/model/conversation_loop.cpp`：
  `ConversationLoopResult::terminal_incomplete_reason`（`std::optional<IncompleteReason>`）；
  续写预算耗尽结算点置位；`ConversationSettled` 载荷增量键
  `output_continuations`（始终存在）与 `incomplete_reason`（仅终态截断时存在）。
- `include/mira/model_contracts.hpp` / `src/model/model_contracts.cpp`：
  `incomplete_reason_name()` 由 TU 内部实现升为公开声明（签名与返回值不变），使事件
  载荷里的稳定名成为可被下游比对的公开契约。
- 测试：新增 `tests/m3/m3_conversation_truncation_test.cpp`（Finite / Continuous 两种
  limit mode 的终态矩阵 + 不误报矩阵）；共享夹具 `incomplete_response()` 迁入
  `tests/support/conversation_loop_support.hpp`；既有用例补一条终态原因断言。
- 文档：[DEC-065](../decisions/DEC-065-conversation-terminal-truncation-reason.md)、
  `docs/api/model-agent-loop.md` ConversationLoop 节、
  `docs/design/continuous-conversation-loop.md` 计数与终态口径。

### 2.2 非目标

- 不改 `safe_summary` 既有文案（上游测试已固化为契约）；结构化字段是新增判别入口。
- 不新增事件类型、不动事件 schema 版本；本轮只做增量键扩展。
- 不改 `ConversationLoop` 的预算、取消、准入、工具执行语义。
- 不改 `ModelGateway::infer` 的 `incomplete_reason` 语义（它不经循环）。
- 不在 Mira 侧实现下游的 IPC 词表或 UI 呈现（issue #100 明确不要求）。
- 不改下游 Mirage 仓库；下游获授权消费 pin 升级后自行删除 `safe_summary` 文本锚点。

## 3. 设计与决策依据

- [DEC-065](../decisions/DEC-065-conversation-terminal-truncation-reason.md)：本轮全部
  公开契约新增（终态原因字段、事件增量键、公开的理由名函数）。
- [DEC-047](../decisions/DEC-047-conversational-loop-and-canonical-tool-parts.md)：
  ConversationLoop 既有循环、终态与规范工具往返不变。
- [DEC-056](../decisions/DEC-056-continuous-conversation-loop.md)：持续模式的段内额度与
  累计计数口径；终态字段在两种 limit mode 下语义一致。
- [DEC-064](../decisions/DEC-064-memory-event-surface.md)：事件载荷增量键与「键存在即
  语义」的同形先例。
- 台账流程：本轮不涉及 Executor 能力边界，**不**登记
  [`docs/executor_feedback/ledger.md`](../executor_feedback/ledger.md)。

## 4. 工作项

- [x] `MNT-202610-49`（#100 / MIRA-20261022-001）结果对象新增终态截断原因字段
  `terminal_incomplete_reason`：仅续写预算耗尽导致的 `Failed` 置位
  （`MaxOutputTokens`），其余终态为空。验收：`Answered` / `MaxTurns` / `Cancelled` /
  其它 `Failed` 均不置位；与既有 `output_continuations` 计数口径分离且写进头注释。
- [x] `MNT-202610-50`（#100 / MIRA-20261022-001）`ConversationSettled` 载荷增量键：
  `output_continuations` 始终存在（int64，整 run 累计），`incomplete_reason` 仅终态
  截断时存在（`incomplete_reason_name()` 的稳定名）。验收：事件面消费方不持有 loop
  结果也能读到终态原因与其余终态的缺省语义。
- [x] `MNT-202610-51`（#100 / MIRA-20261022-001）测试矩阵：Finite 与 Continuous 两种
  limit mode 下的终态原因、事件载荷键、以及「不误报」矩阵（正常应答、容忍截断后应答、
  非 max_tokens 的 Incomplete 恢复耗尽、工具预算耗尽、MaxTurns、取消）。验收：全部
  用例有断言，无跳过项；变异验证记录见 §7。
- [x] `MNT-202610-52`（#100 / MIRA-20261022-001）文档同步：DEC-065、API 手册
  ConversationLoop 节、持续循环设计的计数与终态口径、总计划索引与本记录。
- [x] `MNT-202610-53`（#100 / MIRA-20261022-001）对外动作：PR
  [#108](https://github.com/Linductor-alkaid/mira/pull/108)（分支
  `feat/conversation-terminal-truncation-reason`）与 issue #100 的公开结论回复
  （含契约形状、验收条件对照、验证证据与未覆盖范围）。

## 5. 风险与阻塞

- **误报风险**：若把终态原因挂到所有 `Incomplete` 终态，恢复耗尽会被误分类为输出
  上限耗尽。本轮用「不误报」用例矩阵与变异验证守住（§7 第三条记录）。
- **口径混淆风险**：下游若以 `output_continuations > 0` 判断「本次以输出上限结束」会
  误判（截断后成功的运行计数同样为正）。API 手册与 DEC-065 都写明：终态看字段，过程
  看计数。
- **事件消费方适配**：新增键对忽略未知键的消费方无影响；按键存在判定的消费方需要
  复核。属追加式契约变更，不破坏既有读取。
- 下游真实模型与 Mirage 主机的端到端复验属下游动作（issue #100 验收条件最后一条），
  记录于 §7 限制。

## 6. 测试与退出条件

- [x] `MNT-202610-49`：终态原因字段在唯一结算点置位，其余终态为空（用例矩阵）。
- [x] `MNT-202610-50`：事件载荷两个增量键的存在性与取值（用例矩阵 + 载荷级断言，
  这是本仓库第一条 `ConversationSettled` 载荷级断言）。
- [x] `MNT-202610-51`：Finite / Continuous 两套语义一致；不误报矩阵全覆盖。
- [x] 全量 ctest 通过（Linux x86_64 GCC Debug）与 `architecture-check`、`docs-check`、
  `format-check` 通过（见 §7）。
- [ ] Mirage pin 升级后的下游端到端复验（**不勾选**，见 §7 限制与补跑条件）。

## 7. 验证记录

2026-10-24：实现轮（工作树 = master `e4f116f` + 本轮变更，未提交）。环境：Linux
x86_64，GCC 13.3.0，CMake debug 预设，clang-format 18.1.8。

- 实现：`include/mira/conversation_loop.hpp`（`terminal_incomplete_reason` 字段 +
  `model_contracts.hpp` 显式包含）、`src/model/conversation_loop.cpp`（续写预算耗尽
  结算点置位；`ConversationSettled` 载荷增量键）、
  `include/mira/model_contracts.hpp` + `src/model/model_contracts.cpp`
  （`incomplete_reason_name()` 由匿名命名空间移入 `mira` 命名空间并公开声明，签名与
  返回值不变）。
- 测试：新增 `tests/m3/m3_conversation_truncation_test.cpp`（注册
  `mira_m3_conversation_truncation_test`），3 个用例 / 56 条断言：
  1. `output_limit_exhaustion_reports_the_terminal_reason`：Finite 模式续写预算耗尽 →
     `outcome == Failed`、`output_continuations == 2`、终态原因为 `MaxOutputTokens`；
     `ConversationSettled` 载荷 `output_continuations == 2`、
     `incomplete_reason == "max_output_tokens"`。
  2. `other_terminals_carry_no_truncation_reason`：正常应答、容忍截断后应答（计数为 1
     而终态原因为空）、非 max_tokens 的 Incomplete 恢复耗尽（`recoveries == 2`、
     计数为 0）、工具预算耗尽、MaxTurns、取消——终态原因一律为空，载荷无
     `incomplete_reason` 键。
  3. `continuous_mode_keeps_the_same_terminal_semantics`：Continuous 模式段内额度耗尽 →
     终态原因置位、载荷计数为整 run 累计；完整工具轮重置段内额度后运行答出，计数为 2
     而终态原因为空。
  夹具整理：`incomplete_response()` 从 `tests/m3/m3_conversation_loop_test.cpp` 迁入
  共享支持头 `tests/support/conversation_loop_support.hpp`（同一份实现，两处用例共用），
  并新增 `settled_detail()` 载荷读取助手；既有
  `continuation_budget_exhaustion_settles_output_limit` 补一条终态原因断言。
- 变异验证（逐条执行并复原）：
  1. 去掉续写预算耗尽结算点的终态原因赋值 → `m3_conversation_truncation_test.cpp:65`
     断言失败（终态原因为空）。
  2. 去掉 `ConversationSettled` 的两个增量键 → `:69` 断言失败（载荷无计数键）。
  3. 让恢复耗尽结算也挂上 `MaxOutputTokens` → `:121` 断言失败（不误报矩阵命中）。
- 全量回归：`ctest --test-dir build/debug --output-on-failure` **113/113 通过**
  （新增 `mira_m3_conversation_truncation_test` 计入总数），ctest 退出码 0；既有 5 个
  对话循环测试目标（loop / continuous / input_supply / input_reuse /
  history_transform）全部通过。
- 静态检查：`architecture-check`（行数预算 1200；本轮 `conversation_loop.cpp` 859 行、
  `conversation_loop.hpp` 295 行、新测试文件 245 行、共享支持头 263 行，均在预算内；
  `src/model/model_contracts.cpp` 的超长是既有基线抑制项，本轮行数净变化为 0）、
  `docs-check`、`format-check`、`sbom-check`、`platform-boundary-check` 通过。
- 限制与补跑：Android / Windows 编译路径与真实模型端到端未在本机执行（原因：本机
  无对应工具链与外部模型凭据；负责人：Mira Maintainers；补跑条件：PR CI matrix 与
  Mirage pin 升级后的下游复验）。issue #100 的公开结论回复随 PR 一并发布
  （`MNT-202610-53`）。

2026-10-24：PR [#108](https://github.com/Linductor-alkaid/mira/pull/108)（分支
`feat/conversation-terminal-truncation-reason`）已开出，并在 issue #100 下发布公开结论
回复（契约形状、验收条件对照、验证证据与未覆盖范围）。PR CI 证据待 CI 完成后回填
本节。
