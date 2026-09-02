# Mira LLM API 协议设计

> 状态：Active  
> 版本：1.1  
> 更新日期：2026-08-31   
> 负责人：Mira Maintainers  
> 适用范围：远端 LLM/VLM 请求、OpenAI-compatible wire dialect、流式响应、Structured Outputs、
> Tool Calling、Provider 状态与费用核算  
> 上位设计：[Model Provider 与 Tool 扩展设计](model_provider_and_tool_design.md)  
> 决策依据：[DEC-007](../decisions/DEC-007-llm-api-protocol-strategy.md)

## 1. 背景

Mira 已经定义 `IModelProvider`、预算、取消、Provider 路由与安全边界，但只有这些抽象还不足以
实现行为稳定的 Adapter。本设计补充 canonical 数据契约、wire dialect、状态机、流式事件、错误、
重试、远端状态和兼容性验证，使“OpenAI-compatible”成为可测试的 profile 能力，而不是品牌假设。

官方 OpenAI 文档是 `openai.responses.v1` 的上游协议依据，但不自动证明任何第三方服务兼容。
Mira 对外承诺以本设计、版本化 fixtures 和互操作证据为准。

## 2. 目标与非目标

### 2.1 目标

- 让 Core 使用单一、版本化、供应商无关的 Model 契约。
- 明确定义 Responses 和 Chat Completions 方言的映射及不可映射行为。
- 区分 transport、HTTP、Provider terminal status、structured output 和 Decision 校验失败。
- 让流式、取消、重试、费用、远端 retention、continuation 和 Replay 具有确定语义。
- 让 Model Tool Call 只能形成受 Policy 管理的本地提议，不能直接获得平台 authority。
- 为 Provider contract test 和兼容性矩阵提供规范依据。

### 2.2 非目标

- 不在 Core 内运行通用 LLM/VLM。
- 不统一不同模型的推理质量、提示词效果或安全策略。
- M3 不支持 background polling、webhook server、WebSocket multiplexing、Batch 或 Assistants API。
- M3 不开放供应商 hosted web search、code interpreter、MCP 或 computer-use 工具。
- 不把 prompt cache、conversation ID 或 opaque reasoning 当作 Mira 的事实源。

## 3. 分层与依赖

```text
TaskCoordinator
  -> ContextManager / PromptAssembler
  -> ModelGateway
       -> ModelRouter
       -> canonical ModelRequest validator
       -> IModelProvider
            -> dialect mapper
            -> managed HTTP/SSE transport
            -> dialect response decoder
       -> canonical ModelResponse validator
  -> DecisionParser / ToolProposalParser
  -> PolicyEngine -> Planner -> Environment
```

- `PromptAssembler` 决定 authority、上下文和 schema；Provider 不改写业务提示。
- `ModelRouter` 只选择满足 capability、数据政策、预算和健康要求的固定 profile。
- Dialect mapper 只做可审计的字段映射；不能删掉无法表示的非默认语义。
- Provider 不推进 Task 状态。它返回完整结果，Coordinator 按 `OperationId`、Task epoch 和取消状态
  决定是否接纳 completion。
- `DecisionParser` 和 `ToolProposalParser` 不信任 Provider 已执行的 schema 校验，始终本地复验。

## 4. Canonical 公共契约

以下 C++ 是规范草案，字段布局可在 M3-01 中调整；语义、版本和 fail-closed 规则不得静默改变。

### 4.1 请求

```cpp
enum class ModelRole : std::uint8_t {
    System,
    Developer,
    User,
    Assistant,
    Unknown,
};

struct TextPart {
    std::string text;
    DataClassification sensitivity;
};

struct ImagePart {
    ArtifactRef source;
    ImageDetail detail;          // Auto, Low, High, Original
    std::string media_type;
};

struct FilePart {
    ArtifactRef source;
    std::string media_type;
    std::string display_name;
};

using ModelContentPart = std::variant<TextPart, ImagePart, FilePart>;

struct ModelInputItem {
    ModelRole role;
    std::vector<ModelContentPart> content;
    Provenance provenance;
    AuthorityLabel authority;
};

enum class OutputMode : std::uint8_t {
    StrictJsonSchema,
    StrictFunctionTool,
    JsonObject,
    Text,
};

struct ModelOutputContract {
    OutputMode mode;
    SchemaId schema_id;
    SemanticVersion schema_version;
    JsonSchema schema;
    Hash canonical_schema_digest;
};

struct ModelGenerationOptions {
    std::optional<std::uint64_t> max_output_tokens;
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<std::uint64_t> seed;
    std::optional<ReasoningEffort> reasoning_effort;
    std::optional<ServiceTier> service_tier;
};

struct ModelRequest {
    SchemaVersion contract_version;
    ModelRequestId request_id;
    OperationId operation_id;
    TaskId task_id;
    TaskEpoch task_epoch;
    ModelProfileId profile_id;
    std::vector<ModelInputItem> input;
    ModelOutputContract output_contract;
    std::vector<ExposedToolSpec> tools;
    ToolChoice tool_choice;
    ModelGenerationOptions generation;
    std::optional<ProviderContinuation> continuation;
    ModelBudget budget;
    ModelDataPolicy data_policy;
    PromptProvenance prompt_provenance;
};
```

