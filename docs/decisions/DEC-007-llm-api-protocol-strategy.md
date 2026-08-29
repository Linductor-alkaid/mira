# DEC-007：LLM API 规范契约与协议方言策略

> 状态：Accepted  
> 日期：2026-08-30  
> 负责人：Mira Maintainers  
> 冻结里程碑：M3  
> 替代/被替代：无

## 背景与问题

“OpenAI-compatible”通常只表示某些 HTTP 路径和 JSON 字段相似，不表示 Responses API、Chat
Completions、流式事件、Structured Outputs、Tool Calling、usage、错误或远端状态具有相同语义。
如果 Core 直接依赖任一供应商 wire schema，或者 Adapter 在运行时猜测方言，模型切换将改变
Decision、安全、恢复和 Replay 行为，并可能通过付费请求泄露数据。

Mira 还需要在官方 OpenAI Responses API 的状态化能力与更多兼容服务常见的 Chat Completions
接口之间作出首期选择，同时保持后续扩展空间。

## 决策

- Core 只使用版本化的 Mira canonical `ModelRequest`、`ModelResponse`、`ModelOutputItem`、`ModelUsage`
  和稳定错误，不暴露供应商 JSON、SDK 类型或 endpoint 路径。
- Provider profile 必须显式选择 wire dialect。首批 dialect ID 为 `openai.responses.v1` 和
  `openai.chat-completions.v1`；两者具有独立 mapper、capability manifest 和 contract fixtures。
- 对官方 OpenAI 以及经互操作验证完整支持 Responses 的服务，首选 `openai.responses.v1`。
  Chat Completions 是显式兼容方言，不是同一请求失败后的自动降级路径。
- 不按供应商品牌猜能力，不通过生产推理请求探测 capability，也不在收到 `404`、`400` 或 schema
  错误后自动切换 endpoint。Profile 只有在兼容性证据和测试通过后才能声明能力。
- Model Provider 的成功只表示得到可解析的 terminal `ModelResponse`。只有完整响应满足 Decision
  schema、Tool schema、Task epoch、Policy 和预算校验后，Core 才能产生可规划的 Decision。
- 首期支持同步 HTTP；`openai.responses.v1` 支持可选 Server-Sent Events（SSE）。Background、Webhook、
  WebSocket、Batch、Assistants 和供应商 hosted tools 不属于 M3 在线闭环范围。
- 远端 retention 必须由 data policy 显式决定。默认请求不允许省略 retention 选择；OpenAI Responses
  profile 的安全默认值是 `store=false`，只有 Host 明确授权远端保留时才可设为 `true`。
- Provider 内置 computer-use 等工具不得拥有 Mira 的真实输入能力。所有真实设备动作仍由本地
  Decision、Policy、Confirmation、Action 和 Environment 链路执行。

完整字段、状态机和失败语义由
[LLM API 协议设计](../design/llm-api-protocol-design.md)定义。

## 备选方案

- 只实现 Chat Completions：覆盖面较广，但会把新项目长期绑定在较弱的 item、状态和 continuation
  表达上，不采用。
- 只实现 Responses：会把大量只提供 Chat Completions-compatible 接口的候选服务排除在 v1 之外，
  不采用。
- 使用单一“OpenAI-compatible”mapper并忽略未知字段：同名字段在不同服务上的状态、错误和 tool
  语义并不稳定，且未知安全字段不能默认放行，不采用。
- 运行时试错 endpoint/capability：可能产生费用、数据披露和不可复现路由，不采用。
- 直接采用某一供应商 C++ SDK 类型作为公共 API：破坏 Provider 隔离、取消与 Executor 生命周期，
  不采用。

## 影响与风险

- 需要维护两个 mapper 和两组 fixtures，但 Core、DecisionParser 和 Planner 保持单一契约。
- Responses 独有字段在 Chat Completions profile 上可能不可表示；路由必须在发送前拒绝 capability
  mismatch，不能静默删掉影响语义的字段。
- `store=false` 减少对远端 conversation state 的依赖，但本地 ContextManager、EventStore 和
  ArtifactStore 必须承担恢复与 Replay。
- 第三方兼容服务只能在明确版本、endpoint 和模型组合上获得互操作声明；供应商更新可能使证据
  过期。
- 本决策不承诺具体 Provider 已可互操作；实际支持状态由兼容性矩阵和 M3 验证记录证明。

## 验证方式

- 对两个 dialect 分别运行 request/response、image、strict schema、tool call、usage、error 和取消
  fixtures；不得复用预期输出掩盖协议差异。
- 验证未知 output item、未知 terminal status、冲突 duplicate event 和无 terminal 的 EOF fail closed。
- 验证 profile capability 与实际发送字段一致，不支持参数在网络发送前被明确拒绝。
- 验证 endpoint 失败不会触发隐式 dialect 或供应商切换。
- 验证 raw Provider 响应、流式 preview 和 hosted tool call 都不能直接进入 `IInputProvider`。

## 关联文档和工作项

- [LLM API 协议设计](../design/llm-api-protocol-design.md)
- [OpenAI-compatible 兼容性矩阵](../compatibility/openai-compatible-matrix.md)
- [Model Provider 与 Tool 扩展设计](../design/model_provider_and_tool_design.md)
- [Context 与 Memory 设计](../design/context_and_memory_design.md)
- [M3 Model Provider 与 Agent 闭环](../plans/m3-model-provider-agent-loop.md)

