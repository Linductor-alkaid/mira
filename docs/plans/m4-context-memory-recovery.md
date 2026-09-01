# M4：Context、Memory、Replay 与恢复

> 状态：Planned
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M3
> 建议发布点：Stateful agent beta
> 更新日期：2026-09-01

## 1. 目标

交付可在长任务、暂停、进程重启和模型切换后继续工作的有状态 Agent：以 EventStore/ArtifactStore
为事实源，以确定性 TaskCheckpoint 为恢复投影，以带 scope、来源和有效期的长期 Memory 为可选增强，
并在每次模型调用前构建有预算、可审计且不会裁掉安全约束或未决副作用的 Context。

M4 完成后，Mira 能证明恢复不会盲目重放外部副作用，Memory 不会跨 tenant 泄漏或把不可信文本提升
为系统指令；不表示已交付本地 ONNX 感知、连续控制或生产平台 Adapter。

## 2. 范围与非目标

### 2.1 范围

- `ContextLimits`、`ContextItem`、`PreparedModelContext`、token 估算和 P0–P5 分区。
- 确定性 TaskCheckpoint、持久化 CheckpointStore、投影重建和 schema migration。
- `MemoryRecord`、`MemoryMutation`、scope/ACL、TTL、Supersede、Tombstone 与 Erasure。
- SQLite/WAL 参考后端、FTS5 与可重建的有界向量索引。
- Memory consolidation、混合检索、污染防护和 Human approval。
- Provider exact token count、opaque continuation/compaction 的受限生命周期。
- Offline/Analysis Replay、崩溃恢复、删除后降级和长任务评估。

### 2.2 非目标

- 把 Memory 自动作为训练数据；训练导出属于 M5 且默认关闭。
- 以向量库、自然语言摘要或 Provider conversation ID 取代 EventStore。
- 在 realtime callback 中执行检索、embedding、摘要或持久化。
- 冻结图数据库或远程 Memory 服务为 v1 必需依赖。
- 接管 M3 未完成工作项；`M3-04`、`M3-15`、`M3-19` 必须在原计划中关闭。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M3 全部工作项和退出条件完成；真实 Provider 互操作、远端 upload/delete、HTTP(S) proxy 及
  Windows/Android TLS 仍按 M3 原编号验收，不迁移到 M4。
- SQLite、tokenization、embedding 或其他新增依赖在实现前完成版本、线程模型、许可证、SBOM 和
  目标平台可构建性审计。
- Memory 默认保留期、加密/crypto-shredding 能力和跨 Task 写入授权若需改变现有安全默认值，先更新
  决策记录和威胁模型。

### 3.2 设计与决策依据

- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)
- [事件、资产与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
- [Mira Runtime 设计](../design/mira_runtime_design.md)
- [LLM API 协议设计](../design/llm-api-protocol-design.md)
- [评估与基准体系设计](../design/evaluation_and_benchmark_design.md)
- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
- [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md)、
  [DEC-002](../decisions/DEC-002-public-contract-versioning.md)、
  [DEC-003](../decisions/DEC-003-event-sourced-persistence.md)、
  [DEC-004](../decisions/DEC-004-security-authority-confirmation.md)、
  [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)

## 4. 工作项

### 4.1 Context 安全与预算

- [ ] `M4-01` 冻结并实现版本化 Context/Checkpoint/Memory 公共契约、稳定错误和 schema golden；
  current/previous reader 对未知字段安全降级，公共头不暴露 SQLite、tokenizer 或 embedding 类型。
- [ ] `M4-02` 实现按 ModelProfile 隔离的保守 TokenCounter 与预算报告；所有水位使用 upper bound，
  exact count 失败可降级但不得记零，图片、Tool/schema 与方言开销纳入计算。
- [ ] `M4-03` 实现 P0–P5 ContextManager：固定 Task/environment epoch 和 event sequence，保留最低
  可执行集合，按水位引用/裁剪/压缩，并输出每项选入、排除和替换理由。
- [ ] `M4-04` 实现多模态与 Tool context 降载：历史大载荷转 ArtifactRef、当前视觉证据保真、
  Tool call/result 配对不破坏；最低 P0/P1/P2 超限时明确拒绝或只路由到已授权大窗口 profile。

### 4.2 Checkpoint 与恢复

- [ ] `M4-05` 实现确定性 Checkpoint reducer 和内存 CheckpointStore；checkpoint 固定 Goal、约束、
  已验证事实、未决目标及 `uncertain_side_effects`，可选模型摘要不得决定状态或副作用事实。
- [ ] `M4-06` 实现 SQLite/WAL CheckpointStore、显式 migration、单 writer 有界请求通道和只读诊断模式；
  初始化失败不隐式清库，current/previous schema 可恢复。
- [ ] `M4-07` 实现 pause、Takeover、正常 shutdown、水位触发和投影缺失时的 checkpoint 调度；周期性
  full rebuild 从 EventStore 校验投影，所有 accepted critical write 都有结算结果。
- [ ] `M4-08` 实现崩溃恢复：选择不超过 durable sequence 的最新兼容 checkpoint，重放增量事件，
  校验 Task epoch/终态；非终态只恢复到 Observing/Verifying，未决副作用先 Observe/Verify。

### 4.3 Durable Memory 与检索

- [ ] `M4-09` 实现 SQLite/WAL `IMemory` 参考后端、MemoryRecord/Mutation、幂等 mutation ID、optimistic
  version、双时态 validity、TTL、Supersede 与 Tombstone。
- [ ] `M4-10` 实现 tenant/user/session/environment scope 与 ACL 的强制过滤、敏感度和 provenance
  校验；任何相似度、导入 namespace 或模型文本都不能绕过 scope 与 authority。
- [ ] `M4-11` 实现 exact/FTS5/有界 cosine 混合检索、合并去重、多样性和 token packing；向量索引损坏
  或超时回退 exact/FTS 并报告 quality/index lag，不阻塞 Task 控制面。
- [ ] `M4-12` 实现 Verified Event 到 Memory candidate 的确定性路径，以及可选模型辅助 consolidation；
  冲突检索、policy 校验、Human approval 和高风险写入门禁在 `IMemory.apply()` 前完成。
- [ ] `M4-13` 实现 Erasure/retention：payload、FTS、embedding、索引和 Artifact 引用一致删除；部分失败
  保持 Pending 并阻止该 scope 进入 Context，审计记录不含被删除敏感正文。

### 4.4 Provider 优化、Replay 与生命周期

- [ ] `M4-14` 实现 Provider exact token count、compaction/continuation capability 和 opaque state 绑定；
  绑定 Task/Session/epoch/profile/schema/Tool snapshot/data policy/TTL，切换 Provider、取消、Takeover 或
  进程恢复后失效，并始终可由本地 checkpoint 重建。
- [ ] `M4-15` 扩展 OfflineReplay/AnalysisReplay：支持 checkpoint、Memory 与当时态双时查询，删除或缺失
  Artifact 时显式降级；Replay 无 Network/Tool/Input capability，不加载真实 Provider continuation。
- [ ] `M4-16` 实现 Context/Memory operation 的 Executor supervisor：token count、store I/O、embedding、
  consolidation、GC、重建均有 owner、容量、future/handle、取消和关闭顺序；隐私 Erasure 不得作为
  可静默放弃的后台任务。
- [ ] `M4-17` 建立长任务与 Memory benchmark harness，比较 no-memory/configured-memory、压缩前后、
  恢复前后及 Provider 切换；记录 Goal/constraint/未决副作用保真、重复副作用、质量、成本和尾延迟。
- [ ] `M4-18` 完成公共 API、示例、事件 schema、安全/兼容性/供应链文档和 Stateful agent beta 发布说明，
  将全部验证证据按环境、命令、结果和限制回填本计划。

## 5. Executor 路由与关闭

| 工作 | Executor 能力 | Owner | 背压与结算 |
| --- | --- | --- | --- |
| Context 选择、结构化压缩、本地计数 | `submit_auto()` | ContextManager | 有界输入；future 必须消费 |
| 远程 exact count/embedding | blocking I/O worker | Provider/Embedding Adapter | deadline、取消、稳定 quality |
| SQLite checkpoint/memory I/O | blocking I/O worker | StateStore owner | 单 writer、有界队列、critical write 不丢 |
| consolidation/index update | 普通或 delayed task | Memory supervisor | 可延迟/合并，lag 可观测 |
| retention/GC | soft timer | Store owner | 可取消，不冒充 realtime |
| full rebuild/migration | 显式 maintenance task | Runtime owner | 可暂停、可取消、报告进度 |

关闭顺序固定为：停止 Context/Memory producer，取消远程计数/embedding/consolidation，为可恢复活动
Task 提交最终 checkpoint，结算 critical write，停止 index/GC 并刷新 WAL，关闭 store worker，最后由
非 worker owner 关闭 Executor。若公开 Executor 能力不足，先登记 `EXE-*`，不得引入私有线程池。

## 6. 风险与阻塞

- `RISK-2026-017`：SQLite/FTS5 在 Android、Windows 的构建与线程模式可能扩大供应链和关闭风险。
  Owner：M4 storage owner。解除条件：依赖锁定、三目标构建、故障注入和 shutdown 结算证据齐备。
- `RISK-2026-018`：摘要链和 Memory consolidation 可能累积事实漂移或持久化 prompt injection。
  Owner：M4 memory owner。缓解：确定性 reducer、provenance、周期 full rebuild、authority 隔离和负向测试。
- `RISK-2026-019`：Erasure 与 Event/Artifact/索引/备份之间可能部分完成。Owner：M4 privacy owner。
  解除条件：故障注入覆盖每个删除边界，Pending scope fail closed，补偿/重试可复现。
- `RISK-2026-020`：exact tokenizer 与 Provider 计数结果随模型版本漂移。Owner：M4 model owner。
  缓解：profile digest 隔离、保守上界和误差 telemetry，不跨模型复用计数器。

当前未确认新的 Executor 能力缺口；实现期如发现缺口，按反馈台账流程登记并在本节引用。

## 7. 测试矩阵

| 维度 | 必测场景 |
| --- | --- |
| Context | P0/P1 不裁剪、最低集合超限、各水位、epoch stale、多模态/Tool 配对 |
| Checkpoint | 增量/full rebuild、current/previous schema、损坏/缺失、pause/Takeover/shutdown |
| Recovery | 每个 durable 边界崩溃、终态幂等、未决副作用只 Verify 不重发 |
| Memory | scope/ACL、TTL、双时态、幂等、optimistic conflict、Supersede/Tombstone |
| Retrieval | exact/FTS/vector、去重、deadline partial、索引损坏与重建 |
| Security | 跨 tenant、恶意网页/OCR/Tool/Memory、Secret、Human approval、Erasure Pending |
| Provider | exact count 降级、opaque state 绑定/失效、跨 Provider fallback |
| Lifecycle | rejection、取消、异常、store queue full、migration、shutdown、late completion |
| Replay/Eval | 无真实副作用、删除后降级、no-memory 对照、长任务恢复 |

## 8. 退出条件

- [ ] `M4-01` 至 `M4-18` 全部完成，并有可复现验证记录。
- [ ] 长 Task 多次 checkpoint 后 Goal、安全约束和未决副作用保持一致，崩溃恢复重复副作用为零。
- [ ] Context 最低集合、token 水位、Provider compaction 丢失和跨 Provider fallback 均有确定性结果。
- [ ] Memory scope/ACL、污染、Erasure 和跨 tenant 负向测试全部通过，敏感正文不进入普通事件。
- [ ] SQLite/FTS5 参考后端在声明支持的目标组合实际构建；未运行目标保持未完成并记录补跑条件。
- [ ] ASAN/UBSAN 和适用 TSAN 通过；Executor rejection、取消、异常、queue full 与 shutdown 均结算。
- [ ] OfflineReplay 不执行 Network、Tool 或 Input，删除/缺失数据只降低质量而不伪造事实。
- [ ] benchmark 报告包含 manifest、样本量、基线、尾延迟、成本和限制，不宣称未验证收益。
- [ ] 设计、决策、计划、安全、兼容性、供应链和发布材料与实现同步。

## 9. 验证记录

2026-09-01：依据现有总计划和 Context/Memory 设计创建详细计划；状态为 `Planned`，尚未开始实现，
未执行 M4 构建、测试、平台验证或 benchmark。负责人为 Mira Maintainers；准入条件为 M3 全部开放项
按原编号完成并留下证据。
