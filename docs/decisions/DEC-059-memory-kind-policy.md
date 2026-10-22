# DEC-059：per-kind 策略表（默认 TTL、验证等级下限、允许 scope、检索腿）

> 状态：Accepted
> 日期：2026-10-23
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 记忆 per-kind 策略维护轮
> 替代/被替代：无
> 关闭的缺口：设计 §6.2 承诺但未实现的 per-kind 策略表

## 背景与问题

设计 §6.2 写明「MemoryKind 决定默认 TTL、最低验证等级、允许的 scope 和检索方式」，
但实现里没有任何 per-kind 策略：四个轴全部落空，具体表现是

- **默认 TTL 缺席**：`MemoryRecord::expires_at` 只有调用方显式设置才会存在，仓库内
  没有一条生产写入路径设置它。设计 §5 把语义记忆标为「长期/带 TTL」、§14.1 第 2 步
  按 TTL 过滤，但 TTL 从不生效；`compact()` 的 `expired_records_purged` 恒为 0。
- **验证等级下限缺席**：验证等级参与排序权重（§14.2）与重复判定（不低于已存副本），
  但没有「这类 claim 至少要多强」的概念。
- **允许 scope 缺席**：kind 与 scope 的组合没有任何约束，`MemoryConsolidator` 的分类
  只是按 scope 选一个 kind，写入路径不校验组合。
- **检索方式缺席**：三条检索腿对所有 kind 一视同仁，规范结构化记录（Episode 的
  canonical JSON、Procedure 的索引声明）也被送进嵌入相似度腿。

## 决策

### 1. Core 持有唯一策略表

`MemoryKindPolicy{kind, default_ttl, min_verification, allowed_scope_kinds,
retrieval_legs}` 与 `memory_kind_policy(kind)` 落在 Core（`memory_contracts.hpp`），
每个 `MemoryKind` 一项。表项取自设计 §5 的记忆层级与参考实现的实际 scope 用法，
逐项理由写在设计 §6.3。

策略表是 Core 契约而不是存储细节：任何 `IMemory` 实现都读同一张表。

### 2. 写入时补齐默认 TTL

记录未声明 `expires_at` 时，存储按其 kind 的默认寿命设置到期时间（基准为
`recorded_at`；未打戳的 `recorded_at` 以写入时钟为基准）。这是存储默认值，不是调用方
义务——把 TTL 交给每个写入点只会让默认值漂移。

### 3. 关闭 claim 即结束其寿命

Supersede 关闭前驱、Tombstone 退役记录时清空到期时间。理由：TTL 约束的是「这条
claim 还活着多久」，claim 一旦关闭，它就不是活 claim 了。若保留到期时间，
`compact()` 会在 TTL 到期时物理清除前驱行与墓碑行——那会截断设计 §7.5 的双时态
as-of 链条，也会让 §13.4 承诺的「墓碑行在 `purge_horizon` 之外长期留存」落空。退役行
改由 `purge_horizon` 与版本保留窗口处置。

### 4. 验证等级下限与允许 scope 是写入门

`check_memory_kind_policy()` 在 `MemoryMutation::validate()` 中生效，因此所有
`IMemory` 实现一致适用：验证不足返回 `InvalidRecord`，kind 与 scope 不匹配返回
`ScopeDenied`。

- **Tombstone 不受该门约束**：退役一条记录必须永远可用，包括策略生效之前写入的
  记录。门约束的是「claim」，不是「退役」。
- **带 `source_namespace` 的记录不受该门约束**：这类记录的权威由命名空间自己承担
  （设计 §19 规定导入内容不得声明 HumanConfirmed，provenance 由命名空间给出），
  per-kind 门约束的是 Mira 自己的断言。
- 既有写入路径全部合规，本轮不改动任何生产写入者：确定性 consolidation（Verified
  facts 与 Observed episode）、学习路径（Verified episode 与 recovery lesson）、
  procedure 索引（Unverified procedure，注册表派生而非运行验证）、M23 working-context
  提升（带命名空间的派生投影）与模型辅助候选（命名空间 `consolidation-model`）。

### 5. 查询按 kind 声明的检索腿运行

查询声明 kind 时，存储只运行这些 kind 都声明可用的检索腿。结构化 kind（Episode /
Procedure / RecoveryLesson / SkillHint）不参与嵌入相似度召回：它们的 statement 是
规范 JSON，嵌入相似度对它们没有意义。

