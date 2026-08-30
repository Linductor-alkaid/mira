# Mira Model Provider 与 Tool 扩展设计

> 状态：Active  
> 版本：1.2  
> 更新日期：2026-08-31   
> 适用范围：外部 LLM/VLM、OpenAI-compatible 协议、路由、预算、Tool registry 与隔离  
> 上位设计：[Mira Runtime 设计](mira_runtime_design.md)  
> 协议细化：[LLM API 协议设计](llm-api-protocol-design.md)  
> 决策依据：[DEC-007](../decisions/DEC-007-llm-api-protocol-strategy.md)

## 1. 目标与边界

Model Provider 负责可取消的远端推理传输和供应商能力映射；Tool 负责显式注册的外部功能。两者
都不能直接修改 Task 状态或平台输入，也不能拥有脱离 Runtime 的生命周期。

Core 负责 prompt/context、Decision schema、Policy、Action/Tool 选择和 Recovery。Provider 负责
认证、协议、请求/响应大小、stream transport、usage 和稳定错误映射。Tool 实现只执行 ToolSpec
声明的 operation，不能把自己注册成新的 authority。

## 2. Model Provider 能力

```cpp
struct ModelCapabilities {
    bool text;
    bool image_input;
    bool structured_output;
    bool tool_calling;
    bool streaming;
    bool exact_token_count;
    bool server_compaction;
    std::uint64_t max_context_tokens;
    std::uint64_t max_output_tokens;
    std::uint64_t max_request_bytes;
    std::vector<ImageTransport> image_transports;
    SchemaDialect structured_dialect;
};

class IModelProvider {
public:
    virtual ModelCapabilities capabilities() const = 0;
    virtual Result<ModelResponse> infer(
        const ModelRequest&, const OperationContext&) = 0;
};
```

首期公共契约以完整 response 为 Task completion。streaming 是 Provider 内部/可选事件能力：增量文本
只用于 UI 和受限 parser preview，只有完整 response 通过 schema 校验后才能产生 Decision。Reasoning
token/隐藏推理不要求也不保存。

`ModelRequest`、`ModelResponse`、output item、usage、terminal status 和稳定错误的规范字段定义见
[LLM API 协议设计](llm-api-protocol-design.md)。Provider transport 成功、协议 terminal 和有效 Decision
是三层不同结果，不能用单个 HTTP success 合并表示。

M3 实现落点为 `include/mira/model_provider.hpp`（`IModelProvider` 以 `profile()` 暴露
`ModelCapabilities` 的 manifest 载体 `ModelProfile`，并附带 `last_trace`/`last_sse_stats`/
`last_retry_after_hint` 诊断）与 `OpenAiCompatibleProvider`；字段级演化以公共头与
`m3_*` 契约测试为准。

## 3. OpenAI-compatible Adapter

配置 profile 包含 endpoint、model、protocol dialect、capabilities、timeouts、proxy/TLS、SecretRef、
token/byte/cost limit、retry 和 data policy。兼容服务差异通过 profile/capability plugin，不在 Core
按品牌分支。

首批固定方言为 `openai.responses.v1` 与 `openai.chat-completions.v1`。官方 OpenAI 或已经互操作验证
完整支持 Responses 的服务优先使用前者；后者是独立兼容方言，不是请求失败后的自动 endpoint
降级。不得按品牌猜 capability、通过生产推理试错探测能力，或把某一服务的 Responses 行为外推到
其他“compatible”服务。证据记录见
[OpenAI-compatible 兼容性矩阵](../compatibility/openai-compatible-matrix.md)。

请求流程：

1. 检查 profile 授权、endpoint policy 和 data sensitivity。
2. ContextManager 产出有预算、authority 标注的 `PreparedModelContext`。
3. Adapter 转为供应商 wire format，验证最终 bytes/token upper bound。
4. 记录 request digest、profile/model、schema、artifact refs 和预算；不记录 secret。
5. 使用可中断 HTTP operation；stream 有单 chunk/总 bytes/deadline 限制。
6. 映射 HTTP/协议/供应商错误、usage、finish reason 和 rate-limit metadata。
7. Core DecisionParser 对完整 response 做 schema/Policy 校验。

图片 transport（base64、URL、upload/file ID）各自声明生命周期。临时 URL/token 最小权限、短 TTL，
redirect 和 endpoint 受 SSRF policy；remote upload artifact 在 Task/retention 后清理并审计。

## 4. 取消、超时与重试

