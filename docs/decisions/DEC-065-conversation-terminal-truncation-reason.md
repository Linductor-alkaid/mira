# DEC-065：对话循环的终态截断原因

> 状态：Accepted
> 日期：2026-10-24
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 对话终态截断维护轮
> 替代/被替代：无
> 修订：置位口径由 [DEC-066](DEC-066-conversation-incomplete-output-is-not-an-answer.md)
> 部分修订（2026-10-24）
> 关闭的缺口：ConversationLoop 终态不携带结构化的截断原因，事件面不可观测
> （Mirage 反馈 `MIRA-20261022-001` / [mira#100](https://github.com/Linductor-alkaid/mira/issues/100)）

## 背景与问题

`ConversationLoop` 的有界输出续写能力已经交付：`max_tokens` 截断 → 保留部分文本续写 →
续写预算耗尽后以显式 output-limit summary 结算 `Failed`。但**终态不携带结构化的截断
原因**，留下三个缺口：

1. **宿主拿结果对象无法分类终态**。`ConversationLoopResult` 上区分「输出上限耗尽」与
   其他 `Failed`（恢复耗尽、工具预算、MaxTurns、取消、拒绝）的唯一信号是
   `safe_summary` 的文本锚点。下游把判别写成了字符串匹配——文案一旦演进，分类静默失配，
   而且没有任何契约把这段文案钉死。
2. **只订阅事件面的消费者完全看不到截断**。`ConversationSettled` 载荷只有
   `outcome` / `turns` / `total_turns` / `recoveries` / `tool_executions` /
   `turn_records_dropped`，没有任何截断字段；不持有 loop 结果的宿主进程或面板拿不到
   终态原因。
3. **失败没有转化为明确的结果与事件**。可观测性纪律要求失败对调用方和 Observer 可见，
   而「输出上限耗尽」当前既不是结果字段也不是事件字段。

Mirage 反馈 `MIRA-20261022-001`（P2，可观测性能力缺口）明确要求：**终态截断原因成为
结构化事实**，并把接口形状交由上游定案。本记录定案该形状。

## 决策

### 1. 结果对象携带终态截断原因

`ConversationLoopResult` 新增：

```cpp
// Terminal truncation reason (MIRA-20261022-001 / DEC-065): set only when
// the run settled Failed because a tolerated max_tokens cutoff exhausted
// the continuation budget, and then carries MaxOutputTokens.
std::optional<IncompleteReason> terminal_incomplete_reason;
```

- **仅在续写预算耗尽导致的 `Failed` 终态置位**，值为
  `IncompleteReason::MaxOutputTokens`。
- `Answered`、`MaxTurns`、`Cancelled` 与其它 `Failed`（恢复耗尽、工具预算、请求装配
  失败、拒绝、准入拒绝、取消、deadline）**一律为空**。
- 采用 `std::optional<IncompleteReason>` 而不是布尔标记：理由词表（`MaxOutputTokens` /
  `ContextWindow` / `Other`）已经存在于模型契约（`model_contracts.hpp`），复用它可以让
  未来新增的容忍截断原因不必再扩公开面。
- （2026-10-24 注记：本条置位口径已由
  [DEC-066](DEC-066-conversation-incomplete-output-is-not-an-answer.md) 部分修订——
  终态失败由不完整输出造成时，字段携带**供应商报告的原因**，不再限于续写预算耗尽
  一条路径；「供应商未报告原因则留空」的口径不变。按值判别的消费方行为不变。）

### 2. 事件面携带续写计数与（终态时）原因名

`ConversationSettled` 载荷做**增量键**扩展：

| 键 | 载荷 | 存在性 | 口径 |
| --- | --- | --- | --- |
| `output_continuations` | int64 | **始终存在** | 整 run 累计的被容忍截断次数 |
| `incomplete_reason` | string | **仅终态截断时存在** | `incomplete_reason_name()` 的稳定名（如 `max_output_tokens`） |

- `incomplete_reason` 的**存在性**即语义：该键缺省表示「本次终态不是输出上限耗尽」，
  不是「原因未知」也不是空串。消费方必须按键存在判定。
- 事件载荷的 `detail` 形状（`{"task_id","task_epoch","detail"}`）与既有事件一致，
  本轮**不新增事件类型**，事件 schema 只做增量键扩展，不触发行号或事件版本。

### 3. 计数与终态原因口径分离

`output_continuations` 计的是**过程中**被容忍的截断次数——模型截断后续写成功并给出
答案时，计数为正而终态原因为空；续写预算耗尽时，两者同时为正。两者口径不同，下游
需要的是「为什么结束」而不是「过程中截断过几次」，因此终态原因单独成字段。

Continuous 模式下续写额度按**连续截断段**计算（完整工具轮后重置），而 `output_continuations`
计的仍是整 run 累计；终态原因在两种 limit mode 下语义一致。

### 4. 既有 `safe_summary` 文案不变

`"output-limit continuation budget exhausted; …"` 既有文案保持不变（上游测试已把这段
文本语义固化为契约）。结构化字段是**新增**的判别入口，不是替换；下游可以自行迁移到
结构化字段后删除文本锚点。

### 5. `incomplete_reason_name()` 升为公开声明

该函数此前是 `model_contracts.cpp` 内部实现（无头文件声明），事件载荷一旦公开这份
字符串，它就是公开契约的一部分，因此在 `model_contracts.hpp` 补上声明（签名与返回值
不变）。下游可据此比较事件载荷里的字符串，而不必自己钉死字面量。

## 备选方案

- **布尔标记 `output_limit_exhausted`**：拒绝。它只能表达「输出上限耗尽」这一种事实，
  丢失模型契约已有的原因词表；将来出现第二种被容忍的截断原因时只能再加一个布尔量。
- **继续让下游解析 `safe_summary` 文本**：拒绝。文本语义目前只由上游测试间接固化，
  任何文案演进都会静默破坏下游分类——这正是本条要消除的耦合。
- **下游自建第二个 model/tool 循环去拿 `ModelGateway::infer` 的 `incomplete_reason`**：
  拒绝。会重复 `ConversationLoop` 已交付的预算、取消、准入、续写与结算逻辑，违反下游
  复用纪律；且 `incomplete_reason` 根本不经过循环。
- **新增一个专门的截断事件类型（如 `OutputTruncated`）**：拒绝。本轮要解决的是**终态**
  可观测性，终态事件已经存在；新增事件名要动事件版本与消费方白名单，代价大于收益。
  过程中的截断次数由 `ConversationSettled.output_continuations` 覆盖。
- **把终态原因挂在所有 `Incomplete` 终态上**：拒绝。恢复耗尽（provider 报告
  `ContextWindow` / `Other` / 无原因）不是输出上限耗尽，挂上去就是误报——下游验收条件
  明确要求恢复耗尽时字段为假/缺省。
- **把 `safe_summary` 换成结构化错误码**：拒绝。本轮只补终态可观测性，不动既有结算
  语义与文案契约；两者可以并存。

## 影响与风险

- **公开契约追加**：结果对象新增一个带默认值的可选字段，既有调用方源码兼容；事件
  载荷新增两个键，按键存在判定的消费方需要适配，忽略未知键的消费方不受影响。
- **`incomplete_reason_name()` 的链接可见性变化**：由 TU 内部实现变为公开符号，签名与
  返回值不变，既有内部调用点行为不变。
- **不得误报**：终态原因只在唯一一处赋值（续写预算耗尽）。其它 `Failed` 终态若被误挂，
  下游会把恢复耗尽、工具预算等问题误分类为输出上限问题——这是本轮最主要的回归风险，
  由「不误报」用例矩阵与变异验证守住。
- **计数与原因的口径混淆**：下游若把 `output_continuations > 0` 当成「本次以输出上限
  结束」会误判（截断后成功的运行计数同样为正）。API 手册与本记录都写明：终态看字段，
  过程看计数。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

`tests/m3/m3_conversation_truncation_test.cpp`（新增）三个用例：

- `output_limit_exhaustion_reports_the_terminal_reason`：续写预算耗尽 → 结果字段为
  `MaxOutputTokens`，`ConversationSettled` 载荷含 `output_continuations == 2` 与
  `incomplete_reason == "max_output_tokens"`。
- `other_terminals_carry_no_truncation_reason`：正常应答、容忍截断后应答、非
  max_tokens 的 Incomplete 恢复耗尽、工具预算耗尽、MaxTurns、取消——终态原因一律为空，
  载荷**无** `incomplete_reason` 键。
- `continuous_mode_keeps_the_same_terminal_semantics`：Continuous 下终态字段置位、
  载荷计数为整 run 累计；完整工具轮重置段内额度后，终态原因仍为空。

**变异验证**（实现轮执行，见维护计划验证记录）：去掉终态字段赋值 → 第一个用例失败；
去掉两个事件键 → 载荷断言失败；让恢复耗尽也挂上终态原因 → 「不误报」用例失败。

## 关联文档和工作项

- 下游反馈：Mirage `MIRA-20261022-001`（[mira#100](https://github.com/Linductor-alkaid/mira/issues/100)）
- 工作项：[`maintenance-2026-10-conversation-terminal-truncation`](../plans/maintenance-2026-10-conversation-terminal-truncation.md)
- API：[`docs/api/model-agent-loop.md`](../api/model-agent-loop.md) 的 ConversationLoop 节
- 设计：[`docs/design/continuous-conversation-loop.md`](../design/continuous-conversation-loop.md)
- 相关：[DEC-047](../decisions/DEC-047-conversational-loop-and-canonical-tool-parts.md)（对话循环契约）、
  [DEC-056](../decisions/DEC-056-continuous-conversation-loop.md)（持续模式与计数口径）、
  [DEC-064](../decisions/DEC-064-memory-event-surface.md)（事件面增量键的同形先例）
