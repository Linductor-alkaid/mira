# M3：Model Provider 与视觉离散 Agent 闭环

> 状态：Planned  
> 负责人：Mira Maintainers  
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)  
> 前置：M2  
> 建议发布点：Agent loop alpha  
> 更新日期：2026-08-30

## 1. 目标

交付首个可取消、可观测、可回放的 `Observe -> Reason -> Plan -> Act -> Verify` 视觉离散闭环；实现
canonical Model 契约、OpenAI Responses 和 Chat Completions 显式方言、受管 HTTP/SSE transport、
Structured Decision、Tool Proposal 桥接基础，以及至少一个经过真实证据验证的 Provider profile。

M3 完成表示“Agent loop alpha”可以在 Simulator 和受支持 Provider 上运行，不表示所有
OpenAI-compatible 服务、所有模型、连续控制、本地 ONNX 或生产 Tool 隔离已经完成。

## 2. 范围与非目标

### 2.1 范围

- canonical `ModelRequest`、`ModelResponse`、output item、usage、错误和 profile contract；
- `openai.responses.v1` 同步 HTTP 和 SSE，`openai.chat-completions.v1` 同步 HTTP；
- strict Structured Output、本地 Decision 校验、有限 repair、Provider Tool Proposal 映射；
- endpoint/TLS/Secret/data retention、预算、rate limit、重试、circuit 和费用核销；
- Screenshot/VLM 到离散 Action 的 Simulator 闭环，以及执行后重新 Observe/Verify；
- Event/Artifact、OfflineReplay、取消、shutdown 和兼容性证据。

### 2.2 非目标

- Background、Webhook、WebSocket、Batch、Assistants 和 Provider hosted tools；
- 连续动作和实时控制（M6）；本地 OCR/检测/ONNX（M5）；完整长期 Context/Memory（M4）；
- 任意第三方 Tool 进程隔离和完整 Tool 产品面（M7）；
- 声明所有 DeepSeek、Qwen、OpenRouter 或其他兼容 endpoint 均可互操作；
- 使用生产用户数据探测 capability，或让模型/Provider 直接调用真实输入。

## 3. 设计与决策依据

- [LLM API 协议设计](../design/llm-api-protocol-design.md)
- [Model Provider 与 Tool 扩展设计](../design/model_provider_and_tool_design.md)
- [Mira Runtime 设计](../design/mira_runtime_design.md)
- [Context 与 Memory 设计](../design/context_and_memory_design.md)
- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
- [OpenAI-compatible 兼容性矩阵](../compatibility/openai-compatible-matrix.md)
- [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md)、
  [DEC-002](../decisions/DEC-002-public-contract-versioning.md)、
  [DEC-003](../decisions/DEC-003-event-sourced-persistence.md)、
  [DEC-004](../decisions/DEC-004-security-authority-confirmation.md)、
  [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)、
  [DEC-007](../decisions/DEC-007-llm-api-protocol-strategy.md)

## 4. 工作项

### 4.1 Canonical 契约与 Profile

- [ ] `M3-01` 实现版本化 `ModelRequest`、`ModelResponse`、input/output item、usage、completion status、
  stable error 和 unknown-field 安全语义，并建立 schema golden tests。
- [ ] `M3-02` 实现版本化 Model profile/capability manifest、参数映射策略和 evidence metadata；路由在
  发送前拒绝 capability/data-policy/budget mismatch。
- [ ] `M3-03` 实现 Prompt/Decision/Tool/profile/wire canonical digest 和 Secret/signed URL 排除规则，
  并覆盖 Replay 与脱敏测试。

### 4.2 Transport 与方言

- [ ] `M3-04` 选择并锁定 C++ HTTP/TLS 依赖，验证桌面和目标 Android NDK 构建、TLS、代理、DNS、
  redirect、SSE、取消及内部线程/shutdown；同步供应链与兼容性记录。
