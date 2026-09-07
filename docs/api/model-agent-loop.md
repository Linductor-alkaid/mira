# 模型层与 Agent Loop

> 头文件：`mira/model_contracts.hpp`、`model_profile.hpp`、`model_provider.hpp`、
> `model_gateway.hpp`、`model_supervisor.hpp`、`model_budget.hpp`、`model_schema.hpp`、
> `model_sse.hpp`、`model_dialect.hpp`、`model_transport.hpp`、`model_upload.hpp`、
> `model_tool.hpp`、`model_digest.hpp`、`model_replay.hpp`、`mira/agent_loop.hpp`

模型层采用 API-first 设计：Core 只依赖 OpenAI-compatible HTTP 协议，不依赖任何供应商
SDK；凭据经 `SecretRef`/`ISecretResolver` 解析，绝不进入事件或源码
（[DEC-007](../decisions/DEC-007-llm-api-protocol-strategy.md)）。

## model_contracts.hpp：规范请求/响应

- 输入是 `ModelInputItem` 列表，内容为 `TextPart` / `ImagePart`（`ImageDetail` 可选）/
  `FilePart`（引用 `ArtifactRef`）的 `variant`。
- `ModelRequest` 携带角色（`ModelRole`）、输出模式（`OutputMode`）、推理力度
  （`ReasoningEffort`）、服务层级（`ServiceTier`）、schema 绑定（`SchemaId`）与数据
  策略（`ModelDataPolicy`：payload 远端存储是显式决定，绝不依赖 provider 默认值）。
- `ModelResponse` 是规范化的终态响应；provider 方言差异在 Provider 内部消化，网关之后
  只有规范形态。
- `Hash`（SHA-256）贯穿全链路：wire 请求、prompt、决策、工具快本与数据策略均有独立
  digest（`model_digest.hpp`），并配套 `sanitize_wire_for_events()` /
  `redact_url_for_log()` 做事件脱敏。

## model_profile.hpp：ModelProfile 与 ModelRouter

- `ModelProfile`：一个已配置端点的完整描述——`ModelProfileId`、协议方言
  （`ProtocolDialect::OpenAIResponsesV1` / `ChatCompletionsV1`）、能力位
  （`ModelProfileCapabilities`，每个能力带 `CapabilityEvidence` 证据等级）、参数映射
  策略（`ParamMapping`）、限额（`ProfileLimits`）、传输 deadline（`TransportDeadlines`）、
  代理（`ModelProxyConfig`）与 `SecretRef`。能力声明与互操作证据的对应关系见
  [兼容性矩阵](../compatibility/openai-compatible-matrix.md)。
- `ModelRouter::route(RouteQuery)`：在字节出线之前完成能力、数据策略与预算匹配，
  不匹配直接拒绝（`RouteRejection`）；`register_profile()` 注册，`find()` 查询。

## model_provider.hpp 与 model_transport.hpp

- `IModelProvider`：`profile()` + `infer(request, context, options)`；实现不拥有超出
  注入 transport 的生命周期，也从不推进任务状态。`ProviderInferOptions` 控制流式与
  原始响应捕获（写入受保护 Artifact 并挂引用）。
- `OpenAiCompatibleProvider`：两个固定方言（Responses / Chat Completions）的具体实现；
  一个实例服务一个 profile，方言回退从不发生在单次操作内。
- `IHttpTransport` + `HttpRequest`/`TransportLimits`/`TransportTrace`/`TlsOptions`：
  传输抽象。官方实现由 `Mira::net_transport`（`mira/adapters/net/socket_transport.hpp`，
  `SocketHttpTransport`，POSIX/Winsock，Executor blocking-I/O worker 承载）配合
  OpenSSL（`Mira::openssl_transport`，`mira/adapters/net/openssl_tls.hpp`）或 pinned
  Mbed TLS（`Mira::mbedtls_transport`，`mira/adapters/net/mbedtls_tls.hpp`）提供
  （[DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)）。三个头文件随
  安装包导出；消费者 `find_package(Mira)`（mbedtls 目标按构建条件存在）即可构造官方
  生产传输栈，无需自建（[DEC-013](../decisions/DEC-013-transport-export-and-image-media.md)）。
  `socket_transport.hpp` 的公共接口引用 Executor 类型，消费者需同时
  `find_package(executor)`。
