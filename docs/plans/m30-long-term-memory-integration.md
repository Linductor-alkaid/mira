# M30：长期记忆接入——提取、检索、纠正、删除与回忆反馈

> 状态：In Progress（当前状态见工作项，证据见 [M30 验收文档](m30-memory-acceptance.md)）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)
> 的记忆面与
> [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
> 的价值层；来源需求为
> [Issue #111](https://github.com/Linductor-alkaid/mira/issues/111) §4）
> 前置：M28 关闭（**已满足**）；M29 关闭（**已满足**，PR #124 CI 24 项通过）；
> M23 Memory Promotion 关闭（已满足，[DEC-035](../decisions/DEC-035-context-curator-working-context.md)）；
> DEC-070 契约冻结（已满足，2026-10-28 随 M28 登记一并冻结）
> 建议发布点：非发布物；M31 价值驱动淘汰的输入侧前提
> 更新日期：2026-11-26

mira-bridge 集成反馈及各项验证状态见 [宿主反馈维护计划](maintenance-2026-11-26-mira-bridge-feedback.md)。

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
5. **分类读写**：按 [DEC-119](../decisions/DEC-119-six-memory-systems.md) 的六类契约接入
   人格、事件记忆、社交对象画像、世界模型记忆、目标记忆与技能记忆。

## 2. 范围与非目标

### 2.1 范围

- 协调层与 `IMemory` 之间的**记忆选择**接线：按当前工作召回、投影、装配。
- 记忆**候选**的提出与经既有 `MemoryConsolidator` 管线的写入路径。
- 「以后记住……」「忘掉……」的**意图路由**与作用范围判定。
- 主体身份（`PrincipalContext.user_id`）与记忆 ACL 域（`MemoryScope.subject_id`）
  之间的**映射规则**落地（DEC-046 登记但未提供）。
- 可选的回忆反馈与使用信号增量。

### 2.2 非目标

- 向量召回复用 DEC-111/113 的供给与混合检索，ANN 与规模优化在专项验证后立项。
- **不做** 容量准入、归档与遗忘（归 M31）。
- **不做** 记忆中的授权描述替代当前有效授权：记忆里的 grant 描述**不能**当作授权来源。
- **不做** 跨 session/tenant 泄漏；跨会话召回继续服从**精确 scope/tenant ACL**。
- **不做** UI 的记忆管理界面（Mirage 负责）。
- **不新增**第二套记忆写入路径：写入**必须**走既有 consolidation / promotion 管线。

## 3. 设计与决策依据

- [DEC-123](../decisions/DEC-123-social-identity-directory.md)：社交对象身份与别名索引。
- [DEC-124](../decisions/DEC-124-social-attribute-assertions.md)：画像字段断言与时态检索。
- [DEC-125](../decisions/DEC-125-social-relationship-assertions.md)：关系断言与双端点清理。
- [DEC-126](../decisions/DEC-126-social-erasure-recovery.md)：目录与记忆库的擦除恢复。
- [DEC-127](../decisions/DEC-127-world-fact-address-and-freshness.md)：世界事实地址与新鲜度。
- [DEC-128](../decisions/DEC-128-navigation-world-fact-refresh.md)：导航重新观察和 AppModel 世代。
- [DEC-129](../decisions/DEC-129-durable-goals-and-commitments.md)：一次性目标与承诺持久状态。
- [DEC-130](../decisions/DEC-130-durable-recurring-goal-plans.md)：重复计划与持久触发标识。
- [DEC-131](../decisions/DEC-131-goal-corrections-and-reminder-receipts.md)：目标纠正与提醒回执。
- [DEC-132](../decisions/DEC-132-calendar-goal-plans.md)：带时区快照的日历计划。
- [DEC-133](../decisions/DEC-133-goal-store-live-authorization.md)：目标存储的当前授权。
- [DEC-134](../decisions/DEC-134-current-skill-procedure-recall.md)：按当前发布版本读取技能记忆。
- [DEC-135](../decisions/DEC-135-scoped-skill-procedure-writes.md)：按学习范围写入技能发布投影。
- [DEC-136](../decisions/DEC-136-exact-recovery-lesson-recall.md)：按工作流版本和失败签名读取恢复经验。
- [DEC-137](../decisions/DEC-137-exact-recovery-during-escalation.md)：自动失败升级优先读取精确恢复经验。
- [DEC-138](../decisions/DEC-138-evidenced-skill-hints.md)：以已结算技能调用为证据记录 SkillHint。
- [DEC-139](../decisions/DEC-139-environment-scoped-skill-memory.md)：技能记忆的环境范围。
- [DEC-140](../decisions/DEC-140-source-account-identity.md)：来源账号与当前交谈对象。
- [DEC-141](../decisions/DEC-141-automatic-record-pages.md)：交互结算后自动处理多页事件。
- [DEC-142](../decisions/DEC-142-social-message-evidence.md)：社交消息事件与发言者证据。
- [DEC-143](../decisions/DEC-143-social-message-references.md)：社交消息的发言者与引用对象。
- [DEC-144](../decisions/DEC-144-social-mention-extraction.md)：从已索引消息提取正文对象引用。
- [DEC-145](../decisions/DEC-145-durable-goal-dispatch-receipts.md)：一次性目标的持久派发身份与回执。
- [DEC-146](../decisions/DEC-146-goal-dispatch-polling.md)：一次性目标的周期到期派发。
- [DEC-148](../decisions/DEC-148-recurring-goal-dispatch.md)：重复目标触发的周期派发与提交回执。
- [DEC-149](../decisions/DEC-149-verified-goal-fulfillment.md)：用 Runtime 完成证据兑现一次性目标。
- [DEC-150](../decisions/DEC-150-goal-fulfillment-polling.md)：用持久回执周期发现待结算的一次性目标。
- [DEC-151](../decisions/DEC-151-live-goal-completion-publication.md)：核对时通过协调层自动发布 live 任务完成证据。
- [DEC-152](../decisions/DEC-152-goal-submission-reconciliation.md)：通过持久派发身份区分首次提交和只读对账。
- [DEC-153](../decisions/DEC-153-durable-task-admission-receipts.md)：以操作 ID 持久记录和回查 Runtime 准入结果。
- [DEC-154](../decisions/DEC-154-durable-before-live-task-admission.md)：在 Runtime 发布任务前持久化准入回执。
- [DEC-155](../decisions/DEC-155-durable-before-live-task-completion.md)：在 Runtime 发布成功终态前持久化验证事实。
- [DEC-156](../decisions/DEC-156-m30-implementation-freeze.md)：冻结 M30 剩余实施细则。
- [DEC-157](../decisions/DEC-157-scoped-durable-record-pages.md)：按写入范围保存 Record 页回执。
- [DEC-158](../decisions/DEC-158-loop-settlement-memory.md)：从设备和会话循环的结算事件提取事件记忆。
- [DEC-159](../decisions/DEC-159-evidenced-social-attribute-recording.md)：从已归因消息接纳社交画像候选。
- [DEC-160](../decisions/DEC-160-evidenced-social-relationship-recording.md)：从已归因消息提取有向社交关系。
- [DEC-161](../decisions/DEC-161-social-relationship-reference-evidence.md)：保留社交关系的前序发言证据。
- [DEC-162](../decisions/DEC-162-verified-world-fact-recording.md)：从已提交的验证事实记录结构化世界观察。
- [DEC-163](../decisions/DEC-163-skill-hint-candidate-report.md)：返回技能提示的候选处置报告。
- [DEC-164](../decisions/DEC-164-evidenced-conversation-commitments.md)：从已结算终答记录一次性承诺并返回创建回执。
- [DEC-165](../decisions/DEC-165-batched-conversation-commitments.md)：批量接纳同一终答中的一次性承诺。
- [DEC-166](../decisions/DEC-166-evidenced-recurring-plan-recording.md)：从已结算终答接纳固定间隔及日历重复计划。
- [DEC-167](../decisions/DEC-167-evidenced-agent-persona-suggestions.md)：从已记录用户消息提交待审人格建议。
- [自动记忆与目标执行参考](../design/m30-memory-automation-references.md)：社交认知、到期执行与通用提取。
- [DEC-122](../decisions/DEC-122-durable-conversation-index.md)：持久片段和联合擦除。
- [DEC-121](../decisions/DEC-121-indexed-event-slices.md)：有界事件切片、索引重建与 Record 水位。
- [DEC-120](../decisions/DEC-120-agent-persona-admission.md)：人格目录、宿主任务绑定与 Gateway 版本准入。
- [DEC-119](../decisions/DEC-119-six-memory-systems.md) 与
  [六类记忆设计](../design/m30-memory-systems.md)：分类、事实源、写入、索引和读取契约。
- [记忆存储研究](../design/memory-storage-reference-study.md)与
  [维护计划](maintenance-2026-11-memory-storage.md)：SQLite 宿主接入、检索额度隔离和
  跨进程示例。持久会话片段索引已由 M30-12 交付，旧 JSON 导入仍待单独冻结。

- [DEC-109](../decisions/DEC-109-memory-preference-approval.md)：持久偏好提案、预览、
  显式确认/拒绝、冻结版本与删除失效；本轮不实现承诺与新世代。

- [DEC-107](../decisions/DEC-107-interaction-memory-lifecycle.md)：首批生命周期、
  确定性 scope 白名单与有界结果；不替代本计划其余实现细则的冻结。

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
- [M36](m36-memory-agent-context-integration.md)交付可复用的受管记忆服务与请求准入；
  本里程碑的主体授权、持久承诺、完整纠正与删除交互及同范围新世代按各自工作项验收。
- [记忆代理与长期上下文参考研究](../design/memory-agent-reference-study.md)
  （2026-11-09，`MNT-202611-MA-01`；供 `M30-01/03/04/08` 评审：宿主记忆服务、
  有界记忆代理、既有写入管线、请求装配与删除失效；仅研究输入，未批准新 API 或实现范围）。

## 4. 冻结的契约语义（2026-10-28 由 DEC-070 冻结）

§4 的**行为契约**已由
[DEC-070](../decisions/DEC-070-long-term-memory-behavior-contract.md) 冻结：
写入（§1）、检索与选择（§2）、主体 → `MemoryScope` 映射（§3）、
承诺实体（§4）、纠正与删除（§5）、排序增量的延后（§6）。
门禁矩阵 `M30-G1`–`M30-G8` 同步定义于 DEC-070「验证方式」。

实施细则见 [DEC-156](../decisions/DEC-156-m30-implementation-freeze.md)。
当前验收判定以[验收矩阵](m30-memory-acceptance.md#1-当前验收矩阵)为准。

### 4.1 实施细则验收

| 工作项 | 所有者、契约与复用路径 | 验收依据 |
| --- | --- | --- |
| M30-02 / M30-15 / M30-37 | 当前主体及版本化 scope 摘要约束 `SqliteGoalStore` 的归属与版本，StoreChannel 和既有派发 Executor 分别负责存取及唤起 | M30-G3/G4 |
| M30-03 / M30-38 | Memory Agent 在结算及输入对账后由 Executor worker 提取，经合并或类型化 owner 写入。页区间、版本、取消及 deadline 约束水位推进 | M30-G1/G2 |
| M30-05 | Interaction 解析受控意图，既有提案和擦除 owner 负责归属、版本、幂等与回执 | M30-G5/G6/G7 |
| M30-06 | Gateway 准入结果提供实际使用证据，记录所有者保存独立的值与使用投影 | M30-G2/G7 |
| M30-09 | Memory Agent 擦除 owner 保留屏障并约束同 scope 世代，执行句柄由原 owner 回收 | M30-G5/G6 |
| M30-07 / M30-08 | 验收文档管理证据和覆盖环境，本计划登记工作归属，新增契约同步设计及 API | M30-G8 |

| 决策点 | 选择 | 备选与理由 | 依据 |
| --- | --- | --- | --- |
| 状态所有者与命令路径 | 复用 Memory Agent、记忆管线、目标与擦除 owner | 影子记忆库会产生互相冲突的状态与删除回执 | DEC-070 / DEC-156 |
| 派生视图与时序 | 页回执、准入使用信号和世代屏障分别绑定其来源及精确范围 | 仅使用搜索命中或到达时间无法证明实际使用与新来源 | DEC-069 / DEC-156 |
| Executor 与依赖 | 复用现有 worker、StoreChannel 和宿主派发生命周期 | 私有队列无法与现有取消、背压和关闭语义统一 | DEC-107 / DEC-141 / DEC-146 |

## 5. 工作项

### 5.1 待完成实施项

后续工作按原编号推进。已交付的基础机制和增量证据见第 5.2 节及[验收文档](m30-memory-acceptance.md#2-按交付能力查证)。

- [ ] `M30-03` 补齐共用的候选收集、处置报告及写入边界验收。六类已有专用增量，分别覆盖待审人格建议、Session Record 与 Episode、社交属性与关系、结构化世界观察、一次性承诺与重复计划、技能提示。显式偏好继续沿既有审批入口。生产统一触发由 M30-38 验收。
- [ ] `M30-05` 补齐「记住、纠正、忘掉」的结构化路由、精确范围与歧义处理、逐记录遗忘及可解释回执。显式偏好批准、版本化纠正与范围擦除已有独立证据。
- [ ] `M30-06` 从有证据的已确认重要性和 Gateway 实际准入产出价值信号，区分未入选、失败准入、幂等重放与擦除，供 M31 消费。
- [ ] `M30-09` 按 DEC-156 接通同 scope 删除后的新世代。旧来源、摘要、审批和在途结果继续受已有擦除屏障阻断，新明确输入可重新接纳。与 M31 联验屏障保留与回收。
- [ ] `M30-36` 补齐社交域自动认知闭环。待接通生产来源适配器的消息记录、跨平台账号同一性和反证纠正，并验证有证据的身份、画像与关系召回。宿主已有 M30-35/40～42 与 DEC-159～161 的账号、归因和候选接口。
- [ ] `M30-37` 把持久到期派发接到生产协调层。在常驻有效授权下，自动发起已承诺目标无需逐次确认，并处理完成证据写入前进程丢失时的执行恢复。已有 M30-43～52 的扫描、持久派发与兑现。M33 消费该路径并负责主动行为的准入与打断预算。
- [ ] `M30-38` 把各类型化写入入口接入统一、默认的持久事件触发与跨重启消费。验收无显式记忆指令的普通交互、分类写入、重放去重和擦除后阻断。

### 5.2 已完成实施增量

以下为已验收的各次实施增量。对应环境和结果见[验收索引](m30-memory-acceptance.md#2-按交付能力查证)。

#### 基础范围、记忆类型与事件记忆

- [x] `M30-01` 立项与实施细则冻结。DEC-107/109 和 DEC-119 的已落地契约由 DEC-156 补齐，验收归属见 §4.1。
- [x] `M30-02` 主体 → MemoryScope 映射规则与租户隔离落地（User/Session/Application 内部摘要，自动 User + 当前 Session 白名单，当前授权及归属校验；Linux 离线验收）。
- [x] `M30-04` 按当前工作的记忆选择与上下文装配接线（目标/最新约束、队列接纳边界、Gateway P4 候选、Layer 0 准入与失效探针；Linux 离线验收）。
- [x] `M30-10` 冻结六类契约并交付宿主可配置的独立召回组，验证类别配额、ACL、
      追加查询、去重和 Gateway 候选失效（Linux 离线首增量）。
- [x] `M30-11` 提供按租户与 Agent ID 读取生效人格的宿主 API 及 Gateway 接线，
      验证重载、回滚、授权、角色边界、预算和在途失效（Linux 离线）。
- [x] `M30-12` 交付持久会话片段索引、召回和联合擦除。消费 M30-17 的有界事件切片，验收时间过滤、重建、重开与联合擦除（Linux 离线）。
- [x] `M30-17` 交付 Session/Task/type 的有界事件切片和 Record 取证水位供 M30-12 等入口消费。验收空页、重开、时间边界、错误传播及擦除屏障（Linux 离线）。
- [x] `M30-39` 接通结算后同一 Record 作业内的有界多页提取，验收固定快照、页上限、部分失败和续跑（Linux 离线）。

#### 社交与世界事实

- [x] `M30-18` 交付 M30-13 的社交身份目录，验收同名、跨域、版本覆盖与擦除后重开。
- [x] `M30-19` 交付 M30-13 的画像字段断言，验收来源、有效区间、记录时间、字段索引与跨域隔离。
- [x] `M30-20` 交付有向关系断言，验收有序端点、谓词与时态索引、重开和对象双端擦除。
- [x] `M30-21` 交付目录与记忆库的联合擦除恢复，验收重开、数据库绑定及环境范围隔离。
- [x] `M30-22` 交付结构化世界事实写入及双时态读取，验收隔离、过期、刷新与重开。
- [x] `M30-23` 交付导航重新观察及 AppModel 版本联动，验收过期、换版、取消和撤权。
- [x] `M30-35` 交付消息来源账号的持久身份解析与首次接纳，验收跨来源绑定、同名、重放、迁移和擦除（Linux 离线）。
- [x] `M30-40` 交付已持久消息的发言归因索引，验收同会话多来源、证据冲突、有界召回、重开和擦除后重放（Linux 离线）。
- [x] `M30-41` 接通已索引社交消息的参与者查询，验收发言者、账号提及、引用消息、同名候选和擦除隔离（Linux 离线）。
- [x] `M30-42` 交付已索引对话的模型正文引用提取，验收群聊中名称、前序发言指代、证据边界、取消和擦除（Linux 离线）。

#### 目标与承诺

- [x] `M30-24` 交付一次性任务与承诺的持久状态和到期索引，验收归属、结算、重开和擦除。
- [x] `M30-25` 交付固定 UTC 间隔重复计划，验收到期索引、版本化触发、重开去重和取消。
- [x] `M30-26` 交付一次性目标纠正与提醒回执，验收历史、计数、重试、重开和联合擦除。
- [x] `M30-27` 交付按本地时分与星期计算的日历计划，验收夏令时、规则耗尽、重开和换版。
- [x] `M30-28` 交付目标存储的当前授权入口，验收精确归属、操作隔离、撤权与重试（Linux 离线）。
- [x] `M30-43` 交付一次性目标的待派发查询、稳定准备身份与提交回执，验收重启去重、纠正、取消和擦除（Linux 离线）。
- [x] `M30-44` 通过 Executor 周期发现一次性到期目标，复用稳定 ID 调用宿主任务提交接口，验收分页、重开续派、取消和关闭（Linux 离线）。
- [x] `M30-45` 通过同一 Executor 派发器发现重复计划到期触发，在提交确认后原子推进计划，验收历史回执、积压补跑、重启对账、撤权和擦除（Linux 离线）。
- [x] `M30-46` 通过唯一的持久任务完成证据兑现原版本的一次性目标，验收 Runtime 终态和主体归属、重启重放、冲突及擦除（Linux 离线）。
- [x] `M30-47` 在 Executor 派发器内分页核对已派发的一次性目标并补结算，验收重开、授权、过期后的合法兑现及其他派发继续推进（Linux 离线）。
- [x] `M30-48` 在待结算扫描中自动从匹配的 live Runtime 终态发布完成证据并兑现，验收并存任务、授权恢复和跨重开补结算（Linux 离线）。
- [x] `M30-49` 在一次性目标和重复计划的派发器中区分首次提交与按原操作 ID 的只读对账，验收并发、重开与后续项继续推进（Linux 离线）。
- [x] `M30-50` 持久记录 Runtime 已确认的任务准入并按原操作 ID 对账，验收实际命令、归属、重启与回填（Linux 离线）。
- [x] `M30-51` 在任务发布到 Runtime 注册表前确认持久准入回执，验收可见性、失败拒绝、归属和重启对账（Linux 离线）。
- [x] `M30-52` 在 Runtime 发布成功终态前确认持久验证事实，验收可见性、失败重试、接管、取消和重启兑现（Linux 离线）。

#### 技能记忆

- [x] `M30-29` 交付按当前已授权发布版本读取 `Procedure`，验收版本切换、撤回、归属和重开（Linux 离线）。
- [x] `M30-30` 交付同一技能发布在多个学习 scope 的独立写入，验收旧回执、擦除与并发同步（Linux 离线）。
- [x] `M30-31` 交付宿主按当前学习 scope、工作流版本与失败签名读取恢复经验，验收旧库和范围隔离（Linux 离线）。
- [x] `M30-32` 接通自动失败升级的精确恢复经验检索，验收总预算、回补和迟到失效（Linux 离线）。
- [x] `M30-33` 交付带技能调用证据的 `SkillHint` 写入与当前版本读取，验收归属、撤权和擦除（Linux 离线）。
- [x] `M30-34` 交付按工作或社交环境隔离的技能记忆范围，验收换绑、事件与技能隔离及擦除。

### 5.3 能力验收汇总

保留原编号以便追溯。下列是对子项的验收汇总，后续工作由对应的实施项或发布门禁承接。

| 原编号 | 已验收的实现范围 | 后续依赖 |
| --- | --- | --- |
| `M30-13` 社交身份、画像及关系存储 | M30-18～21 已完成。稳定对象和关系、同名歧义、属性证据、环境内索引和联合擦除均有 Linux 验收 | 生产消息、跨来源同人判断与自动认知闭环见 M30-36 |
| `M30-14` 世界模型事实 | M30-22/23 已完成（Linux / Simulator）。环境与应用事实、双时态和过期重新观察均有证据 | 生产环境键与目标平台见验收文档 |
| `M30-15` 目标和承诺持久层 | M30-24～28 已完成。版本、授权、到期索引、取消、重复计划与重启去重均已验收，兑现证据补充见 M30-46 | 实际到期执行由 M30-37 负责，主动提醒与打断预算由 M33 负责 |
| `M30-16` 技能版本化记忆 | M30-29～34 已完成 Linux 离线验证。发布版本、撤回、版本迁移、遵循当前权限的读取及环境隔离均有证据 | 原定生产目录、真实授权和环境联动待验收，追踪在 M30-07 的[补跑项](m30-memory-acceptance.md#3-里程碑关闭与补跑) |

## 6. 测试与退出条件

- `M30-07`（In Progress）管理 [M30-G1～G8 验收矩阵](m30-memory-acceptance.md#1-当前验收矩阵)及
  [目标宿主、模型和全平台补跑](m30-memory-acceptance.md#3-里程碑关闭与补跑)。
- `M30-08`（In Progress）管理最终决策、设计、API、术语和总计划同步，在实施与平台验证齐备后复核。
- 实施项和上述验收工作通过后，才在总计划将 M30 标为 Completed。

## 7. 风险与阻塞

- **依赖 M28/M29**：本阶段依赖上下文归属与并发归属已冻结。
- **价值公式风险**：issue #111 §4 明确「不应先冻结激活公式而遗漏完整交互闭环」。
  本阶段**先交付闭环**，排序增量按可选项推进；**未经验证的模型评分不得**成为
  全部规则（DEC-069 §3/F1）。
- **范围界定风险**：`MemoryScope.subject_id`（记忆 ACL 域）与
  `PrincipalContext.user_id`（安全主体）是**两个不同语义**（DEC-046），
  映射规则必须显式，不得隐式复用同一字符串。

## 8. 验证记录

见 [M30 验收文档](m30-memory-acceptance.md)，包括当前矩阵、能力证据索引和完整历史记录。
