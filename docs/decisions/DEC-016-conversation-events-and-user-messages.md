# DEC-016：对话事件、会话投影与步边界用户消息

> 状态：Accepted
> 日期：2026-09-07
> 负责人：Mira Maintainers
> 冻结里程碑：M4 后维护轮（[maintenance-2026-09-agent-harness-closure.md](../plans/maintenance-2026-09-agent-harness-closure.md)）
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §6.2/§6.3
> 所述 Conversation 一等公民的最小子集；完整 patch 语义仍由 M8-04 冻结）

## 背景与问题

DEC-014 审计确认：代码中不存在 Conversation 实体（"conversation" 仅为供应商 wire 字段），
用户无法在任务执行中介入；会话没有可重建的对话视图。架构设计 §6.2/§6.3 要求 Session
承载共享状态、Conversation History 作为按 `RULE-07` 管理的持久化工件（EventStore 是事实
源、会话视图是投影），完整语义（patch、三类目标区分）属 M8-04。缺一个立即可用的最小
机制让 Harness 在 M8 之前具备"用户可在运行中介入、事后可回看对话"的能力。

## 决策

1. **步边界消息队列**：`AgentLoop::enqueue_user_message` 提供线程安全、有界（默认 16 条，
   超限拒绝，`RULE-08`）的用户消息队列；消息在循环步边界（每步迭代开头、取消检查之后）
   被取出，之后作为常驻指令注入本轮及后续每轮模型请求（用户意图不因单轮而消失）。
2. **对话事件**：消息注入时发 `UserMessageInjected`（载荷含消息文本、长度与 digest，
   `EventClass::State`）；循环终态沿用既有 `LoopSettled`。事件不重复携带大载荷。
3. **会话投影**：`build_conversation_view(IEventStore, SessionId)` 从事件存储按会话顺序
   重建 `UserMessage` / `LoopOutcome` 两类条目；EventStore 仍是唯一事实源，视图可随时重建
   （`RULE-07`/DEC-003）。不引入第二存储。
4. **脱敏责任**：入队文本由宿主按其数据策略先行脱敏（如凭据、输入法敏感内容）；Mira 在
   事件与请求中不尝试自行猜测敏感字段。回填模型请求时以 `mira.agent-loop.user-message.v1`
   provenance 来源标记，不提升权威（`RULE-09`）。
5. 本决策不冻结对话 patch、暂停/恢复语义、WorkflowRun 介入或跨任务会话合并——它们仍由
   M8-04 与后续阶段决策承载；本轮机制是它们的载体而非替代。

## 备选方案

- **等 M8-04 一次性交付完整 Conversation**：运行中介入能力继续缺失，miracle demo 无法
  验证方向；最小机制先行、语义后冻存的分阶段方式更稳，不采用。
- **把对话历史直接写入独立存储（新表/新文件）**：违反 `RULE-07` 单一事实源，制造平行
  状态，不采用。
- **队列语义为"仅下一轮生效"**：循环每轮重建上下文，一次性注入会让用户指令在后续轮次
  消失，违背"任务调整持续有效"的产品语义，不采用。

## 影响与风险

- 公开 API 新增 `conversation_log.hpp`；`AgentLoop` 新增 `enqueue_user_message` 与配置
  字段；`AgentLoop` 对象自此可被并发入队，队列互斥由循环自有（所有权清晰）。
- 用户消息文本进入 EventStore 与模型请求：宿主必须遵守第 4 条脱敏责任；该约束写入
  API 手册。
- 事件 schema 新增类型字符串（`UserMessageInjected`）：载荷版本化随事件 envelope 既有
  机制，无兼容性迁移。

## 验证方式

- 单测：入队边界（满队列拒绝）、步边界注入（脚本化 Provider 断言请求中出现用户消息与
  provenance）、事件序列、投影重建（含空会话、乱序追加）。
- 集成测试：Runtime 会话内运行中介入 + 事后投影回看（见维护计划）。

## 2026-11-07：ConversationLoop 终态投影补充

`BUG-20261107-002` / `MNT-202611-15`：投影同时识别 AgentLoop 的 `LoopSettled`
和 ConversationLoop 的 `ConversationSettled`，两者均产生 `LoopOutcome`。保留生产者
事件名和载荷，不改写历史事件，也不移除 AgentLoop 支持。ConversationSettled 的摘要
按已存在的数值字段展示 turns、total_turns、recoveries、tool_executions；turns 是保留记录数，
total_turns 是全程推进尝试数。旧 LoopSettled 保持原 steps 摘要。

只解析视图关心的事件：UserMessageInjected 的 text、两类终态的 outcome 必须可读，
否则返回 mira.conversation/DataLoss；无关事件即使载荷不可读也跳过。可选计数字段
缺失或非数值时不展示，不凭空补零。投影保持 session 顺序、源 EventId、时间戳和
session_sequence，可从现存日志直接重建，无 schema/ABI 或存储迁移。
这是公开投影行为修复，旧缓存视图须重建，已生成的下游派生资产由宿主按其策略重建。
不把运行摘要当作模型答案，不扩大到 checkpoint reducer 或长期记忆固化事件消费面。
不新增线程或调度器，继续复用既有 Executor fixture。

方案不采用重命名生产者事件（会改变消费契约），也不把未知事件当错误。
依据与验证见[会话终态投影维护计划](../plans/maintenance-2026-11-conversation-outcomes.md)。

## 关联文档和工作项

- [维护计划：Agent Harness 闭合（2026-09）](../plans/maintenance-2026-09-agent-harness-closure.md)
- [DEC-003](DEC-003-event-sourced-persistence.md)、[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)、
  [DEC-015](DEC-015-builtin-tool-execution-boundary.md)
- [Agent Harness 参考研究](../design/harness_reference_study.md) §5.2（Pi 消息队列模式）
- M8-04（后续完整语义冻结点）