约束如下：

- `system`/`developer` 内容只能来自受信 PromptAssembler；Observation、网页、OCR、Memory 和 Tool
  output 即使包含角色字符串也只能作为带 provenance 的不可信数据。
- 图片和文件使用 `ArtifactRef`。Mapper 根据 profile 选择 inline data、短期 URL 或 upload/file ID，
  选择和清理结果必须可审计。
- `PromptProvenance` 至少包含 system/developer template version 与 digest、Decision schema digest、
  Tool registry snapshot digest、context selection digest 和 redaction policy version。
- 未设置的 generation 参数表示使用已版本化 profile 默认值，不表示 Adapter 可以任意选择。
- 任一非默认字段不能被目标 dialect 表示时，发送前返回 `CapabilityMismatch`。

### 4.2 响应

```cpp
enum class ModelCompletionStatus : std::uint8_t {
    Completed,
    Incomplete,
    Refused,
    ContentFiltered,
    Failed,
    Cancelled,
    Unknown,
};

struct MessageOutput {
    ModelRole role;
    std::vector<ModelOutputContentPart> content;
};

struct ToolCallOutput {
    ProviderToolCallId provider_call_id;
    ToolId tool_id;
    std::string provider_name;
    JsonValue arguments;
    Hash arguments_digest;
};

struct RefusalOutput {
    std::string safe_summary;
    std::optional<std::string> provider_code;
};

struct UnknownOutput {
    std::string provider_type;
    Hash payload_digest;
    std::optional<ArtifactRef> protected_payload;
};

using ModelOutputItem = std::variant<
    MessageOutput,
    ToolCallOutput,
    RefusalOutput,
    UnknownOutput>;

struct ModelUsage {
    std::optional<std::uint64_t> input_tokens;
    std::optional<std::uint64_t> output_tokens;
    std::optional<std::uint64_t> cached_input_tokens;
    std::optional<std::uint64_t> reasoning_tokens;
    UsageQuality quality;        // Exact, ProviderReported, Estimated, Partial, Missing
};

struct ModelResponse {
    SchemaVersion contract_version;
    ModelRequestId request_id;
    OperationId operation_id;
    ModelProfileId profile_id;
    std::string requested_model;
    std::optional<std::string> resolved_model;
    std::optional<std::string> provider_response_id;
    std::optional<std::string> provider_request_id;
    ModelCompletionStatus status;
    std::optional<IncompleteReason> incomplete_reason;
    std::vector<ModelOutputItem> output;
    ModelUsage usage;
    RateLimitMetadata rate_limit;
    std::optional<ProviderContinuation> continuation;
    std::optional<ArtifactRef> protected_raw_response;
};
```

`Result<ModelResponse>` 的边界是：

- DNS、TLS、HTTP transport、超限、无法解析 JSON、未知 terminal status 等返回稳定 `Error`。
- 可解析的 Provider terminal failure、refusal、content filter 和 incomplete 返回 `ModelResponse`，由
  `status` 表达，不伪装为成功 Decision。
- `Completed` 只表示供应商完成响应，不表示输出已经成为有效 Decision。
- 未知纯诊断 output item 可以保留摘要；未知 tool/action/authority-bearing item 必须使本次语义解析
  fail closed。

### 4.3 三层成功条件

一次模型调用必须依次通过：

1. `TransportAccepted`：HTTP/SSE 操作正常结束且大小、deadline 和取消约束满足。
2. `ProtocolCompleted`：响应属于已知 dialect，有唯一 terminal 状态并能映射为 canonical response。
3. `SemanticAccepted`：本地 schema、Tool、Task epoch、Policy 和上下文关联校验通过。

事件和 UI 不得把前两层显示为“Agent 已作出有效决策”。

## 5. Profile、方言与 Capability

### 5.1 固定方言

```cpp
enum class ProtocolDialect : std::uint8_t {
    OpenAIResponsesV1,
    OpenAIChatCompletionsV1,
};
```

