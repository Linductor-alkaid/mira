# M30：长期记忆接入——提取、检索、纠正、删除与回忆反馈

> 状态：Planned（2026-10-28 随 [M28](m28-interaction-coordinator-first-stage.md)
> 一并登记；**§4 契约语义已由
> [DEC-070](../decisions/DEC-070-long-term-memory-behavior-contract.md) 冻结**，
> 剩余的实现细则与门禁待本里程碑立项时完成）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)
> 的记忆面与
> [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
> 的价值层；来源需求为
> [Issue #111](https://github.com/Linductor-alkaid/mira/issues/111) §4）
> 前置：M28 关闭（**未满足**）；M29 关闭（**未满足**，本阶段依赖上下文归属与并发归属）；
> M23 Memory Promotion 关闭（已满足，[DEC-035](../decisions/DEC-035-context-curator-working-context.md)）；
> DEC-070 契约冻结（已满足，2026-10-28 随 M28 登记一并冻结）
> 建议发布点：非发布物；M31 价值驱动淘汰的输入侧前提
> 更新日期：2026-10-28（登记）

## 1. 目标

把长期记忆从「已有的存储与检索组件」接到「统一交互」的生命周期上：

1. **写入**：从交互与已验证结果提出长期偏好、事件经历、事实、经验等候选，
   经既有 consolidation / promotion 管线校验后写入。
2. **选择**：按当前任务选择**实际需要**的记忆进入上下文；全局偏好与任务局部要求
   的生效范围显式区分。
3. **纠正与删除**：用户纠正、「以后记住……」与「忘掉……」能关联到正确记录/范围，
   经既有权限与 policy 处理；冲突、来源、有效期与删除状态**可解释**。
4. **召回排序（可选增量）**：可验证回忆反馈、importance/使用信号与查询相关性分离的排序。
   **是否新增 API、分几步做由立项评审决定**——不得先冻结激活公式而遗漏完整交互闭环。

## 2. 范围与非目标

### 2.1 范围

- 协调层与 `IMemory` 之间的**记忆选择**接线：按当前工作召回、投影、装配。
- 记忆**候选**的提出与经既有 `MemoryConsolidator` 管线的写入路径。
- 「以后记住……」「忘掉……」的**意图路由**与作用范围判定。
- 主体身份（`PrincipalContext.user_id`）与记忆 ACL 域（`MemoryScope.subject_id`）
  之间的**映射规则**落地（DEC-046 登记但未提供）。
- 可选的回忆反馈与使用信号增量。

### 2.2 非目标

- **不做** 向量索引生产化：`IEmbeddingProvider` 生产实现、ANN 索引
  （当前向量腿是**有界线性余弦扫描**，`max_vector_scan=1'024`，
  [`include/mira/sqlite_memory_store.hpp:26`](../../include/mira/sqlite_memory_store.hpp#L26)）。
- **不做** 容量准入、归档与遗忘（归 M31）。
- **不做** 记忆中的授权描述替代当前有效授权：记忆里的 grant 描述**不能**当作授权来源。
- **不做** 跨 session/tenant 泄漏；跨会话召回继续服从**精确 scope/tenant ACL**。
- **不做** UI 的记忆管理界面（Mirage 负责）。
- **不新增**第二套记忆写入路径：写入**必须**走既有 consolidation / promotion 管线。

## 3. 设计与决策依据

- [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
  §2（计入范围）、§3（分层遗忘）、§4（保护集合）——本阶段提供**价值信号**，
  M31 提供**淘汰裁决**。
- [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)
  §6（上下文组织）——本阶段提供「哪些记忆进入该工作的上下文」。
- [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)（学习闭环）、
  [DEC-035](../decisions/DEC-035-context-curator-working-context.md)（Working Context 与晋升）、
  [DEC-057](../decisions/DEC-057-scoped-memory-read-and-apply-check-order.md)（scope 读取与 apply 顺序）、
  [DEC-058](../decisions/DEC-058-mutation-ledger-scope-binding.md)（台账 scope 绑定）、
  [DEC-059](../decisions/DEC-059-memory-kind-policy.md)（per-kind 策略）、
  [DEC-061](../decisions/DEC-061-memory-claim-key.md)（claim key 与冲突检测）、
  [DEC-064](../decisions/DEC-064-memory-event-surface.md)（记忆事件面）。
- [Context 与 Memory 设计](../design/context_and_memory_design.md)
  §5（记忆层级）、§6.1（MemoryScope）、§13（长期 Memory 写入）、§14（混合检索）。
- [Context Curator 设计](../design/context_curator_design.md) §13（Stage W4 晋升纪律）。

## 4. 冻结的契约语义（2026-10-28 由 DEC-070 冻结）

§4 的**行为契约**已由
[DEC-070](../decisions/DEC-070-long-term-memory-behavior-contract.md) 冻结：
写入（§1）、检索与选择（§2）、主体 → `MemoryScope` 映射（§3）、
承诺实体（§4）、纠正与删除（§5）、排序增量的延后（§6）。
门禁矩阵 `M30-G1`–`M30-G8` 同步定义于 DEC-070「验证方式」。

本里程碑立项时**仍须**回答并冻结的实现细则：

1. 承诺实体的**具体存储形态**（DEC-070 §4/P3 要求：可查询、可纠正、可删除、
   计入 DEC-069 预算、属保护集合；具体类型与表结构由本里程碑冻结）。
2. 主体 → `MemoryScope` 映射的**稳定内部表示**的具体生成方式
   （DEC-070 §3 已冻结语义：稳定、不可逆、不含外部账号信息）。
3. 候选提出的**具体触发点**（哪些既有管线节点提出候选、提出什么 kind/scope）。
4. 与 DEC-069 §3 四层遗忘的**接口实现**：本阶段只产出**价值信号**，
   淘汰裁决归 M31；**不得**在本阶段实现物理删除。
5. 召回排序增量的**分步方案**：使用信号与查询相关性**分离**，
   可验证回忆反馈的可选增量——按 DEC-070 §6/O4，在 §4 闭环交付**之后**另行评审，
   **不并入**本里程碑验收。

## 5. 工作项（立项时细化）

- [ ] `M30-01` 立项与跑前冻结。
- [ ] `M30-02` 主体 → MemoryScope 映射规则与租户隔离落地。
- [ ] `M30-03` 记忆候选提出与经既有管线的写入路径。
- [ ] `M30-04` 按当前工作的记忆选择与上下文装配接线。
- [ ] `M30-05` 「记住 / 忘掉 / 纠正」的路由、范围判定与可解释输出。
- [ ] `M30-06` 价值信号（importance / 使用信号）产出（供 M31 使用）。
- [ ] `M30-07` IVA 测试矩阵（`tests/m30/`）。
- [ ] `M30-08` 文档同步。

## 6. 测试与退出条件

- [ ] 工作项与门禁全部勾选，验证记录落档。
- [ ] 覆盖 issue #111 的验收场景 5（跨时记忆：偏好被认可、被纠正、被删除）。
- [ ] 覆盖跨 session/tenant/scope 的**不泄漏**用例。
- [ ] 「频繁提到 ≠ 更可信」的负向用例。
- [ ] CI 全矩阵绿；受影响的决策/设计/API 手册/术语表已同步。
- [ ] 未执行的真实模型与平台验证保持未勾选并记录原因与补跑条件。

## 7. 风险与阻塞

- **依赖 M28/M29**：本阶段依赖上下文归属与并发归属已冻结。
- **价值公式风险**：issue #111 §4 明确「不应先冻结激活公式而遗漏完整交互闭环」。
  本阶段**先交付闭环**，排序增量按可选项推进；**未经验证的模型评分不得**成为
  全部规则（DEC-069 §3/F1）。
- **范围界定风险**：`MemoryScope.subject_id`（记忆 ACL 域）与
  `PrincipalContext.user_id`（安全主体）是**两个不同语义**（DEC-046），
  映射规则必须显式，不得隐式复用同一字符串。

## 8. 验证记录

> 尚无记录。
