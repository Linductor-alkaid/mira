# M4：Context、Memory、Replay 与恢复

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M3
> 建议发布点：Stateful agent beta
> 更新日期：2026-09-03

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

- [x] `M4-01` 冻结并实现版本化 Context/Checkpoint/Memory 公共契约、稳定错误和 schema golden；
  current/previous reader 对未知字段安全降级，公共头不暴露 SQLite、tokenizer 或 embedding 类型。
- [x] `M4-02` 实现按 ModelProfile 隔离的保守 TokenCounter 与预算报告；所有水位使用 upper bound，
  exact count 失败可降级但不得记零，图片、Tool/schema 与方言开销纳入计算。
- [x] `M4-03` 实现 P0–P5 ContextManager：固定 Task/environment epoch 和 event sequence，保留最低
  可执行集合，按水位引用/裁剪/压缩，并输出每项选入、排除和替换理由。
- [x] `M4-04` 实现多模态与 Tool context 降载：历史大载荷转 ArtifactRef、当前视觉证据保真、
  Tool call/result 配对不破坏；最低 P0/P1/P2 超限时明确拒绝或只路由到已授权大窗口 profile。

### 4.2 Checkpoint 与恢复

- [x] `M4-05` 实现确定性 Checkpoint reducer 和内存 CheckpointStore；checkpoint 固定 Goal、约束、
  已验证事实、未决目标及 `uncertain_side_effects`，可选模型摘要不得决定状态或副作用事实。
- [x] `M4-06` 实现 SQLite/WAL CheckpointStore、显式 migration、单 writer 有界请求通道和只读诊断模式；
  初始化失败不隐式清库，current/previous schema 可恢复。
- [x] `M4-07` 实现 pause、Takeover、正常 shutdown、水位触发和投影缺失时的 checkpoint 调度；周期性
  full rebuild 从 EventStore 校验投影，所有 accepted critical write 都有结算结果。
- [x] `M4-08` 实现崩溃恢复：选择不超过 durable sequence 的最新兼容 checkpoint，重放增量事件，
  校验 Task epoch/终态；非终态只恢复到 Observing/Verifying，未决副作用先 Observe/Verify。

### 4.3 Durable Memory 与检索

- [x] `M4-09` 实现 SQLite/WAL `IMemory` 参考后端、MemoryRecord/Mutation、幂等 mutation ID、optimistic
  version、双时态 validity、TTL、Supersede 与 Tombstone。
- [x] `M4-10` 实现 tenant/user/session/environment scope 与 ACL 的强制过滤、敏感度和 provenance
  校验；任何相似度、导入 namespace 或模型文本都不能绕过 scope 与 authority。
- [x] `M4-11` 实现 exact/FTS5/有界 cosine 混合检索、合并去重、多样性和 token packing；向量索引损坏
  或超时回退 exact/FTS 并报告 quality/index lag，不阻塞 Task 控制面。
- [x] `M4-12` 实现 Verified Event 到 Memory candidate 的确定性路径，以及可选模型辅助 consolidation；
  冲突检索、policy 校验、Human approval 和高风险写入门禁在 `IMemory.apply()` 前完成。
- [x] `M4-13` 实现 Erasure/retention：payload、FTS、embedding、索引和 Artifact 引用一致删除；部分失败
  保持 Pending 并阻止该 scope 进入 Context，审计记录不含被删除敏感正文。

### 4.4 Provider 优化、Replay 与生命周期

- [x] `M4-14` 实现 Provider exact token count、compaction/continuation capability 和 opaque state 绑定；
  绑定 Task/Session/epoch/profile/schema/Tool snapshot/data policy/TTL，切换 Provider、取消、Takeover 或
  进程恢复后失效，并始终可由本地 checkpoint 重建。
- [x] `M4-15` 扩展 OfflineReplay/AnalysisReplay：支持 checkpoint、Memory 与当时态双时查询，删除或缺失
  Artifact 时显式降级；Replay 无 Network/Tool/Input capability，不加载真实 Provider continuation。
- [x] `M4-16` 实现 Context/Memory operation 的 Executor supervisor：token count、store I/O、embedding、
  consolidation、GC、重建均有 owner、容量、future/handle、取消和关闭顺序；隐私 Erasure 不得作为
  可静默放弃的后台任务。
- [x] `M4-17` 建立长任务与 Memory benchmark harness，比较 no-memory/configured-memory、压缩前后、
  恢复前后及 Provider 切换；记录 Goal/constraint/未决副作用保真、重复副作用、质量、成本和尾延迟。
- [x] `M4-18` 完成公共 API、示例、事件 schema、安全/兼容性/供应链文档和 Stateful agent beta 发布说明，
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