| 方言 | Endpoint family | Canonical 输入映射 | 状态/输出映射 | M3 transport |
| --- | --- | --- | --- | --- |
| `openai.responses.v1` | `/responses` | item/content-part | typed output items | 同步 HTTP、可选 SSE |
| `openai.chat-completions.v1` | `/chat/completions` | messages/content | choices/message/tool calls | 同步 HTTP；SSE 仅在独立 fixture 通过后声明 |

Profile 配置 endpoint origin 和 API prefix，mapper 不能用字符串拼接接受模型提供的 URL。方言切换
需要新 RouteDecision；同一个 operation 内不自动 fallback 到另一方言。

### 5.2 Capability 来源

Capability 记录必须包含：

- dialect、endpoint origin、model selector 和可选 immutable model revision；
- text、image、file、strict schema、function tool、parallel tool、SSE、exact token count、continuation、
  remote retention、upload 等能力；
- generation 参数逐项支持状态和映射策略；
- request/context/output/图片/文件/Tool 数量和 byte 上限；
- `Configured`、`Documented`、`FixtureVerified` 或 `InteropVerified` 证据等级、证据日期和摘要；
- profile/version digest。

生产 Runtime 不通过付费推理探测能力。允许的启动检查仅限无正文的数据面连通性或供应商明确
提供的 metadata endpoint，而且检查失败不能擅自更换方言。真正 capability 由离线 fixture 和受控
互操作测试更新到[兼容性矩阵](../compatibility/openai-compatible-matrix.md)。

### 5.3 参数映射

每个参数使用以下策略之一：

- `Native`：方言和模型已验证原生支持；
- `Mapped`：存在文档化且经过 fixture 的等价字段；
- `OmitIfUnset`：只有 canonical 请求未设置时才省略；
- `Unsupported`：请求设置该字段时返回 `CapabilityMismatch`。

Adapter 不静默 clamp、重命名、删除非默认值或同时发送互斥参数。Model alias 和 Provider 返回的
resolved model 都进入事件；Replay 使用已记录 response，不重新调用 alias。

## 6. 请求构造与 Wire Digest

请求发送流程：

1. Router 固定 profile snapshot；检查数据分类、地域、retention、capability、健康和预算。
2. ContextManager 生成 `PreparedModelContext`，保留 pinned Goal、安全约束和未决 Tool/Action 关联。
3. 本地校验 canonical schema、Tool name/ID、字段大小、参数范围和 continuation binding。
4. Mapper 生成 wire object；处理图片/文件 transport，并再次计算 byte/token upper bound。
5. 对不含 secret、短期 URL token 和随机 multipart boundary 的 canonical wire representation 计算
   `wire_request_digest`；敏感值另存受保护 Artifact 或只保留摘要。
6. 记录 `ModelRequestPrepared` 和预算 reservation，经 Executor 管理的 transport 发送。
7. 接收 terminal 结果后核销 usage/cost，记录 `ModelResponseReceived` 或稳定错误。

`wire_request_digest` 用于关联和 Replay 诊断，不作为 Provider 幂等保证。

## 7. Structured Outputs 与 Decision

### 7.1 输出策略

路由优先级为：

1. `StrictJsonSchema`：适合单个 Mira Decision envelope；
2. `StrictFunctionTool`：适合需要模型选择一个 Mira Tool/Decision function 的 profile；
3. `JsonObject`：只有产品 profile 明确允许非 strict 且本地校验、修复预算和风险门禁存在时使用；
4. `Text`：只能用于不产生 Action/Tool 的解释性结果。

无论远端是否声明 strict，本地都使用请求携带的 schema version 和 canonical digest 重新校验。M3
支持的 JSON Schema 子集必须按 dialect/profile 列入 manifest；`$ref`、组合关键字、格式、深度、属性数
等超出已验证子集时，在发送前拒绝，而不是等待 Provider 返回错误。

### 7.2 Terminal 分类

| Provider 结果 | Canonical 结果 | 是否解析 Decision | 是否 schema repair |
| --- | --- | --- | --- |
| 完整且符合 schema | `Completed` | 是 | 否 |
| 明确 refusal | `Refused` | 否 | 否 |
| 达到输出 token 上限 | `Incomplete(MaxOutputTokens)` | 否 | 否；可由 Recovery 重新规划请求 |
| content filter | `ContentFiltered` | 否 | 否；不得换供应商绕过 |
| 完整但 schema 不符 | `Completed` + semantic error | 否 | 仅按有限 repair policy |
| transport EOF/取消 | transport error/`Cancelled` | 否 | 否 |

Schema repair 是一个新的、有新 `ModelRequestId` 的付费 operation。它引用原请求和 validation error
摘要，最多使用 profile 配置的有限次数，并占用 Task token、费用和 deadline 预算。旧的 partial JSON、
refusal 或隐藏推理不能产生 Action。首次 schema 编译/缓存耗时单独计入 telemetry，不能从普通模型
latency 中消失。