- `ISecretResolver`：凭据解析边界。
- `model_replay.hpp` 的 `ReplayModelProvider` 回放录制的规范响应，供离线 Replay。

## model_gateway.hpp：ModelGateway

编排一次模型调用的全链路：

```
route -> provider -> retry/circuit 监督 -> 预算结算 -> 本地决策解析与工具解析
```

- 构造：`ModelGateway(executor, router, artifact_source, price_table, config)`；
  `register_provider()`、`set_event_store()`、`set_admission_gate()`。
- `infer()` 返回 `ModelCallOutcome`：`RouteDecision`、规范 `ModelResponse`、
  `DecisionParseResult`、可选 `ToolProposalBatch`、`BudgetReservation`/`BudgetSettlement`、
  wire digest、SSE 统计与 `admitted` 标志——三层成功（传输接受/规范响应/语义决策）
  严格分离，任何一层不能冒充下一层。
- `TaskAdmissionGate`：epoch 与生命周期准入由协调者（而非 provider）决定；网关拿到
  迟到响应时先问 gate，被拒的响应不结算进任务。测试与无协调者循环可用
  `SimpleAdmissionGate`。

## 监督、预算、解析与流

- `model_supervisor.hpp`：`RequestStage` 分类（`classify_stage()`）驱动重试表
  （`RetryBudget`/`RetryAction`）；`ProviderCircuit`（`CircuitState`）提供熔断。
- `model_budget.hpp`：`PriceTable` + `estimate_input_tokens()` 预估、
  `BudgetReservation`/`BudgetSettlement`/`BudgetLedger` 结算；未知价格默认拒绝而非
  零成本放行。
- `model_schema.hpp`：`gate_schema_subset()` 限制请求携带的 JSON Schema 子集；
  `parse_decision()` 把响应文本解析为 `DecisionCandidate`（`DecisionParseOutcome`），
  配合 `RepairPolicy`/`RepairBudget` 做有界修复。原始模型文本永远不能直接触发平台输入。
- `model_sse.hpp`：`SseFramingParser`（带 `SseFramingLimits` 的帧解析）与
  `ResponsesSseParser`（Responses 流事件到规范响应），`SseStreamStats` 记录流质量。
- `model_dialect.hpp`：`IDialectMapper` 方言映射与 HTTP 错误码到稳定 `Error` 的转换。
- `model_upload.hpp`：`IRemoteFileStore` / `OpenAiRemoteFileStore` 受管远端文件生命周期
  与审计（`RemoteFileAudit`）。
- `model_tool.hpp`：provider 侧 hosted tool 的提案桥接（`ToolProposal`/`ToolProposalBatch`，
  `kDefaultToolBridgeLimits`）；工具执行模组化属后续范围
  （[DEC-009](../decisions/DEC-009-tool-module-boundary.md)）。

## agent_loop.hpp：离散闭环

`AgentLoop` 驱动 Observe -> Reason -> Plan -> Act -> Verify：

```cpp
mira::AgentLoop loop(environment, gateway, AgentLoopConfig{});
loop.set_event_store(events, runtime_id, session_id);
mira::ModelDoneVerifier verifier;   // 或自定义 ILoopVerifier
auto result = loop.run(AgentLoopSpec{task, session, epoch, goal, profile_id},
                       context, verifier);
```

- `AgentLoopConfig`：`max_steps`、每步恢复上限、模型调用 deadline、观察新鲜度期望、
  单次运行工具执行预算 `max_tool_executions`、用户消息队列上限
  `max_pending_user_messages`。
- 每次迭代是有界工作单元；取消、准入拒绝和终态在任何新动作派发前停止循环。
- `ILoopVerifier::verify(fresh_observation, decision)`：完成后必须对新观察验证，
  模型自称"done"永远不够（`Verdict::Satisfied/NotSatisfied/Invalid`）。
- `AgentLoopResult`：`LoopOutcome`（`Completed/Failed/Cancelled/MaxSteps`）+ 逐步
  `LoopStepRecord`（观察、请求、决策 digest、动作摘要、验证结果）。
