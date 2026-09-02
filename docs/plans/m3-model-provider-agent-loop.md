# M3：Model Provider 与视觉离散 Agent 闭环

> 状态：Completed（实现、跨平台传输与 MiniMax-M3 Responses 分能力互操作证据已验收）
> 负责人：Mira Maintainers  
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)  
> 前置：M2  
> 建议发布点：Agent loop alpha（[发布说明](../releases/agent-loop-alpha.md)）  
> 更新日期：2026-09-02

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
  [DEC-007](../decisions/DEC-007-llm-api-protocol-strategy.md)、
  [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)（替代历史
  [DEC-008](../decisions/DEC-008-transport-dependency-strategy.md)）

## 4. 工作项

### 4.1 Canonical 契约与 Profile

- [x] `M3-01` 实现版本化 `ModelRequest`、`ModelResponse`、input/output item、usage、completion status、
  stable error 和 unknown-field 安全语义，并建立 schema golden tests。
  （`model_contracts.hpp/.cpp`、`json.hpp/.cpp`；golden/round-trip/未知枚举 fail-closed 见
  `mira_m3_canonical_test`）
- [x] `M3-02` 实现版本化 Model profile/capability manifest、参数映射策略和 evidence metadata；路由在
  发送前拒绝 capability/data-policy/budget mismatch。
  （`model_profile.hpp/.cpp`：证据等级、参数映射策略、profile digest、`ModelRouter`；
  路由拒绝与敏感度政策见 `mira_m3_canonical_test`、`mira_m3_gateway_test`）
- [x] `M3-03` 实现 Prompt/Decision/Tool/profile/wire canonical digest 和 Secret/signed URL 排除规则，
  并覆盖 Replay 与脱敏测试。（`model_digest.hpp/.cpp`；secret 不影响 digest、事件脱敏、URL 查询剥离
  见 `mira_m3_canonical_test`，回放摘要见 `mira_m3_replay_test`）

### 4.2 Transport 与方言