### 7.3 Decision 唯一性

一个 terminal response 只能形成以下之一：

- 一个符合当前 schema 的 `DecisionCandidate`；
- 一个 `ToolProposalBatch`；
- refusal/incomplete/failure。

同时出现可执行 Decision 和 Tool Call、重复冲突 Tool Call、多个互斥 Decision 或无法识别的执行型
output item 时返回 `AmbiguousModelOutput`，不得选择“看起来最合理”的一项。

## 8. Tool Calling 桥接

### 8.1 暴露与映射

- 每次请求只暴露当前 Task、状态、权限和预算允许的 `ToolSpec` snapshot。
- Provider tool name 是 wire alias；内部 identity 始终是 `ToolId + major version + spec digest`。
- `provider_call_id` 与 `(ModelRequestId, ToolId, arguments_digest)` 一同映射成稳定 `OperationId`，并
  在执行前写入 EventStore。
- Tool arguments 完成接收后才解析 JSON、canonicalize 并进行 schema、大小、权限、confirmation 和
  资源检查。流式 argument delta 不能提前调用 Tool。
- Tool result 经过 schema、敏感度和大小校验后，mapper 以原 `provider_call_id` 回填
  `function_call_output` 或对应 Chat Completions tool message。

### 8.2 多调用、重复与并行

- 多个调用先形成 `ToolProposalBatch`，不因 Provider 标记 parallel 就自动并行执行。
- 只有相互独立、只读或具备经过验证幂等语义的调用，才能由 Planner 明确并行；副作用调用默认串行，
  每项分别经过 Policy 和 intent logging。
- 同一 response/continuation 中相同 call ID 和相同 digest 的重复项去重并记录诊断；相同 ID 不同
  arguments 是 `ProtocolViolation`。
- Tool output 回填失败不重放已经产生的副作用；恢复使用 EventStore receipt 构造下一轮上下文。
- `tool_choice` 只能引用已暴露 Tool，`required` 不能被 Adapter 静默降为 `auto`。

### 8.3 Hosted tools

M3 profile 的 hosted tools allowlist 为空。Provider 返回 web search、code interpreter、MCP、file
search 或 computer-use 等内置调用时，若请求并未声明该能力则视为协议违规。未来开放 hosted tool
需要独立的数据访问、费用、provenance、隔离和 Replay 设计。

尤其不得把 Provider computer-use 的动作或截图回路直接映射到 `IInputProvider`；真实输入只能来自
Mira 本地 Action pipeline。

## 9. 流式 SSE 状态机

### 9.1 状态

```text
Prepared
  -> Connecting
  -> AwaitingHeaders
  -> Created
  -> InProgress
  -> TerminalCompleted | TerminalFailed | TerminalIncomplete | TerminalCancelled

Connecting/AwaitingHeaders/Created/InProgress
  -> TransportFailed | ProtocolFailed | Cancelled
```

Responses mapper识别至少以下语义事件族：response created/in-progress、output item added/done、content
part added/done、text/refusal delta/done、function arguments delta/done、response completed/failed，以及
transport-level error。具体 wire 名称和字段由 fixture 固定。

### 9.2 Parser 规则

- SSE parser 支持任意网络分片，包括 UTF-8 code point、SSE line 和 JSON token 跨 chunk；单 event、
  单 delta、累计 bytes、item 数、嵌套深度和总时长均有上限。
- 本地为每个接收事件分配连续 `stream_sequence`。方言提供 remote sequence 时检查 gap；同 sequence
  同 digest 可去重，冲突 duplicate 或 gap 是 `ProtocolViolation`。
- output item/content part 必须满足 add/delta/done 配对。Function arguments 在 done 前仅保存有界
  buffer，不调用 JSON parser 或 Tool。
- 只接受一个 terminal response。Terminal 后的业务事件、未知 terminal、HTTP EOF 无 terminal、
  done 前缺失片段都使本次 protocol 失败。
- 收到取消后立即请求 transport wakeup/close；随后到达的 terminal response可以作为迟到诊断结算，
  但 Coordinator 的 epoch/cancellation admission 阻止它产生 Decision。

### 9.3 背压与预览

协议 parser 的输入队列有界且不可丢事件；满时取消 transport并返回 `ResourceExhausted`。UI preview
使用独立有界订阅，可合并文本 delta 或丢弃低价值更新，并报告 drop count。Preview 是
`UnvalidatedModelPreview`，不得写入 Memory、触发 Tool/Action 或被当作最终答复。

EventStore 默认记录 request、stream started、首/末字节时间、terminal、总 event/byte/drop 数和摘要，
不逐 token 持久化 delta。需要调试时可将脱敏后的完整 stream 存入受保护 Artifact，并遵循 retention。