- `compile_discrete_action(decision)`：把已验证决策编译为 `InputSequence`；坐标必须
  是规范 `[0, 1]`，越界 fail closed。`agent_decision_schema()` 是闭环标准决策 schema，
  其 digest 随每个请求记录。schema 不做动作参数条件必填（wire 关键字兼容边界，见
  [维护计划](../plans/maintenance-2026-09-decision-compile-repair.md)）：通过 schema 但
  缺参数的决策在 compile 失败，循环在 `max_recoveries_per_step` 预算内把编译诊断
  （静态安全字符串）作为下一轮请求 feedback 重试，预算耗尽才终态 `Failed`。
- 图像 wire 媒体类型（[DEC-013](../decisions/DEC-013-transport-export-and-image-media.md)）：
  `build_request` 的截图 `ArtifactRef`（media type / byte size）来自
  `ScreenFrameDescriptor.payload_*`（工件发布时的 store 记录），不假设原始帧布局——
  宿主在注入的 store 内转码（RGBA→PNG/JPEG）即可控制 wire 格式，内联 8 MiB 门槛按
  实际编码字节数判定。方言层对非 `image/*` 媒体类型（如未编码原始帧的
  `application/octet-stream`）在 fetch 前 fail closed（`UnsupportedCapability`）。

## tool_executor.hpp：BuiltIn 工具执行边界

[DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md) 定义的最小 BuiltIn 工具
边界（模组体系仍属 M7/[DEC-009](../decisions/DEC-009-tool-module-boundary.md)）：

```cpp
auto registry = std::make_shared<mira::BuiltinToolRegistry>();
auto wait = mira::make_wait_tool();
registry->register_tool(wait.spec, wait.handler);
loop.set_tool_registry(registry);
```

- `BuiltinToolSpec`：ToolId、版本、wire_name（不得与宿主供应商工具名冲突）、描述、
  参数 JSON Schema（须过 `gate_schema_subset`）、副作用声明。注册表按 wire_name 排序
  生成确定性 `ExposedToolSpec` 快照进入 `ModelRequest.tools`。
- 执行 fail closed：未注册工具、与暴露快照不一致的身份（运行中被篡改）、重复
  `OperationId`（至多一次派发）拒绝；参数经本地 schema 校验。模型可修复的失败
  （参数不合规、处理函数错误）以 `failed` 的 `ToolExecutionRecord` 回填下一轮请求；
  取消传播为 `Cancelled` 错误。
- 处理函数在循环占用的 Executor 任务内联执行：必须有界、轮询取消、不得抛出。
  `wait` 工具硬上限 10 秒，切片睡眠轮询取消。
- 事件：每次执行发 `ToolExecuted`（wire_name、operation_id、failed、参数摘要 digest），
  不携带原始参数或结果载荷。

## agent_loop.hpp：工具分支与用户消息

- 挂接注册表后，模型 ToolProposals 逐个执行、结果以
  `mira.agent-loop.tool-result.v1` 来源的输入项回填下一轮（每条 2 KiB、总量 8 KiB
  截断）；未挂接注册表时工具提案 fail closed，循环终态 `Failed`。单次运行执行数超过
  `max_tool_executions` 终态 `Failed`（RULE-08）。
- `enqueue_user_message(text)`（[DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)）：
  线程安全、有界队列；消息在步边界取出、发 `UserMessageInjected` 事件（文本、字节数、
  digest），并作为常驻指令进入本轮及后续每轮请求（provenance
  `mira.agent-loop.user-message.v1`）。**脱敏责任在宿主**：入队文本原样进入事件存储
  与模型请求，凭据、输入法敏感内容必须在入队前移除。

## conversation_log.hpp：会话对话投影

[DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md) 的投影 API：

```cpp
auto view = mira::build_conversation_view(event_store, session_id);
// std::vector<ConversationEntry{kind: UserMessage|LoopOutcome, recorded_at, text, origin}>
```

EventStore 是唯一事实源（RULE-07）；投影从 `UserMessageInjected` 与 `LoopSettled` 事件
按会话顺序重建，不可读载荷 fail closed。完整对话 patch、暂停/恢复语义由 M8-04 冻结，
本投影不承载它们。

## 相关文档

- [LLM API 协议设计](../design/llm-api-protocol-design.md)、
  [模型 Provider 与工具设计](../design/model_provider_and_tool_design.md)
- [Agent loop alpha 发布说明](../releases/agent-loop-alpha.md)