- [x] `M4-01` 至 `M4-18` 全部完成，并有可复现验证记录。
- [x] 长 Task 多次 checkpoint 后 Goal、安全约束和未决副作用保持一致，崩溃恢复重复副作用为零。
- [x] Context 最低集合、token 水位、Provider compaction 丢失和跨 Provider fallback 均有确定性结果。
- [x] Memory scope/ACL、污染、Erasure 和跨 tenant 负向测试全部通过，敏感正文不进入普通事件。
- [x] SQLite/FTS5 参考后端在声明支持的目标组合实际构建；未运行目标保持未完成并记录补跑条件。
- [x] ASAN/UBSAN 和适用 TSAN 通过；Executor rejection、取消、异常、queue full 与 shutdown 均结算。
- [x] OfflineReplay 不执行 Network、Tool 或 Input，删除/缺失数据只降低质量而不伪造事实。
- [x] benchmark 报告包含 manifest、样本量、基线、尾延迟、成本和限制，不宣称未验证收益。
- [x] 设计、决策、计划、安全、兼容性、供应链和发布材料与实现同步。

## 9. 验证记录

2026-09-01：依据现有总计划和 Context/Memory 设计创建详细计划；状态为 `Planned`，尚未开始实现，
未执行 M4 构建、测试、平台验证或 benchmark。负责人为 Mira Maintainers；准入条件为 M3 全部开放项
按原编号完成并留下证据。

2026-09-03：交付 M4 第一增量（Context Safety 与恢复核心，对应设计 CM0 阶段加 `M4-08`）。
状态改为 `In Progress`。

- 范围：`M4-01` 至 `M4-05`、`M4-07`、`M4-08` 完成；`M4-06` 及 4.3/4.4 节全部工作项未开始。
  新增公共契约 `include/mira/context_contracts.hpp`、`context_manager.hpp`、`task_checkpoint.hpp`、
  `task_recovery.hpp`、`memory_contracts.hpp`（冻结 Memory 契约；持久后端属 `M4-09`+），实现位于
  `src/context/`、`src/storage/checkpoint_store.cpp`、`src/runtime/task_recovery.cpp`。
- 关键语义：P0/P1/最低 P2 构成不可裁剪最低集合，超限时按 `MinimumSetTooLarge` 拒绝或仅路由到请求中
  显式授权的大窗口 profile；水位动作与逐项审计（选入/排除/替换/压缩及稳定 reason code）确定性输出，
  相同请求产生相同 selection digest；Tool call/result 配对原子决策，未消费 result 进入最低集合；
  item epoch 与请求边界不一致时按 `StaleBuild` 拒绝。Checkpoint reducer 消费 `Task*` JSON 事件与
  ActionJournal/AgentLoop 既有事件（`ActionDispatchStarted`、`ActionReceipt`、
  `ActionExecutionUncertain`、`ActionDispatched`、`VerificationResult`、`LoopSettled`），
  增量与 full rebuild 投影 digest 一致；恢复只选择 `through_event_sequence <= durable sequence`
  的 checkpoint，终态幂等（`AlreadyTerminal`），非终态仅恢复到 Observing/Verifying，
  未决副作用保持 pinned 并要求先 Observe/Verify。Executor 承载：本增量组件为同步纯函数，
  由宿主在 Executor 任务内调用（`m4_recovery_test.cpp` 以 `submit_auto()` + 消费 future 验证）；
  专项 supervisor 属 `M4-16`。
- 验证环境：Ubuntu 24.04，x86_64，g++ 13.3.0，CMake 3.28.3，ninja 1.11.2，clang-format 18.1.8；
  本机无 clang/clang-tidy/Windows/Android 工具链，由 CI 补跑。
- 本地验证：`cmake --build build/<release|debug|asan|ubsan|tsan>` 全部 0 error；release/debug/asan/ubsan
  ctest 34/34 通过，tsan（`setarch x86_64 -R`）33/33 通过（mbedtls portable 测试按设计禁用）；
  `clang-format --dry-run --Werror`、`check_docs.py`、`check_sbom.py`、`check_platform_boundary.py`
  均通过。
- 限制：`M4-06` SQLite/WAL 后端、混合检索、consolidation、Erasure、Provider continuation、Replay
  扩展与 benchmark（`M4-09` 至 `M4-18` 中未勾选项）尚未实现，不因本增量声明任何能力；本机未运行
  clang 编译器与 clang-tidy，CI quality/matrix 结果以 GitHub Actions 为准。
- 同步：本文件工作项与状态已更新；总计划 M4 状态改为 `In Progress`。设计文档无需变更（实现按
  `context_and_memory_design.md` CM0 语义落地）。


