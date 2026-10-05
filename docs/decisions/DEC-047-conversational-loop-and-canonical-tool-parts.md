# DEC-047：无观察对话循环与规范工具回填契约

> 状态：Accepted
> 日期：2026-10-05
> 负责人：Mira Maintainers
> 冻结里程碑：维护轮（2026-10，Mirage 反馈第二轮）
> 替代/被替代：无（扩展 [DEC-015](DEC-015-builtin-tool-execution-boundary.md) 的回填面，不改变其执行边界）

## 背景与问题

下游 Mirage 桌面 Host 反馈（[mira#73](https://github.com/Linductor-alkaid/mira/issues/73)，
MIRA-20261004-001）：现有 `AgentLoop` 是设备闭环——每步强制 `ObservationMode::Full`
（`required.screen`），没有 screen provider 的环境在第一次模型调用前就失败；且文本回答
必须伪装成 `done`/`fail` 决策 JSON 并经设备观察验证。通用对话 harness（文字问答 +
工具往返）无法由现有公开 API 承载，Mirage 只能在集成层自建受限 adapter。

同时，工具结果回填存在契约缺口：`build_tool_result_input` 产出的是线格式 `JsonValue`
（`function_call_output` / tool role 消息），无法作为 `ModelInputItem` 加入
`ModelRequest.input`——`ModelInputItem` 只有 Text/Image/File 三种 part。运行时只能把工具
结果序列化为文本粘贴进 user item（`AgentLoop` 现状），供应商侧失去原生
function_call/function_call_output 语义。

## 决策

1. **规范工具 part**：`ModelContentPart` 新增两个闭式变体（`include/mira/model_contracts.hpp`）：
   - `ToolCallPart`：assistant 工具调用的规范回显（provider call id、wire name、arguments、
     arguments digest），由 `make_tool_call_item(proposal)` 构造，role 为 Assistant。
   - `ToolResultPart`：一次已执行工具调用的规范结果（镜像 `ToolExecutionRecord`：result、
     large payload artifact 引用、failed、safe error summary），由
     `make_tool_result_item(record)` 构造，role 为 User。
   两者经 `model_request_to_json/from_json` 往返、进入 `validate_model_request` 校验
   （call id 非空、arguments 为对象且 digest 匹配、失败结果必须有界 safe summary、超限
   结果必须以 artifact 引用承载）并参与 canonical digest。工具 part 必须独占其输入项；
   与其他 part 混排在线格式映射层 fail closed。
2. **线格式映射**（`src/model/model_tool_wire.cpp`，两个 OpenAI dialect 复用）：
   Responses dialect 渲染为顶层 `function_call` / `function_call_output` 项；Chat
   Completions dialect 渲染为 assistant `tool_calls` 消息 / `role: tool` 消息。结果载荷
   envelope 复用 `build_tool_result_input` 的唯一形状，不再派生第二映射。
3. **Text 模式的工具提案语义**：`parse_decision` 在 `OutputMode::Text` 下，响应携带工具
   调用时解析为 `ToolProposals`（原为 `Ambiguous`）；伴随的评注文本不构成终态。非 Text
   模式维持原语义（混排仍 `Ambiguous`）。文本回答仅在没有工具调用时构成会话终态。
4. **公开无观察对话循环**：新增 `include/mira/conversation_loop.hpp` 的 `ConversationLoop`
   ——model → tool proposal → execute → result → model 循环。复用既有契约：
   `AgentLoopSpec`（task/session/epoch/goal/profile）、`ModelGateway`（admission、预算、
   重试/熔断）、`BuiltinToolRegistry`（DEC-015 执行边界）与 `IEventStore` 事件面。终态为
   `Answered / Failed / Cancelled / MaxTurns`；纯文本回答即 `Answered` 终态，无观察、无
   设备验证、不强制决策 JSON。预算形状沿用 RULE-08：`max_turns`、`max_recoveries`、
   `max_tool_executions`、每轮 generation 上限与整环 request 预算。循环不拥有线程、队列
   或调度器，由宿主在 Executor worker 上驱动（与 `AgentLoop::run()` 相同）。

`AgentLoop` 的文本回填路径保持不变（设备闭环语义冻结）；两个回填面并存，设备环继续用
文本块，对话环用规范 part。

## 备选方案

- **给 `AgentLoopConfig` 加"跳过观察"开关**：让设备环承担第二种人格，验证器、决策
  schema 与状态机语义分叉，测试面翻倍；否决。
- **扩展 `build_tool_result_input` 返回 `ModelInputItem`**：该函数的契约是线格式
  `JsonValue`（ dialect 相关）；混入规范模型类型会让一个函数承载两层抽象；否决。
- **新增 `ModelRole::Tool`**：Chat dialect 有 tool role，但 Responses dialect 的
  function_call_output 无 role 概念；把 dialect 差异提升进规范 role 集是错误的抽象层；
  否决——role 保持 Assistant/User，由 part 类型承载语义。
- **Mirage 持续在集成层自建 adapter**：反馈明确指出其受限回填不是原生语义；维持下游
  私有边界与上游演进方向相悖；否决。

## 影响与风险

- 公开契约扩展（新增 variant 备选与两个自由函数）对现有消费者源码兼容；对
  `ModelContentPart` 做穷尽 visit 的第三方代码需要补分支（Mira 树内无此类代码，全树
  编译验证）。
- `parse_decision` 的 Text 模式语义变化是行为变更：Text + 工具调用从 `Ambiguous` 变为
  `ToolProposals`。树内 Text 模式消费者不依赖旧行为（回归由测试确认）。
- `ConversationLoop` 不做设备观察验证，语义责任由调用方承担：它不得用于需要环境副作用
  验证的设备任务（那是 `AgentLoop` 的职责）。
- 两个回填面并存有轻微重复；若后续设备环迁移到规范 part，需要单独决策。

## 验证方式

- 契约测试：新 part 的 validate/to_json/from_json 往返与失败路径；两 dialect 的
  function_call / function_call_output / tool_calls 线格式；混排 fail closed。
- 循环测试：脚本化 provider 驱动（a）纯文本问答终态（b）工具往返后再回答（c）非法提案、
  拒绝、取消、预算耗尽、空回答恢复（d）无 screen provider 环境（环境零依赖）。
- 回归：全量 ctest 与既有 m3/m8 门禁。

## 关联文档和工作项

- 反馈：[mira#73](https://github.com/Linductor-alkaid/mira/issues/73)（MIRA-20261004-001）
- [DEC-015](DEC-015-builtin-tool-execution-boundary.md)：BuiltIn 工具执行边界（不变）
- [DEC-016](DEC-016-conversation-events-and-user-messages.md)：会话事件与用户消息
- [model-agent-loop.md](../api/model-agent-loop.md)：API 手册同步
- 维护计划：[maintenance-2026-10-mirage-feedback-round2.md](../plans/maintenance-2026-10-mirage-feedback-round2.md)
