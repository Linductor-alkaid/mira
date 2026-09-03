# Stateful Agent Beta 发布说明（M4）

> 状态：Active
> 版本：0.1.0-beta（Stateful agent beta）
> 发布日期：2026-09-03
> 关联计划：[M4 Context、Memory、Replay 与恢复](../plans/m4-context-memory-recovery.md)

## 1. 发布概述

M4 交付 Mira 的有状态基线：以 EventStore/ArtifactStore 为事实源、SQLite/WAL 参考后端
为持久层、确定性 Checkpoint 为恢复投影、带 scope/ACL/双时态的长期 Memory 为增强，
并在每次模型调用前构建有预算、可审计、不会裁掉安全约束或未决副作用的上下文。
本发布点对应总计划 "Stateful agent beta"。

## 2. 能力清单

| 能力 | 入口 | 说明 |
| --- | --- | --- |
| Context 预算与 P0–P5 分区 | `include/mira/context_manager.hpp` | 水位、最低集合、多模态/Tool 降载（`M4-01`–`M4-04`） |
| SQLite/WAL CheckpointStore | `include/mira/state_store.hpp` | 单 writer 有界通道、显式 migration、只读诊断模式（`M4-06`） |
| 确定性 reducer 与崩溃恢复 | `include/mira/task_checkpoint.hpp`、`task_recovery.hpp` | 终态幂等、非终态只恢复到 Observing/Verifying（`M4-05`/`M4-08`） |
| 长期 Memory 参考后端 | `include/mira/sqlite_memory_store.hpp` | 幂等 mutation、optimistic version、双时态、TTL、Supersede/Tombstone（`M4-09`） |
| scope/ACL 与污染防护 | Memory 查询与 `memory_consolidation.hpp` | scope 相等性过滤、敏感度/provenance 校验、注入内容拒绝（`M4-10`/`M4-12`） |
| 混合检索 | `SqliteMemoryStore::query` | exact/FTS5/有界 cosine、去重、多样性、token packing、降级与 index lag（`M4-11`） |
| Erasure/retention | `IMemory::erase`、`compact` | 一致删除、Pending fail-closed、审计不含敏感正文（`M4-13`） |
| Provider continuation 生命周期 | `include/mira/provider_continuation.hpp` | 绑定失效矩阵、TTL、恢复清空、exact count capability 门（`M4-14`） |
| Analysis Replay | `include/mira/stateful_replay.hpp` | checkpoint+Memory+双时态视图、缺失 artifact 显式降级、无副作用能力（`M4-15`） |
| Executor supervisor | `include/mira/context_memory_supervisor.hpp` | 有界准入、协作取消、§17.2 关闭顺序、诊断事件（`M4-16`） |
| Benchmark harness | `tests/benchmark/m4_stateful_benchmark.cpp` | 长任务/检索/压缩/切换场景与保真不变量（`M4-17`） |

## 3. 事件与可观测性

supervisor 通过可选 `IEventStore` 以 Diagnostic 级别发出脱敏事件（设计
Context/Memory §20 的 M4 实现面）：`ContextBuildStarted/Finished/Failed`、
`ContextCheckpointRequested/Stored/Failed`、`MemoryQueryStarted/Finished/Degraded`、
`MemoryMutationProposed/Applied`、`MemoryErasureRequested`、`MemoryErased`、
`MemoryConsolidationStarted/Finished/Failed`、`ContextMemoryOperation*`（通用回退）。
载荷仅含 label/类别/结论与计数，不含 memory 正文或凭据。存储层自身不直接发事件；
其审计面是 `memory_erasure_log`（计数与原因，无正文）。

## 4. 已知限制

- SQLite/FTS5 参考后端在 Linux（GCC/Clang）、Windows（MSVC）与 Android arm64-v8a
  （NDK 26.3/API 24）构建通过；Android 真机运行证据仍属 M7。
- 混合检索的向量腿是线性 cosine 扫描（默认上限 1024 条）；达到设计声明的阈值后
  才引入 HNSW 类索引（设计 §14.3）。
- Model-assisted consolidation 的候选提取接口已冻结（`IConsolidationModel`），但默认
  无模型实现；当前 consolidation 全部走确定性路径。
- Replay 不加载 Provider continuation，也不执行任何 Network/Tool/Input 能力。

## 5. 升级与兼容

- 新增直接依赖 SQLite `3.53.4`（amalgamation vendored，Public Domain）；锁定与审计见
  [供应链文档](../supply-chain/direct-dependencies.md)。
- `ProviderContinuation` 增加 `provider`/`conversation`/`profile_digest`/`session_id`/
  `environment_epoch` 绑定字段；旧 JSON 载荷缺省解码为"未声明"，按兼容规则不触发
  失效（设计 §15.2，DEC-002 兼容语义）。
- JSON 解析修复：19 位整数（纳秒时间戳）此前落入 double 路径损失精度并丢失
  Integer 语义；现在按 int64 精确解析（`src/support/json.cpp`）。
- `MemoryRecord` 增加 `version` 字段（读侧由后端填充）；序列化为新增成员，旧读取方
  按未知字段忽略。

## 6. 相关文档

- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)
- [M4 长任务与 Memory Benchmark](../benchmarks/long-task-memory.md)
- [平台构建矩阵](../compatibility/platform-matrix.md)
- [M4 计划与验证记录](../plans/m4-context-memory-recovery.md)
