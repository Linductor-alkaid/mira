# M3：Model Provider 与视觉离散 Agent 闭环

> 状态：In Progress（代理与 upload/delete 已交付；`M3-04` Windows/Android 目标构建/运行证据和 `M3-19` 互操作未完成，里程碑保持打开）
> 负责人：Mira Maintainers  
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)  
> 前置：M2  
> 建议发布点：Agent loop alpha（[发布说明](../releases/agent-loop-alpha.md)）  
> 更新日期：2026-09-01

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

- [ ] `M3-04` 选择并锁定 C++ HTTP/TLS 依赖，验证桌面和目标 Android NDK 构建、TLS、代理、DNS、
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
  未完成：当前主机无 MSVC/Android NDK runner；Google/GitHub NDK binary 下载经当前代理 TLS 失败，故尚无
  新目标构建/运行证据。负责人 Mira Maintainers；补跑条件：递归 checkout 后运行 Windows CI 与 Android
  NDK 26.3 arm64 构建 `mira_mbedtls_transport`，并在至少一个目标平台执行 TLS contract test。
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
- [ ] `M3-19` 在受控测试账户、非用户数据和费用上限下，至少完成一个明确 Provider/dialect/model 的
  `InteropVerified`，并更新兼容性矩阵；没有凭据或网络时本项保持未完成。
  当前环境无受控凭据；网络只通过受限代理部分可用，不能替代账户授权。负责人 Mira Maintainers；
  补跑条件：受控测试账户 + 明确 model + 费用上限 + CA bundle + endpoint/proxy allowlist，运行默认
  fail-closed 的 `mira_m3_interop_probe`，按兼容性矩阵第 6 节补齐其余 capability 并回填。
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
  状态（2026-08-31）：fixture 层已固化（`FixtureVerified`）；互操作仍未执行，风险未解除。
- `RISK-2026-011`：HTTP/TLS 库可能在 Android、SSE取消或 Executor 生命周期方面不满足要求。
  Owner：M3 transport owner。解除条件：`M3-04` 原型验证并记录依赖/线程/shutdown证据；若是 Executor
  通用能力不足，登记 `EXE-*` 后再决定兼容边界。
  状态（2026-09-01）：DEC-010 锁定 Mbed TLS `v3.6.7`；nonblocking BIO 无隐藏线程，Linux direct/
  CONNECT TLS、取消、shutdown、deadline 证据齐备。Windows/Android 构建与运行待目标 runner，风险
  部分缓解未解除。
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

- [ ] `M3-01` 至 `M3-20` 全部完成并有可复现验证记录。（2026-09-01：`M3-15` 已关闭；仅
  `M3-04` Windows/Android 目标证据与 `M3-19` 互操作保持开放）
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
- [ ] 至少一个明确 Provider/dialect/model 达到 `InteropVerified`，其余服务不被误标为支持。
  （未执行；所有真实服务保持 `Unknown`，见 `M3-19`）
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

限制与待补跑：`M3-04` 代理子项、Windows/Android TLS 通道、`M3-15` upload 生命周期与 `M3-19`
互操作未完成（原因与补跑条件见工作项注记）。

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
- 当前主机没有 Linux Clang 或 Android NDK 26.3，故这两项不在本地标记完成。负责人 Mira
  Maintainers；补跑条件：将本修复推送至 PR #1，等待同一 workflow 的 Clang Debug/Release、Android、
  Windows Debug/Release 与 quality jobs 全部通过，并在本记录追加 run 链接。