Chat Completions SSE 只有在其独立 chunk、`[DONE]`、tool argument 和 usage fixtures 全部通过后才能由
profile 声明，不复用 Responses 状态机的 wire 事件假设。

## 10. 取消、Deadline、重试与模糊完成

### 10.1 Deadline

Profile 分别配置 DNS/connect、TLS、write、first-byte、idle-read 和 total deadline。最终 deadline 是
Task、operation、预算和 profile 上限的最小值。Executor queued timeout 不是网络 deadline。

### 10.2 重试决策表

| 失败点 | 远端可能接收/计费 | 默认动作 |
| --- | --- | --- |
| 本地 validation/admission 失败 | 否 | 不发送，不重试 |
| DNS/connect/TLS 在 request bytes 前失败 | 否 | 在 deadline、次数和 backoff 预算内可重试 |
| request 部分/全部写出后断线 | 是 | `AmbiguousCompletion`；无已验证 Provider idempotency 时不自动重试 |
| 429/明确可重试 5xx，且请求可安全重发 | 可能 | 尊重 `Retry-After`，经 cost/retry budget 后有界重试 |
| auth/permission/invalid request/capability | 通常否或已拒绝 | 不重试，不换凭据或方言 |
| content filter/refusal | 已完成 | 不重试，不 fallback 绕过政策 |
| schema semantic failure | 已计费 | 只进入独立 repair policy |
| stream EOF 无 terminal | 可能 | `AmbiguousCompletion`；先记录旧 request，不把 partial output作为 Decision |

只有兼容矩阵明确验证 endpoint/header 幂等语义时，profile 才可发送 idempotency key。Mira 的
`ModelRequestId`、`wire_request_digest` 或 Provider request ID 本身不等于远端幂等保证。发生模糊完成时
记录可能费用，并由 Recovery 决定放弃、重新观察后发新请求或在受支持的远端 retrieve 接口查询；
不能假装原请求未发生。

`Retry-After` 必须被解析为受上限约束的 duration；无效或超出 Task deadline 时不等待。Backoff、
circuit probe 和 retry wakeup 使用 Executor timer，取消必须解除等待。

## 11. 稳定错误归一化

下表“Model domain code”是 `Error.domain == "mira.model"` 时稳定的 `domain_code` 符号名；公共
`ErrorCode` 继续使用[核心公共契约](core_contracts_and_state_machine.md)已冻结的粗粒度枚举，避免另造
一套跨模块错误类型。

| Model domain code | 公共 `ErrorCode` | 典型来源 | 可重试提示 |
| --- | --- | --- | --- |
| `EndpointPolicyDenied` | `PermissionDenied` | URL/redirect/地域/数据政策 | 否 |
| `AuthenticationFailed` | `PermissionDenied` | 401、无效 credential | 否 |
| `ProviderPermissionDenied` | `PermissionDenied` | 403、project/model 无权限 | 否 |
| `InvalidModelRequest` | `InvalidArgument` | 参数、schema、消息格式 | 否 |
| `CapabilityMismatch` | `UnsupportedCapability` | 方言/模型不支持所需能力 | 路由前处理，不重发原请求 |
| `ContextLimitExceeded` | `ContextOverflow` | context 超限 | 由 ContextManager 重新打包 |
| `RateLimited` | `ResourceExhausted` | request/token/concurrency rate limit | 策略提示；非自动授权 |
| `ProviderOverloaded` | `Unavailable` | 明确 overload/可重试 5xx | 策略提示；有界 |
| `TransportFailed` | `Unavailable` | DNS/connect/TLS/read | 取决于发送阶段 |
| `ProtocolViolation` | `DataLoss` | malformed JSON、未知 terminal、事件冲突 | 默认否、打开降级信号 |
| `ResponseTooLarge` | `ResourceExhausted` | byte/item/depth 超限 | 否 |
| `ModelRefused` | `InvalidModelOutput` | explicit refusal | 否 |
| `ContentFiltered` | `SafetyRejected` | provider content policy | 否 |
| `IncompleteModelOutput` | `InvalidModelOutput` | max output token/其他 incomplete | 由 Recovery 决定新请求 |
| `MalformedStructuredOutput` | `InvalidModelOutput` | terminal 完整但本地 schema 失败 | 仅 repair policy |
| `AmbiguousModelOutput` | `InvalidModelOutput` | Decision/Tool 混合或互斥输出 | 否 |
| `AmbiguousCompletion` | `ExecutionUncertain` | write 后断线/stream 无 terminal | 默认否 |
| `ModelCancelled` | `Cancelled` | 本地取消 | 否 |
| `ModelDeadlineExceeded` | `DeadlineExceeded` | operation期限 | 否 |
| `ModelResourceExhausted` | `ResourceExhausted` | queue、bytes、预算、内存 | 解除压力后新 operation |

