# 维护计划：记忆面事件面补齐（2026-10）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：[MNT-202610-43…44 M22 评估确定性](maintenance-2026-10-m22-eval-determinism.md)（Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-23

## 1. 目标

关闭设计 §20 事件面在记忆面的最后一个缺口：**七个被设计点名的事件从未发出**。

设计 §20 列出的记忆面事件里，supervisor 只发出了其中一部分；其余七个要么名字在
文档里写着、代码里从不出现，要么文档与代码对不上：

| 事件 | 现状 |
| --- | --- |
| `ContextBuildFinished` | 成功时落到通用回退 `ContextMemoryOperationFinished`——发布说明却宣称已发出 |
| `MemoryQueryFinished` | 同上，落到通用回退 |
| `MemoryMutationRejected` | 失败时落到通用回退 `ContextMemoryOperationFailed` |
| `MemorySuperseded` | 从未发出：supervisor 只知道「一次 mutation」，不知道它关闭了前驱 |
| `MemoryTombstoned` | 从未发出：同上 |
| `MemoryIndexLagged` | 从未发出：向量投影的 lag 只存在于查询结果与修复报告里 |
| `MemoryIndexRebuilt` | 从未发出：重建过程没有事件面 |

## 2. 范围与非目标

### 2.1 范围

- `src/runtime/context_memory_supervisor.cpp` / `include/mira/context_memory_supervisor.hpp`：
  - 补齐三个标签映射（`ContextBuildFinished` / `MemoryQueryFinished` /
    `MemoryMutationRejected`）。
  - 成功的 mutation 按**结果**里的变更类型发出转移事件：`MemorySuperseded` /
    `MemoryTombstoned`。
  - 新增 `schedule_embedding_repair()`：一条显式维护任务路由（Deferrable），跑向量
    投影的修复 pass，并发出 `MemoryIndexLagged` / `MemoryIndexRebuilt`。
  - 事件载荷支持**计数**（设计 §20：「载荷仅含 label/类别/结论与计数」）。
- `tests/m4/m4_supervisor_test.cpp`：事件名字与载荷的断言。
- 文档：设计 §20、发布说明的 §3、API 文档、`DEC-064`、本计划。

### 2.2 非目标

- 不补设计 §20 里属于别的面的事件（`ContextBudgetEstimated/ExactCounted`、
  `ContextItemTrimmed/ReplacedByReference`、`ProviderCompaction*`）：它们属于 Context
  预算面与 Provider 面，不在 M4 记忆实现面内（见发布说明 §3 的口径）。
- 不改存储层的事件行为：存储层仍然不直接发事件（它的审计面是 `memory_erasure_log`），
  事件仍由 supervisor 统一发出。
- 不改任何公开方法签名以外的契约；`MemoryQueryResult::quality.index_lag` 之类的既有
  数据面不动。
- 不引入新的 Executor 能力使用方式；修复 pass 走既有 Deferrable 路由。
- 不登记 Executor 反馈台账：本轮不涉及 Executor 能力边界。

## 3. 设计与决策依据

- 决策记录：[DEC-064](../decisions/DEC-064-memory-event-surface.md)。
- 设计基线：`docs/design/context_and_memory_design.md` §20（事件面与载荷口径）、
  §17（Executor 映射：Retention/GC 走 soft periodic timer、Full projection rebuild 走
  显式 maintenance task）、§13.4（Supersede / Tombstone 语义）。
- 现状核查：`started_event_for` / `finished_event_for` 两个标签映射表里没有上面七个
  名字；`MemorySuperseded` / `MemoryTombstoned` 需要 mutation 的**变更类型**，而
  supervisor 目前只知道「一次 mutation」；向量投影的 lag 只有两个数据面出口（查询结果的
  `quality.index_lag` 与修复报告的 `index_lag`），没有事件面。

## 4. 工作项

- [x] `MNT-202610-45` 标签映射补齐：`ContextBuildFinished` / `MemoryQueryFinished` /
      `MemoryMutationRejected` 三个名字进映射表，成功与失败不再落到通用回退。
- [x] `MNT-202610-46` 转移事件：成功的 mutation 按 `MemoryMutationResult::applied` 的
      变更类型发出 `MemorySuperseded` / `MemoryTombstoned`；其余仍是
      `MemoryMutationApplied`。发出方仍是 supervisor（存储层不发事件）。
- [x] `MNT-202610-47` 投影事件：新增 `schedule_embedding_repair()` 显式维护任务路由，
      修复 pass 观测到 lag 时发 `MemoryIndexLagged`、真的重建了时发
      `MemoryIndexRebuilt`，载荷带计数。
- [x] `MNT-202610-48` 载荷计数与测试文档：事件载荷支持计数（§20 的载荷口径）；事件
      名字与载荷在 supervisor 测试里逐条断言；设计 §20、发布说明 §3、API 文档、
      `DEC-064` 与本计划同步。

## 5. 风险与阻塞