- connect、TLS、write、first-byte、idle-read 和 total deadline 分开配置。
- cancel 必须关闭/取消 transport 等待并最终结算 operation；若 SDK 无取消，放入可中断 blocking
  boundary 或判定不适用。
- Executor queued timeout 不是网络 deadline。
- 只在没有向对端产生计费/请求或具有 provider idempotency key 且策略允许时自动 transport retry。
- 429/5xx 尊重 `Retry-After`，有总次数/时间/费用预算，不跨 Task deadline。
- 完整响应丢失但可能计费不是环境副作用；可重试与否由 cost/recovery policy 决定并记录旧 request。
- structured output 修复最多有界次数；修复是新 request，计入 token/cost budget。

## 5. Model Router 与预算

Router 输入：任务需要的 text/vision/schema capability、敏感等级、授权 Provider、context size、延迟/
费用预算、健康/circuit state。输出记录 `RouteDecision`，包含候选、拒绝原因和实际 profile。

预算至少分层：runtime、tenant/user、session、task 和单 request：

- input/output tokens、images/bytes。
- request 次数、并发和 rate。
- estimated/actual monetary cost。
- wall/deadline 和 retry/repair 次数。

预估超过 hard budget 在发送前拒绝。实际 usage 超预估触发后续 admission/circuit，不撤销已经发生的
费用。Provider fallback 只有在数据政策、地域、capability 和授权均满足时允许；模型质量偏好必须由
可验证 eval profile 决定。

## 6. Provider 状态与降级

健康状态：`Unknown -> Healthy -> Degraded -> OpenCircuit -> Probing`。健康只影响路由，不改变
授权。失败分类区分 DNS/connect/TLS/auth/rate/server/schema/content policy/cancel/deadline。

- auth/permission 不自动 retry/fallback 到不同凭据。
- TLS/pin 不匹配 fail closed。
- content policy 拒绝作为明确结果，不偷偷换供应商绕过。
- usage/cost metadata 缺失标 quality degraded。
- Provider opaque conversation/compaction 仅作同 Provider/model 的优化，不能替代 Event/Checkpoint。
- Provider retention 必须显式映射；OpenAI Responses profile 默认 `store=false`，远端 conversation、
  upload 和 raw response 的保留/删除由 `ModelDataPolicy` 控制。

## 7. ToolSpec

```cpp
struct ToolSpec {
    ToolId id;
    SemanticVersion version;
    std::string name;
    JsonSchema arguments;
    JsonSchema result;
    std::vector<CapabilityId> required_capabilities;
    ResourcePolicy resources;
    SideEffectClass side_effect;
    IdempotencySupport idempotency;
    DataAccessDeclaration data_access;
    ToolLimits limits;
    IsolationMode isolation;
};

class ITool {
public:
    virtual ToolSpec spec() const = 0;
    virtual Result<ToolResult> invoke(
        const ToolArguments&, const OperationContext&) = 0;
};
```

Tool name 只用于模型 schema，内部使用稳定 ID+version。注册时验证名称冲突、schema size/depth、权限、
签名/来源和 shutdown contract。运行中不能由模型新增 native Tool。

## 8. Tool 调用协议

1. DecisionParser 只接受当前 Task 暴露的 Tool ID/version。
2. JSON schema、canonicalization、size 和 resource descriptor 解析。
3. Policy 评估 capability、scope、risk、budget 和 confirmation。
4. 有副作用 Tool 使用 EventStore intent logging；支持时传 idempotency key。
5. ExecutionSupervisor 调度并保留 future/handle，Tool 定期检查 cancellation/deadline。
6. result 做 schema、size、sensitivity、provenance 和 redaction 校验。
7. 具有副作用或声明 expected outcome 的 Tool 进入 Verify。

Tool 返回 success 仅表示 Tool contract 完成，不自动表示 Agent goal 完成。timeout/transport error 若
side effect 可能发生，返回 `ExecutionUncertain`。

## 9. Tool 隔离模型

首期支持：

- `BuiltIn`: 与 Mira 同进程、审计过的窄接口。
- `HostProvided`: 由受信 Host bridge 实现，仍经过 Provider contract。
- `OutOfProcess`: 推荐给第三方/复杂 Tool，通过版本化 IPC、进程权限和资源限制隔离。

首期不把任意动态库当作安全插件机制。若将来支持 loadable plugin，需单独 ABI、签名、卸载和崩溃
隔离设计。

OutOfProcess Tool：

- 单独 OS identity/sandbox，最小文件/网络/设备权限。
- IPC message 大小、并发、deadline 和 schema 有界。
- process crash/exit/signal 转稳定 Error；有副作用时按 receipt 确定/不确定分类。
- shutdown 先停止 invocation producer，协作 cancel，deadline 后由 Host 终止进程并记录不安全结算。
- stdout/stderr 视为受保护 diagnostic，限流和脱敏。

