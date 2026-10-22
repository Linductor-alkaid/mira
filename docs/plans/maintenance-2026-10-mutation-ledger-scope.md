# 维护计划：幂等台账 scope 绑定与 Memory schema 1.1（2026-10）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：[MNT-202610-19…22 记忆读取可见性](maintenance-2026-10-memory-read-visibility.md)（Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-22

## 1. 目标

关闭 [DEC-057](../decisions/DEC-057-scoped-memory-read-and-apply-check-order.md)
记录在案的残余风险：`memory_mutations` 幂等台账按 `mutation_id` 单键索引，重放查询不
受调用方 scope 约束，持有他人 scope 内 `mutation_id` 的调用方可以读到该 scope 的
`record_id` 与版本号。

本轮把台账归属 scope 固化进条目本身（Memory schema 1.1），使重放只在其归属 scope 内
被承认，并补齐 1.0 → 1.1 的显式迁移与版本比较的 minor 维度。

## 2. 范围与非目标

### 2.1 范围

- `src/storage/sqlite_memory_store.cpp`：
  - `memory_mutations` 增加 `scope_key` 列；台账写入带上 scope。
  - 重放命中时比较条目 scope 与 mutation 声明的 scope，不一致（含归属未知）返回
    `ScopeDenied`；不回落到正常写入路径。
  - Memory schema 提升到 1.1；`open()` 在版本较旧时先执行 1.0 → 1.1 迁移步骤再写
    版本戳：加列 → 从 `memory_records` 回填 → 从 `memory_erasure_log` 回填。
  - 版本比较补齐 minor：更新的 minor 版本按"文件比本 reader 新"处理，写入被拒。
- `tests/m4/m4_sqlite_memory_test.cpp`：新增 `mutation_ledger_is_scope_bound` 与
  `migration_adds_ledger_scope`；后者用手写 SQL 构造 1.0 布局文件。
- `tests/CMakeLists.txt`：M4 store 测试目标链接 `Mira::sqlite3`（迁移测试需要直接构造
  旧布局文件；`tests` 模块在架构策略中已允许 sqlite 头族）。
- 文档：设计文档 §16.2/§16.3、API 文档、`DEC-058`，以及 `DEC-057` 的前向链接。

### 2.2 非目标

- 不改变 `IMemory::apply` 的公开签名。
- 不改变 `IMemory::query` / `get` / `compact` / `erase` 的语义。
- 不给台账加全局清理或保留期策略（两个台账仍是 append-only）。
- 不改变 `MemoryRecord` / `MemoryMutation` 的 JSON schema 与内容契约。
- 不修改 `third_party/kairo`，不引入新的 Executor 能力使用方式；记忆读写仍经既有
  `ContextMemorySupervisor` 路由。
- 不登记 Executor 反馈台账：本轮改动不涉及 Executor 能力边界。

## 3. 设计与决策依据

- 决策记录：[DEC-058](../decisions/DEC-058-mutation-ledger-scope-binding.md)。
- 设计基线：`docs/design/context_and_memory_design.md` §8.3（`IMemory` 契约）、
  §16.2（SQLite 参考 schema、迁移在 Runtime 接受 Task 前完成）、§16.3（Schema
  Migration：显式、可诊断、不静默清空数据）。
- 现状核查：`memory_mutations` 主键为 `mutation_id`；重放查询只按主键；mutation id
  为 UUIDv4 或由 UUIDv4 的 `run_id` 派生，不可猜；台账条目不进模型上下文。

## 4. 工作项

- [x] `MNT-202610-23` 台账记录归属 scope：`memory_mutations` 增加 `scope_key` 列，
      写入时带上 scope；schema 提升到 1.1。
- [x] `MNT-202610-24` 重放按 scope 承认：条目 scope 与 mutation scope 不一致（含归属
      未知）返回 `ScopeDenied`，不披露 `record_id` 与版本，不回落写入路径。
- [x] `MNT-202610-25` 1.0 → 1.1 显式迁移：加列 → 存活记录回填 → 已擦除记录回填 →
      再写版本戳；归属无法确定的历史行保持拒绝；版本比较补齐 minor。
- [x] `MNT-202610-26` 测试与文档：台账 scope 正负用例、迁移与版本拒绝用例；设计文档
      §16.2/§16.3、API 文档、`DEC-058` 与 `DEC-057` 前向链接同步。

## 5. 风险与阻塞

- **历史行部分无法归属**：其记录被 scope 级 Erasure 删除的历史台账行，重放在任何
  scope 下都被 `ScopeDenied` 拒绝。这些 mutation 在升级前已应用、记录已不存在，拒绝
  不会重新写入任何内容，但相关宿主的重试会收到错误而不是幂等成功。
- **持久化格式变更**：Memory schema 1.0 → 1.1。1.1 reader 打开 1.0 文件时在写入前完成
  迁移；不产生半迁移状态（迁移包在事务内，失败回滚）。
- **旧 reader 行为不在本轮控制范围**：既有版本比较只比 major，旧 reader 打开 1.1 文件
  会把它当作更旧版本并盖上自己的版本戳。本轮已让 1.1 reader 遇到更新的 minor 时拒绝，
  但无法从新代码侧约束旧 reader。
- **新增可观察失败**：跨 scope 复用 `mutation_id` 由"静默返回他人台账条目"变为
  `ScopeDenied`。该用法无合法用途，属异常路径。
- 无 Executor 阻塞，无平台条件阻塞，无凭据或设备依赖。

## 6. 测试与退出条件

