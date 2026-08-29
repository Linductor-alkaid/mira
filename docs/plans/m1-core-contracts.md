# M1：核心契约、持久化与安全边界

> 状态：Planned  
> 负责人：Mira Maintainers  
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)  
> 前置：M0  
> 建议发布点：Core contract alpha  
> 更新日期：2026-08-30

## 1. 目标

冻结首个可实现版本的公共类型、Runtime/Session/Task 状态语义、串行控制协议、事件与资产持久化、
安全权限和 Human Confirmation。M1 完成后，M2 以后只能以兼容扩展或显式决策迁移这些契约。

## 2. 范围与非目标

范围包括平台无关公共 API、命令接纳/结算、状态转换、EventStore/ArtifactStore、崩溃恢复、身份与
权限上下文、风险分级和确认协议。非目标包括真实 Android 输入、外部模型、OCR/ONNX 和连续控制。

## 3. 设计与决策依据

- [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)
- [事件、资产与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
- [Mira 威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
- [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md)、[DEC-002](../decisions/DEC-002-public-contract-versioning.md)、
  [DEC-003](../decisions/DEC-003-event-sourced-persistence.md)、[DEC-004](../decisions/DEC-004-security-authority-confirmation.md)

## 4. 工作项

### 4.1 公共契约与状态机

- [ ] `M1-01` 实现强类型 Runtime/Session/Task/Step/Operation/Event/Action/Observation ID，并定义
  生成、解析、比较和日志表示。
- [ ] `M1-02` 实现稳定 `Error`、`Result<T>`、`CommandReceipt`、`CommandOutcome` 和 `TaskOutcome`；
  跨公共边界不泄漏第三方异常或 Executor 类型。
- [ ] `M1-03` 实现 Runtime、Session、Task 状态表和唯一合法转换函数，终态提交幂等。
- [ ] `M1-04` 实现每 Runtime 串行控制上下文、命令序号、Task epoch、StepId 和 OperationId 的
  completion admission 校验。
- [ ] `M1-05` 实现异步命令的 accepted/rejected 与 settled 分离；Handle 可等待终态但不在
  Coordinator 内阻塞。
- [ ] `M1-06` 实现取消层级和 operation registry，证明迟到、重复、错 epoch completion 只产生
  诊断事件，不改变业务状态。

### 4.2 EventStore、ArtifactStore 与恢复

- [ ] `M1-07` 实现内存和文件参考 EventStore，支持有界 append、严格 session sequence、幂等
  event ID 和 durable watermark。
- [ ] `M1-08` 实现内容寻址 ArtifactStore、临时写入、hash 校验、原子 publish、引用 metadata 和
  retention tombstone。
- [ ] `M1-09` 实现 `ActionPrepared -> ActionDispatchStarted(durable) -> receipt` 协议；崩溃窗口
  恢复为 `ExecutionUncertain`，不自动重发。
- [ ] `M1-10` 实现尾部损坏检测、只读诊断、schema migration 和 OfflineReplay 副作用隔离。
- [ ] `M1-11` 实现 Event 与 Artifact 删除/脱敏策略，明确 Replay 在 erasure 后的降级语义。

### 4.3 安全、权限和确认

- [ ] `M1-12` 实现 `PrincipalContext`、Session authority snapshot、capability grants 和默认拒绝策略。
- [ ] `M1-13` 实现 Action/Tool 风险分类、PolicyDecision 和不可伪造的 Human Confirmation challenge。
- [ ] `M1-14` 确认绑定 action digest、Task epoch、environment epoch、目标、过期时间和单次消费；
  任一变化均使旧确认失效。
- [ ] `M1-15` 对 prompt injection、跨 tenant Memory、SSRF、路径遍历、日志泄密、Replay 副作用和
  takeover 竞态建立负向测试。
- [ ] `M1-16` 建立敏感数据分类、Redaction、retention、export 和 erasure 审计事件。

### 4.4 契约与兼容性

- [ ] `M1-17` 为公共类型、event payload 和持久化 manifest 建立 schema golden tests。
- [ ] `M1-18` 建立当前/上一兼容版本 reader 测试和 unknown-field/unknown-enum 安全处理。
- [ ] `M1-19` 更新 Runtime 总设计、示例、计划、决策和文档链接，使同一术语只有一套定义。

## 5. 风险与阻塞

- `RISK-2026-003`：若 EventStore durable append 与输入调用之间无法保持“先记录后副作用”，恢复
  无法区分未派发与不确定派发。输入路径必须以 durable ack 为硬门禁。
- `RISK-2026-004`：确认 UI 属于宿主，不属于 Core。M1 只能冻结 challenge/response 协议和校验，
  不能假设宿主展示可信；真实平台还需 M2 绑定可信宿主身份。
- `RISK-2026-005`：删除要求与历史 Replay 冲突。M1 必须验证 tombstone、脱敏占位和 cryptographic
  erasure 至少一种部署策略，而不是承诺删除后仍可完整回放 payload。

## 6. 测试与退出条件

- [ ] 所有合法/非法 Runtime、Session、Task 转换由表驱动测试覆盖。
- [ ] Command accepted 不会被误认为 settled；取消、Takeover、shutdown 可等待明确结算结果。
- [ ] 崩溃注入覆盖 Artifact publish、Event append 和副作用派发的每个边界。
- [ ] 截断或校验失败的事件尾不会静默跳过；Runtime 进入恢复或只读诊断模式。
- [ ] 确认 token 不能跨 Task、epoch、环境、目标、动作参数或到期时间复用。
- [ ] OfflineReplay 和 AnalysisReplay 在测试中无法获得真实 Input/Network/Tool capability。
- [ ] 全部 `M1-01` 至 `M1-19` 以及适用安全负向测试完成。

## 7. 验证记录

2026-08-30：完成 M1 设计基线和工作项拆分；尚无代码、构建或测试证据，所有实施项保持未勾选。

