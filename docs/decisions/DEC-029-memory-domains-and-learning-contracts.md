# DEC-029：Memory 四类组织与 Workflow 学习契约（阶段 F 前半）

> 状态：Accepted
> 日期：2026-09-09
> 负责人：Mira Maintainers
> 冻结里程碑：[M13](../plans/m13-memory-and-learning-loop.md)
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §10「Memory
> 架构」与 §14「长期学习闭环」的契约层冻结；运行时落点由
> [DEC-030](DEC-030-learning-loop-runtime-semantics.md) 冻结）

## 背景与问题

DEC-014 §10 把 Mira 的长期记忆组织为四类（Environment Model、User Model、Procedural
Memory、Episodic Memory），并在 §14 要求学习闭环持续积累资产。M4 已交付事实层的记忆
体系（`MemoryScope`/`MemoryKind`/`MemoryRecord`/`IMemory`、确定性 consolidation、混合
检索与 SQLite 后端）；M11（轨迹编译与入库门禁）交付了 Procedural 资产的生产端；
M12（App Model 与导航）交付了 Environment 资产的图形式投影。但四类组织仍只是设计
叙述，且闭环缺两个环节：

- **失败经验不可检索**：一次 Workflow Run 的失败、升级与恢复只存在于事件流里；下一次
  同类失败发生时，Agent 的续跑上下文拿不到「这个 Workflow 之前在这里怎么坏、怎么修好
  的」，每次恢复都从零开始。
- **四类记忆没有域级视图**：`MemoryKind` 是七个平行枚举值，调用方无法以 DEC-014 的四类
  语义发起检索或盘点；Environment（含 App Model 投影）、User、Procedural（含 Workflow
  库）、Episodic 四域的资产归属只能靠文档约定。

阶段 F 需要冻结：(1) `MemoryKind` 到四类域的确定性组织；(2) Workflow 运行经验
（Episode）与恢复经验（RecoveryLesson）的版本化契约及其与 `MemoryRecord` 的纯函数
转换；(3) 失败签名与失败检索查询的确定性构建。

## 决策

### 1. Memory 四类域组织（`MemoryDomain`，确定性全映射）

```text
MemoryDomain（封闭集）
├── EnvironmentModel   ← EnvironmentFact、ApplicationFact
├── UserModel          ← Preference
├── ProceduralMemory   ← Procedure、SkillHint、RecoveryLesson
└── EpisodicMemory     ← Episode
```

- `memory_domain_of(MemoryKind) -> MemoryDomain` 是**全函数**：每个 `MemoryKind` 恰好
  落入一个域，映射与 [DEC-014 §10.2](DEC-014-agent-harness-workflow-dual-plane.md) 的
  演进表逐行一致；逆函数 `memory_kinds_of_domain(MemoryDomain)` 返回该域的
  `MemoryKind` 全集，供 `MemoryQuery::kinds` 过滤器构建域级检索。
- 域有 `name`/`parse` 封闭字符串集（`environment`/`user`/`procedural`/`episodic`），
  未知名字 fail closed。
- 域是**组织视图，不是新的存储分层**：不新增表、不迁移数据、不改变 `IMemory` 契约与
  M4 的 scope/ACL/审批规则。四域与既有资产的对应关系按 DEC-014 §10.2 固化：
  Environment 域的图级知识载体是 M12 的 `AppModel` 投影；Procedural 域的版本化本体是
  M11 的 Workflow 库（`WorkflowVersionHistory`）；User 域维持 M4 的人工审批默认；
  Episodic 域由本决策的 Episode 契约落地。
- 域不承载授权语义（`RULE-09`）：检索过滤仍以 `MemoryQuery::scopes` 的 ACL 白名单为
  第一道门，域与 kind 只是过滤器，不构成绕过 scope 的通道。

### 2. Workflow Episode 契约（`WorkflowEpisodeRecord`）

一次**真实派发**（非 DryRun）的 Workflow Run 到达终态后，可固化为一条结构化情景记录：

```text
WorkflowEpisodeRecord
├── schema_version {major=1, minor=0}        版本化公共契约（DEC-002）
├── run_id / workflow_id                     稳定字符串 ID
├── ir_digest                                创建时钉住的版本内容（十六进制）
├── policy                                   结算时有效策略名（封闭枚举名）
├── outcome                                  completed | failed | cancelled
├── failed_step_id? / failure_reason_code?   仅 failed 时必填；reason_code 为有界
│                                            安全标识符（见 §4 消毒规则）
├── escalations / checkpoint_handoffs        有界计数器（恢复史与检查点史摘要）
└── recorded_at_ms                           调用方传入的 epoch 毫秒（无时钟读取）
```

