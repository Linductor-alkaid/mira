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
  传输抽象。实现由 `mira_net_transport` + OpenSSL（`Mira::openssl_transport`）或
  pinned Mbed TLS（`Mira::mbedtls_transport`）提供（[DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)）。
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

- `AgentLoopConfig`：`max_steps`、每步恢复上限、模型调用 deadline、观察新鲜度期望。
- 每次迭代是有界工作单元；取消、准入拒绝和终态在任何新动作派发前停止循环。
- `ILoopVerifier::verify(fresh_observation, decision)`：完成后必须对新观察验证，
  模型自称"done"永远不够（`Verdict::Satisfied/NotSatisfied/Invalid`）。
- `AgentLoopResult`：`LoopOutcome`（`Completed/Failed/Cancelled/MaxSteps`）+ 逐步
  `LoopStepRecord`（观察、请求、决策 digest、动作摘要、验证结果）。
- `compile_discrete_action(decision)`：把已验证决策编译为 `InputSequence`；坐标必须
  是规范 `[0, 1]`，越界 fail closed。`agent_decision_schema()` 是闭环标准决策 schema，
  其 digest 随每个请求记录。

## 相关文档

- [LLM API 协议设计](../design/llm-api-protocol-design.md)、
  [模型 Provider 与工具设计](../design/model_provider_and_tool_design.md)
- [Agent loop alpha 发布说明](../releases/agent-loop-alpha.md)
