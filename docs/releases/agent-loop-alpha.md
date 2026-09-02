# Agent loop alpha（M3）发布说明

> 日期：2026-09-02
> 里程碑：[M3 Model Provider 与视觉离散 Agent 闭环](../plans/m3-model-provider-agent-loop.md)
> 发布点：Agent loop alpha
> 状态：M3 Completed；MiniMax-M3 Responses 的 text、strict schema、Function Tool、同步/SSE、usage、
> 错误与取消范围达到 `InteropVerified`，其他 Provider 或未验证 capability 不在声明范围

## 摘要

Mira 首个可取消、可观测、可回放的 `Observe -> Reason -> Plan -> Act -> Verify` 视觉离散闭环。
本里程碑交付 canonical Model 契约、`openai.responses.v1`（同步 + SSE）与
`openai.chat-completions.v1`（同步）两个显式方言、Executor 受管 HTTP/SSE 传输、Structured
Decision 校验与有界 repair、Tool Proposal 桥接、预算/重试/circuit、Replay 扩展，以及在
Simulator 上的完整闭环。

## 能力

- **Canonical 契约**：版本化 `ModelRequest`/`ModelResponse`、output item、usage 质量、稳定
  model error domain（`mira.model`，20 个 `domain_code` 符号名）与 unknown-field/enum fail-closed
  序列化（`model_contracts.hpp`，golden/round-trip 测试）。
- **Profile 与路由**：capability manifest（证据等级、参数映射策略、上限）、profile digest；
  路由在发送前拒绝 capability/数据政策/预算不匹配（`model_profile.hpp`）。
- **传输**：`IHttpTransport` 契约 + POSIX/Winsock 实现于 Executor blocking I/O worker；
  分阶段 deadline（DNS/connect/TLS/write/first-byte/idle/total）、poll 切片取消、大小上限、
  redirect 逐跳重验证（跨源剥离凭证）、SSRF 地址策略（环回/私网/链路本地/CGNAT/元数据默认拒绝）、
  确定性 shutdown（`adapters/net/`，[DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)）。
- **TLS 与代理**：可插拔 `ITlsChannelFactory`；锁定 Mbed TLS `v3.6.7` 跨平台通道与 Linux OpenSSL
  参考通道均强制证书/主机名校验；HTTP absolute-form、HTTPS CONNECT、proxy/target 双重 SSRF 与
  独立 proxy Secret 已通过 loopback contract；未配置通道时 https fail closed。
- **方言**：Responses 同步 mapper + SSE typed-event 状态机（任意分片、add/done 配对、唯一
  terminal、remote sequence 检查、EOF 无 terminal → `AmbiguousCompletion`、有界预览隔离）；
  Chat Completions 同步 mapper，不可表示字段显式 `CapabilityMismatch`，不隐式切换 endpoint。
- **Decision 与 Tool**：strict JSON Schema 子集门禁 + 本地 validator；三 outcome（Decision/
  ToolProposals/拒绝类）；有界 schema repair（新 `ModelRequestId`，引用脱敏校验摘要）；call ID
  映射与稳定 `OperationId`、duplicate 冲突检测、hosted tool fail-closed、结果回填（大载荷替换为
  ArtifactRef）。
- **网关**：Router → 预算预留 → 重试/circuit 监督 → usage 核销 → 本地 Decision 解析；Task epoch
  admission（迟到/取消/终态响应不产生 Action）；有限 provider fallback（仅可重试非模糊失败）。
- **闭环**：`AgentLoop` 在 Simulator 上执行观察（含截图 Artifact 引用）→ 结构化 Decision → 离散
  Action（canonical 坐标）→ 重新观察 → Verify；覆盖成功、恢复（观察/模型/动作失败与 429 重试、
  schema repair）、拒绝、incomplete、最大步数、取消与 admission 拒绝。
- **回放**：`ReplayModelProvider` 服务记录态 canonical response，无网络/无输入/无 Tool 副作用；
  原始 payload 删除后以 tombstone 记录质量降级。
- **Upload/delete**：`OpenAiRemoteFileStore` 提供 multipart `/files`、Responses `file_id` 绑定、立即或
  Executor timer 延时删除、shutdown handle/future 结算及脱敏失败审计；敏感 Artifact fail closed。

## 已知限制

- **互操作范围有限**：仅 `https://api.minimaxi.com/v1`、`MiniMax-M3`、`openai.responses.v1` 的已验证
  字段可声明互操作；Responses image 实测 5xx 并标记 `Failed`，file/upload、parallel Tool、continuation、
  429/Retry-After、region/ZDR 保持 `Unknown`。详见[兼容性矩阵](../compatibility/openai-compatible-matrix.md)。
- **平台证据边界**：Windows Debug/Release 已运行 Mbed TLS direct/CONNECT/error-CA contract；Android
  NDK 26.3 arm64 构建通过，但 Android 设备/模拟器 runtime 仍属于后续平台交付，不能从构建证据外推。
- **DNS 解析不可中断**：`getaddrinfo` 阶段预算以事后测量执行，受 total deadline 兜底。
- **Chat Completions SSE**：仅当 profile 声明且独立 fixture 通过后可用（本里程碑未开启）。

## 验证入口

```sh
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure   # 当前本地基线 30/30，其中 m3 标签 14 项
```

故障注入与 golden 细节见 [M3 验证记录](../plans/m3-model-provider-agent-loop.md)。
