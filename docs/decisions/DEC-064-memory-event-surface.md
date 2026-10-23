# DEC-064：记忆面事件面补齐

> 状态：Accepted
> 日期：2026-10-23
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 记忆事件面维护轮
> 替代/被替代：无
> 关闭的缺口：设计 §20 点名的七个记忆面事件从未发出

## 背景与问题

设计 §20 列出了记忆面的事件面。supervisor 实现了其中的一部分，但**七个被点名的
事件从未发出**，而发布说明却宣称其中两个已经发出：

| 事件 | 现状 |
| --- | --- |
| `ContextBuildFinished` | 成功时落到通用回退 `ContextMemoryOperationFinished` |
| `MemoryQueryFinished` | 同上，落到通用回退 |
| `MemoryMutationRejected` | 失败时落到通用回退 `ContextMemoryOperationFailed` |
| `MemorySuperseded` | 从未发出 |
| `MemoryTombstoned` | 从未发出 |
| `MemoryIndexLagged` | 从未发出 |
| `MemoryIndexRebuilt` | 从未发出 |

三个问题叠在一起：

1. **标签映射表缺名字**：`started_event_for` / `finished_event_for` 两张表里没有上面
   七个名字，成功与失败都落到通用回退。
2. **supervisor 不知道变更类型**：`MemorySuperseded` / `MemoryTombstoned` 描述的是
   **记录的生命周期转移**（关闭一条前驱 / 退役一条记录），而 supervisor 在路由
   mutation 时只知道「一次 mutation」。
3. **向量投影没有事件面**：lag 只有两个数据面出口（查询结果的 `quality.index_lag` 与
   修复报告的 `index_lag`），没有任何事件。

发布说明与代码不一致这一点尤其要记：文档声称 `ContextBuildFinished/Finished` 已发出，
而代码里它们从不出现——这类不一致正是变更控制流程要拦住的。

## 决策

### 1. 补齐标签映射

`ContextBuildFinished` / `MemoryQueryFinished` / `MemoryMutationRejected` 进映射表：
成功的 context build 与 memory query 报自己的完成名，被拒的 mutation 报拒绝名，不再
落到通用回退。

### 2. 转移事件由变更类型命名

`MemorySuperseded` / `MemoryTombstoned` 描述记录的生命周期转移，**替换**该次 mutation
的通用完成名（`MemoryMutationApplied`）——与既有先例一致：erasure 路由报
`MemoryErased` 而不是 `MemoryMutationApplied`。

事件名由**请求的变更类型**决定（`MemoryMutation::type`）：Supersede → `MemorySuperseded`、
Tombstone → `MemoryTombstoned`、Add/Update/Noop → `MemoryMutationApplied`。失败路径
报 `MemoryMutationRejected`，与转移名互斥。

实现上，`submit()` 增加一个可选的 `outcome_event`（成功时报告的事件名，空 = 用标签的
默认名）。这是**追加式**的公开契约变更，既有调用方不受影响。

### 3. 投影事件由显式维护任务发出

`MemoryIndexLagged` / `MemoryIndexRebuilt` 描述向量投影的状态，而投影是**宿主驱动**的
可重建投影（[DEC-060](DEC-060-memory-embedding-provider.md)）。因此事件不来自存储层
（存储层不发事件，它的审计面是 `memory_erasure_log`），而来自一条**显式维护任务路由**：

```cpp
schedule_embedding_repair(indexer, memory, scope, options);  // Deferrable
```

这一路由正是设计 §17 里「Full projection rebuild | 显式 maintenance task | 可暂停、
可取消、报告进度」那行对应的入口。修复 pass 结束时：

- 投影仍落后（`index_lag > 0`）→ 发 `MemoryIndexLagged`（带落后条数）；
- 这一趟真的重建了（`repaired > 0`）→ 发 `MemoryIndexRebuilt`（带修复/失败/跳过计数）。

