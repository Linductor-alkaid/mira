# DEC-104：AssistantSpeech 一等事件与会话视图的助手发言条目

> 状态：Accepted
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-BR-02

## 背景与问题

跨 burst 记忆的实现（宿主以"每 burst 一个新 task 的 ConversationLoop + 事件
存储回放前缀"延续个体记忆）只认 `UserMessageInjected`（用户的话）与两类
settled（机械结算记录）。事件模型没有"assistant 发言"的一等公民表达：
模型经工具发出的消息内容、终文本回答只活在 burst 内存 turn 历史里，burst
结束即消失。回放前缀里有"用户说了什么"，没有"个体答了什么"——多消息连续
对话时个体对自身发言零记忆。宿主想补记"自己说了什么"也没有合法入口
（伪造 `UserMessageInjected` 会污染说话者归属）。

此外，`ConversationSettled` 的 `Answered` 结算条目措辞不区分"经工具发言"
与"仅终文本"；在工具发声宿主下后者实为说话失败，渲染成无差别成功样例会
误导回放消费。

## 决策

1. **新事件 `AssistantSpeech`**（`EventClass::State`）：`ConversationLoop`
   在每轮对模型实际产出的话语记录一条事件：
   - `turn`：轮序号；`sensitivity`：`"Internal"`（模型自产内容分类，固定）。
   - `text`：该轮可见 assistant 文本（有界 4096 字符，超限截断并置
     `text_truncated`）。仅非空时输出。
   - `tool_calls`：该轮处理的工具提案数组（仅非空时输出），每项含
     `wire_name`、`arguments`（序列化后有界 1024 字符）、`arguments_digest`
     与 `status`（`executed` / `failed` / `skipped` / `not_executed`）。
   - 发射点：工具轮提案批处理完毕后一条（覆盖 abort 前的半轮），终文本
     回答一条。rejected/ambiguous/截断丢弃等未实际发声的输出不记录；
     MaxOutputTokens 的续写片段不单独记录（其最终完成文本由终文本条目承载）。
   - AgentLoop（设备闭环）不在本轮范围。
2. **会话视图条目**：`ConversationEntry` 新增 `Kind::AssistantSpeech`；
   `build_conversation_view` 识别 `AssistantSpeech` 事件并渲染
   `assistant said: <text>` 与逐调用 `assistant tool call <name> <arguments>
   (<status>)` 行（多行合入同一条目文本）。已识别事件的损坏载荷 fail closed
   （DataLoss），与既有投影纪律一致。宿主回放由此可拼出真正的多轮对话
   前缀，并可按 Kind 映射 assistant 角色，而非全部折叠进 User。
3. **结算措辞区分**：`ConversationSettled` 投影在 `outcome == "Answered"`
   时按 `tool_executions` 区分措辞：`> 0` 渲染
   `(terminal text + N tool call(s))`，否则渲染 `(terminal text only)`。
   措辞保持事实性——loop 不知道哪些工具"发声"，语义判断留给宿主；
   `LoopSettled`（AgentLoop）与非 Answered 结算不变。
4. **M17 分段**：`segment_conversation` 的条目行前缀为 AssistantSpeech
   增加 `assistant: `。

## 备选方案

- 最小方案（仅给 `ConversationSettled` payload 加 `answer_text` /
  `sent_summary`）：被否决为本轮主方案——它无法表达逐轮发言与工具调用
  参数，且在 settled 事件中复制大文本违反"不在多个事件中复制大内容"的
  事件纪律；终文本已由 AssistantSpeech 条目承载，settled 保持纯计数。
- 把发言渲染进 assistant 角色的重放请求：属于宿主投影策略，Core 只提供
  Kind 与文本事实，不替宿主决定角色折叠。

## 影响与风险

- 事件 schema 扩展：新事件类型只增不改，既有消费者按"未知事件跳过"纪律
  不受影响；视图重建会多出此前不存在的条目，缓存与派生资产的重建由宿主
  决定（同 DEC-016 先例）。
- 发言文本与工具参数进入事件存储：均为模型自产内容，`sensitivity` 标注
  `Internal`；大小有界（text 4096、arguments 1024/项）。宿主若经工具转发
  Secret 级输入，应自行把关工具参数的敏感性——事件面只承诺有界与分类
  标注，不承诺内容脱敏。
- `ConversationEntry::Kind` 新增值是公开枚举扩展，C++ 消费者需重编译；
  switch 完备性在库内更新（M17 分段）。
- 真实桥接宿主的回放收益需在宿主侧验证；本轮只验收 Linux 离线 fixture。

## 验证方式

测试覆盖：真实 ConversationLoop 的工具轮与终文本各产生一条 AssistantSpeech，
载荷字段、有界与截断标记、状态值正确；视图渲染逐行文本与 Kind、顺序、源
ID、序列号正确；损坏载荷（非字符串 text、非数组 tool_calls、缺字段、无
发言内容）返回 DataLoss；settled 措辞两条路径；M17 分段含 assistant 行。

## 关联文档和工作项

- [维护计划](../plans/maintenance-2026-11-bridge-host-feedback.md)
- [DEC-016](DEC-016-conversation-events-and-user-messages.md)（事件投影先例，本决策扩展其条目类型）
- [持续会话设计](../design/continuous-conversation-loop.md)
- [Context Intelligence 设计](../design/context_intelligence_design.md)
- [模型 API](../api/model-agent-loop.md)