`retryable` 只是一项机制提示，不能直接触发重试。最终决策还必须检查 request stage、幂等证据、费用、
deadline、circuit、数据政策和 Task recovery policy。`safe_message` 不包含响应正文、secret、完整 URL
query 或敏感 prompt。

## 12. Usage、Token 与费用

### 12.1 计数质量

- `Estimated`：本地 tokenizer 或保守 upper bound；用于发送前 admission。
- `Exact`：同 profile/model 的供应商准确计数接口或经验证等价实现。
- `ProviderReported`：terminal response 的 usage；用于事后核销，但不回溯放行已拒请求。
- `Partial`：例如 stream 中止时只有部分字段。
- `Missing`：Provider 未返回；产生 quality degraded 事件。

输入计数必须覆盖 message framing、图片/文件、Tool schema、Structured Output schema 和方言开销。
Usage 分开保留 input、output、cached input、reasoning 和供应商扩展分类；未知字段不能合并后伪装成
精确总量。

### 12.2 预算核销

发送前按 upper bound 预留 token、request、image、byte 和费用预算。Terminal 后使用 Provider usage
与实际 price table 核销；缺失或 partial usage 采用保守 reservation，并标记待审计。Price table 包含
版本、币种、有效时间、model revision 和来源；价格未知时由产品 policy 决定只允许 token hard limit
还是拒绝，不能记录虚假的零成本。

Prompt caching 仅是成本/延迟优化。Cache hit、cached token 和 breakpoint 不影响语义正确性、Context
保留、Replay 或 capability 授权。

## 13. Conversation、Continuation 与 Retention

### 13.1 本地事实优先

Mira 的 EventStore、Checkpoint 和 ContextManager 是恢复事实源。以下 Provider 数据只用于同一
profile 的优化：conversation ID、previous response ID、output item ID、encrypted reasoning、server
compaction item 和其他 opaque continuation。

`ProviderContinuation` 除既有字段外必须绑定：

- Task/Session/epoch 和 profile digest；
- requested/resolved model compatibility；
- prompt、Decision schema 和 Tool snapshot digest；
- data policy 与 `store` 模式；
- creation/expiry 和 remote deletion state。

任一绑定改变、到期、Provider/model切换、Task 终态、取消、Takeover 或进程恢复都会使 continuation
失效。崩溃恢复从本地 checkpoint 重建请求，不依赖 opaque state；旧 remote ID 只保留为受数据政策
约束的诊断和清理目标。

### 13.2 Retention

`ModelDataPolicy` 明确：

- remote storage 是否允许，`store` 必须显式映射，不能依赖 Provider 默认值；
- Zero Data Retention（ZDR）/retention profile、地域和组织/project header；
- raw request/response、stream、image/file upload 的本地与远端保留期限；
- remote object 删除责任、重试、tombstone 和审计；
- metadata/safety identifier 的最小化和禁止字段。

OpenAI Responses profile 默认 `store=false`。若启用远端 conversation 或 `store=true`，必须有 Host
授权、retention 说明和删除流程。临时 upload/file ID 在 Task 或 retention 到期后由受管清理任务删除；
清理失败产生事件和重试台账，不阻塞 Runtime 最终 shutdown 无限等待。

## 14. 首期不支持协议

| 能力 | M3 状态 | 后续设计前置 |
| --- | --- | --- |
| Background + polling | 不支持 | durable remote operation、poll schedule、取消/删除、崩溃恢复 |
| Webhook | 不支持 | 公网接收边界、签名验证、防重放、事件持久化和 tenant 路由 |
| Responses WebSocket | 不支持 | connection owner、multiplex lane、重连、continuation cache 和背压 |
| Batch | 不支持在线闭环 | 离线 job、结果关联、时效和数据保留设计 |
| Hosted tools | 不支持 | capability/数据访问/费用/副作用/Replay 专项决策 |

Adapter 遇到这些请求或响应必须返回 `CapabilityMismatch`/`ProtocolViolation`，不能部分执行。

## 15. 安全与隐私

- Secret 只由 `ISecretResolver` 在 transport 边界解析；Authorization、proxy credential、signed URL、
  org/project secret 和完整 header 不进入普通事件。
- Endpoint、每次 redirect、DNS/IP 和 proxy 均执行 SSRF/TLS policy；模型输出不能修改 endpoint。
- Provider-specific prompt patch 只能存在于版本化 profile mapper，不能提升不可信内容的 authority，
  也不能改变 SafetyPolicy。