两个事件都由 supervisor 发出，宿主用 soft periodic timer 驱动这条路由即可。

### 4. 载荷带计数，不带正文

设计 §20 的载荷口径是「label/类别/结论与计数，不含敏感正文」。既有实现只有
label/class/outcome，**没有任何计数**。本轮让 `emit_locked` 接受可选计数并写进载荷：
索引事件带 lag / scanned / repaired / failed / ineligible。计数是数量，不是内容——
statement、向量、凭据都不进事件流。

## 备选方案

- **让存储层直接发事件**：拒绝。存储是低层组件，事件由 supervisor 统一发出；存储的
  审计面是 `memory_erasure_log`（计数与原因，无正文），这层分工是 M4 的既定选择。
- **把 `MemorySuperseded` 作为额外事件，与 `MemoryMutationApplied` 并存**：拒绝。既有
  先例是替换（erasure 报 `MemoryErased`），并存会让「mutation 应用次数」被重复计数。
- **用标签区分转移（`mutation_supersede`）**：拒绝。标签标识的是**操作**，事件名标识
  的是**事实**；supersede 仍然是一次 mutation。标签膨胀还会让按 label 过滤的消费方
  漏掉转移。
- **每次查询都发 `MemoryIndexLagged`**：拒绝。查询结果的 `quality.index_lag` 已经
  报告当前值，事件流不必承载持续的高频信号；lag 事件报告的是「发现仍然落后」。
- **把投影事件交给宿主自己发**：拒绝。事件口径是 Core 的设计契约，让每个宿主各自
  拼名字等于放弃这份契约。

## 影响与风险

- **事件名变化**：成功的 context build 与 memory query 现在报自己的完成名（此前是通用
  回退）；被拒的 mutation 报 `MemoryMutationRejected`（此前是通用失败名）；supersede 与
  tombstone 报转移名（此前是 `MemoryMutationApplied`）。依赖通用回退名字做统计的
  消费方需要复核。
- **`MemoryIndexLagged` 的观测点**：lag 在存储层产生（statement 改变即丢弃向量），而
  存储层不发事件；本轮由修复 pass 报告「仍然落后 N 条」，查询侧仍以
  `MemoryQueryResult::quality.index_lag` 报告当前值。两处口径不同：一个是发现，一个是
  当前状态。
- **公开契约追加**：`submit()` 增加一个带默认值的可选参数，既有调用方源码兼容。
- **只补记忆面**：设计 §20 里属于别的面的事件（`ContextBudgetEstimated/ExactCounted`、
  `ContextItemTrimmed/ReplacedByReference`、`ProviderCompaction*`）不在 M4 记忆实现面内，
  本轮不补。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

`tests/m4/m4_supervisor_test.cpp` 新增两个用例：

- `memory_plane_events_are_named_and_sanitized`：context build、memory query、mutation
  （成功/被拒）、supersede、tombstone 各自的事件名逐条断言；通用回退名不再出现；载荷
  不含 statement / text，只含 label / class / outcome。
- `embedding_repair_reports_the_projection_state`：一趟修好了 → `MemoryIndexRebuilt` 且
  没有 lag 事件；生产者故障 → `MemoryIndexLagged`（带 lag 计数）且没有 rebuild 事件。

**变异验证**：把命名映射改回通用回退 → 第一个用例失败（`ContextBuildFinished` 缺失）；
去掉两个投影事件 → 第二个用例失败（`MemoryIndexRebuilt` 缺失）。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-memory-event-surface`](../plans/maintenance-2026-10-memory-event-surface.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §0、§13.4、§14.4、§17、§20
- 发布说明：[stateful agent beta](../releases/stateful-agent-beta.md) §3
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 相关：[DEC-060](../decisions/DEC-060-memory-embedding-provider.md)（投影由宿主驱动）、
  [DEC-035](../decisions/DEC-035-context-curator-working-context.md)（Working Context 契约）