2026-09-03：交付 M4 第二增量（CM1/CM2 主体 + Provider 生命周期 + supervisor + benchmark，
即 `M4-06`、`M4-09` 至 `M4-18`）。M4 全部工作项实现完毕。

- 范围：`mira_state_store` 新目标（SQLite 3.53.4 amalgamation vendored）承载
  `SqliteCheckpointStore`（M4-06）与 `SqliteMemoryStore`（M4-09..M4-13）；mira_core 新增
  `memory_consolidation`（M4-12）、`provider_continuation`（M4-14）、`stateful_replay`
  （M4-15）、`context_memory_supervisor`（M4-16）；benchmark harness 与示例（M4-17/18）。
  Executor 路由按 §5 表格落地：SQLite I/O 走专属 blocking-I/O worker（单 writer、有界
  请求通道、WAL），Context/retrieval/consolidation/GC 走 supervisor 的 `submit_auto`
  分类任务（Critical/Interactive/Deferrable，§17.2 关闭顺序）。未发现 Executor 能力缺口，
  未新增 `EXE-*` 台账记录。
- 关键语义：store 初始化/迁移显式发生且失败不清库，更新版本文件以只读诊断模式打开；
  查询 scope 相等性在 SQL 强制，FTS 命中不越权，排序腿无命中返回空而非全量兜底；
  向量腿维度失配/损坏按 index lag 降级不阻塞；Supersede 在原 id 上闭合前驱区间，
  双时态 as-of 按"闭合区间链"解析；Erasure 部分失败整体回滚并以独立事务落盘 scope
  hold（fail-closed），审计仅含 id/计数/原因；continuation 绑定矩阵覆盖
  provider/profile/conversation/task/session/epoch/schema/policy/TTL，恢复/接管清空缓存；
  `ProviderContinuation` 新增字段对旧 JSON 缺省解码为"未声明"（DEC-002 兼容语义）。
  附带修复 `parse_json` 19 位整数落入 double 路径的精度缺陷（纳秒时间戳往返）。
- 验证环境：Ubuntu 24.04，x86_64，g++ 13.3.0，CMake 3.28.3，ninja 1.11.2，
  clang-format 18.1.8；本机无 clang/clang-tidy/Windows/Android 工具链，由 CI 补跑。
- 本地验证（debug 构建全量 43/43：41 contract/unit + benchmark + stateful example）：
  新增 `mira_m4_sqlite_checkpoint_test`（创建/迁移/只读诊断/垃圾文件不擦除/有界队列
  拒绝/并发单 writer/每任务上限）、`mira_m4_sqlite_memory_test`（幂等/版本冲突/
  Supersede 链与双时态/TTL/Tombstone/跨 tenant 与敏感度与导入 provenance 负向/
  Erasure Pending fail-closed 与重试/重开）、`mira_m4_memory_retrieval_test`
  （exact+FTS+vector 合并、FTS 操作符注入负向、降级与 lag、去重/多样性/packing、
  deadline partial）、`mira_m4_consolidation_test`（确定性候选/Supersede/重复 no-op、
  禁止内容与注入拒绝、Preference 人工审批门）、`mira_m4_continuation_test`
  （绑定失效矩阵、TTL、恢复清空、exact count capability 门与降级不记零）、
  `mira_m4_stateful_replay_test`（checkpoint+Memory+双时态视图、缺失 artifact 显式
  降级、无副作用能力）、`mira_m4_supervisor_test`（拒绝/容量/取消/异常隔离/Erasure
  结算/诊断事件）、`mira_m4_stateful_benchmark`（保真不变量断言 + JSON manifest）、
  `mira_stateful_consumer_test`（示例）。release/asan/ubsan/tsan 构建与 ctest 通过
  （tsan 以 `setarch x86_64 -R` 运行）；`clang-format --dry-run --Werror`、
  `check_docs.py`、`check_sbom.py`、`check_platform_boundary.py` 通过。
- Benchmark：[docs/benchmarks/long-task-memory.md](../benchmarks/long-task-memory.md)
  记录 manifest、基线、分位数与限制（恢复保真失败 0、重复副作用 0、no-memory 0/60 vs
  configured 60/60、压缩 token 节省约 98% 且 P0/P1 保留、Provider 切换回退本地重建）。
- 文档同步：设计文档新增 §0 实现状态注记（版本 0.4）；威胁模型 §12 补 M4 后端证据；
  供应链新增 SQLite 条目（vendored、archive+逐文件 SHA-256 锁定、`check_sbom.py` 扩展
  vendored 校验）；平台矩阵 0.5 新增 `mira_state_store` 行（Android 为 Build verified，
  真机运行属 M7）；发布说明 [docs/releases/stateful-agent-beta.md](../releases/stateful-agent-beta.md)。