- 原始 payload 默认不持久化。调试开启时写受保护 Artifact，包含访问控制、加密、retention 和删除。
- Provider refusal、content filter 和 moderation信号如实归一化；fallback 不能用来规避政策。
- Unknown enum、tool、output item、schema major 和 safety-bearing field fail closed。

## 16. Executor、所有权与 Shutdown

所有 Mira 发起的模型工作受 Runtime 注入的 Executor 管理：

| 工作 | Executor 路径 | Owner/结算 |
| --- | --- | --- |
| 有界 request 构造、JSON decode、schema validation | `submit_auto()` | ModelGateway 保留并消费 future |
| 长期 HTTP/SSE event loop 或可阻塞 SDK | blocking I/O worker / 已批准 external-loop bridge | ProviderTransport 持有 `WorkerHandle` |
| retry、`Retry-After`、circuit probe、remote cleanup delay | Executor timer | ProviderSupervisor 持有 `TimerHandle` |
| UI stream preview 分发 | 有界普通任务/Observer 队列 | ObserverHub 结算 drop 和异常 |

`OperationContext` 的取消必须调用 transport wakeup/close，使 DNS/socket/SDK 等待有解除路径。Provider
不得创建 Mira 私有线程、隐藏全局 event loop 或 fire-and-forget cleanup。第三方库内部线程必须在
兼容性和供应链文档声明，并提供确定 shutdown。

关闭顺序：停止新 Model producer；取消/唤醒所有 operation；取消 retry/cleanup timer；停止并结算
transport worker；消费 parse/validation future；持久化 terminal/uncertain 事件；最后由非 worker
owner 执行 Runtime/Executor shutdown。迟到 response 只记录诊断，不推进终态 Task。

当前 Executor 的 `submit_auto()`、timer 和 blocking worker 能覆盖已设计路径，因此本设计不新增
Executor 反馈台账项。若选定 HTTP 库要求 Executor 无法承载的 event-loop 生命周期，必须先按
`docs/executor_feedback/ledger.md` 流程登记，不能引入隐藏线程绕行。

## 17. 可观测性与 Replay

至少记录：

- profile/dialect/model/request/operation ID 和 request/schema/prompt/tool/profile digest；
- RouteDecision、capability evidence version、data policy 和 retention mode；
- estimated/reserved/reported token、费用、price table version 和核销质量；
- DNS/connect/TLS/write/first-byte/stream/parse/validation/total latency；
- response/request ID、resolved model、terminal/incomplete/refusal/error 类别；
- retry/repair 次数、`Retry-After`、circuit transition 和 ambiguous completion；
- stream event/byte/preview drop 计数，Tool call/result 关联；
- continuation created/invalidated/deleted 和 remote cleanup result。

事件默认只保存摘要和 digest。OfflineReplay 使用 recorded canonical `ModelResponse`，不访问网络；
AnalysisReplay 可以用新的 parser/schema 对受保护 Artifact 重算，但结果作为新分析事件，不能改写历史
Decision。删除原始 payload 后保留 tombstone，并明确 Replay quality 降级。

## 18. Contract Test 矩阵

### 18.1 Canonical 与 mapper

- text、multi-part image/file、角色 authority、unknown item/enum 和大小/depth 上限；
- Responses 与 Chat Completions 的请求 golden、响应 golden 和双向不可表示字段；
- strict schema 子集、schema digest、Tool alias/ID、generation 参数和 model revision；
- secret/header/signed URL 不进入 digest、event 或 diagnostic。

### 18.2 Streaming

- 每个语义事件、任意 byte/UTF-8/JSON 分片、duplicate、gap、乱序、terminal 后事件；
- function argument delta、refusal delta、多个 output item、无 terminal EOF、错误中断；
- parser queue 满、preview drop、cancel/terminal race 和 shutdown wakeup。

### 18.3 错误、重试和费用

- DNS/connect/TLS/write/read/total timeout，401/403/404/408/429/5xx 和 malformed JSON；
- request bytes 前后失败、ambiguous completion、无幂等证据不重试；
- `Retry-After`、deadline、retry/cost budget、repair budget 和 circuit transition；
- usage exact/partial/missing、cached/reasoning tokens、价格未知和 reservation reconciliation。

### 18.4 Tool、状态与安全

- call ID duplicate/conflict、多调用、parallel eligibility、tool result回填和副作用不重放；
- continuation 跨 Task/epoch/profile/model/schema/tool/data policy 失效；
- `store=false`、upload/delete、fallback地域/敏感等级和 raw payload retention；
- hosted tool/computer-use、隐式 dialect fallback、生产 capability probe 均被拒绝；
- cancel、Task终态和 shutdown 后的迟到 response 不产生 Decision/Action。