- [x] `M3-04` 选择并锁定 C++ HTTP/TLS 依赖，验证桌面和目标 Android NDK 构建、TLS、代理、DNS、
  redirect、SSE、取消及内部线程/shutdown；同步供应链与兼容性记录。
  已交付：依赖决策由 [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md) 锁定为自研
  socket transport + Mbed TLS `v3.6.7` 跨平台通道 + Linux OpenSSL 参考通道；Linux 桌面完成 HTTP/1.1、chunked、
  SSE、DNS、redirect、取消与 shutdown 的 loopback 验证，TLS 完成真实握手与错误 CA fail-closed 验证
  （`mira_m3_transport_test`、`mira_m3_tls_test`）。跨平台构建证据：CI run
  [`33332557571`](https://github.com/Linductor-alkaid/mira/actions/runs/33332557571) 中 Windows
  MSVC Debug/Release（含 Winsock 传输与其 loopback 测试）与 Android arm64（显式构建
  `mira_net_transport`）均通过。
  2026-09-01 新增 HTTP absolute-form、HTTPS CONNECT、proxy/target 双重 SSRF/allowlist、独立
  Proxy SecretRef 和 CONNECT 后真实 TLS 测试（`mira_m3_transport_test`、两种 `m3_*tls_test`）。
  `mira_mbedtls_transport` 使用 nonblocking BIO，不创建线程，Windows/Android CI 构建目标已接入。
  2026-09-02 新增不依赖 OpenSSL 的 `mira_m3_mbedtls_portable_test`，以锁定的 Mbed TLS 作为
  loopback server，覆盖 direct TLS、CONNECT 后 TLS、proxy credential 和错误 CA fail-closed。CI run
  [`33578613423`](https://github.com/Linductor-alkaid/mira/actions/runs/33578613423) 完成 Windows
  Debug/Release 运行（各 28/28）及 Android NDK 26.3 arm64 构建；Windows 日志明确执行并通过该
  portable contract，据此关闭目标构建与至少一个非 Linux 目标 TLS runtime 验收。Android 设备/模拟器
  runtime 支持声明仍属于后续平台交付，不由 M3 的 NDK 构建证据外推。
- [x] `M3-05` 实现 Executor 受管 transport：blocking worker或批准的 external-loop bridge、可解除 socket
  等待、分阶段 deadline、大小上限和确定 shutdown。（`adapters/net/socket_transport.cpp`：
  `mira-provider-io` blocking I/O worker、poll 切片取消、DNS/connect/TLS/write/first-byte/idle/total
  deadline、请求/响应上限、`WorkerHandle` join 结算）
- [x] `M3-06` 实现 `openai.responses.v1` 同步 request/response mapper，覆盖 text、image/file、strict
  schema、Tool Call、usage、refusal、incomplete 和 unknown output。
  （`ResponsesV1Mapper`，含与 SSE 终态共享的 `decode_responses_terminal_body`；见
  `mira_m3_dialect_test` golden 用例）
- [x] `M3-07` 实现 Responses SSE typed-event parser，覆盖任意分片、事件配对、唯一 terminal、背压、
  preview 隔离、partial EOF、取消竞态和 EventStore 摘要。（`model_sse.hpp/.cpp`：逐字节分片、
  CRLF/CR/LF 行结束、add/done 配对、remote sequence gap、唯一 terminal、EOF 无 terminal →
  `AmbiguousCompletion`、有界 `UnvalidatedModelPreview` 与 drop 计数；事件摘要经
  `ModelResponseReceived` 载荷（仅 digest））
- [x] `M3-08` 实现 `openai.chat-completions.v1` 同步 mapper及独立 fixtures；不可表示字段明确失败，
  不隐式切换 endpoint。Chat Completions SSE 不作为本项完成条件。（`ChatCompletionsV1Mapper`；
  continuation/`store=true`/file part/未验证 SSE 全部 `CapabilityMismatch`，见
  `mira_m3_dialect_test`）

### 4.3 Structured Decision、Tool 与闭环

- [x] `M3-09` 实现 strict JSON Schema dialect gate、本地 schema validator、Decision唯一性检查和有界
  repair operation；refusal、filter、incomplete 和 malformed output具有不同结果。
  （`model_schema.hpp/.cpp`：子集门禁拒绝 `$ref`/组合关键字/深度超标；validator 覆盖
  type/enum/const/required/additionalProperties/items/长度/范围/pattern；`parse_decision` 的
  outcome 分类与 `build_schema_repair_request` 有界修复，见 `mira_m3_schema_test`、
  `mira_m3_agent_loop_test` 的 repair 用例）
- [x] `M3-10` 实现 Provider call ID 到 `ToolId`/`OperationId` 的映射、完整 arguments 后解析、多调用
  batch、duplicate/conflict 和 Tool result 回填；hosted tools fail closed。（`model_tool.hpp/.cpp`：
  稳定 `OperationId` 派生、同 digest 去重/异 digest 冲突、digest 校验、批量上限、两种方言的
  结果回填与大载荷 ArtifactRef 替换；hosted/未知名称 `ProtocolViolation`，见
  `mira_m3_tool_test`）
- [x] `M3-11` 实现 ModelGateway/Router/DecisionParser 与 Task epoch admission；迟到、取消和终态 response
  不推进状态或产生 Action。（`model_gateway.hpp/.cpp` + `SimpleAdmissionGate`：发送前与结算时双重
  admission，未准入响应标记 `admitted=false` 且不产生 Decision；见 `mira_m3_gateway_test`、
  `mira_m3_agent_loop_test` 的 admission 用例）
- [x] `M3-12` 在 Simulator 交付 Screenshot/VLM -> structured Decision -> discrete Action -> fresh
  Observation -> Verification 闭环，覆盖成功、恢复、失败和最大步数。（`agent_loop.hpp/.cpp`：
  截图 Artifact 引用进入请求、canonical 坐标动作编译、动作后重新观察与 Verifier 判定；成功/拒绝/
  incomplete 恢复/schema repair/最大步数/验证分歧/取消/admission 拒绝/429 恢复全部覆盖于
  `mira_m3_agent_loop_test`）

### 4.4 可靠性、安全和成本

- [x] `M3-13` 实现 request-stage 重试表、`Retry-After`、circuit、ambiguous completion 和有限 fallback；
  retry/timer/费用/deadline 约束有表驱动测试。（`model_supervisor.hpp/.cpp` 纯决策表 +
  `ProviderCircuit`；有限 fallback 在 gateway 中仅对可重试、非模糊失败切换候选 profile 并发出
  `ModelProfileFallback` 事件；retry 等待经 Executor timer。表驱动用例见
  `mira_m3_supervisor_budget_test`，fallback 见 `mira_m3_gateway_test`）
- [x] `M3-14` 实现 token/byte/request/cost reservation 与 reported usage 核销，区分 estimated/exact/
  partial/missing、cached/reasoning tokens 和版本化 price table。（`model_budget.hpp/.cpp`：
  保守预留/核销/释放（ambiguous 只保留 cost）、MissingUsage 与 Partial 保留待审计、UnknownPrice
  不记零成本、价格生效窗口与版本；见 `mira_m3_supervisor_budget_test`、`mira_m3_gateway_test`）
- [x] `M3-15` 实现 explicit `store`/retention、region/org/project policy、upload/delete lifecycle 和受保护
  raw response Artifact；默认日志不含 prompt、截图、Secret 或完整 response。
  已交付：`store` 显式序列化（Responses 始终发送布尔值，不依赖 Provider 默认）、region/org/project
  header 映射、受保护 raw response Artifact（`Sensitivity::Sensitive` + digest 引用）、事件/日志默认只含
  digest 的负向测试（`mira_m3_gateway_test` 检索 prompt/响应/Secret 字符串均不存在）。
  2026-09-01 完成 `/files` multipart upload、attempt-local `file_id` binding、立即/Executor timer 延时删除、
  handle/future shutdown 结算、敏感 Artifact 拒绝及仅含 provider ID digest 的删除失败/取消审计；见
  `model_upload.hpp/.cpp`、`mira_m3_upload_test`。协议与生命周期由 DEC-010 冻结。
- [x] `M3-16` 建立 endpoint allowlist、redirect/DNS SSRF、TLS、credential、prompt injection、unknown
  tool/item/schema 和 hosted computer-use 负向测试。（allowlist/SSRF 地址族/scheme/userinfo：
  `mira_m3_transport_test`；redirect 跨源剥离凭证：同文件；TLS 错误 CA fail-closed：
  `mira_m3_tls_test`；credential 只在 socket 边界出现且不入结果：同上；prompt injection 保持
  user-role/不可信 provenance：`mira_m3_gateway_test`；unknown tool/hosted computer-use：
  `mira_m3_tool_test`；unknown item/enum/schema：`mira_m3_canonical_test`、
  `mira_m3_dialect_test`、`mira_m3_schema_test`）

### 4.5 Replay、互操作与集成

- [x] `M3-17` 扩展 Event/Artifact schema，OfflineReplay 使用 recorded canonical response 且无 Network/
  Tool/Input capability；删除 raw payload 后以 tombstone 表达质量降级。（`model_replay.hpp/.cpp`：
  `ReplayModelProvider` 无 transport/secret/artifact 写路径，stream 请求 `CapabilityMismatch`；
  与 `OfflineReplayEnvironment` 组合的闭环回放、脚本耗尽上界与 raw payload 删除降级见
  `mira_m3_replay_test`）
- [x] `M3-18` 建立两个 dialect 的 mock/golden contract suite 和故障注入 server，覆盖 transport、协议、
  stream、Tool、usage、错误、取消、shutdown 和资源上限。（golden：`mira_m3_dialect_test`；mock
  transport 契约：`tests/support/m3_support.hpp` 的 `MockHttpTransport`；真实 socket 故障注入 server：
  `ScriptedHttpServer` 覆盖连接中断/挂起/SSE 中途取消/超限/redirect 循环；协议故障注入（序列 gap、
  重复 terminal、字段不匹配）在 `mira_m3_sse_test`）
- [x] `M3-19` 在受控测试账户、非用户数据和费用上限下，至少完成一个明确 Provider/dialect/model 的
  `InteropVerified`，并更新兼容性矩阵；没有凭据或网络时本项保持未完成。
  2026-09-02 使用受控 MiniMax 测试凭据、`https://api.minimaxi.com/v1`、
  `openai.responses.v1` 和 `MiniMax-M3` 完成最多六请求的真实互操作。text、同步/SSE strict schema、
  Provider-reported usage、命名 Function Tool、错误映射和协作取消达到 `InteropVerified`；Responses
  image 真实返回 5xx 并记为 `Failed`；file/upload、parallel Tool、continuation、429/Retry-After、region/ZDR
  保持 `Unknown`。逐项证据、digest、环境和补跑条件见[兼容性矩阵 §5.1](../compatibility/openai-compatible-matrix.md)。
  同日补充排查确认 image 失败与 data URL 编码、图片尺寸无关，归因 MiniMax 服务端图片管道或账号能力，
  见本文验证记录末条与矩阵同日补充记录。
- [x] `M3-20` 同步公共 API、示例、设计、安全、兼容性、供应链和验证记录，产出 Agent loop alpha
  release notes。（[发布说明](../releases/agent-loop-alpha.md)；[DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)；
  设计 §20、兼容性矩阵、平台矩阵、供应链说明与本文验证记录同步）

## 5. Executor 路由与关闭

| 工作 | Executor 能力 | Owner | 结算证据 |
| --- | --- | --- | --- |
| request构造、parse、schema/Decision校验 | `submit_auto()` | ModelGateway | 已消费 future/result |
| HTTP/SSE阻塞等待 | blocking I/O worker或批准 external-loop bridge | ProviderTransport | `WorkerHandle` terminal status |
| retry、`Retry-After`、circuit、远端清理 | timer | ProviderSupervisor | `TimerHandle` cancel/terminal |
| Task状态推进 | Runtime串行控制面 | TaskCoordinator | event sequence/state snapshot |

M3 落地：传输为 `mira-provider-io` blocking worker（`adapters/net/socket_transport.cpp`），每个
exchange 的 settlement future 由调用方有界消费；retry backoff 经 `submit_delayed_with_handle`；
闭环各阶段为有界工作单元，宿主经 `submit_auto()` 提交 `AgentLoop::run` 并消费 future。未发现
Executor 能力缺口，未新增反馈台账记录。

关闭必须停止 producer、取消并 wake transport、取消 timer、结算 worker和有限 future、提交 terminal或
uncertain事件，再由非 worker owner 关闭 Executor。若 HTTP 库的 event loop 无法满足上述边界，先登记
Executor反馈台账；不得新增裸线程或隐藏全局 loop。

## 6. 风险与阻塞

- `RISK-2026-010`：第三方“compatible”语义漂移。Owner：M3 Provider owner。缓解：固定 dialect/profile
  digest、证据有效期、fixture 和 fail-closed capability；解除条件：至少一个 profile 真实互操作通过。
  状态（2026-09-02）：M3 范围已解除。MiniMax-M3 Responses 的逐 capability 互操作揭示并修复 SSE
  官方事件名前缀差异；image 失败和所有未运行字段继续由矩阵隔离，证明不能按品牌整体推断兼容。
- `RISK-2026-011`：HTTP/TLS 库可能在 Android、SSE取消或 Executor 生命周期方面不满足要求。
  Owner：M3 transport owner。解除条件：`M3-04` 原型验证并记录依赖/线程/shutdown证据；若是 Executor
  通用能力不足，登记 `EXE-*` 后再决定兼容边界。
  状态（2026-09-02）：已解除。DEC-010 锁定 Mbed TLS `v3.6.7`；nonblocking BIO 无隐藏线程；Linux
  direct/CONNECT TLS、取消、shutdown、deadline，Windows direct/CONNECT/错误 CA runtime，以及
  Android NDK arm64 构建证据均由本地测试与 CI run `33578613423` 固化。
- `RISK-2026-012`：严格 schema 的供应商子集或首次编译延迟影响闭环。Owner：M3 model owner。
  缓解：发送前 dialect gate、schema cache telemetry、有限 repair 和预热基准。
- `RISK-2026-013`：真实 API 测试需要凭据、费用、网络和数据政策。Owner：M3 release owner。
  状态（2026-09-02）：M3 范围已解除；受控 MiniMax-M3 Responses 测试完成，credential 未入库或日志。
  image=`Failed` 与未执行能力仍保留在兼容性矩阵，不由已通过的 text/Tool 能力外推。

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

- [x] `M3-01` 至 `M3-20` 全部完成并有可复现验证记录。（2026-09-02：跨平台传输、upload fixture 与
  MiniMax-M3 Responses 分能力互操作均已回填）
- [x] 两个 dialect 的同步 mapper contract suite 通过；Responses SSE 全部状态、分片和取消用例通过。
  （`mira_m3_dialect_test`、`mira_m3_sse_test`）
- [x] 所有模型 Action 都来自完整 terminal response 并通过本地 schema、epoch、Policy 和 Planner 校验。
  （gateway 只在 terminal + 本地 schema 校验后产出 outcome；epoch admission 双重检查；闭环动作编译
  含 canonical 坐标校验）
- [x] Simulator 闭环覆盖成功、拒绝、incomplete、schema失败、429、取消、Takeover和Verify恢复。
  （Takeover 以 admission 拒绝路径覆盖：迟到的 takeover 语义 = 任务未准入，闭环停止派发新动作）
- [x] ASAN/UBSAN通过；支持环境下 TSAN通过，未支持时保留未完成项和补跑条件。
  （本地 Linux：ASAN/UBSAN/TSAN 27/27 通过；CI run
  [`33332557571`](https://github.com/Linductor-alkaid/mira/actions/runs/33332557571) 复验
  ASAN/UBSAN/TSAN、Clang Debug/Release、clang-tidy/clang-format 18.1.8、MSVC Debug/Release 与
  Android arm64 全部通过）
- [x] Shutdown 后无在途 transport、timer、future、callback或第三方 SDK worker；迟到结果不改变终态。
  （`mira_m3_transport_test` 的 shutdown 结算/入队拒绝用例；gateway admission 用例；无第三方 SDK
  worker——自研 transport 的 worker 经 `WorkerHandle` join）
- [x] 至少一个明确 Provider/dialect/model 达到 `InteropVerified`，其余服务不被误标为支持。
  （MiniMax-M3 / `https://api.minimaxi.com/v1` / `openai.responses.v1` 的 text、strict schema、Function Tool、
  同步/SSE、usage、错误和取消范围达到 `InteropVerified`；image=`Failed`，其余能力仍为 `Unknown`）
- [x] OfflineReplay 在能力图和测试中无法访问真实 Network、Tool 或 Input。
  （`ReplayModelProvider` 无网络路径；`mira_m3_replay_test` 断言回放环境无 capture/dispatch 能力）
- [x] Secret、Authorization、signed URL、敏感 prompt/截图/response 不出现在普通日志和事件。
  （`mira_m3_gateway_test` 事件负向检索；`mira_m3_transport_test` 凭证只在 socket 边界出现）
- [x] 兼容性、供应链、安全、设计、计划和 release notes 与实现同步。（本文档、
  [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)、矩阵、
  [发布说明](../releases/agent-loop-alpha.md)）

## 9. 验证记录

2026-08-30：完成 M3 设计基线、协议决策、兼容性矩阵和工作项拆分；尚无实现、构建、API 调用或
互操作证据，所有实施与退出项保持未勾选。

2026-08-31：M3 实现与本地验证（Linux x86_64，Ubuntu 24.04，GCC 13.3.0，CMake 3.28，
Executor 4fd8e60，OpenSSL 3.0.13）。交付公共头 12 个（`model_contracts/profile/digest/schema/
dialect/sse/tool/budget/supervisor/provider/transport/gateway/replay/agent_loop`）、
`src/model/` 实现 13 个编译单元与 `adapters/net/`（socket transport + 可选 OpenSSL 通道）；
测试新增 12 个 `m3_*` 目标与共享 fixture（`tests/support/m3_support.hpp`）。

命令与结果（工作树，提交前）：

- `cmake --preset debug && cmake --build --preset debug && ctest --test-dir build/debug`：
  27/27 通过（含既有 M0–M2 用例无回归）。
- `cmake --preset release && cmake --build --preset release && ctest --test-dir build/release`：
  27/27 通过。
- ASAN（`-DMIRA_ENABLE_ASAN=ON`）：27/27 通过；UBSAN：27/27 通过；
  TSAN（CI 同等 `setarch x86_64 -R ctest`）：27/27 通过。
- `format-check`（conda clang-format，LLVM 风格 + 4 空格缩进 + 100 列）、`docs-check`、
  `sbom-check`、`platform-boundary-check` 全部通过；`git diff --check` 仅提示矩阵文档头部
  markdown 硬换行的既有双空格风格（与原文件一致）。

覆盖说明（关键用例映射）：SSE 任意分片/配对/唯一 terminal/EOF/取消竞态在 `mira_m3_sse_test`；
transport 层 SSRF/allowlist/redirect 凭证剥离/取消/shutdown/上限/deadline 在
`mira_m3_transport_test`（真实 loopback socket + 脚本化故障注入 server）；TLS 真实握手与
错误 CA fail-closed 在 `mira_m3_tls_test`；epoch admission、retry/circuit/fallback、事件脱敏在
`mira_m3_gateway_test`；闭环成功/恢复/失败/最大步数/取消/admission/repair/429 在
`mira_m3_agent_loop_test`；预算与重试表驱动在 `mira_m3_supervisor_budget_test`；回放无副作用与
tombstone 在 `mira_m3_replay_test`。

历史限制与待补跑（截至 2026-08-31）：当时 `M3-04` 代理/目标 TLS、`M3-15` upload 生命周期与
`M3-19` 互操作尚未完成；其后验收见本文后续日期记录，不能把这段历史状态当作当前结论。

2026-08-31：CI 收敛记录。首推提交 `99f28c1` 的 run
[`33329362206`](https://github.com/Linductor-alkaid/mira/actions/runs/33329362206) 中 Windows
Debug/Release、Linux GCC、全部 sanitizer 通过；Linux/Clang 与 Android 因
`-Wunused-const-variable` 失败，quality 的 clang-tidy 报出 branch-clone、无效 move、optional
往返转换等 8 项。修复提交 `4e05adb`、`9f5adfa`、`65273f9`、`dd47246`、`64a9689` 依次消除
clang/clang-tidy 逐层暴露的问题（无效 move、moved-from 使用、死存储、trivially-copyable move、
noexcept 相等比较等），对应 runs `33329703785`、`33330168349`、`33330792453`、`33331083224`、
`33331850562` 均仅余 quality 一项且逐次收敛。最终修复提交 `f96d170`（connect 路径死存储）后
run [`33332557571`](https://github.com/Linductor-alkaid/mira/actions/runs/33332557571) 全部
11 个 job 通过：Linux GCC/Clang Debug+Release、Windows Debug/Release（含 Winsock 传输目标与
m3 loopback 测试）、Android arm64（含 `mira_net_transport`）、ASAN/UBSAN/TSAN 与 quality
（clang-tidy、clang-format 18.1.8、docs、SBOM、平台边界）。中间失败均由静态检查驱动，无行为
变更；据此把退出条件中的 sanitizer/Clang/MSVC 项与 `M3-04` 的跨平台构建证据回填为已验证。

2026-09-01：M3 剩余项收敛（工作树，Linux x86_64，Ubuntu 24.04，GCC 13.3.0，CMake 3.28.3，
OpenSSL 3.0.13，Mbed TLS `v3.6.7`/`068ff080b369`）。`M3-15` 完成；`M3-04` 只余目标 runner 证据；
`M3-19` 仍无受控凭据。

- 代理：`ModelProxyConfig` 纳入 profile digest；HTTP absolute-form、HTTPS CONNECT、proxy/target 独立
  DNS/SSRF/allowlist、Proxy SecretRef、跨隧道 TLS hostname verification 和 header injection fail-closed。
- TLS：新增 `mira_mbedtls_transport`，BIO callback 无线程且不拥有 socket；与 OpenSSL 参考通道共用
  direct TLS、CONNECT TLS、错误 CA contract。recursive submodule、dependency lock、Apache-2.0 选择和
  CycloneDX SBOM 已同步。
- Upload：新增 `OpenAiRemoteFileStore`，完成 multipart `user_data`、Responses `file_id`、立即/延时删除、
  Executor timer handle/future shutdown 结算、敏感 Artifact 拒绝和脱敏审计。
- Interop：新增默认 fail-closed probe；缺少 key/model/CA/request cap 时退出 2 且不联网。当前主机只发现
  proxy 环境变量，没有 API credential；未产生费用或 Provider 请求。
- 验证：Debug、Release、ASAN、UBSAN、TSAN 构建与测试均 29/29 通过（M3 13 项；TSAN 使用 CI
  同口径 `setarch x86_64 -R ctest`）；clang-tidy 18.1.3 + `-Werror` 静态分析全量构建通过；
  `format-check`、`docs-check`、`sbom-check`、`platform-boundary-check` 与 `git diff --check` 通过。
  Android NDK r26d 从
  Google/GitHub 官方 binary 端点下载均在当前代理 TLS handshake 失败；无 MSVC runner，故相应证据保持
  未完成。负责人 Mira Maintainers；补跑命令与条件见平台矩阵。

2026-09-02：PR #1 CI 修复记录（工作树，Windows x64，MSVC 19.44.35214，Windows SDK
10.0.26100.0，CMake 4.1.0，Mbed TLS `v3.6.7`/`068ff080b369`）。失败 run
[`33483539978`](https://github.com/Linductor-alkaid/mira/actions/runs/33483539978) 暴露三项跨平台问题：
Clang/Android 将 Mbed TLS 上游 C 头中的旧式转换按 Mira `-Wold-style-cast -Werror` 处理；MSVC
拒绝 interop probe 的 `getenv`；Windows CONNECT 407 测试服务器等待不存在的 body，耗尽与客户端
相同的 2 秒 deadline。修复将锁定的 Mbed TLS include 标记为 system include，probe 在 Windows 使用
`_dupenv_s` 并以 RAII 释放副本，CONNECT fixture 增加 headers-only 读取模式。

- `cmake --preset windows-debug && cmake --build --preset windows-debug`：通过；
  `ctest --test-dir build/windows-debug -C Debug --output-on-failure`：27/27 通过。
- `cmake --preset windows-release && cmake --build --preset windows-release`：通过。
- `clang-format 18.1.8 --dry-run --Werror`（受影响 `.cpp`）、`check_docs.py`、`check_sbom.py`、
  `check_platform_boundary.py` 与 `git diff --check`：通过。
- GitHub Actions run
  [`33541025071`](https://github.com/Linductor-alkaid/mira/actions/runs/33541025071)：11/11 jobs
  通过，包括 Linux GCC/Clang Debug+Release、Windows Debug/Release、Android arm64、
  ASAN/UBSAN/TSAN 与 quality；据此关闭本次 PR CI 缺陷。该 run 只证明目标构建及既有测试矩阵，
  不替代 `M3-04` 尚未完成的 Android/Windows TLS 目标运行证据或 `M3-19` 真实 Provider 互操作。
- 后续验收新增 `mira_m3_mbedtls_portable_test`，移除 Windows TLS contract 对 OpenSSL test server 的
  依赖；Linux Debug 全量 30/30（M3 14 项）与 clang-tidy 18.1.3 `--warnings-as-errors=*` 全量构建通过。
  本记录待 follow-up PR 的 Windows test job 通过后回填 run 链接并关闭 `M3-04`。
- Follow-up PR #2 首次 run
  [`33578014919`](https://github.com/Linductor-alkaid/mira/actions/runs/33578014919) 验证了 Android NDK
  构建，但 Windows CTest 因未把 `mira_mbedtls_transport.dll` 目录加入测试 `PATH` 以 `0xc0000135`
  退出；TSAN 则发现同进程 Mbed TLS client/server fixture 共享上游 PSA 全局状态。修复为 Windows
  CTest 显式注入目标 DLL 目录，并仅在 TSAN 配置禁用该双端同库 fixture；TSAN 下 Mbed TLS client
  仍由既有 OpenSSL server contract 覆盖。两项均为测试基础设施边界，不改变生产 transport 并发模型。
- 修复后 PR run
  [`33578613423`](https://github.com/Linductor-alkaid/mira/actions/runs/33578613423) 11/11 jobs 通过：
  Windows Debug/Release 各 28/28，日志明确执行 `mira_m3_mbedtls_portable_test` 并通过；Android NDK
  arm64 显式构建 `mira_mbedtls_transport`；Linux GCC/Clang Debug+Release、ASAN/UBSAN/TSAN 与 quality
  全绿。据此关闭 `M3-04`；该证据不替代仍开放的 `M3-19` 真实 Provider 互操作。

2026-09-02：MiniMax-M3 Responses 互操作验收（Linux x86_64，Ubuntu 24.04，GCC 13.3.0，Debug，
Mbed TLS `3.6.7`，branch `codex/m3-final-acceptance`，worktree based on
`d57e4cf99aee7114830f2fe833b638e469a4a0e0`）。凭据通过本地精确忽略且权限 `0600` 的 secret 文件注入；
其内容、Authorization、原始响应和账户标识均未打印或提交。请求使用公开文本、1×1 合成 PNG、无副作用
Tool schema，明确 `store=false`，完整模式最多六次请求。

- 首次真实 SSE 暴露 parser 只接受无前缀别名、拒绝官方 `response.output_item.added` 的缺陷；修复为仅对
  已知 Responses 子事件规范化 `response.` 前缀，未知事件继续 fail closed，`mira_m3_sse_test` 以官方事件名
  增加回归。同步/SSE strict schema 随后均通过。
- 最终 profile ID `8d8e365190e84a508f0908be9b53f70d`，digest
  `2a498cea2bec7f41899ff185c987eed70becad9b6347e5830636aca9b7b96a9b`；请求与 resolved model 均为
  `MiniMax-M3`，sync/stream usage 均为 `ProviderReported`。schema digest
  `79a8e4f7f22c08f2fbd0847e5e2803f3c771318088bae2c03ba20798ecdc6122`，同步 response digest
  `62bb15a259e032a6f45128b6e47215864e68a4c0d51687c4928f8e9524f65d2e`，Provider response ID 只记录摘要
  `4fc7966678127cbf13381948cff510cc32da75690faa9ec95896f891001c8d5a`。
- text、同步/SSE strict schema、命名 Function Tool、usage、无效 model 错误映射和 Executor timer 驱动的
  10ms 协作取消通过。Responses image 使用有效 1×1 PNG 返回 5xx，Mira 映射为 `ProviderOverloaded`，
  因此明确记为 `Failed`；完整 probe 退出 1 正是该 capability failure，不隐藏为绿色总布尔值。
- upload/delete、parallel Tool、continuation、429/Retry-After、region/ZDR 未运行并保持 `Unknown`；负责人
  Mira Maintainers，补跑条件为对应 capability 的独立费用/副作用授权与受控测试窗口。逐字段结论见
  [兼容性矩阵 §5.1](../compatibility/openai-compatible-matrix.md)。据此关闭 `M3-19` 和 M3，但支持声明严格
  限于已通过字段。
- 代码与文档回填后的本地门禁：Debug 全量 30/30（M3 14 项）通过；`MIRA_ENABLE_CLANG_TIDY=ON`
  全量构建通过；`format-check`、`docs-check`、`sbom-check`、`platform-boundary-check` 与
  `git diff --check` 通过。跨平台复验由本分支 PR CI 记录补充。

2026-09-02：MiniMax-M3 image 编码因素补充排查（同环境，branch `codex/m3-final-acceptance` 工作
树，凭据注入与脱敏方式同上，共 7 请求、`store=false`、全部公开合成数据与公网 fixture）。应验收
追问“排除 data URL 格式因素”，`mira_m3_interop_probe` 新增单请求诊断 case（`MIRA_INTEROP_CASE=
image|image-red|image-red-b64|image-url|image-red-chat|text-chat`）与只改写 `image_url` 编码、
打印 HTTP 状态和 4xx/5xx 错误体摘要的传输装饰器；其余 Executor/TLS/provider 栈保持真实路径。

- 基线复现：Responses + data URL + 1×1 PNG 稳定复现 HTTP 500 `system error (1033)`/
  `server_error`，排除瞬时故障。
- 图片尺寸/内容（部分降级）：当轮 64×64 红色 PNG + data URL 同样 500 (1033)；事后发现该
  fixture 存在转录缺陷（132/134 字节，损坏 IDAT），此条不再单独作为尺寸排除证据（见同日
  SiliconFlow 条目），有效 fixture 重跑待执行；下述其余证据不受影响。
- data URL 格式排除：裸 base64 得到确定性 HTTP 400 `invalid param: image url must be
  http(s):// or data:...;base64 (2013)`——MiniMax 明确要求 data URL 或 http(s) URL，Mira
  原有编码格式正确，5xx 不是格式错误。
- URL 形式排除：公网 http PNG URL 亦 500 `unknown error (1000)`。
- 方言对照：Chat Completions + data URL 红图同样 500 (1033)；Chat Completions 纯文本对照
  HTTP 200（64 token 上限时 `Incomplete`，1024 时 `Completed`，strict `json_schema` 决策未过
  本地校验，疑与 MiniMax-M3 thinking 输出或 `response_format` 支持相关，未深入，不作为能力
  声明）。
- 结论：MiniMax 官方文档声明 MiniMax-M3 原生支持图片/视频输入，但受控账号在两种方言、两种
  官方要求编码下 image 请求均为服务端 5xx。image=`Failed` 维持；归因候选为 MiniMax 服务端图片
  管道缺陷或该测试账号多模态能力未开通，需供应商侧确认，Mira 侧无需改动 wire 格式。补跑条件：
  供应商确认或能力开通后重跑诊断 case。新增潜在偏差登记：MiniMax `detail` 枚举为
  `low`/`default`/`high`（无 `auto`），Mira `ImageDetail::Auto` 默认发送 `auto`，image 通道恢复
  后需 profile 级映射修正（后续工作项跟踪）。
- 本地门禁：Debug 全量 30/30 通过；`clang-format --dry-run --Werror` 通过；docs-check、
  sbom-check、platform-boundary-check 与 `git diff --check` 通过；clang-tidy 本机不可用
  （仅有 conda clang-format 18.x），由本分支 PR CI quality job 复验。

2026-09-02：SiliconFlow `Qwen/Qwen3.5-4B` 视觉互操作（同环境，受控测试凭据，公开合成
fixture，`mira_m3_interop_probe` 单请求 case + curl 隔离实验，共 5 请求）。这是首个 image
input 达到 `InteropVerified` 的 profile，为 Mira 视觉闭环提供可用 VLM 通道；逐项证据见
[兼容性矩阵 §5.2](../compatibility/openai-compatible-matrix.md)。

- text + strict `json_schema`：200/`Completed`，本地 decision 校验通过。
- image input：64×64 纯红 PNG data URL（base64 含 `+`），模型正确返回 `{"red":true}`；
  走 Mira 真实 Executor/TLS/chat mapper 栈。`Qwen/Qwen3-VL-8B-Instruct` 交叉验证通过。
- 隔离实验：裸 `+` 可用、`%2B` 预转义被 400 拒绝（服务端不做 URL 解码）、损坏 PNG 被确定性
  400 拒绝。配置勘误：硅基流动无 `/responses` 端点，`wire_api` 必须为 chat；
  `reasoning_effort` 仅个别模型支持。
- 过程更正与 MiniMax 证据影响：初轮 image 失败（`broken PNG`）系探针红色 fixture 转录缺陷
  （132/134 字节），已程序化重嵌并复验通过；同缺陷波及上文 MiniMax 补测的 image-red 条目，
  已在该条降级标注，MiniMax 归因结论依赖的有效 1×1 基线与公网 URL 证据不受影响。
- 后续：有效 fixture 的 MiniMax image-red 重跑（1 请求）待执行；SiliconFlow 正式接入需独立
  profile 固化（含 SSE、Tool、upload 等 `Unknown` 项的补测），可作为视觉闭环短期通道。
