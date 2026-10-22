# 维护计划：记忆读取可见性与 apply 检查顺序（2026-10）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：M4（Completed）、M13（Completed）、M23（Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-22

## 1. 目标

收敛长期记忆读取路径上的两处 scope 缺口，使 `IMemory` 的按 id 读取与 `apply()`
的写入路径都受设计 §6.1 的 scope ACL 约束：

1. `IMemory::get(MemoryId)` 不接受 scope，实现为裸主键读取，绕过查询路径强制的
   scope allowlist。
2. `SqliteMemoryStore::apply()` 的 scope 检查排在版本比较之后，两种不同的错误码
   使跨 scope 调用方可以二分探测记录的当前版本。

同时把「Tombstone 不是删除」这一既有设计的宿主责任写入契约注释与设计文档，避免
宿主把 tombstone 当作隐私删除使用。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/memory_contracts.hpp`：`IMemory::get` 签名改为
  `get(MemoryId, const MemoryScope &)`；scope 不匹配返回空 optional，与"记录不存在"
  不可区分；契约注释写明生命周期不过滤的原因与 erase/tombstone 分工。
- `include/mira/sqlite_memory_store.hpp`、`src/storage/sqlite_memory_store.cpp`：
  新增 `Impl::read_scoped_record`（scope 过滤），`SqliteMemoryStore::get` 走该路径；
  `apply()` 的 `Update`/`Supersede`/`Tombstone` 分支把 scope 检查前置到
  `expected_version` 比较之前。
- `tests/m4/m4_sqlite_memory_test.cpp`：新增 `get_enforces_scope_acl` 与
  `apply_scope_check_precedes_version_check`。
- 测试 fake 同步签名与语义：`tests/support/m13_support.hpp`、
  `tests/m13/m13_learning_persistence_test.cpp`、`tests/m15/m15_eval_profile_harness.cpp`、
  `tests/m23/m23_promotion_support.hpp`、`tests/m4/m4_supervisor_test.cpp`、
  `tests/consumer/main.cpp`。
- 文档：`docs/design/context_and_memory_design.md` §8.3/§13.4/§16、
  `docs/api/context-memory.md`、新增 `DEC-057`。

### 2.2 非目标

- 不改变 `query()` 的混合检索语义、排名权重、多样性上限、token 打包与双时态解析。
- 不改变 `erase()` 的物理删除路径（payload、FTS、embedding、版本、provenance、
  关联 artifact）。
- 不改变 Tombstone 语义。设计 §13.4 已规定 tombstone 表示"不再参与检索"而非删除，
  既有测试 `tombstone_and_ttl_expiry` 已编码该意图；改变它属于设计变更，须另开 DEC。
- 不为 `memory_mutations` 幂等台账增加 `scope_key` 列（见 §5 残余风险），本轮不做
  schema 迁移。
- 不引入新的 Executor 能力使用方式；记忆读写仍经既有 `ContextMemorySupervisor`
  路由，存储 I/O 仍在 `mira-memory-store` blocking-I/O worker 上。
- 不登记 Executor 反馈台账：本轮改动不涉及 Executor 能力边界。

## 3. 设计与决策依据

- 决策记录：[DEC-057](../decisions/DEC-057-scoped-memory-read-and-apply-check-order.md)。
- 设计基线：`docs/design/context_and_memory_design.md` §6.1（scope 是 ACL 不是排序
  特征）、§8.3（`IMemory` 契约）、§13.4（Supersede/Tombstone/Erasure 的分工）、
  §14.1（检索不得越过 scope allowlist）、§16（持久化与一致性）。
- 现状核查：`IMemory::get` 在仓库内零调用方（含 tests、examples、adapters），因此
  破坏性签名变更的树内迁移成本为零；下游适配器需要同步改签名。项目版本
  `0.1.0`（pre-1.0）。

## 4. 工作项

- [x] `MNT-202610-19` `IMemory::get` 按 scope 绑定：公开签名携带调用方声明的 scope，
      scope 不匹配返回空 optional 且与"记录不存在"不可区分；`SqliteMemoryStore`
      实现走 scope 过滤路径；契约注释说明生命周期不过滤与 erase/tombstone 分工。
- [x] `MNT-202610-20` `apply()` 的 scope 检查前置到版本比较之前：跨 scope 调用方在
      `expected_version` 正确与否时得到同一个 `ScopeDenied`，版本号不再可二分探测；
      同 scope 的乐观并发语义不变。
- [x] `MNT-202610-21` 测试覆盖：scope ACL 正负用例（跨 tenant、跨 subject、
      tenant-less scope、不存在 id）、apply 检查顺序正负用例，以及既有
      tombstone/TTL/erase 用例保持通过；全部 `IMemory` 实现者（真实实现与 6 个
      fake）签名同步。
- [x] `MNT-202610-22` 文档同步：设计文档 §8.3/§13.4/§16、API 文档、
      `DEC-057` 与本计划的验收证据一致。

## 5. 风险与阻塞

- **残余风险（本轮未修复，后续已关闭）**：`memory_mutations` 幂等台账没有 `scope_key` 列，
  `apply()` 的幂等重放查询不按 scope 隔离。持有他人 scope 内 mutation id 的调用方
  可读出 `(record_id, result_version)` 元数据（不含内容）。修复需要为台账增加
  `scope_key` 列：schema 1.1 + `ALTER TABLE` + 存量行回填策略（存量行无 scope，
  需决定按"未知 scope 放行"还是"拒绝重放"）。该迁移及其取舍属于独立工作项，
  不与本轮的读取可见性修复混做。
  **后续：本条残余风险由 [DEC-058](../decisions/DEC-058-mutation-ledger-scope-binding.md)
  与[台账 scope 绑定维护计划](maintenance-2026-10-mutation-ledger-scope.md)关闭**。
- **破坏性契约变更**：下游 `IMemory` 实现需要同步 `get` 签名。仓库内已全部同步，
  外部适配器需在下一次集成时跟进。
- **宿主语义依赖**：若某个下游宿主此前依赖"按 id 直读任意 scope"的行为，改动后
  必须显式声明目标 scope 或改走 `query()`。本轮在契约注释与 API 文档中写明。
- **既有 store/fake scope 比较口径差异**：`scope_key_of()` 用
  `tenant_id.value_or("")` 展平 scope，因此 `tenant_id` 为 `nullopt` 与空字符串在
  store 中是同一个 scope（`query()` 一直如此），而测试 fake 用 `MemoryScope` 结构
  相等比较，二者在这一个边界上不一致。本轮 `get()` 有意沿用 `scope_key_of`，保证
  「`query()` 能查到的记录必定能用 `get()` 读到」；差异属既有行为，未在本轮修正。
- 无 Executor 阻塞，无平台条件阻塞，无凭据或设备依赖。

## 6. 测试与退出条件

- [x] `get_enforces_scope_acl` 覆盖：owner scope 可读；跨 tenant、跨 subject、
      tenant-less scope 与不存在 id 一律空 optional；被拒读取不修改记录。
- [x] `apply_scope_check_precedes_version_check` 覆盖：跨 scope + 错误版本与跨 scope +
      正确版本返回同一个 `ScopeDenied`；owner 仍得到 `VersionConflict`；记录未被修改。
- [x] 既有记忆测试组（m4 / m13 / m23）与全量 `ctest --preset debug` 通过。
- [x] `architecture-check` 通过，无新增跨模块依赖或外部头族。

## 7. 验证记录

### 2026-10-22：实现、测试与文档同步完成

- **范围**：`IMemory::get` 改为按 scope 绑定；`SqliteMemoryStore` 新增 scope 过滤读取
  路径；`apply()` 的 scope 判定前置到版本比较之前；6 个 `IMemory` 实现者（真实实现 +
  5 个测试 fake + `tests/consumer` 消费端 fake）签名与语义同步；新增两个测试函数；
  设计文档 §0/§8.3/§13.4、API 文档与 `DEC-057` 同步。
- **依据**：[DEC-057](../decisions/DEC-057-scoped-memory-read-and-apply-check-order.md)；
  设计 §6.1、§8.3、§13.4、§14.1、§16。
- **环境**：Linux 7.0.0-38-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；14 核。
  基线提交 `23a53b7`（本轮改动未提交，工作树状态）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警（改动文件 0 条告警） |
  | `ctest --preset debug -j 14` | **109/109 通过，0 失败** |
  | `ctest --preset debug -R "m4\|m13\|m23"` | 17/17 通过 |
  | `cmake --build --preset static-analysis -j 14` | 通过；改动文件 0 条告警（余下告警全部来自 `third_party/kairo` 与 libstdc++ 头文件实例化） |
  | `cmake --build build/debug --target architecture-check` | `architecture policy is clean`（8 模块，17 条既有 baseline 抑制项，0 条 stale） |
  | `python3 tools/check_docs.py .` | `Markdown links and fences: OK` |
  | `python3 tools/check_platform_boundary.py` | `Mira Core platform boundary is clean` |
  | `cmake --build build/debug --target format-check` | 通过（264 个文件；首轮在 `sqlite_memory_store.hpp` 等 7 个文件报换行违规，已用 `clang-format -i` 修正并复验） |
  | `cmake --build --preset asan -j 14` + `ctest --preset asan -R "m4\|m13\|m23"` | **17/17 通过**，无 ASan 报告 |
  | `cmake --build --preset ubsan -j 14` + `ctest --preset ubsan -R "m4\|m13\|m23"` | **17/17 通过**，无 UBSan 报告 |
  | `setarch x86_64 -R ctest --test-dir build/tsan -R "m4\|m13\|m23" -j 4` | **17/17 通过**，0 条 ThreadSanitizer 告警 |

- **限制与未执行项**：
  - TSAN 首次直接运行（未关闭 ASLR）时全部用例在进程启动阶段以
    `FATAL: ThreadSanitizer: unexpected memory mapping` 失败，包含与本轮改动无关的
    纯契约测试；按本仓库既有取证模式（M2/M8/M9/M10/M11 计划）关闭 ASLR 后全绿。
    该失败为运行环境限制，非数据竞争。
  - ASAN/UBSAN/TSAN 仅覆盖记忆相关测试组（m4 / m13 / m23，17 项）；全量 109 项
    仅在 debug 配置下执行。
  - 未在 Windows 与 Android 目标上构建验证：本轮改动不涉及平台相关代码路径，但
    `IMemory::get` 是公开契约，跨平台适配器若自行实现 `IMemory` 需同步签名。
  - 未做"中和守卫后确认新测试失败"的变异验证：该操作需在工作树中移除 scope ACL
    守卫，被执行环境的权限策略拒绝；改以断言本身论证判别力——修复前 `get()` 不
    接受 scope 参数，跨 scope 读取在结构上不可能被拒，新测试的
    `get(owned.id, foreign)` 断言在缺少 scope 过滤时必然失败。
- **同步**：`docs/design/context_and_memory_design.md`（§0、§8.3、§13.4）、
  `docs/api/context-memory.md`、`docs/decisions/DEC-057-scoped-memory-read-and-apply-check-order.md`（新增）、
  本计划（新增）。
- **剩余项**：§5 所列 `memory_mutations` 台账 `scope_key` 缺失导致的
  `(record_id, result_version)` 元数据跨 scope 披露，本轮按 §2.2 非目标处理；
  后续由 [DEC-058](../decisions/DEC-058-mutation-ledger-scope-binding.md) 与
  [台账 scope 绑定维护计划](maintenance-2026-10-mutation-ledger-scope.md)关闭。