- [x] `mutation_ledger_is_scope_bound` 覆盖：归属 scope 重放得到幂等结果；外部 scope
      持同一 `mutation_id` 得到 `ScopeDenied` 且未写入；归属 scope 记录未改动；外部
      scope 用全新 `mutation_id` 正常写入。
- [x] `migration_adds_ledger_scope` 覆盖：1.0 布局文件打开后报告 `Migrated`；记录仍可
      查；迁移前的 `mutation_id` 在归属 scope 内恢复可重放；再次打开为 `UpToDate`；
      标记为更新 minor 版本的文件被拒绝（`UnsupportedVersion`）。
- [x] 全量 `ctest --preset debug` 通过；记忆测试组在 asan / ubsan / tsan 下通过。
- [x] `architecture-check`、`check_docs`、平台边界与格式检查通过。

## 7. 验证记录

### 2026-10-22：实现、测试与文档同步完成

- **范围**：`memory_mutations` 增加 `scope_key` 并在写入时记录归属 scope；重放只在其
  归属 scope 内被承认，跨 scope 复用 `mutation_id` 返回 `ScopeDenied`；Memory schema
  1.0 → 1.1 显式迁移（加列 → 存活记录回填 → 已擦除记录回填）；版本比较补齐 minor；
  新增两个测试函数；设计文档 §8.3/§16.2/§16.3、API 文档、`DEC-058` 与 `DEC-057`
  前向链接同步。
- **依据**：[DEC-058](../decisions/DEC-058-mutation-ledger-scope-binding.md)；设计 §8.3、
  §16.2、§16.3。
- **环境**：Linux 7.0.0-38-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；14 核；
  固定 SQLite amalgamation 3.53.4。读可见性一轮见
  [PR #101](https://github.com/Linductor-alkaid/mira/pull/101) 的第一个提交（按 id 读取的
  scope 绑定），本轮为同一 PR 的第二个提交（台账 scope 绑定与 schema 1.1）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警（改动文件 0 条） |
  | `ctest --preset debug -j 14` | **109/109 通过** |
  | `ctest --preset debug -R "m4\|m13\|m23"` | 17/17 通过 |
  | `cmake --build --preset static-analysis -j 14` | 通过；改动文件 0 条告警（余下全部来自 `third_party/kairo` 与 libstdc++ 头文件实例化） |
  | `cmake --build --preset asan -j 14` + `ctest --preset asan -R "m4\|m13\|m23"` | **17/17 通过**，无 ASan 报告 |
  | `cmake --build --preset ubsan -j 14` + `ctest --preset ubsan -R "m4\|m13\|m23"` | **17/17 通过**，无 UBSan 报告 |
  | `cmake --build --preset tsan -j 14` + `setarch x86_64 -R ctest --test-dir build/tsan -R "m4\|m13\|m23" -j 4` | **17/17 通过**，0 条 ThreadSanitizer 告警 |
  | `cmake --build build/debug --target architecture-check` | `architecture policy is clean`（8 模块，17 条既有 baseline 抑制项，0 条 stale） |
  | `python3 tools/check_docs.py .` | `Markdown links and fences: OK` |
  | `python3 tools/check_platform_boundary.py` | `Mira Core platform boundary is clean` |
  | `cmake --build build/debug --target format-check` | 通过（264 个文件） |

- **限制与未执行项**：
  - 首轮全量 `ctest --preset debug -j 14` 出现 1 项失败：`mira_m9_tools_test`
    （`tests/m9/m9_tools_test.cpp:115`，恢复后的运行状态期望 `running`）。该用例隔离
    重跑 5/5 通过，随后一轮全量 109/109 通过。判定为并行负载下的既有时序敏感：该文件
    对 `IMemory` / `SqliteMemoryStore` / memory 的引用数为 **0**，与本轮改动无调用关系。
    按 §5 规则如实记录，未以重跑结果掩盖首轮失败。
  - TSAN 直接运行（未关闭 ASLR）时全部用例在进程启动阶段报
    `FATAL: ThreadSanitizer: unexpected memory mapping`，含与本轮无关的纯契约测试；
    按本仓库既有取证模式（M2/M8/M9/M10/M11 计划）关闭 ASLR 后全绿。属运行环境限制。
  - ASAN/UBSAN/TSAN 仅覆盖记忆相关测试组（m4 / m13 / m23，17 项）；全量 109 项仅在
    debug 配置下执行。
  - 未在 Windows 与 Android 目标上构建验证：迁移步骤只用 SQLite 通用 DDL 与
    `PRAGMA table_info`，不含平台相关代码路径，但迁移路径尚未在目标平台上实测。
  - 未做"中和守卫后确认新测试失败"的变异验证：移除 scope 守卫的操作被执行环境权限
    策略拒绝；改以断言本身论证判别力——没有台账 scope 校验时，外部 scope 的
    `apply()` 会命中重放分支并返回归属 scope 的台账条目，`ScopeDenied` 断言必然失败；
    没有迁移步骤时，1.0 布局文件打开后 `disposition` 不会是 `Migrated`，且重放查询会因
    缺少 `scope_key` 列而失败，迁移断言必然失败。
- **同步**：`docs/design/context_and_memory_design.md`（§0、§8.3、§16.2、§16.3）、
  `docs/api/context-memory.md`、`docs/decisions/DEC-058-mutation-ledger-scope-binding.md`（新增）、
  `docs/decisions/DEC-057-scoped-memory-read-and-apply-check-order.md`（前向链接）、
  `docs/plans/maintenance-2026-10-memory-read-visibility.md`（残余风险关闭标注）、本计划（新增）。
- **剩余项**：无。§5 列出的历史行归属缺口为设计取舍（fail closed），不是待办项。