- **序列化与校验**：JSON 往返无损；未知字段、未知版本、未知枚举名、空 ID、畸形 digest、
  负计数与 `WorkflowLearningLimits` 上限（`RULE-08`）全部 fail closed，语义与既有
  workflow 契约解码同源。
- **内容寻址**：`workflow_episode_digest` 为规范化 JSON 的 SHA-256；同内容同 digest。
- **纯函数转换**：`episode_to_memory_record(episode, scope, evidence, now)` 产出
  `MemoryRecord`：`kind=Episode`、`statement=episode 规范化 JSON`、
  `verification=Verified`（终态来自已提交事件，是事实而非猜测）、`confidence=1.0`、
  `provenance=evidence`（`WorkflowRunSettled` 事件 ID，满足 M4「可追溯记忆」校验）、
  `validity.valid_from = recorded_at`、`sensitivity=Internal`。转换不读时钟、同输入同
  输出。
- **statement 即检索面**：规范化 JSON 中的 `workflow_id`、`failed_step_id`、
  `failure_reason_code` 构成 exact/FTS 检索的命中面；statement **只含 ID、digest、
  枚举名、原因码与计数器**，参数值与用户文本不进入（DEC-022 §5 同源的脱敏边界，
  `RULE-09`）。
- Episode 是**投影**（`W-03`/`RULE-07`）：事件序列（`WorkflowRunStarted`、
  `WorkflowStepSettled`、`WorkflowRunSettled`）足以重建它；Memory 不是事实源。

### 3. Workflow RecoveryLesson 契约（`WorkflowRecoveryLesson`）

一次「失败升级 → Agent 恢复 → 最终完成」的可复用恢复经验：

```text
WorkflowRecoveryLesson
├── schema_version {major=1, minor=0}
├── lesson_id                                稳定字符串 ID
├── workflow_id / ir_digest / recovered_run_id
├── failure                                  WorkflowFailureSignature（§4）
├── recovery[]                               恢复动作序列，每项：
│   ├── patch_id / patch_digest              升级后应用的 patch（可多项）
│   └── targets                              parameters | policy | skip 的目标摘要
│                                            （无参数值、无步骤实参）
├── resumed_without_patch                    无 patch 直接续跑完成的标记
├── outcome                                  recovered（唯一值；只在观察到恢复
│                                            成功后记录，无失败教训形态）
└── recorded_at_ms                           调用方传入
```

- 与 Episode 同源的序列化、fail-closed、digest 与上限规则；`recovery` 条目数受
  `RULE-08` 上限约束。
- **纯函数转换**：`recovery_lesson_to_memory_record(lesson, scope, evidence, now)` 产出
  `kind=RecoveryLesson`、`verification=Verified`、`confidence=1.0` 的记录（证据为完成
  Run 的 `WorkflowRunSettled` 事件）；`recovery_lesson_from_record(const MemoryRecord&)`
  从 statement 解析回结构化 lesson，statement 不是规范化 lesson JSON 时 fail closed
  （复用面只认本契约的 canonical 形态）。
- Lesson 只记录**观察到的成功恢复**（准入由 DEC-030 §4 冻结）；「失败教训」的负向形态
  由 Episode 的 `outcome=failed` 承载，不引入第二套语义。

### 4. 失败签名与检索查询构建（确定性纯函数）

```text
WorkflowFailureSignature
├── workflow_id                              必填
├── step_id? / step_kind?                    失败步骤（已知时）
└── reason_code                              有界安全标识符
```

- **消毒规则**：`reason_code`/`step_kind` 等自由字符串字段只允许
  `[A-Za-z0-9._:-]`、长度 ≤ 128；超集字符 fail closed（错误码域本身是稳定命名，异常
  文本不得进入检索面或记忆面）。
- `failure_retrieval_query(signature, scope, limits) -> MemoryQuery`：scopes 固定为
  学习上下文声明的单一 scope（DEC-030 §1）；`kinds = {Episode, RecoveryLesson}`（即
  Episodic 域中服务恢复的子集）；`exact_terms = {workflow_id} ∪ {step_id?}`；
  `text` 为签名 token 的空格连接（FTS 腿按 AND 短语消费）；`max_results`、
  `token_budget`、`deadline` 取自 `WorkflowLearningLimits`（缺省 8 / 1024 / 250ms，
  暂定默认值，`RULE-10`）。同签名同查询（确定性），检索排序仍由 M4 既有排名规则决定。
