# 维护计划：per-kind 策略表（2026-10）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：[MNT-202610-19…22 记忆读取可见性](maintenance-2026-10-memory-read-visibility.md)（Completed）、
> [MNT-202610-23…26 台账 scope 绑定](maintenance-2026-10-mutation-ledger-scope.md)（Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-23

## 1. 目标

关闭设计 §6.2 承诺但未实现的 per-kind 策略表：`MemoryKind` 决定的默认 TTL、最低验证
等级、允许的 scope 和检索方式在实现里全部落空。

本轮把四个轴落成 Core 契约（唯一策略表），并给出各自可执行的落地方式：存储在写入时
补齐默认 TTL，Core 写入门执行验证等级下限与允许 scope，查询按 kind 声明的检索腿运行。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/memory_contracts.hpp`：
  - 新增 `MemoryRetrievalLeg`、`MemoryKindPolicy`；新增
    `memory_kind_policy()`、`check_memory_kind_policy()`、`memory_default_expiry()`、
    `memory_verification_rank()`。
  - `MemoryQueryQuality` 新增 `semantic_leg_withheld`（追加式字段）。
- `src/context/memory_contracts.cpp`：策略表实现（每个 `MemoryKind` 一项）、
  `memory_verification_rank()`、写入门与默认到期时间折算。
- `src/context/memory_contracts.cpp`（`MemoryMutation::validate()`）：Add / Update /
  Supersede 走 per-kind 门，Tombstone 不走。
- `src/context/memory_consolidation.cpp`：候选在进入存储前先过 per-kind 门，拒绝时
  给出稳定 reason code `kind-policy`。
- `src/storage/sqlite_memory_store.cpp`：
  - 写入时按 kind 补齐默认 `expires_at`（未声明时）。
  - Supersede 关闭前驱、Tombstone 退役记录时清空到期时间。
  - 查询按 kind 声明的可用检索腿运行；被扣下的嵌入不构成排序请求。
- `tests/m4/m4_contracts_test.cpp`：新增 `memory_kind_policy_table_and_gate`。
- `tests/m4/m4_sqlite_memory_test.cpp`：新增 `kind_policy_bounds_the_store` 与
  `kind_policy_withholds_the_semantic_leg`。
- 文档：设计文档 §0/§5/§6.2/§6.3/§13.4/§14.1、API 文档、`DEC-059`、本计划。

### 2.2 非目标

- 不改变 `IMemory` 的任何公开方法签名。
- 不改变 `MemoryRecord` / `MemoryMutation` 的 JSON schema 与内容契约。
- 不改变 Memory 持久化 schema（本轮无 schema 变更、无迁移步骤）。
- 不改变 `MemoryConsolidator` 的 kind 分类规则（scope → kind 的既有映射不变）。
- 不改变 `RetrievalWeights` 与排名公式。
- 不新增 `IMemory` 实现者；既有 7 个实现者（真实实现 + 测试 fake）不需要改签名。
- 不引入新的 Executor 能力使用方式；记忆读写仍经既有 `ContextMemorySupervisor` 路由。
- 不登记 Executor 反馈台账：本轮改动不涉及 Executor 能力边界。

## 3. 设计与决策依据

- 决策记录：[DEC-059](../decisions/DEC-059-memory-kind-policy.md)。
- 设计基线：`docs/design/context_and_memory_design.md` §5（记忆层级）、
  §6.2（MemoryKind 承诺）、§13.4（Supersede/Tombstone/Erasure 分工）、
  §14.1（检索阶段）、§19（导入契约）。
- 现状核查：`expires_at` 只有调用方显式设置才会存在，仓库内无任何生产写入路径设置
  它；`compact()` 的 `expired_records_purged` 恒为 0；验证等级不参与写入判定；
  kind 与 scope 的组合无约束；三条检索腿对所有 kind 一视同仁。

## 4. 工作项

- [x] `MNT-202610-27` Core 策略表：`MemoryKindPolicy` 与
      `memory_kind_policy()` 落成公开契约，每个 `MemoryKind` 一项，四个轴齐全；
      `memory_verification_rank()` 公开化。
- [x] `MNT-202610-28` 写入侧落地：存储按 kind 补齐默认 `expires_at`；Supersede /
      Tombstone 清空到期时间；`MemoryMutation::validate()` 执行验证等级下限与允许
      scope（Tombstone 豁免，带 `source_namespace` 的记录豁免）；consolidation 候选
      拒绝时给出 `kind-policy` reason code。
- [x] `MNT-202610-29` 读取侧落地：查询按 kind 声明的检索腿运行；被扣下的嵌入不构成
      排序请求；`MemoryQueryQuality::semantic_leg_withheld` 报告该事实。
- [x] `MNT-202610-30` 测试与文档：策略表与写入门、存储侧 TTL 与退役语义、检索腿
      扣下各有自动化覆盖；设计 §6.3 与 API 文档给出表内容；`DEC-059` 记录取舍。

## 5. 风险与阻塞

- **既有行为收紧**：低于 kind 验证等级的 claim 与越界的 kind/scope 组合不再能写入。
  仓库内既有写入路径全部合规（逐条核对见 DEC-059 §4）；外部宿主若曾把未观察的环境
  事实作为 `EnvironmentFact` 写入，会开始收到 `InvalidRecord`。
- **新增默认到期时间**：`compact()` 会按 TTL 物理清除到期的活记录（连带 Artifact）。
  这是设计 §5「带 TTL」的应有行为，但此前从未发生；`purge_horizon` 默认为 0，墓碑行
  仍长期留存。
- **历史行**：策略生效前写入的记录仍可读可查；更新它们需要把验证等级提到对应 kind
  的下限，退役不受该门约束。
- **查询结果变化**：声明 kind 的查询对结构化 kind 不再走嵌入相似度腿；未声明 kind
  的查询行为不变。
- **命名空间豁免是显式取舍**：带 `source_namespace` 的记录不受 per-kind 门约束
  （其权威由导入契约承担，设计 §19）。这是 M23 提升路径与模型辅助候选保持不变的
  前提，代价是该门不是绝对的。
- 无 Executor 阻塞，无平台条件阻塞，无凭据或设备依赖。

## 6. 测试与退出条件

- [x] `memory_kind_policy_table_and_gate` 覆盖：表项完整性、验证等级有序、合规
      claim 通过、低于下限被拒（`InvalidRecord`）、越界组合被拒（`ScopeDenied`）、
      命名空间豁免、默认到期时间折算、写入门与 Tombstone 豁免。
- [x] `kind_policy_bounds_the_store` 覆盖：默认 TTL 补齐、下限与 scope 拒绝、命名
      空间记录写入成功、Tombstone 与 Supersede 清空到期时间、后继记录带自己的到期
      时间。
- [x] `kind_policy_withholds_the_semantic_leg` 覆盖：kind 过滤时语义腿被扣下并报告，
      无 kind 过滤时语义腿照常运行。
- [x] 全量 `ctest --preset debug` 通过。
- [x] `architecture-check`、`check_docs`、平台边界与格式检查通过。

## 7. 验证记录

### 2026-10-23：实现与文档同步完成

- **范围**：`MemoryKindPolicy` 策略表落成 Core 公开契约；写入时按 kind 补齐默认
  TTL；Supersede / Tombstone 清空到期时间；`MemoryMutation::validate()` 执行验证
  等级下限与允许 scope；consolidation 候选拒绝给出 `kind-policy`；查询按 kind 声明
  的检索腿运行；新增三个测试函数；设计文档 §0/§6.3/§13.4/§14.1、API 文档、
  `DEC-059` 与本计划同步。
- **依据**：[DEC-059](../decisions/DEC-059-memory-kind-policy.md)；设计 §5、§6.2、
  §6.3、§13.4、§14.1、§19。
- **环境**：Linux 7.0.0-38-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；14 核；
  固定 SQLite amalgamation 3.53.4。基线提交 `792cccf`（PR #101 合并后的 master）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警（改动文件 0 条） |
  | `ctest --preset debug -j 14` | **109/109 通过** |
  | `ctest --test-dir build/debug -R "m4\|m13\|m23" -j 8` | 17/17 通过 |
  | `cmake --build --preset static-analysis -j 14` | 通过；改动文件 0 条告警（余下全部来自 `third_party/kairo` 与 libstdc++ 头文件实例化） |
  | `cmake --build --preset asan -j 14` + `ctest --test-dir build/asan -R "m4\|m13\|m23" -j 8` | **17/17 通过**，无 ASan 报告 |
  | `cmake --build --preset ubsan -j 14` + `ctest --test-dir build/ubsan -R "m4\|m13\|m23" -j 8` | **17/17 通过**，无 UBSan 报告 |
  | `cmake --build --preset tsan -j 14` + `setarch x86_64 -R ctest --test-dir build/tsan -R "m4\|m13\|m23" -j 4` | **17/17 通过**，0 条 ThreadSanitizer 告警 |
  | `cmake --build build/debug --target architecture-check` | `architecture policy is clean`（8 模块，17 条既有 baseline 抑制项，0 条 stale） |
  | `python3 tools/check_docs.py .` | `Markdown links and fences: OK` |
  | `python3 tools/check_platform_boundary.py` | `Mira Core platform boundary is clean` |
  | `cmake --build build/debug --target format-check` | 通过（264 个文件；首轮在 `memory_contracts.cpp` 与 `sqlite_memory_store.cpp` 报换行违规，已用 `clang-format -i` 修正并复验） |
  | `build/debug/mira_stateful_consumer` / `mira_working_context_host_consumer` / `mira_minimal_consumer` | 三个可执行消费端均正常退出（0） |

- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 仅覆盖记忆相关测试组（m4 / m13 / m23，17 项）；全量 109 项仅在
    debug 配置下执行。
  - TSAN 直接运行（未关闭 ASLR）时按本仓库既有取证模式关闭 ASLR 后执行。
  - 未在 Windows 与 Android 目标上本地构建验证：本轮改动不含平台相关代码路径，但
    `MemoryKindPolicy` 等是公开契约，跨平台适配器若自行实现 `IMemory` 需按新契约
    校验其写入。
  - 未做「中和守卫后确认新测试失败」的变异验证：移除策略门的操作会被执行环境的权限
    策略拒绝；改以断言本身论证判别力——没有默认 TTL 时写入后的 `expires_at` 不会
    存在，没有验证等级门时低于下限的写入会成功，没有 scope 门时越界组合会写入成功，
    没有检索腿门时 `semantic_leg_withheld` 不会置位且语义腿会运行。
  - 未做跨记录的生命周期长跑验证：默认 TTL 以天为单位，验证只覆盖到期时间的计算与
    关闭 claim 时的语义，`compact()` 按 TTL 物理清除的路径沿用既有 retention-expiry
    分支（`tombstone_and_ttl_expiry` 已覆盖）。
- **同步**：`docs/design/context_and_memory_design.md`（§0、§6.2、§6.3、§13.4、
  §14.1）、`docs/api/context-memory.md`、
  `docs/decisions/DEC-059-memory-kind-policy.md`（新增）、本计划（新增）。