## 10. 文件、网络和 Secret Tool

- 文件参数解析成 canonical descriptor，授权 root 之外拒绝；使用防 symlink race 的平台打开方式。
- 网络 endpoint 每次 redirect/DNS resolution 都应用 SSRF policy；metadata、loopback、link-local、私网
  默认拒绝。
- Tool 不接收 Secret 明文 JSON；使用最小 scope `SecretRef`，由隔离边界按调用获取。
- 任意 shell/代码执行 Tool 默认不存在。产品确需时属于独立 R4 能力、沙箱和安全设计。
- Tool output 不能包含未声明大文件；大载荷写 ArtifactStore 并返回 descriptor。

## 11. Tool schema 暴露与 Context

每次模型请求只暴露当前状态、权限和任务真正可用的 Tool schema，避免全部 registry 消耗 context
并扩大攻击面。ContextManager 记录选择/排除理由。Tool output：

- 未消费结果保留到模型产生依赖它的后续 Decision。
- 已消费大载荷替换为 ArtifactRef/结构化摘要。
- 外部指令性文本标 `UntrustedExternalData`。
- Secret、认证 header、内部路径和 sandbox diagnostic 不进入模型。

## 12. Versioning 与兼容

- Tool argument/result schema 和语义按 major/minor 版本。
- Task 计划绑定 Tool ID+major；minor 更新只有在向后兼容测试通过后热更新。
- 在途 invocation 固定 registry snapshot，更新不改变其实现 identity。
- removed/revoked Tool 不接受新调用；旧 completion 仍结算但不能开启后续 action。
- Replay 使用 recorded ToolSpec digest/result，不加载真实 Tool。

## 13. Executor 与关闭

- HTTP transport event loop/worker 使用 Executor blocking I/O worker 或受支持外部 loop bridge。
- response parse、schema validation 和有限压缩使用 `submit_auto()`。
- retry/backoff/circuit probe 使用 Executor timer。
- Tool invocation 按 workload 使用普通或 blocking path，不能自行创建 Mira worker。
- SDK/库内部线程在 Provider/Tool compatibility 文档声明并有 shutdown；不能缓存跨 Runtime callback。
- 关闭分别结算 timer、transport/blocking、Tool process 和默认 future，再关闭 Runtime 控制面/Executor。

## 14. 可观测性

Model：route/profile/model、request digest、token/byte/latency、retry/repair、rate metadata、finish/error、
cost estimate/actual、circuit 和 redaction；不记录 secret/默认原始 payload。

Tool：spec digest、policy/confirmation、arguments digest、安全资源摘要、side-effect start/receipt、bytes/
latency、sandbox/exit、idempotency 和 Verification。敏感 argument/result 只存受保护 Artifact 或摘要。

## 15. Contract Tests

### 15.1 Model

- 两个 OpenAI-compatible dialect 分别维护 fixtures：text/image/structured/tool/stream/error/usage；
  不复用预期结果掩盖协议差异。
- connect/TLS/read/total timeout、cancel、partial stream、oversize、malformed JSON。
- rate-limit bounded retry、repair budget、fallback data policy、circuit transition。
- Provider capability 与真实请求行为一致；opaque state 丢失可由 Event/Checkpoint 恢复。
- unknown terminal/output、stream duplicate/gap/EOF、refusal/incomplete/content filter 和 ambiguous
  completion 均 fail closed。

### 15.2 Tool

- schema/unknown version/name conflict、argument bombs、oversize result。
- capability/ACL/confirmation、path traversal/symlink、SSRF/redirect/DNS rebinding。
- explicit failure、uncertain side effect、idempotency duplicate、cancel/crash/shutdown。
- Replay 不加载/调用真实 Tool。

## 16. 关联文档

- [核心公共契约与状态机](core_contracts_and_state_machine.md)
- [威胁模型与确认协议](../security/threat_model_and_confirmation.md)
- [Event/Artifact 与崩溃一致性](event_artifact_crash_consistency.md)
- [Context 与 Memory](context_and_memory_design.md)
- [评估与基准体系](evaluation_and_benchmark_design.md)
- [LLM API 协议设计](llm-api-protocol-design.md)
- [OpenAI-compatible 兼容性矩阵](../compatibility/openai-compatible-matrix.md)
- [M3 Model Provider 与 Agent 闭环](../plans/m3-model-provider-agent-loop.md)