- [ ] `M3-05` 实现 Executor 受管 transport：blocking worker或批准的 external-loop bridge、可解除 socket
  等待、分阶段 deadline、大小上限和确定 shutdown。
- [ ] `M3-06` 实现 `openai.responses.v1` 同步 request/response mapper，覆盖 text、image/file、strict
  schema、Tool Call、usage、refusal、incomplete 和 unknown output。
- [ ] `M3-07` 实现 Responses SSE typed-event parser，覆盖任意分片、事件配对、唯一 terminal、背压、
  preview 隔离、partial EOF、取消竞态和 EventStore 摘要。
- [ ] `M3-08` 实现 `openai.chat-completions.v1` 同步 mapper及独立 fixtures；不可表示字段明确失败，
  不隐式切换 endpoint。Chat Completions SSE 不作为本项完成条件。

### 4.3 Structured Decision、Tool 与闭环

- [ ] `M3-09` 实现 strict JSON Schema dialect gate、本地 schema validator、Decision唯一性检查和有界
  repair operation；refusal、filter、incomplete 和 malformed output具有不同结果。
- [ ] `M3-10` 实现 Provider call ID 到 `ToolId`/`OperationId` 的映射、完整 arguments 后解析、多调用
  batch、duplicate/conflict 和 Tool result 回填；hosted tools fail closed。
- [ ] `M3-11` 实现 ModelGateway/Router/DecisionParser 与 Task epoch admission；迟到、取消和终态 response
  不推进状态或产生 Action。
- [ ] `M3-12` 在 Simulator 交付 Screenshot/VLM -> structured Decision -> discrete Action -> fresh
  Observation -> Verification 闭环，覆盖成功、恢复、失败和最大步数。

### 4.4 可靠性、安全和成本

- [ ] `M3-13` 实现 request-stage 重试表、`Retry-After`、circuit、ambiguous completion 和有限 fallback；
  retry/timer/费用/deadline 约束有表驱动测试。
- [ ] `M3-14` 实现 token/byte/request/cost reservation 与 reported usage 核销，区分 estimated/exact/
  partial/missing、cached/reasoning tokens 和版本化 price table。
- [ ] `M3-15` 实现 explicit `store`/retention、region/org/project policy、upload/delete lifecycle 和受保护
  raw response Artifact；默认日志不含 prompt、截图、Secret 或完整 response。
- [ ] `M3-16` 建立 endpoint allowlist、redirect/DNS SSRF、TLS、credential、prompt injection、unknown
  tool/item/schema 和 hosted computer-use 负向测试。

### 4.5 Replay、互操作与集成

- [ ] `M3-17` 扩展 Event/Artifact schema，OfflineReplay 使用 recorded canonical response 且无 Network/
  Tool/Input capability；删除 raw payload 后以 tombstone 表达质量降级。
- [ ] `M3-18` 建立两个 dialect 的 mock/golden contract suite 和故障注入 server，覆盖 transport、协议、
  stream、Tool、usage、错误、取消、shutdown 和资源上限。
- [ ] `M3-19` 在受控测试账户、非用户数据和费用上限下，至少完成一个明确 Provider/dialect/model 的
  `InteropVerified`，并更新兼容性矩阵；没有凭据或网络时本项保持未完成。
- [ ] `M3-20` 同步公共 API、示例、设计、安全、兼容性、供应链和验证记录，产出 Agent loop alpha
  release notes。

## 5. Executor 路由与关闭

| 工作 | Executor 能力 | Owner | 结算证据 |
| --- | --- | --- | --- |
| request构造、parse、schema/Decision校验 | `submit_auto()` | ModelGateway | 已消费 future/result |
| HTTP/SSE阻塞等待 | blocking I/O worker或批准 external-loop bridge | ProviderTransport | `WorkerHandle` terminal status |
| retry、`Retry-After`、circuit、远端清理 | timer | ProviderSupervisor | `TimerHandle` cancel/terminal |
| Task状态推进 | Runtime串行控制面 | TaskCoordinator | event sequence/state snapshot |

