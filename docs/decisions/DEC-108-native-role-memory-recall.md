# DEC-108：记忆召回的原生角色回放与事实带框块

> 状态：Accepted（实际部署反馈：召回上下文的说话人仅靠文本前缀标注，模型无法在输入层面分辨身份）
> 日期：2026-11-10
> 负责人：Mira Maintainers
> 冻结里程碑：M30 记忆上下文增量
> 替代/被替代：修订 [DEC-101](DEC-101-memory-agent-context-admission.md) 第 1 条、补充 [DEC-107](DEC-107-interaction-memory-lifecycle.md) 第 4 条

## 背景与问题

部署会话中，记忆 agent 拼接的召回上下文把用户与助手发言全部注入为
`role:"user"` 消息，说话人区分只存在于文本前缀（`user:` / `assistant:` /
`assistant said:`）中。投影层曾出现双重前缀（`conversation_log` 把
`assistant said:` 烘焙进条目文本，`entry_line` 再加 `assistant:` 前缀），
既占用 token，也不能被 LLM 在消息结构层面原生分辨。主流 agent harness
（Letta/MemGPT、LangChain、Mem0、OpenAI Assistants、Anthropic）的共识是：
**逐字会话回放用原生 user/assistant 消息角色区分说话人；事实与摘要合并为
单个带框架头的块**；没有主流实现在单条 user 消息内部用文本前缀区分说话人。

## 决策

1. **说话人结构贯穿**。`ConversationEntry::Kind` 不再在投影产物中丢失：
   `ConversationSegmentEntry` 携带 `kind`，`ContextIndexAsset`（仅
   ConversationSegment 资产）与 `ContextCandidate` 携带 speaker 标注的
   `entries`/`turns`，受 `ContextIndexPolicy::max_asset_entries` 约束。
2. **原生角色回放**。`ContextItem` 新增 `wire_role`（仅 `RetrievedMemory`
   可设置，且只允许 `User`/`Assistant`；`validate()` 拒绝 System/Developer）。
   `context_items_from_candidate` 把候选按连续同说话人轮次分组展开为逐条
   `ContextItem`：user 轮 → User 消息、assistant 轮 → Assistant 消息、
   loop-outcome 行保留 `loop:` 前缀并入相邻 User 消息；条目 id 由
   `context_asset_id_from_seed` 确定性派生。`role_for` 尊重 `wire_role`。
   **不可信召回数据永不获得 System/Developer 角色（RULE-09 不变）**；
   assistant 轮回放的是本会话真实产生过的助手发言，信任等级与既有
   P4/Public 候选相同，authority/provenance 标注全部保留。
3. **事实召回合并为带框块**。一次 Recall 的全部选中 `MemoryRecord.statement`
   合并为单个 `RetrievedMemory` 条目：`[recalled memory v1|count=N]` 头 +
   每条 statement 一行；条目 sensitivity 取所选记录的最严格值，provenance
   合并，id 由 operation id 确定性派生。选择语义不变（首批按 memory_ids
   过滤、空保留全部；扩展查询去重并受 `max_records` 约束）。
4. **前缀单一来源**。`entry_line` 是唯一的前缀渲染器，服务文本消费者
   （检索文本、consolidation transcript、curator、working-context seam）；
   `conversation_log` 的 AssistantSpeech 条目文本不再烘焙
   `assistant said:` / `assistant tool call` 前缀（工具调用行改为
   `tool call …`）。
5. **压缩检查点维持单条带框架 user 消息**（DEC-101 §7 不变，对齐业界
   compaction 惯例）；compress 系统提示要求摘要中引用发言归因到 speaker，
   使压缩后的上下文保留谁说了什么。

## 备选方案

继续用文本前缀标注（现状）：模型无法原生分辨身份且 token 开销随轮次增长；
把召回提升为 System 消息：违反 RULE-09 与 DEC-101 的准入边界；逐条事实
消息加来源前缀：开销依旧且仍是文本层标注。逐轮回放 + 事实带框块是与业界
实践一致且不放宽信任边界的方案。

## 影响与风险

`ContextItem`/`ConversationSegmentEntry`/`ContextCandidate`/`ContextIndexAsset`
为加法字段，公开 API 变化需要消费者重编译；candidate/item JSON codec 为
加法成员（读者忽略未知成员，schema 主版本不变）。逐轮回放可能增加
`request.input` 条目数，受既有 `max_input_items` 与候选字节预算约束。
召回的 assistant 轮若被污染的记忆存储伪造，模型会将其视为自己说过的话——
与既有 P4 候选相同的注入面，由 EventStore 派生的确定性重建（RULE-07）、
provenance 审计与 scope 隔离缓解；不新增存储 schema。

## 验证方式

m3（投影文案）、m17（segment kind、候选 turns、JSON round-trip、per-run
展开与越界截断）、m19（transcript 文案）、m30（召回带框块、scope 隔离、
真实 Gateway 请求）、m36（准入接受 Assistant wire_role、拒绝 System/
Developer、role_for 映射）测试覆盖；新增连续同说话人合并与 alternation
安全断言。逐轮原生角色在 wire 层的最终表现由 m36 admission 断言
（`request.input` 中 assistant 轮为 `ModelRole::Assistant`）。

## 关联文档

- [记忆代理设计](../design/memory_agent_design.md)
- [完整请求准入 DEC-101](DEC-101-memory-agent-context-admission.md)
- [交互记忆生命周期 DEC-107](DEC-107-interaction-memory-lifecycle.md)