- 限制与补跑：Windows（MSVC Debug/Release）与 Android arm64（NDK 26.3/API 24，构建
  `mira_state_store`/`mira_stateful_consumer`）以本 PR 的 GitHub Actions pipeline 为准，
  结果回填下一条记录；clang 编译器与 clang-tidy 由 CI quality job 补跑。Android 真机
  SQLite 运行、真实 Provider exact count 网络路径与 HNSW 向量索引不在 M4 声明范围。

2026-09-03：CI 全绿，M4 关闭。PR
[#4](https://github.com/Linductor-alkaid/mira/pull/4)（分支 `codex/m4-durable-memory-completion`）的
push pipeline run [`33792131779`](https://github.com/Linductor-alkaid/mira/actions/runs/33792131779)
全部 job 成功：Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android arm64-v8a
（NDK 26.3.11579264/API 24，含 `mira_state_store` 与 `mira_stateful_consumer` 构建）、ASAN/UBSAN/TSAN
（41-43 tests 全过，mbedtls portable 按 CI 配置禁用）与 quality（clang-tidy + clang-format 18.1.8 +
docs/sbom/platform-boundary 检查）。

迭代中修复并回归的跨平台问题（全部有测试或 CI 证据）：Android NDK libc++ 缺 `<stop_token>`（改用
可移植 `SupervisorToken`）；Windows checkout 换行破坏 vendored 摘要（`.gitattributes -text`）；
`std::function` 堆分配路径的 clang-analyzer 误报（store 通道改为模板直传 callable）；supervisor
shutdown 取消路径不 resolve future 的死锁（本地 30 次压测复现、80 次验证修复）；Windows 下清理
SQLite 侧文件抛 `filesystem_error` 触发 0xC0000409（error_code 清理 + 顶层异常屏障）；以及
`parse_json` 19 位整数精度缺陷（见上一条记录）。M4 退出条件全部满足，状态改为 `Completed`；
Stateful agent beta 发布说明见 [docs/releases/stateful-agent-beta.md](../releases/stateful-agent-beta.md)。

2026-09-05：修复 `BUG-20260905-001`（安装包导出缺口；DEC-011 外部消费验证的首个发现）。

- 范围：`mira_state_store` 与 vendored `sqlite3` 进入 `MiraTargets` 安装导出集，补
  `EXPORT_NAME state_store` 与 `MiraConfig.cmake.in` 的 `find_dependency(Threads)`；
  `mira_installed_consumer_test` 扩展为链接 `Mira::state_store` 并实际打开/关闭
  `SqliteCheckpointStore` 与 `SqliteMemoryStore`。此前安装包随 `include/` 暴露
  `state_store.hpp`/`sqlite_memory_store.hpp` 头文件，却没有可链接的导出目标，外部消费者
  无法使用 M4 持久化后端。
- 依据：[DEC-011](../decisions/DEC-011-demo-first-external-validation.md) 第 3 条（demo 只经
  `find_package(Mira)` 安装接口消费 mira）。
- 验证：Ubuntu 24.04.4 LTS，x86_64，g++ 13.3.0，CMake 3.28.3，Ninja 1.13.2，clang-format 18.1.8；
  debug 与 release `ctest` 43/43 通过（含扩展后的 `mira_installed_consumer_test`：install ->
  `find_package(Mira)` -> 链接 `Mira::state_store` -> 打开/关闭双 store -> Executor 干净
  shutdown）；`clang-format --dry-run --Werror`、`check_docs.py`、`check_sbom.py`、
  `check_platform_boundary.py` 通过。
- CI 补跑（PR [#5](https://github.com/Linductor-alkaid/mira/pull/5)，push pipeline runs
  [`33902440606`](https://github.com/Linductor-alkaid/mira/actions/runs/33902440606) 与
  [`33902459808`](https://github.com/Linductor-alkaid/mira/actions/runs/33902459808) 全部 job
  成功）：Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）与 ASAN/UBSAN/TSAN
  的 `ctest` 通过，含扩展后的 `mira_installed_consumer_test`（安装 → `find_package(Mira)` →
  链接 `Mira::state_store` → 运行）；Android arm64-v8a（NDK 26.3.11579264/API 24）完成含
  `mira_state_store` 与 `mira_stateful_consumer` 的目标构建（构建验证；安装消费运行需目标
  设备，其交付随 M7 重定义处理）；quality job（clang-tidy、clang-format 18.1.8、docs/sbom/
  platform-boundary 检查）通过。本机未运行的 clang 编译器与 clang-tidy 由此补齐。
- 同步：DEC-011 新增；总计划 v1 边界、里程碑状态与决策索引更新；M5/M6 置 `Cancelled`、
  M7 置 `Blocked` 并附验证记录。