互操作测试必须记录服务、endpoint、模型/版本、日期、配置摘要和结果；mock/golden fixture 不能作为
真实互操作证据。

## 19. 分阶段落地

1. 冻结 canonical 类型、错误、profile manifest 和两个 dialect fixtures。
2. 实现受管 HTTP transport、Responses 同步 mapper、strict Decision 和费用核销。
3. 实现 Responses SSE parser、预览隔离、取消与背压。
4. 实现 Chat Completions 同步 mapper；其 SSE 由独立 capability gate 控制。
5. 实现 Tool Call 桥接、continuation优化和完整 contract/security tests。
6. 使用受控凭据执行互操作测试并更新兼容性矩阵；未验证 Provider 保持 `Unknown`。

交付工作项和退出条件见
[M3 Model Provider 与 Agent 闭环](../plans/m3-model-provider-agent-loop.md)。

M3 实现落点：canonical 契约与错误（`include/mira/model_contracts.hpp`）、profile/路由
（`model_profile.hpp`）、digest 与脱敏（`model_digest.hpp`）、JSON Schema 子集与 Decision/repair
（`model_schema.hpp`）、两个方言 mapper（`model_dialect.hpp`）、Responses SSE 状态机
（`model_sse.hpp`）、Tool 桥（`model_tool.hpp`）、预算（`model_budget.hpp`）、重试/circuit
（`model_supervisor.hpp`）、Provider 与传输契约（`model_provider.hpp`、`model_transport.hpp`）、
网关与 admission（`model_gateway.hpp`）、回放 Provider（`model_replay.hpp`）与闭环
（`agent_loop.hpp`）；传输实现位于 `adapters/net/`。设计文档中的 C++ 片段与实现存在字段级
演化（如 `TransportTrace` 阶段回填），以公共头与测试为准。

## 20. 已知限制与待验证项

- 传输实现已按 [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md) 落地为自研
  socket transport + 可插拔 TLS 通道，并锁定 Mbed TLS `v3.6.7` 作为
  Windows/Linux/Android 共用实现；HTTP absolute-form 与 HTTPS CONNECT 代理已交付。Linux 的直接
  TLS、CONNECT 后 TLS 和错误 CA 已运行验证；Windows 已运行 portable direct/CONNECT/error-CA
  contract，Android NDK arm64 构建通过（设备 runtime 不由此外推）。MiniMax-M3 Responses 已完成
  分 capability 互操作；支持与失败边界见兼容性矩阵。
- 各 Provider 对 strict JSON Schema 子集、usage、rate-limit header、idempotency 和 model alias 的实际
  行为必须逐 profile 验证。
- 官方 API 会演进；上游新增字段不自动成为 Mira 支持能力，须经 schema/fixture 版本更新。
- 首次 schema 编译、图片 token 和 reasoning/cached usage 的精确成本不在所有 Provider 可得。
- 本设计不宣称 background/WebSocket 会比 HTTP/SSE 更适合 Mira；需要后续 benchmark 和生命周期
  设计后再决定。

## 21. 关联文档与官方依据

- [DEC-007：LLM API 规范契约与协议方言策略](../decisions/DEC-007-llm-api-protocol-strategy.md)
- [DEC-010：跨平台 TLS、代理与 upload 生命周期](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)
- [OpenAI-compatible 兼容性矩阵](../compatibility/openai-compatible-matrix.md)
- [Model Provider 与 Tool 扩展设计](model_provider_and_tool_design.md)
- [Context 与 Memory 设计](context_and_memory_design.md)
- [EventStore、ArtifactStore 与崩溃一致性](event_artifact_crash_consistency.md)
- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
- [M3 Model Provider 与 Agent 闭环](../plans/m3-model-provider-agent-loop.md)

OpenAI 官方协议参考（访问并核对日期：2026-08-30）：

- [Create a model response](https://developers.openai.com/api/reference/resources/responses/methods/create)
- [Streaming API responses](https://developers.openai.com/api/docs/guides/streaming-responses)
- [Structured model outputs](https://developers.openai.com/api/docs/guides/structured-outputs)
- [Function calling](https://developers.openai.com/api/docs/guides/function-calling)
- [Conversation state](https://developers.openai.com/api/docs/guides/conversation-state)
- [Background mode](https://developers.openai.com/api/docs/guides/background)
- [Webhooks](https://developers.openai.com/api/docs/guides/webhooks)
- [WebSocket mode](https://developers.openai.com/api/docs/guides/websocket-mode)
- [Counting tokens](https://developers.openai.com/api/docs/guides/token-counting)
- [Rate limits](https://developers.openai.com/api/docs/guides/rate-limits)
- [Prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching)