Exact 与 Lexical 腿对所有 kind 都可用：冲突检测管线（`exact_terms` + kind 过滤）与
失败签名召回都依赖 Exact 腿。

被策略扣下的嵌入**不构成排序请求**：否则查询会因「没有排名腿命中」而返回空集，而
kind 过滤后的集合本来就是调用方要的答案。`MemoryQueryQuality::semantic_leg_withheld`
报告该事实；这不是降级（`degraded` 不置位）。

### 6. 策略表是公开契约

`MemoryKindPolicy`、`memory_kind_policy()`、`check_memory_kind_policy()`、
`memory_default_expiry()`、`memory_verification_rank()` 与
`MemoryQueryQuality::semantic_leg_withheld` 都进入公开头文件，宿主可读取同一张表并
预检自己的写入。设计文档 §6.3 与 API 文档同步给出表内容。

## 备选方案

- **把策略表放进存储选项（`SqliteMemoryStoreOptions`）**：拒绝。策略属于 Core 契约，
  放进存储会让非 SQLite 后端与宿主各持一份表，kind 的含义随后端漂移。
- **把策略门只放在 `MemoryConsolidator`**：拒绝。那样绕过 consolidation 直接调用
  `IMemory::apply()` 的宿主就不受约束，而 `apply()` 才是真正的写入口。
- **把验证等级下限与允许 scope 做成可关闭的开关**：拒绝。默认值可关闭等于没有默认值；
  宿主要豁免某类写入，应使用 `source_namespace` 这条已有的、有设计依据的导入契约。
- **不实现检索腿这一轴**：拒绝。四个轴是 §6.2 一起承诺的；只做三个会留下同样的
  缺口，且结构化记录被送进嵌入腿是实际存在的浪费。
- **对所有 kind 一律禁止嵌入召回**：拒绝。偏好与环境/应用事实是散文，语义召回对它们
  有效。

## 影响与风险

- **既有行为收紧**：低于 kind 验证等级的 claim、kind 与 scope 不匹配的 claim 不再能
  写入。既有仓库内写入路径全部合规；外部宿主若曾把未观察的环境事实作为
  `EnvironmentFact` 写入，会开始收到 `InvalidRecord`。
- **新增默认到期时间**：记录默认带到期时间，`compact()` 会按 TTL 物理清除到期的
  活记录（连带其 Artifact）。这是 §5「带 TTL」的应有行为，但此前从未发生。
- **历史行**：策略生效前写入的记录仍可读、可查；它们的更新需要把验证等级提到对应
  kind 的下限（否则被 `InvalidRecord` 拒绝），退役不受该门约束。
- **查询结果变化**：声明 kind 的查询不再走嵌入相似度腿（结构化 kind）。未声明 kind
  的查询行为不变。
- `IMemory` 的公开方法签名不变；新增的是 Core 契约函数与 `MemoryQueryQuality` 的一个
  布尔字段（追加式变更）。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

- `tests/m4/m4_contracts_test.cpp` 新增 `memory_kind_policy_table_and_gate`：每个 kind
  都有完整表项；验证等级有序；合规 claim 通过门；低于下限的 claim 返回
  `InvalidRecord`；越界的 kind/scope 组合返回 `ScopeDenied`；带命名空间的记录被豁免；
  默认到期时间与表项一致；写入门拒绝违规 claim 而放行 Tombstone。
- `tests/m4/m4_sqlite_memory_test.cpp` 新增 `kind_policy_bounds_the_store`：写入后
  `expires_at` 等于 `recorded_at` + kind 默认寿命；低于下限与越界组合被拒；带命名
  空间的记录写入成功；Tombstone 与 Supersede 清空到期时间而后继记录带自己的到期
  时间。
- 同文件新增 `kind_policy_withholds_the_semantic_leg`：kind 过滤的 Episode 查询扣下
  语义腿并报告 `semantic_leg_withheld`，返回 kind 过滤后的集合；无 kind 过滤时语义腿
  照常运行。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-memory-kind-policy`](../plans/maintenance-2026-10-memory-kind-policy.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §0、§5、§6.2、§6.3、§13.4、§14.1、§19
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 相关：[DEC-057](DEC-057-scoped-memory-read-and-apply-check-order.md)、
  [DEC-058](DEC-058-mutation-ledger-scope-binding.md)
