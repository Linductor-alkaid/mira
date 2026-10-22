# DEC-057：按 scope 绑定的记忆读取与 apply 检查顺序

> 状态：Accepted
> 日期：2026-10-22
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 记忆读取可见性维护轮
> 替代/被替代：局部修订 [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)
> 关于记忆域与学习契约；不改变 `IMemory` 的其余契约

## 背景与问题

`IMemory::get(MemoryId)` 的签名不接受 scope，实现（`SqliteMemoryStore::get` →
`Impl::read_record`）是裸 `SELECT ... WHERE id = ?1`，没有 scope、status、expiry
或 kind 过滤。这与设计 §6.1「scope 是访问控制过滤器，检索永不跨 scope 相似度越界」
以及 §14.1「结果绝不包含 allowlist 之外 scope 的记录」形成落差：查询路径强制
scope ACL，按 id 直读路径却完全绕过它。

`SqliteMemoryStore::apply()` 对 `Update`/`Supersede`/`Tombstone` 的检查顺序是
「先比较 `expected_version`，再检查 scope」。两者抛出的错误码不同
（`VersionConflict` 与 `ScopeDenied`），因此持有 record id 的调用方可以用不同
`expected_version` 反复试探，二分出跨 scope 记录的当前版本号。

两个问题的共同前提是"调用方需要知道 record id"。常规 id 由 `Id128::generate()`
以 `std::random_device` 生成（UUIDv4，122 bit 熵），不可猜、不可枚举；memory id
也不进入模型上下文。因此当前不存在可直接利用的路径，风险是契约层面的：接口把
"按 id 读取任意记录"作为常规用法暴露出去，而它在语义上比 `query()` 宽松得多，
没有任何提示、审计或拒绝。

设计 §8.3 的 `IMemory` 草图同样是这个形状，所以这是继承自原型的契约缺口，而非
实现引入的偏离。

## 决策

### 1. `IMemory::get` 按 scope 绑定

签名改为 `get(MemoryId record, const MemoryScope &scope)`。scope 与记录所属 scope
不一致时返回**空 optional**，与"记录不存在"完全不可区分。

不返回 `ScopeDenied` 错误是刻意选择：错误码会区分"记录存在但 scope 不符"与
"记录不存在"，按 id 直读将退化为跨 scope 的存在性预言机。空 optional 与
`query()` 的行为一致——查询同样不会因为某条记录被过滤掉而报错。

### 2. 生命周期不在 `get()` 中过滤

`get()` 不过滤 `status` 与 `expires_at`。设计 §13.4 已明确：「Tombstone 表示不再
参与检索。隐私 Erasure 走独立的物理删除/crypto-shredding 流程，不能只用
Tombstone 冒充删除。」因此 tombstoned、superseded、expired 的记录行在其自身
scope 内仍可按 id 读取，用于审计与保留期工作。

隐私删除的责任在宿主：必须走 `IMemory::erase()`（物理删除 payload、FTS、
embedding、版本、provenance 与关联 artifact），而不是 `Tombstone`。契约注释与
设计文档同步写明这一分工，避免宿主把 tombstone 当作删除语义使用。

### 3. `apply()` 的 scope 检查前置

`Update`/`Supersede`/`Tombstone` 分支改为：先读取目标记录并判定 scope，再比较
`expected_version`。跨 scope 的调用方无论 `expected_version` 是否正确都得到
同一个 `ScopeDenied`，版本号不再可被二分探测。

同 scope 内的乐观并发语义不变：`expected_version` 不匹配仍返回
`VersionConflict`。owner 内部的版本冲突检查没有被削弱。

### 4. 变更性质

`IMemory::get` 是公开契约的破坏性变更。项目当前为 `0.1.0`（pre-1.0）。`get()`
在仓库内零调用方（含 tests、examples、adapters），树内迁移成本为零；下游适配器
需要同步改签名。

## 备选方案

- **保留无 scope 的 `get(MemoryId)` 重载**：拒绝。保留漏洞面，且让"应该用哪个
  重载"成为调用点的隐性判断题。
- **让 `get()` 同时过滤 tombstoned / superseded / expired**：拒绝。与设计 §13.4
  的 tombstone 语义冲突，并与既有测试 `tombstone_and_ttl_expiry` 编码的意图
  直接矛盾（该测试断言 tombstone 后记录仍可按 id 读出）。若将来要改变这一
  语义，属于设计变更，须另开 DEC，而不是在缺陷修复中顺带推翻。
- **scope 不符时返回 `ScopeDenied` 错误**：拒绝。重新引入存在性预言机，见上。
- **新增 `get_raw()` 之类的运维专用入口**：拒绝。当前没有需要绕过 scope 的
  内部调用方，新增入口只会扩大可被误用的接口面。
- **把 scope 检查与版本检查一起放进 `IMemory` 契约的文档说明而不改实现**：
  拒绝。文档不能约束一个不带参数的读取路径。

## 影响与风险

- 公开契约破坏性变更，下游 `IMemory` 实现需要同步改签名；仓库内 5 个测试 fake
  与 `tests/consumer` 的消费端 fake 已同步。
- 保留残余风险：`memory_mutations` 幂等台账没有 `scope_key` 列，`apply()` 的
  幂等重放查询同样不按 scope 隔离。持有他人 scope 内 mutation id 的调用方可以
  读出 `(record_id, result_version)` 元数据（不含内容）。修复需要为台账增加
  `scope_key` 列（schema 1.1 + ALTER + 存量行回填策略），属于独立的迁移工作，
  本轮不处理。
- `get()` 现在需要调用方持有 scope。若某个下游实现此前依赖"按 id 直读任意
  scope"的行为（例如跨租户运维看板），改动后必须显式声明目标 scope，或改走
  `query()`。
- 本决策不改变 `query()` 的混合检索语义、排名权重、bitemporal 解析，也不改变
  `erase()` 的物理删除路径。

## 验证方式

- `tests/m4/m4_sqlite_memory_test.cpp` 新增 `get_enforces_scope_acl`：同 scope
  可读；跨 tenant、跨 subject、tenant-less scope 与不存在 id 一律返回空 optional；
  被拒读取不修改记录。
- 同文件新增 `apply_scope_check_precedes_version_check`：跨 scope + 错误版本与跨
  scope + 正确版本返回同一个 `ScopeDenied`；owner 仍得到 `VersionConflict`；
  记录未被修改。
- 全量 `ctest --preset debug` 通过。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-memory-read-visibility`](../plans/maintenance-2026-10-memory-read-visibility.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §8.3、§13.4、§16
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 相关：[DEC-029](DEC-029-memory-domains-and-learning-contracts.md)（记忆域与学习契约）、
  [DEC-003](DEC-003-event-sourced-persistence.md)（事件溯源持久化与删除留痕）