- **事件名变化会影响下游消费方**：`ContextBuildFinished` 与 `MemoryQueryFinished` 之前
  从不出现（落到通用回退），现在出现；`MemoryMutationApplied` 在 supersede / tombstone
  上不再出现（改为转移事件名）。这正是设计 §20 要的口径，但依赖通用回退名字做统计的
  消费方需要复核。
- **`MemoryIndexLagged` 的观测点**：lag 在存储层产生（statement 改变即丢弃向量），而
  存储层不发事件；本轮由修复 pass 发出「观测到 lag」，查询侧仍以
  `MemoryQueryResult::quality.index_lag` 报告当前值。两处口径不同：一个是发现，一个是
  当前状态。
- **载荷计数是新增字段**：既有事件类型不变，载荷多出计数字段；只报告计数，不含
  memory 正文或凭据（§20 的载荷口径）。
- 无 Executor 阻塞，无平台条件阻塞，无凭据或设备依赖。

## 6. 测试与退出条件

- [x] supervisor 测试逐条断言七个事件名字：context build 的 Started/Finished、memory
      query 的 Started/Finished、mutation 的 Proposed/Applied、失败的 mutation 的
      Rejected、supersede 的 Superseded、tombstone 的 Tombstoned、修复 pass 的
      IndexLagged / IndexRebuilt。
- [x] 载荷断言：计数在载荷里、正文不在载荷里。
- [x] 全量 `ctest --preset debug` 通过；ASAN / UBSAN / TSAN 通过。
- [x] `architecture-check`、`check_docs`、平台边界与格式检查通过。

## 7. 验证记录

### 2026-10-23：事件面补齐

- **范围**：三个标签映射、两个转移事件、一条投影维护路由、载荷计数、测试与文档。
- **依据**：[DEC-064](../decisions/DEC-064-memory-event-surface.md)；设计 §17、§20、
  §13.4；发布说明 §3。
- **环境**：Linux 7.0.0-38-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；14 核；
  固定 SQLite amalgamation 3.53.4。基线提交 `4029c37`（PR #106 合并并结项后的
  master）；本轮经 [PR #107](https://github.com/Linductor-alkaid/mira/pull/107) 提交，
  合并提交 `62ae5cb`（squash）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警（改动文件 0 条） |
  | `ctest --preset debug -j 14` | **112/112 通过** |
  | `ctest --test-dir build/debug -R "m4" -j 8` | 14/14 通过 |
  | `cmake --build --preset static-analysis -j 14` | 通过；改动文件 0 条告警（余下全部来自 `third_party/kairo` 与 libstdc++ 头文件实例化） |
  | `cmake --build --preset asan -j 14` + `ctest --test-dir build/asan -R "m4\|m13\|m23" -j 8` | **20/20 通过**，无 ASan 报告 |
  | `cmake --build --preset ubsan -j 14` + `ctest --test-dir build/ubsan -R "m4\|m13\|m23" -j 8` | **20/20 通过**，无 UBSan 报告 |
  | `cmake --build --preset tsan -j 14` + `setarch x86_64 -R ctest --test-dir build/tsan -R "m4\|m13\|m23" -j 4` | **20/20 通过**，0 条 ThreadSanitizer 告警 |
  | `cmake --build build/debug --target architecture-check` | `architecture policy is clean`（8 模块，17 条既有 baseline 抑制项，0 条 stale） |
  | `python3 tools/check_docs.py .` | `Markdown links and fences: OK` |
  | `python3 tools/check_platform_boundary.py` | `Mira Core platform boundary is clean` |
  | `cmake --build build/debug --target format-check` | 通过（269 个文件） |
  | `mira_stateful_consumer` / `mira_working_context_host_consumer` / `mira_minimal_consumer` | 三个可执行消费端均正常退出（0） |
  | `gh pr checks 107` | **24/24 通过**（push run 39964261408 与 pull_request run 39965625979 各 12 项） |
  | `git diff 639269c origin/master` | 空——合并进 master 的树就是 CI 验证过的树 |
  | 合并后在 master 上复验 | `ctest --preset debug -j 14` **112/112 通过** |

- **变异验证**：
  - 把命名映射改回通用回退（`ContextBuildFinished` / `MemoryQueryFinished` /
    `MemoryMutationRejected` 落回通用名、转移名失效）→ `memory_plane_events_are_named_and_sanitized`
    失败（`ContextBuildFinished` 缺失）。
  - 去掉两个投影事件 → `embedding_repair_reports_the_projection_state` 失败
    （`MemoryIndexRebuilt` 缺失）。
- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 仅覆盖记忆相关测试组；全量项仅在 debug 配置执行。
  - 未在 Windows 与 Android 目标上本地构建验证：本轮改动不含平台相关代码路径。
  - 事件名的下游消费方复核不在本仓库范围内：口径变化已写进发布说明与决策记录。
- **同步**：`docs/design/context_and_memory_design.md`（§0、§17、§20）、
  `docs/releases/stateful-agent-beta.md`（§3 事件面）、
  `docs/api/context-memory.md`、
  `docs/decisions/DEC-064-memory-event-surface.md`（新增）、本计划（新增）。
