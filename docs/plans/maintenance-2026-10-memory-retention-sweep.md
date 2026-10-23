# 维护计划：跨 scope 的保留期清扫（2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：[MNT-202610-35…38 claim key](maintenance-2026-10-memory-claim-key.md)（Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-23

## 1. 目标

关闭保留期机制的实现缺口：设计 §17 的 Executor 映射表写着「Retention/GC | soft periodic
timer」，但实现里只有 `IMemory::compact(scope)`——一个绑定单个 scope 的清扫。跨 scope 的
入口不存在，宿主只能回收它已经知道的 scope，而 TTL 到期、墓碑过期、版本历史增长恰恰
是不知道发生在哪里的那些。

本轮补上跨 scope 的保留期清扫入口，并修掉同一条路径上的三个缺陷：清除失败被吞掉、
部分删除被提交、清除会释放隐私删除的 hold。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/memory_contracts.hpp` / `src/context/memory_contracts.cpp`：
  - 新增 `IMemoryRetentionSweep`（存储所有者的窄接口）与 `MemorySweepOptions`。
  - `MemoryCompactionResult` 新增 `more_pending` / `scopes_swept` / `erasures_failed`。
- `include/mira/sqlite_memory_store.hpp` / `src/storage/sqlite_memory_store.cpp`：
  - `SqliteMemoryStore` 实现 `IMemoryRetentionSweep`：`sweep(options)` 跑一趟有界的跨
    scope 清扫（到期清除、版本历史裁剪、越过 `purge_horizon` 的墓碑清除）。
  - 清除失败整体回滚（savepoint）并计数；被 hold 的 scope 不参与清扫。
  - 记录级清除不再释放 hold；记录级隐私删除失败会 hold 住自己的 scope。
  - 写回的记录文档带存储自己的版本号（读回的版本是乐观并发的令牌）。
- `include/mira/context_memory_supervisor.hpp` / `src/runtime/context_memory_supervisor.cpp`：
  新增 `schedule_global_retention_sweep()`（Deferrable 类）。
- `tests/m4/m4_retention_sweep_test.cpp`：七个用例；`tests/CMakeLists.txt` 注册。
- 文档：设计文档 §0/§8.3/§16.4/§17、API 文档、`DEC-062`、本计划。

### 2.2 非目标

- 不把跨 scope 清扫并入 `IMemory`：`IMemory` 的每个方法都绑定 scope，跨 scope 清扫是
  存储所有者的操作（设计 §16.4、`DEC-062`）。
- 不改 `IMemory` 的任何公开方法签名，不改 `MemoryQuery` / `MemoryRecord` 的既有字段语义。
- 不改 Memory schema，不做迁移：清扫不新增列、不改表结构。
- 不改 `purge_horizon` 的默认语义（0 = 墓碑永不物理清除）。
- 不引入新的 Executor 能力使用方式；清扫由宿主用 soft periodic timer 驱动，路由复用
  既有 `Deferrable` 类。
- 不登记 Executor 反馈台账：本轮不涉及 Executor 能力边界。
- 不做存量的清扫脚本：清扫是运行时机制，不是迁移工具。

## 3. 设计与决策依据

- 决策记录：[DEC-062](../decisions/DEC-062-memory-retention-sweep.md)。
- 设计基线：`docs/design/context_and_memory_design.md` §8.3（`IMemory` 与隐私删除的
  fail-closed）、§13.4（MemoryMutation 语义）、§16.2（参考后端）、§16.4（本轮新增）、
  §17（Executor 映射表里的 Retention/GC 行）。
- 现状核查：`IMemory` 只有 `compact(scope)`；`SqliteMemoryStore::compact` 在一个事务里
  做三件事并全部绑定 `scope_key`；清除失败（`ErasureStatus::Pending`）既不计数也不写
  note；`erase_locked` 在任何成功清除后都会删掉该 scope 的 hold；记录级隐私删除失败时
  `held_scope` 为空，scope 不会被挡出 Context；`write_record` 把调用方的版本号写进记录
  文档，读回的版本因此是过期的。

## 4. 工作项

- [x] `MNT-202610-39` 清扫契约：`IMemoryRetentionSweep` 与 `MemorySweepOptions` 落成 Core
      公开契约；`MemoryCompactionResult` 追加 `more_pending` / `scopes_swept` /
      `erasures_failed`。
- [x] `MNT-202610-40` 存储侧清扫：`SqliteMemoryStore` 实现跨 scope 有界清扫（到期清除、
      版本裁剪、墓碑清除）；上限处停下并报告 `more_pending`；被 hold 的 scope 不参与。
- [x] `MNT-202610-41` 失败语义修正：清除失败整体回滚（savepoint）并计数写 note，不再
      提交部分删除；清除不再释放 hold；记录级隐私删除失败 hold 住自己的 scope；读回的
      版本是存储自己的版本。
- [x] `MNT-202610-42` 路由与文档：`schedule_global_retention_sweep()` 走 Deferrable 类；
      七个新用例覆盖跨 scope 回收、有界续跑、版本裁剪、墓碑 horizon、hold 排除、失败
      回滚与空 pass 拒绝；设计 §16.4、API 文档、`DEC-062` 与本计划同步。

## 5. 风险与阻塞

- **存储行为变更**（都是修正，但会影响现有部署）：清除失败不再被吞掉；失败不再提交部分
  删除；清除不再释放 hold；记录级隐私删除失败现在会挡住 scope；读回的版本号变了。依赖
  「清除会释放 hold」或「读到的版本是调用方写的那个」的下游代码需要复核。
- **被 hold 的 scope 在隐私删除重试成功前不会被回收**：其到期记录留在库里。这是刻意的
  ——隐私删除优先。
- **`more_pending` 不表示失败**：清除失败由 `erasures_failed` 与 note 表达，宿主按周期
  重跑即可（失败的记录仍然到期、仍然是下一趟的候选）。
- **清扫跑在存储的单写者通道上**：一趟有界，短于通道的操作超时；多趟收敛。
- 无 Executor 阻塞，无平台条件阻塞，无凭据或设备依赖。

## 6. 测试与退出条件

- [x] `sweep_reclaims_expired_records_across_scopes`：两个 scope 的到期记录都被回收，
      未到期的留下；第二趟无事可做。
- [x] `sweep_is_bounded_and_resumable`：`max_records = 2` 时第一趟回收两条并报告
      `more_pending`，第二趟回收最后一条。
- [x] `sweep_prunes_versions_beyond_the_keep_window`：版本历史裁剪到保留窗口，记录本身
      不受影响。
- [x] `sweep_purges_stale_tombstones_past_the_horizon`：horizon 内的墓碑保留，越过
      horizon 的清除。
- [x] `sweep_leaves_held_scopes_alone`：被 hold 的 scope 不参与清扫，其 hold 也不被释放。
- [x] `failed_retention_erasure_is_rolled_back_and_counted`：清除失败整体回滚、计入
      `erasures_failed`，scope 的活 claim 不受影响，下一趟重试。
- [x] `sweep_rejects_an_empty_pass`：`max_records = 0` 被拒绝。
- [x] 全量 `ctest --preset debug` 通过。
- [x] `architecture-check`、`check_docs`、平台边界与格式检查通过。

## 7. 验证记录

### 2026-10-23：实现、测试与文档同步完成

- **范围**：`IMemoryRetentionSweep` 与 `MemorySweepOptions` 契约、`SqliteMemoryStore` 的
  跨 scope 有界清扫、失败语义修正（回滚 / 计数 / hold 规则 / 版本读回）、
  `schedule_global_retention_sweep()`、七个新测试函数；设计文档 §0/§8.3/§16.4/§17、
  API 文档、`DEC-062` 与本计划同步。
- **依据**：[DEC-062](../decisions/DEC-062-memory-retention-sweep.md)；设计 §8.3、§13.4、
  §16.2、§16.4、§17。
- **环境**：Linux 7.0.0-38-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；14 核；
  固定 SQLite amalgamation 3.53.4。基线提交 `a8a328d`（PR #104 合并并结项后的 master）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警（改动文件 0 条） |
  | `ctest --preset debug -j 14` | **112/112 通过** |
  | `ctest --test-dir build/debug -R "m4\|m13\|m23" -j 8` | 20/20 通过 |
  | `cmake --build --preset static-analysis -j 14` | 通过；改动文件 0 条告警（余下全部来自 `third_party/kairo` 与 libstdc++ 头文件实例化） |
  | `cmake --build --preset asan -j 14` + `ctest --test-dir build/asan -R "m4\|m13\|m23" -j 8` | **20/20 通过**，无 ASan 报告 |
  | `cmake --build --preset ubsan -j 14` + `ctest --test-dir build/ubsan -R "m4\|m13\|m23" -j 8` | **20/20 通过**，无 UBSan 报告 |
  | `cmake --build --preset tsan -j 14` + `setarch x86_64 -R ctest --test-dir build/tsan -R "m4\|m13\|m23" -j 4` | **20/20 通过**，0 条 ThreadSanitizer 告警 |
  | `cmake --build build/debug --target architecture-check` | `architecture policy is clean`（8 模块，17 条既有 baseline 抑制项，0 条 stale） |
  | `python3 tools/check_docs.py .` | `Markdown links and fences: OK` |
  | `python3 tools/check_platform_boundary.py` | `Mira Core platform boundary is clean` |
  | `cmake --build build/debug --target format-check` | 通过（269 个文件） |
  | `mira_stateful_consumer` / `mira_working_context_host_consumer` / `mira_minimal_consumer` | 三个可执行消费端均正常退出（0） |

- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 仅覆盖记忆相关测试组（m4 / m13 / m23）；全量 112 项仅在 debug
    配置下执行。
  - TSAN 直接运行（未关闭 ASLR）时按本仓库既有取证模式关闭 ASLR 后执行。
  - 未在 Windows 与 Android 目标上本地构建验证：本轮改动不含平台相关代码路径。
  - 未做大 store 压测：一趟 pass 的上限（`max_records` / `max_versions`）按构造有界，
    但未测万级 scope 的实际耗时。
  - 未验证「清扫与隐私删除并发」的时序：两者跑在同一条单写者通道上，天然串行；本轮用
    「被 hold 的 scope 不参与清扫」覆盖可观测的那一半。
- **变异验证**：三个修正各还原一次，各有一个用例失败：去掉 savepoint 回滚 → 失败清除
  的记录被部分删除；去掉 hold 排除 → 被 hold 的 scope 的记录被清扫掉；让记录级清除重新
  释放 hold → 按 scope 清扫后 hold 消失、scope 回到 Context。
- **CI 上的一次既有抖动**（记录，不掩盖）：PR #105 的首个 push run 中
  `linux (clang, Debug)` 的 `mira_m22_auto_trigger_eval` 报 G2 门失败
  （`coexistence mismatches 1`）。同一份代码的下一个 run（只差一个纯文档提交）该用例
  通过——两份 run 的差异只有那一个文档提交，故判定为既有抖动而非本轮回归。依据：��用例
  的判定含一个自旋等待（最多 1e8 次 `yield` 等存储水位到达目标），在 CI 的双核 runner 上
  属于时序敏感；且该评估不链接记忆存储实现（本轮的存储改动不在其二进制里），只共用
  supervisor 与契约头文件。本轮未修改该用例。
- **同步**：`docs/design/context_and_memory_design.md`（§0、§8.3、§16.4、§17）、
  `docs/api/context-memory.md`、
  `docs/decisions/DEC-062-memory-retention-sweep.md`（新增）、本计划（新增）。