- 查询不携带 embedding；向量腿不参与失败检索（首期无 embedding 消费者，避免无证据的
  语义召回承诺）。

### 5. 确定性 ID 派生

Episode/lesson 的 `MemoryId` 与 `MutationId` 从稳定种子经 SHA-256 前 16 字节派生
（种子 = `"mira.workflow.episode|<run_id>"` / `"mira.workflow.lesson|<run_id>|<序号>"`
等模块内约定）：同一 Run 的记录请求在重放、重试或重建时得到同一 ID，`IMemory.apply`
的 mutation-id 幂等语义因此对学习闭环生效；ID 派生是纯函数，不依赖随机源。

## 备选方案

- **为四类域新增存储表或独立 Memory 接口**：违反「EventStore 是事实源、Memory 是可重建
  投影」的单一体系（`W-03`），且 M4 的 scope/ACL/erasure 会被迫双轨。不采用；域保持
  为 `MemoryKind` 之上的确定性组织视图。
- **Episode 以自然语言摘要为 statement**：不可重建（摘要依赖模型输出）、不可 exact
  检索、脱敏面不可枚举。不采用；statement 固定为 canonical JSON。
- **失败检索走向量语义腿**：首期无 embedding 供给与消费者，语义召回质量无证据
  （`RULE-10`），且引入配置面。不采用；exact+FTS 两腿已覆盖签名命中的确定场景，
  向量腿留待有评估证据的后续里程碑。
- **Lesson 记录失败形态（outcome=failed 的 lesson）**：与 Episode 的失败形态语义重叠，
  且「未验证的恢复建议」违反 M4「未验证猜测不自动写入」原则。不采用。
- **把 Workflow/Skill 版本资产自动索引为 Procedure 记录**：方向正确（DEC-014 §10.2），
  但当前无检索消费者，自动索引只会生产无验收的写入面。显式推迟（见非目标）。

## 影响与风险

- `Mira::workflow` 新增 `workflow_learning.hpp`（四类域组织、Episode/Lesson/签名/查询
  契约与纯函数）；公开面扩大，installed-consumer 需覆盖。
- 失败检索的召回质量取决于 exact/FTS 腿对签名的命中，同一 Workflow 的不同失败原因若
  共享 `workflow_id` 会一起召回（排序由 M4 规则决定）；这是保守取舍，文档披露，调优
  留给有真实语料的评估（`RULE-10`）。
- `WorkflowLearningLimits` 缺省值（检索条数、token 预算、deadline）是暂定默认值，冻结
  点为首个以真实失败语料校准的里程碑。
- Episode 的 `confidence=1.0` 表达「事实发生过」，不表达「该经验对下次有用」；有用性
  由检索排序与 lesson 的 Verified 恢复史表达，不混淆两种置信语义。

## 验证方式

- 域矩阵：`memory_domain_of` 对全部 `MemoryKind` 的映射逐值断言；逆映射覆盖全域；
  name/parse 封闭集与未知名字负向。
- 契约矩阵：Episode/Lesson/签名的 JSON 往返无损；未知字段、未知版本、未知枚举、空 ID、
  越界计数、超限字段、非法字符 reason_code 全部 fail closed；digest 确定性（同内容两次
  相等、一次改动即变）。
- 转换矩阵：`to_memory_record` 产物的 kind/verification/confidence/provenance/validity
  逐字段断言且通过 `MemoryRecord::validate`；`recovery_lesson_from_record` 对 canonical
  statement 往返相等、对非 canonical statement fail closed；纯函数性（同输入同输出）。
- 查询矩阵：同签名同查询（逐字段断言）；kinds 恒为 Episodic 服务子集；step_id 缺席时
  不进入 exact_terms；limits 上限负向。

## 关联文档和工作项

- [M13](../plans/m13-memory-and-learning-loop.md)：工作项承载
- [DEC-030](DEC-030-learning-loop-runtime-semantics.md)（运行时语义）、
  [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §10/§14、
  [DEC-003](DEC-003-event-sourced-persistence.md)（投影与事实源）、
  [DEC-019](DEC-019-workflow-ir-contract.md) 至
  [DEC-028](DEC-028-navigation-planner-and-navigate-resolution.md)（既有 Workflow 契约）
- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)（M4 事实层）、
  [Workflow Runtime 设计](../design/workflow_runtime_design.md)（阶段 F 章节）
- API 手册 workflow-contracts