关闭必须停止 producer、取消并 wake transport、取消 timer、结算 worker和有限 future、提交 terminal或
uncertain事件，再由非 worker owner 关闭 Executor。若 HTTP 库的 event loop 无法满足上述边界，先登记
Executor反馈台账；不得新增裸线程或隐藏全局 loop。

## 6. 风险与阻塞

- `RISK-2026-010`：第三方“compatible”语义漂移。Owner：M3 Provider owner。缓解：固定 dialect/profile
  digest、证据有效期、fixture 和 fail-closed capability；解除条件：至少一个 profile 真实互操作通过。
- `RISK-2026-011`：HTTP/TLS 库可能在 Android、SSE取消或 Executor 生命周期方面不满足要求。
  Owner：M3 transport owner。解除条件：`M3-04` 原型验证并记录依赖/线程/shutdown证据；若是 Executor
  通用能力不足，登记 `EXE-*` 后再决定兼容边界。
- `RISK-2026-012`：严格 schema 的供应商子集或首次编译延迟影响闭环。Owner：M3 model owner。
  缓解：发送前 dialect gate、schema cache telemetry、有限 repair 和预热基准。
- `RISK-2026-013`：真实 API 测试需要凭据、费用、网络和数据政策。Owner：M3 release owner。未具备时
  `M3-19` 和里程碑保持未完成，不能用 mock 代替互操作声明。

当前没有确认的 Executor 能力缺口，因此不新增 `docs/executor_feedback/ledger.md` 记录。

## 7. 测试矩阵

| 维度 | 必测场景 |
| --- | --- |
| Canonical/schema | current/previous、unknown enum/item、strict subset、不可表示字段 |
| Responses | text、image、file、Tool、refusal、incomplete、usage、unknown output |
| Chat Completions | text、vision（若声明）、Tool、finish reason、usage、deviation |
| SSE | byte/UTF-8/JSON分片、delta/done、duplicate/gap、EOF、唯一 terminal、背压 |
| Transport | DNS/TLS/connect/write/read/total timeout、redirect、proxy、取消、shutdown |
| Retry/cost | request发送阶段、429/5xx、`Retry-After`、预算耗尽、ambiguous completion |
| Tool/security | call ID冲突、多调用、副作用不重放、SSRF、Secret、hosted tool拒绝 |
| Runtime | Task epoch、迟到response、pause/cancel/Takeover、最大步数、Verify失败 |
| Replay | 无网络/输入/Tool、raw Artifact存在/删除、schema migration |
| Interop | 明确endpoint/model/profile、同步/stream、错误、retention和cleanup |

## 8. 退出条件

- [ ] `M3-01` 至 `M3-20` 全部完成并有可复现验证记录。
- [ ] 两个 dialect 的同步 mapper contract suite 通过；Responses SSE 全部状态、分片和取消用例通过。
- [ ] 所有模型 Action 都来自完整 terminal response 并通过本地 schema、epoch、Policy 和 Planner 校验。
- [ ] Simulator 闭环覆盖成功、拒绝、incomplete、schema失败、429、取消、Takeover和Verify恢复。
- [ ] ASAN/UBSAN通过；支持环境下 TSAN通过，未支持时保留未完成项和补跑条件。
- [ ] Shutdown 后无在途 transport、timer、future、callback或第三方 SDK worker；迟到结果不改变终态。
- [ ] 至少一个明确 Provider/dialect/model 达到 `InteropVerified`，其余服务不被误标为支持。
- [ ] OfflineReplay 在能力图和测试中无法访问真实 Network、Tool 或 Input。
- [ ] Secret、Authorization、signed URL、敏感 prompt/截图/response 不出现在普通日志和事件。
- [ ] 兼容性、供应链、安全、设计、计划和 release notes 与实现同步。

## 9. 验证记录

2026-08-30：完成 M3 设计基线、协议决策、兼容性矩阵和工作项拆分；尚无实现、构建、API 调用或
互操作证据，所有实施与退出项保持未勾选。

