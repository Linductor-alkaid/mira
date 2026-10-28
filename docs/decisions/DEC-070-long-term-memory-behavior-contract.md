# DEC-070：长期记忆的行为契约

> 状态：Proposed
> 日期：2026-10-28
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 替代/被替代：无
> 来源需求：[Issue #111（`MIRA-20261027-001`）](https://github.com/Linductor-alkaid/mira/issues/111)
> §4（长期记忆的写入、检索、纠正与遗忘）
> 补位说明：[DEC-068](DEC-068-interaction-coordinator-and-principal-input-entry.md)
> 承接 §1–§3、§5；[DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
> 承接 §6；**§4 在本决策之前没有 Mira 侧承接**

## 背景与问题

### 现状核对（2026-10-28 静态审计）

Mira 的记忆子系统**已经相当完整**，缺的不是组件而是**行为契约**：

- **已有能力**：`MemoryScope`（scope ACL，deny-by-default）、
  `MemoryKind` 与 per-kind 策略表（`memory_kind_policy`）、
  `MemoryClaimKey` 与 `memory_same_claim`（同主同值即同一 claim）、
  `MemoryVerification`（`Unverified < Observed < Verified < HumanConfirmed`）、
  `MemoryMutation` 与乐观并发 `expected_version`、
  `IMemory`（`query`/`get`/`apply`/`compact`/`erase`）、
  `IMemoryRetentionSweep`（跨 scope 有界清扫）、
  `MemoryConsolidator`（共享晋升管线）、
  `WorkingContextPromotionPolicy`（Working Context → 长期记忆）、
  事件面（[DEC-064](DEC-064-memory-event-surface.md)）。
- **写时自动盖章**：`expires_at` 由存储按 kind 默认折算
  （[`src/storage/sqlite_memory_store.cpp:669-675`](../../src/storage/sqlite_memory_store.cpp#L669-L675)），
  调用方无义务。
- **记忆层级**已冻结（[Context 与 Memory 设计 §5](../design/context_and_memory_design.md)）：
  Request Context / Working Memory / TaskCheckpoint / Episodic / Semantic / Procedural /
  Event+Artifact。

### 缺口

| # | 缺口 | 依据 |
| --- | --- | --- |
| M1 | **主体身份 → `MemoryScope` 的映射规则未提供**。DEC-046 明确划界（「`subject_id` 是记忆访问控制域，不是环境实体身份」）但只**登记**未**给出映射** | [DEC-046](DEC-046-entity-vocabulary-alignment.md) 决策 3 |
| M2 | **「承诺（promise）」没有承载实体**。承诺被定义为「独立任务/约束状态管理」，「不能只靠无限期记忆命中维持调度」，但 `MemoryRecord` 没有 `promise_id`/`fulfilled`/`reminder_count` 之类字段 | Mirage DEC-119 与路线图 §4.4 的承诺约定 |
| M3 | **用户纠正与「忘掉……」的行为契约未冻结**：关联到哪条记录/哪个范围、冲突怎么裁决、删除状态如何可解释 | issue #111 §4 |
| M4 | **「频繁提到 ≠ 更可信」没有契约**：现有 `verification` 等级是**单调**的，但没有任何规则说明「重复提及」不得提升它 | issue #111 §4 原文 |
| M5 | **记忆中的授权描述**与**当前有效授权**的关系未冻结 | issue #111 §4 |
| M6 | **物理删除的归属已在 DEC-069**，但**「谁有权删除」**（用户 / 系统 / 保留期 / 归档）不在任何决策里 | issue #111 §4 与 §6 的交叉 |

## 决策

### 1. 写入：候选 → 既有管线 → 校验 → 提交

- **W1（不新增写入路径）** 长期记忆的写入**必须**经既有 consolidation / promotion 管线
  （`MemoryConsolidator` 与 `WorkingContextPromotionPolicy`）。
  **禁止**新增第二套写入路径、旁路 `IMemory::apply` 的直写，或在宿主侧维护影子记忆库。
- **W2（模型只提候选）** 模型可以提出候选（statement、kind、scope、验证等级），
  但**确定性 owner** 校验版本、引用、权限、预算与保护条件后才提交
  （与 [DEC-069 §3/F1](DEC-069-bounded-capacity-and-forgetting-lifecycle.md) 的确定性纪律一致）。
  未经校验的模型输出**不得**成为已验证事实。
- **W3（验证等级不得由提及次数提升）** `MemoryVerification` 的提升**只能**来自
  既有可验证来源：显式用户确认（`HumanConfirmed`）、已验证事件（`Observed`）、
  经管线固化的经验（`Verified`）。
  **重复提及、重复检索命中、模型评分都不得提升验证等级。**
  「频繁提到一件事不等于它变得可信」在本决策中是**契约**，不是建议。
- **W4（冲突不静默覆盖）** 同一 claim key 上的冲突（`memory_same_claim` 判定为同一主张
  但断言值不同）**不得**静默覆盖；按既有 `MutationReasonCode` 与
  `MemoryStatus`（`Active`/`Superseded`/`Tombstoned`）裁决，并保留来源可追溯。
- **W5（双时态）** `validity` 与 `recorded_at` 语义不变；纠正写**新版本**而非原地改写
  （既有乐观并发语义）。

### 2. 检索与选择：按当前工作，白名单 deny-by-default

- **S1（选择而非拼接）** 每次模型请求按**当前工作**选择记忆：
  目标、任务状态、约束与相关偏好；**不得**以「拼接所有会话历史」处理
  （与 [DEC-068 §6](DEC-068-interaction-coordinator-and-principal-input-entry.md) 一致）。
- **S2（scope 白名单强制）** `MemoryQuery::scopes` 必须显式给出且非空，
  否则 `ScopeDenied`（既有语义，`deny-by-default`）。
  跨会话召回**继续服从精确 scope/tenant ACL**。
- **S3（同一入口不取消隔离）** 统一输入口**不**削弱任务隔离、授权与隐私删除约束。
- **S4（层 0 仍是唯一请求期准入）** 记忆条目进入模型请求仍只经
  Layer 0 准入转换；本决策**不**新增第二条准入路径。

### 3. 主体身份 → `MemoryScope` 的映射规则（补 M1）

冻结一条**显式**映射规则，作为 DEC-046 登记项的实现细则：

```text
MemoryScope{kind = User,  subject_id = <UserId 的稳定内部表示>}
MemoryScope{kind = Session, subject_id = <SessionId 的稳定内部表示>}
MemoryScope{kind = Application, subject_id = <Application 的稳定内部标识>}
```

- 映射是**单向且确定**的：`UserId` → `subject_id` 是稳定映射；
  `subject_id` **不得**反推 `UserId`。
- `subject_id` **不得**直接使用外部账号信息（邮箱、用户名）；
  必须使用稳定内部标识或不可逆映射
  （[Context 与 Memory 设计 §6.1](../design/context_and_memory_design.md)）。
- 租户隔离仍由 `MemoryScope::tenant_id` 承担，**不**隐含在 `subject_id` 里。
- **记忆中的授权描述不能代替当前有效授权**：召回出的记录里即便含有
  「用户允许我做 X」的表述，也**不构成**能力授权；
  授权只来自 `PrincipalContext::grants` 与 `PolicyEngine`
  （[DEC-068 §5/R9](DEC-068-interaction-coordinator-and-principal-input-entry.md)）。

### 4. 承诺（promise）是独立实体，不是普通记忆（补 M2）

- **P1（独立承载）** 承诺**不**是普通情景记忆条目。它是**受配额保护、优先保留**
  的独立实体，具有稳定标识、期限、兑现状态与提醒计数。
- **P2（不得靠相似度维持）** 承诺的到期检查**不得**依赖「无限期记忆命中」；
  调度依据来自承诺实体本身，而不是检索。
- **P3（承载位置）** 承诺实体的**存储形态**属实现选择，但**必须**：
  - 计入 [DEC-069 §2](DEC-069-bounded-capacity-and-forgetting-lifecycle.md) 的总量预算；
  - 属 DEC-069 §4 的**保护集合**成员（未兑现承诺不按普通情景淘汰）；
  - 受「用户固定记忆有限配额」（DEC-069 §4/P3）约束，**不是**无限期豁免；
  - 可被用户**查看、纠正与删除**，删除后不得被召回重新生成。
- **P4（兑现状态可解释）** 用户询问「我答应过什么」时，回答**必须**能区分
  已兑现 / 未兑现 / 已过期 / 已删除，并说明依据。

### 5. 纠正与删除（补 M3、M6）

- **C1（纠正是 mutation，不是覆盖）** 用户纠正走 `MemoryMutation`
  （`Update`/`Supersede`），带 `MutationReasonCode::HumanCorrection`，
  保留原记录可追溯；**不得**原地抹除。
- **C2（删除有四类发起者）** 记忆删除可由四类发起：
  | 发起者 | 语义 | 路径 |
  | --- | --- | --- |
  | 用户显式删除（「忘掉…」） | 立即、针对明确记录或范围 | `IMemory::erase`（既有） |
  | 保留期到期 | 按 kind TTL | `IMemoryRetentionSweep`（既有） |
  | 容量压力淘汰 | 按冻结价值序 | DEC-069 §3（**M31**） |
  | 归档清理 | 按归档保留期 | DEC-069 §5–§7（**M31**） |
  四类**分别结算**，**不得**互相冒充（尤其容量回收**不**释放 `Erasure Pending` 的 hold，
  DEC-069 §7/X2）。
- **C3（删除状态可解释）** 对用户解释删除时**必须**说明：删了什么范围、
  由哪类发起者发起、是否仍可回查、哪些派生数据（摘要/索引/备份）随之不可回查。
  **不得**用摘要或模型猜测重建已删正文。
- **C4（授权与 policy）** 用户删除与纠正经既有权限与 policy 处理；
  范围删除的合法性与 scope ACL 一致；**不得**借此读取未授权 scope。
- **C5（失败轮次正文受保护）** Mirage DEC-104 要求：宿主后置核验判失败后，
  模型已产生的最终答案**仍保留为正文**。因此**已结算的失败轮次正文**
  在 DEC-069 的四层遗忘中**不按普通低价值内容清理**——
  它属保护集合成员，或在淘汰时保留其「正文存在但核验未过」的语义标记。
  （补 DEC-069 §4 保护集合的成员清单。）

### 6. 可选增量：回忆反馈与激活排序（本轮**不**冻结公式）

issue #111 §4 明确：可验证回忆反馈、importance/使用信号与查询相关性**分离**的排序
「属于本层可选增量。是否新增 API、分几步做由上游评审，不应先冻结激活公式而遗漏完整交互闭环」。

- **O1（先闭环后排序）** 本决策**只**冻结 §4 的**行为契约**（写入/选择/纠正/删除/承诺/解释）。
  排序公式与回忆反馈 API **不在**本决策范围。
- **O2（排序的既有基础已具备）** 检索侧已有三腿（exact / FTS / semantic）与权重表
  （`RetrievalWeights`），importance/使用信号属于**可增量**的方向，
  不需要新建检索路径。
- **O3（排序不得成为唯一价值依据）** 排序权重**不得**单独决定物理删除；
  删除裁决属 DEC-069 §3 的冻结价值序。
- **O4（延后到 M30 之后）** 排序增量与回忆反馈 API 的取舍，
  在 M30 交付 §4 闭环后**另行评审**，不并入 M30 验收。

## 备选方案

- **新建「承诺记忆库」**：否决。承诺是**受保护的独立实体**，
  不是「一种新的 MemoryKind」——把它塞进 `MemoryScopeKind`/`MemoryKind` 闭集
  会破坏既有 per-kind 策略表与 TTL 语义，且无法表达兑现状态。
- **让宿主直接写 `IMemory::apply`**：否决。会绕过 consolidation 管线的验证与预算校验
  （W2）。
- **用「检索命中次数」提升验证等级**：否决（W3）——这是 issue #111 §4 明确点名的反模式。
- **把 `subject_id` 直接设为外部账号**：否决。违反脱敏与稳定标识要求；
  `MemoryScope` 的 subject 必须是稳定内部标识或不可逆映射。
- **承诺靠「每天扫一遍记忆找 deadline」维持**：否决（P2）——记忆会被检索排序裁掉，
  承诺的调度不能依赖召回。
- **把用户纠正做成原地覆盖**：否决（C1）——破坏可追溯性与冲突解释。
- **把物理删除纳入本决策**：否决。物理删除的资格、序与度量已在 DEC-069 冻结；
  本决策只定义**谁有权删除**与**如何解释**（C2/C3）。
- **本轮就冻结激活公式**：否决（O1/O4）——issue #111 §4 明确反对先冻结公式。

## 影响与风险

- **新增公开契约面**：承诺实体的公开类型（标识、期限、兑现状态、提醒计数）
  与其查询/纠正/删除入口；主体 → `MemoryScope` 映射规则**文档化**（不新增 API，
  复用既有 `MemoryScope`）。
- **既有契约**：**不改** `IMemory`、`MemoryScope`、`MemoryKind`、`MemoryRecord`、
  `MemoryMutation` 的既有字段语义。承诺实体是**新增**类型。
- **风险 D-1（承诺实体变成第二个记忆库）**：缓解：承诺只有一类、
  只有兑现状态这一个可变状态；不引入自由文本库；纳入 DEC-069 预算与保护集合；
  交付时必须证明它**没有**形成第二套检索路径。
- **风险 D-2（映射规则被误用为身份等价）**：缓解：映射单向；`subject_id` 不可反推
  `UserId`；租户隔离仍独立字段。
- **风险 D-3（删除解释与实际状态不一致）**：缓解：C3 要求解释与四类发起者一致；
  DEC-069 的三种度量（逻辑删除/可重用空间/实际占用）提供可核对的事实。
- **风险 D-4（排序增量被提前引入）**：缓解：O1/O3 明确排序不构成删除依据；
  排序增量另案评审。
- **本决策不涉及 Executor 能力边界**：记忆写入/检索/纠正/删除全部落在既有
  `IMemory` 与 `MemoryConsolidator` 通道内；**不登记** Executor 反馈台账。

## 验证方式

由 [M30 计划](../plans/m30-long-term-memory-integration.md) 承载，
门禁矩阵 `M30-G1`–`M30-G8`：

- `M30-G1` 写入路径唯一性：候选 → 既有管线 → 校验 → 提交；
  宿主直写 `IMemory::apply` 被拒或被标记为绕过。
- `M30-G2` 验证等级单调性且**不被提及次数提升**；
  同一 claim 的重复写入不提升等级。
- `M30-G3` 主体 → scope 映射：User/Session/Application 三种映射可复现；
  `subject_id` 不可反推 `UserId`；跨租户不可见。
- `M30-G4` 承诺实体：可创建、纠正、删除；到期检查不依赖检索；
  兑现状态可解释；删除后不被召回重建。
- `M30-G5` 纠正与删除：用户纠正保留原记录可追溯；
  四类删除发起者分别结算且互不冒充；容量回收**不**释放 Erasure hold。
- `M30-G6` 解释面：已删内容明确不可回查；**不**用摘要或模型猜测重建正文；
  失败轮次正文仍作为正文保留。
- `M30-G7` 授权隔离：记忆中的授权表述**不**构成能力授权。
- `M30-G8` 架构门禁与契约四件套同步。

真实模型语义质量（召回准确率、巩固正确性）按 issue #111 的要求留待
标注场景与真实模型评估轮；M30 只声明管线行为指标（RULE-10）。

## 关联文档和工作项

- 工作项：[`M30：长期记忆接入——提取、检索、纠正、删除与回忆反馈`](../plans/m30-long-term-memory-integration.md)
- 来源需求：[Issue #111](https://github.com/Linductor-alkaid/mira/issues/111) §4
- 相关决策：[DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](DEC-030-learning-loop-runtime-semantics.md)、
  [DEC-035](DEC-035-context-curator-working-context.md)、
  [DEC-046](DEC-046-entity-vocabulary-alignment.md)（身份词表划界）、
  [DEC-057](DEC-057-scoped-memory-read-and-apply-check-order.md)、
  [DEC-058](DEC-058-mutation-ledger-scope-binding.md)、
  [DEC-059](DEC-059-memory-kind-policy.md)、
  [DEC-061](DEC-061-memory-claim-key.md)、
  [DEC-064](DEC-064-memory-event-surface.md)、
  [DEC-068](DEC-068-interaction-coordinator-and-principal-input-entry.md)、
  [DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
- 设计：[Context 与 Memory 设计](../design/context_and_memory_design.md)、
  [Context Curator 设计](../design/context_curator_design.md)
- 术语：[公共术语表](../project/glossary.md)
