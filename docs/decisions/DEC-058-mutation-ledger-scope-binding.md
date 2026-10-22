# DEC-058：幂等台账按 scope 绑定与 Memory schema 1.1

> 状态：Accepted
> 日期：2026-10-22
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 记忆台账 scope 绑定维护轮
> 替代/被替代：无；关闭 [DEC-057](DEC-057-scoped-memory-read-and-apply-check-order.md)
> 记录在案的残余风险（`memory_mutations` 台账未按 scope 隔离）

## 背景与问题

`memory_mutations` 是长期记忆的幂等台账：`apply()` 以 `mutation_id` 为主键，重放
同一 id 的 mutation 直接返回已记录的结果（`record_id`、`result_version`）而不重复写入。
台账查询只按 `mutation_id`：

```sql
SELECT applied_type, record_id, result_version FROM memory_mutations WHERE mutation_id = ?1
```

查询结果不受调用方声明的 scope 约束。因此持有另一个 scope 内 `mutation_id` 的调用方
可以读到该 scope 的 `record_id` 与版本号——这是 DEC-057 修复的 `get()` 跨 scope 读取
之外的第二条同类路径。

当前不可直接利用：mutation id 要么是 `MutationId::generate()`（UUIDv4，122 bit 熵），
要么由 `workflow_episode_mutation_id(run_id)` 派生，而 `run_id` 本身也是 UUIDv4。台账
条目也不进入模型上下文。但接口语义上，按 id 直读台账与 `query()` 的 scope allowlist
不一致，缺少拒绝路径。

台账本身还不能加 scope：主键是 `mutation_id`，新增列不会改变查找范围。

## 决策

### 1. 台账记录归属它的 scope

Memory schema 提升到 **1.1**：`memory_mutations` 增加 `scope_key` 列，每个条目记录
写入时所属的 scope。新写入的台账条目总是带上 scope。

### 2. 重放只在其归属 scope 内被承认

`apply()` 命中台账时比较条目 scope 与本次 mutation 声明的 scope：一致才返回幂等
重放结果；不一致（包括归属未知的空值）返回 `ScopeDenied`。不返回条目内容，不返回
记录 id 与版本。

scope 不一致时**不回落到正常写入路径**：回落会让同名的 `mutation_id` 在台账主键上
冲突，宿主会看到一条与根因无关的写入失败。让拒绝显式且与根因一致。

### 3. scope 是台账条目的固有属性，不在重放时反查

重放时通过 `memory_records` / `memory_erasure_log` 反查记录归属被否决：scope 级
Erasure 只写一条不带 `record_id` 的审计行，scope 级删除后被删记录在审计表里不再
可归属。此时反查会丢失归属，而归属丢失恰好发生在最需要它的地方——重试一条已被擦除
的 mutation 必须仍然是幂等的、且不得把已擦除的数据重新写回。

把 scope 固化在台账条目上，归属不随记录被擦除而丢失。

### 4. 1.0 → 1.1 显式迁移

`open()` 在版本较旧时先执行迁移步骤再写版本戳（设计 §16.2：迁移在 Runtime 接受 Task
前完成，不由第一个随机请求隐式触发）：

1. `ALTER TABLE memory_mutations ADD COLUMN scope_key TEXT NOT NULL DEFAULT ''`（列已存在时跳过）
2. 从 `memory_records` 回填存活记录的归属
3. 从 `memory_erasure_log` 回填已擦除记录的归属

两个台账都是 append-only，已擦除的记录在审计表里仍可归属。**归属仍无法确定的历史行**
（其记录由 scope 级 Erasure 删除）保留空占位符，其 `mutation_id` 在任何 scope 下都被
拒绝：宁可拒绝一次重放，也不把无法归属的 scope 的记录 id 交给持有该 id 的调用方。拒绝
不会重新写入任何内容。

### 5. 版本比较补齐 minor

`open()` 原先只比较 major。1.1 之后若文件带有更新的 minor 版本，原逻辑会把它当作
"更旧"接受并盖上本 reader 的版本戳。版本比较补齐 minor：更新的 minor 版本按"文件比
本 reader 新"处理，写入被拒（只读诊断模式按既有规则）。

## 备选方案

- **重放时从 `memory_records` / `memory_erasure_log` 反查归属**：拒绝。scope 级
  Erasure 不写逐条审计行，删除后归属不可反查，重试会失去幂等性。
- **把台账主键改成 `(scope_key, mutation_id)`**：拒绝。需要重建表；且跨 scope 的 id
  冲突会被静默当成两次独立写入。对异常更应该 fail closed，而不是静默接受。
- **让 `mutation_id` 全局唯一并直接拒绝任何重放**：拒绝。台账正是"重试恰好一次"和
  "重试不得把已擦除记录写回"的实现依据。
- **只过滤返回值、不改台账**：拒绝。`record_id` 本身就是披露内容。
- **给 `IMemory::apply` 增加 scope 参数**：拒绝。mutation 已经携带 scope，无需扩大
  公开契约。

## 影响与风险

- **持久化格式变更**：Memory schema 1.0 → 1.1，显式迁移步骤。旧 reader 打开 1.1 文件
  的行为不在本轮控制范围内（既有版本比较逻辑只比 major），但 1.1 reader 打开 1.0 文件
  会迁移，写入前完成，不产生半迁移状态。
- **历史行部分无法归属**：其记录被 scope 级 Erasure 删除的历史台账行，重放在任何
  scope 下都被拒绝。这些 mutation 在升级前已应用、其记录已不存在，拒绝不会重新写入
  任何内容。
- **新增可观察失败**：跨 scope 复用 `mutation_id` 从"静默返回他人台账条目"变为
  `ScopeDenied`。该用法没有合法用途，属异常路径。
- `IMemory::apply` 的公开签名不变，`IMemory` 其余契约不变。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

- `tests/m4/m4_sqlite_memory_test.cpp` 新增 `mutation_ledger_is_scope_bound`：归属
  scope 重放仍得到幂等结果；外部 scope 持同一 `mutation_id` 得到 `ScopeDenied` 且
  未在外部 scope 写入任何记录；归属 scope 的记录未被改动；外部 scope 使用全新
  `mutation_id` 仍正常写入。
- 同文件新增 `migration_adds_ledger_scope`：手写 1.0 布局文件 → 打开时报告
  `Migrated` → 记录仍可查 → 迁移前用过的 `mutation_id` 在归属 scope 内恢复可重放 →
  再次打开为 `UpToDate` → 标记为更新 minor 版本的文件被拒绝（`UnsupportedVersion`）。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-mutation-ledger-scope`](../plans/maintenance-2026-10-mutation-ledger-scope.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §8.3、§16.2、§16.3
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 相关：[DEC-057](DEC-057-scoped-memory-read-and-apply-check-order.md)（按 id 读取的 scope
  绑定与 apply 检查顺序；本记录关闭其列出的台账残余风险）
