# Mira Context Curator 与 Working Context 设计（可执行工作状态的持续维护）

> 状态：Active（方向与 Stage W1/W2 契约冻结；W1 确定性基础设施已由
> [M20](../plans/m20-working-context-stage-w1.md) 承载并本地交付——
> [working-context 评估 v1](../benchmarks/context-intelligence-working-context-v1.md)；
> W2 `IContextCurator` 契约与模型供给参考实现已由
> [M21](../plans/m21-context-curator-stage-w2.md) 承载并本地交付——
> [curation 评估 v1](../benchmarks/context-intelligence-working-context-curation-v1.md)；
> W3–W5 进入里程碑的门槛见 §13）
> 版本：0.2
> 更新日期：2026-09-15
> 负责人：Mira Maintainers
> 上位设计：[Context Intelligence 设计](context_intelligence_design.md)、
> [Context 与 Memory 架构设计](context_and_memory_design.md)
> 决策依据：[DEC-035](../decisions/DEC-035-context-curator-working-context.md)
> 需求来源：[Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)

## 1. 文档目的

本文把 issue #48 提出的 Context Curator / Working Context 方向固化为 Mira 的专项设计。
DEC-032 的 Stage B–D 已交付检索召回、重排对照与会话语义固化
（`ConversationCheckpoint`），但固化产物仍是**面向会话段**的语义投影——它回答"这段
会话里出现了哪些重要信息"，不回答"现在继续执行这个任务，模型真正需要知道什么"。
本方向在既有 `ConversationCheckpoint`、Memory 与检索架构之上引入
**`WorkingContextSnapshot`**：一个 task/session 导向、可增量维护、可回溯、可重建的
当前状态视图，并为其定义 `IContextCurator`（模型驱动的维护者，后续阶段）。

本文所有代码片段均为**契约草案或伪代码**；标注"现有"的组件以 M19 交付代码为准。
实现进入里程碑前，本文与头文件注释、示例、测试按 DEC-002 同步。

## 2. 设计原则

沿用 DEC-032 的纪律，并新增三条：

1. **语义组件提供候选，确定性组件掌握准入权威。** Curator 与快照永不拥有上下文
   准入权：`WorkingContextSnapshot` 的唯一入口是 Layer 0 候选转换
   （§7），token、安全、epoch 与 authority 裁决全部留在
   `StandardContextManager`。
2. **压缩不等于删除。** 原始消息、工具调用与结果、执行事件、artifact 组成
   Trace Plane（§3），是唯一事实源；Working Context 是可重建投影
   （RULE-07），任何时刻可从其 `source` 重新推导。
3. **Working Context 不是第二份 Memory。** 快照是短期、任务导向、可频繁覆盖的
   派生状态，生命周期绑定 task/session/epoch；跨任务知识仍只能经
   `MemoryConsolidator` 的 policy、scope、验证与人工审批管线进入长期记忆。
   Curator 可以产生 Memory candidate，不能直接写 Memory。

## 3. 三平面模型

上下文体系按事实与投影分为三个 plane（不使用 L0/L1 命名，避免与 DEC-032 的
Layer 0–3 混淆）：

```text
Trace Plane（事实层）
    原始消息、tool call/result、execution event、artifact、权威运行时状态
    —— EventStore / ArtifactStore / ConversationLog（现有，不变）

Working State Plane（工作状态层）
    Context Curator 持续维护的 WorkingContextSnapshot
    —— 本设计新增；派生投影，可重建（RULE-07）

Retrieval Plane（按需召回层）
    按当前请求从 Trace / Memory / Episode / Lesson 召回的证据
    —— Layer 1/2（现有，不变）
```

主 Agent 默认消费 Working State Plane 与近期热上下文；需要更具体证据时经既有
Layer 1/2 链路回查 Trace Plane。上下文装配仍由 Layer 0 统一执行
（§7）。

## 4. WorkingContextSnapshot 契约

### 4.1 数据模型（W1 冻结部分）

快照是结构化状态，不是叙事摘要。每条语义状态携带 provenance，可反向定位到源
checkpoint 语句及其事件：

```cpp
struct WorkingContextItem final {
    std::string content;                  // 有界文本（复用语句过滤后的表面）
    std::vector<EventId> source_events;   // provenance（逐条非空）
    SessionSequence source_sequence = 0;  // 真实时序（Layer 0 排序键）
    double confidence = 0.0;
};

struct WorkingContextSnapshot final {
    SchemaVersion schema_version;         // mira.working_context.snapshot.v1
    WorkingContextSnapshotId id;          // 确定性派生（§5.1）
    SessionId session_id;
    TaskId task_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    std::uint64_t through_event_sequence = 0;  // 会话水位
    std::vector<WorkingContextItemId> source_checkpoints;  // 输入 checkpoint id
    Timestamp created_at;
    std::vector<WorkingContextItem> constraints;   // 用户约束与禁止项
    std::vector<WorkingContextItem> decisions;     // 决策（含理由）
    std::vector<WorkingContextItem> open_issues;   // 未决问题 / blockers
    Hash state_digest{};                  // 权威字段 canonical digest
};
```

W1 只冻结上列三个 section——它们恰好是 `ConversationCheckpoint` 的确定性可导出
部分（constraints / decisions / unresolved_threads）。issue #48 数据模型中的
`goal / active_tasks / verified_facts / failed_attempts / important_refs /
next_actions` 需要 Curator 语义判断才能填充，在 W2 随 `IContextCurator` 以
**加法式 schema minor 升级**引入；提前定义无人填充的空 section 属于投机 API。
（2026-09-15 实现注记：W2 已按此升级——`mira.working_context.snapshot` schema
1.1 新增 `active_tasks` / `verified_facts` / `failed_attempts` /
`important_refs` / `next_actions` 五个 section 与 `generated_by` 标注，v1.0
载荷保持可读；[M21](../plans/m21-context-curator-stage-w2.md) §4.2。）

**刻意偏离 issue 草案的一点**：快照不含 `goal`。当前 Goal 是 Layer 0 P1 的确定性
任务帧内容（`ContextItemKind::Goal`，权威来源为 Runtime），快照若复述会制造重复
authority；Curator 后续如产出 sub-goal 候选，以普通候选 kind 进入 Layer 0，
不得冒充任务帧。

### 4.2 与 ConversationCheckpoint 的关系

`ConversationCheckpoint` 保持 segment-oriented 的阶段性语义固化结果不变（M19 契约
不动）；`WorkingContextSnapshot` 是 task/session-oriented 的当前状态视图。W1 的
确定性投影关系：

```text
checkpoint.constraints      → snapshot.constraints
checkpoint.decisions        → snapshot.decisions
checkpoint.unresolved_threads → snapshot.open_issues
checkpoint.summary          → 不投影（非权威叙事，同 M19 转换纪律）
checkpoint.preferences      → 不投影（等待 MemoryConsolidator 人工审批管线）
```

W2 起 Curator 输入为 `previous snapshot + new checkpoint + recent events`，
输出 snapshot candidate；增量 merge 语义（保留、supersede、冲突保留）届时随
`IContextCurator` 契约冻结。W1 不引入 `previous` 参数：确定性源只有 checkpoint，
整体替换语义与 Stage D 冻结的固化提交模型一致，不发明未冻结的合并策略。

## 5. 生命周期与提交纪律

### 5.1 确定性身份与 digest

- `WorkingContextSnapshotId` 由 seed 确定性派生：
  `mira.working_context.snapshot|<session>|<task>|<task_epoch>|<environment_epoch>|<through_event_sequence>`。
  同一身份输入永远派生同一 id，重建投影幂等再注册（RULE-07）。
- `state_digest` 覆盖全部权威字段（身份五元组、`source_checkpoints`、三个
  section 及其内容），排除 `id` 与 `created_at`，使重建时间不影响身份与比较。
- 会话/任务/epoch 变化时身份随之变化：旧快照不会覆盖新 epoch 的快照，epoch
  失效通过身份隔离表达（§5.3）。

### 5.2 提交校验（沿用 Stage D 五元组纪律）

提交校验是纯函数，输入 `(store, candidate, live)`，`live` 携带
`session / task / task_epoch / environment_epoch / session_terminal /
task_terminal`：

| 检查顺序 | 条件 | 处置 |
| --- | --- | --- |
| 1 | candidate `validate()` 失败 | DiscardedStale（`invalid-candidate`） |
| 2 | session 或 task 已终态 | DiscardedTerminal（终态幂等：迟到结果丢弃，永不重激活） |
| 3 | session / task / task_epoch / environment_epoch 任一不匹配 | DiscardedStale（保留已存快照） |
| 4 | 水位 < 已存快照水位 | DiscardedStale（`stale-watermark`） |
| 5 | 水位 == 已存快照水位 且 digest 相同 | IdempotentNoOp（幂等重放） |
| 6 | 水位 == 已存快照水位 且 digest 不同 | DiscardedStale（`conflicting-watermark`，fail-closed） |
| 7 | 其余 | Committed |

同水位冲突不静默覆盖：冲突说明同身份输入产生了不同投影，必须显式暴露（修复路径
是从源 checkpoint 重建，见 §9）。与 Stage D 的差异只有一处：W1 快照由 checkpoint
确定性导出，正常管线中同水位冲突不可达；该分支存在是为 fail-closed 地兜住
W2 Curator 的非确定性行为。

### 5.3 Store 与水位单调

`IWorkingContextStore` 会话内按水位单调保留快照环（默认 8 个，文档化默认值）：

- `put` 拒绝同 `(session, task, task_epoch, environment_epoch)` 链内的水位回退。
- epoch 变化开启新链：新旧链按身份共存于环内，`latest` 按提交序返回最新；
  Layer 0 消费方凭条目上的 epoch 标注识别陈旧投影（§7）。
- `erase_session` 服务隐私擦除（沿用既有 Erasure 语义；快照是投影，删除不触及
  Trace Plane）。

## 6. W1 确定性 merge 与 W2 Curator

**W1（本里程碑，无模型）**：`working_context_from_checkpoint(checkpoint, options)`
是纯确定性函数——校验输入 checkpoint（复用其 `validate()`），按 §4.2 映射三个
section（逐条携带 `source_events` / `source_sequence` / `confidence`），执行
RULE-08 边界（section 计数、单条字节、条目总数上限，超限整体拒绝——投影是全有或
全无，不做部分截断），产出 candidate。标记过滤不在 merge 重复执行：输入是已提交
checkpoint，敏感内容过滤在固化阶段强制（M19 D3 门禁），与
`context_items_from_checkpoint` 信任已提交投影的边界一致。

**W2（M21 已交付，模型供给）**：`IContextCurator` 以
`previous snapshot + new checkpoint + recent events` 为输入产出 candidate，
经 §5.2 同一提交管线落库。模型边界复用 M19 已验证的范式：StrictJsonSchema、
编号输入、provenance 绑定（引用越界丢弃）、deadline/cancellation、有界输出、
fail-closed 解析。宿主经 `IModelProvider` 注入当前可用的源模型即可——包括生成
原输出上下文的主模型；不要求专用小模型（[DEC-036](../decisions/DEC-036-consolidation-model-supply.md)，
2026-09-14 修订），Core 不绑定模型。（实现注记：增量 merge 语义在
[M21 §4.1](../plans/m21-context-curator-stage-w2.md) 冻结为"指令契约 +
机械校验"——保留 / supersede / 冲突保留由模型经三段编号转录
（`prev:` / `ckpt:` / `event:` 行格式，checkpoint 偏好语句不入转录）的引用
表达，运行时只做 provenance 绑定、上界与标记校验；另冻结**退化防护**：
previous 非空而候选零 previous 引用时整体拒绝（`degenerate-merge`），
recent events 越水位在调用前拒绝。）

## 7. Layer 0 准入路径

`context_items_from_working_context(snapshot)` 是纯转换，产出普通 `ContextItem`
候选（DEC-035 重申：快照不自准入）：

- constraints → P1 `UserConstraint`（与 checkpoint 转换同分区纪律）；
- decisions / open_issues → P3 `CheckpointSummary`；
- 全部条目 `authority = UntrustedExternalData`（模型介导的派生投影，RULE-09，
  永不 SystemPolicy / VerifiedState）；条目携带 source event provenance 与
  `source_sequence` 真实时序；
- 条目 `task_epoch` / `environment_epoch` 取快照身份，陈旧投影由 Layer 0 既有
  stale-build 检测识别，不引入新语义；
- 条目 id 由快照 id 确定性派生（与 checkpoint 条目 id 空间分离，双投影并存时
  可审计区分）；
- `summary`/`preferences` 等价物不转换。

W1 不引入新的 `ContextItemKind`：快照 section 与 checkpoint 语句在 W1 内容同源，
复用既有 kind 保持"不修改 StandardContextManager 与 Layer 0 既有语义"的纪律；
W2 快照内容与 checkpoint 语句分道后，如需独立审计粒度，再以加法式变更引入专用
kind。宿主应择一喂给 Layer 0（快照或 checkpoint），不做去重承诺。

## 8. Executor 路由与调度（W1 部分）

`ContextMemorySupervisor::schedule_working_context_commit(store, checkpoint, live,
options)`：Deferrable 类，归入既有 Context/Memory 关闭顺序（停生产者 → 取消
Deferrable → 有界等待 Critical → 消费 future）。取消探针经 options 透传；被取消
的在途工作 resolve `Cancelled`，不悬挂 future；shutdown 开始后的提交被拒绝。
W1 的 merge 本身是纯函数，Deferrable 分类依据是：快照链刷新是机会性的（W2 将
在此路由后替换为模型调用并做 coalescing），store 的单调提交是迟到结果的兜底，
统一路由使 W2 替换实现时不改变 shutdown 语义。

自动触发（token watermark、event count、task boundary、coalescing、forced
flush）归 W3，不在 W1 范围。（2026-09-15 实现注记：W3 触发语义在
[M22 §4](../plans/m22-working-context-stage-w3.md) 冻结——"token watermark"
细化为**会话对话序列水位距离**，请求体量预算留在 `ContextCurationOptions`
既有上界，不引入 tokenizer 依赖；event count 轴为宿主上报的执行事件增量，
与对话序列正交；频率预算由阈值 + coalescing 承担。触发为宿主上报信号的
确定性策略，全部工作仍经本节 Deferrable 路由，无隐藏后台循环。）

## 9. 故障与降级

| 故障 | 行为 |
| --- | --- |
| 输入 checkpoint 缺失/无效 | 无新快照，保留已存快照；Layer 0 继续以既有候选闭合请求 |
| merge 边界超限 | 整体拒绝该 candidate（无部分快照），记诊断原因码 |
| 提交竞态（水位回退 / epoch 不匹配 / 终态迟到） | 按 §5.2 丢弃，保留已存快照 |
| 同水位 digest 冲突 | fail-closed 丢弃并暴露冲突；修复路径为从 `source_checkpoints` 重建（§10） |
| store 丢失/进程重启 | 快照是可重建投影（RULE-07）：从最近已提交 checkpoint 重新导出，幂等恢复同 id 同 digest |
| Curator（W2）失败 | 不阻塞 Agent loop；调用方继续使用上一已提交快照 + 近期热上下文 + 检索回补 |

## 10. 恢复与可重建性

恢复 = 重导出：`working_context_from_checkpoint(最近已提交 checkpoint)` 在空
store 上重放必须产出与丢失前相同的 id 与 `state_digest`（字节级可复现，
§12 门禁）。快照不引入新的持久化格式；其持久化需求（跨进程）与 checkpoint store
同层评估，W1 保持 volatile（与 `InMemoryConversationCheckpointStore` 同档，
RULE-07 允许）。

## 11. 安全与隐私

- 快照内容继承 checkpoint 的脱敏纪律（forbidden/injection 标记在固化阶段强制）；
  W1 merge 不重复过滤也不放宽边界。
- 全部条目 `UntrustedExternalData`：检索命中与快照语句都不得提升为 policy
  （RULE-09）；授权状态只能来自权威运行时，快照永远不能凭一句"用户已授权"
  绕过授权系统。
- 会话归属：快照以 session 为 ACL owner，转换条目携带源 provenance；
  `erase_session` 沿用既有擦除语义。
- 训练导出边界不变（RULE-12）：快照不是训练数据集。

## 12. 评估设计

W1 评估对象是**管线行为**，不是语义质量（RULE-10）：冻结数据集 + 确定性
checkpoint 供给方，门禁跑前冻结，报告落
[working-context v1](../benchmarks/context-intelligence-working-context-v1.md)。

- **投影保真**：快照三 section 与输入 checkpoint 逐条一致（content /
  provenance / sequence / confidence）。
- **生命周期纪律**：幂等重放、水位单调、同水位冲突 fail-closed、五元组竞态、
  终态迟到丢弃、epoch 失效隔离。
- **Layer 0 纪律**：authority 全部 UntrustedExternalData、分区映射正确、
  不转换 summary/preferences、id 确定性且与 checkpoint 条目空间不冲突。
- **恢复**：空 store 重建与原快照字节级一致。
- **确定性**：跨进程报告字节级一致。

W2 起引入 issue #48 的对照指标（Continuation correctness、Constraint
retention、Failed-attempt recurrence 等，A/B/C 三臂冻结数据集）；W1 不声明
token 收益或任务成功率（无 Curator、无模型，无可测对象）。

## 13. 分阶段落地与门禁

本方向阶段命名为 **W1–W5**（Working Context），与 DEC-032 §12 的 Stage
E（miracle 真机评估）/ Stage F（提示压缩）并行不冲突、不占用其编号；两方向的
交点在 Stage E：真机评估矩阵将以当时的 Curator 形态分列。

| Stage | 内容 | 门禁 |
| --- | --- | --- |
| W1 | `WorkingContextSnapshot` 确定性契约（store、水位、digest、epoch、provenance、Layer 0 转换、恢复）——无模型，**已交付**（[M20](../plans/m20-working-context-stage-w1.md)，2026-09-14，基线见[working-context v1](../benchmarks/context-intelligence-working-context-v1.md)：W1-G1–G6 全绿、恢复 12/12 幂等重建、跨进程报告字节级一致） | M19 关闭（已满足）；设计/决策冻结 |
| W2 | `IContextCurator` 契约 + model-backed 参考实现（StrictJsonSchema、provenance 绑定、fail-closed、previous-snapshot 增量输入）——**已交付**（[M21](../plans/m21-context-curator-stage-w2.md)，2026-09-15，基线见[curation v1](../benchmarks/context-intelligence-working-context-curation-v1.md)：W2-G1–G6 全绿、脚本化确定性供给方口径） | W1 关闭（已满足）；可用源模型供给（[DEC-036](../decisions/DEC-036-consolidation-model-supply.md)：经 `IModelProvider` 注入，含主 Agent 模型；不要求专用小模型，无新增供应链项）（已满足） |
| W3 | Supervisor 自动触发：watermark / event count / task boundary 触发、coalescing、forced flush、失败回退——由 [M22](../plans/m22-working-context-stage-w3.md) 承载（2026-09-15 立项，触发策略随其 §4 跑前冻结） | W2 关闭（已满足）；快照链在长会话基线上可复现（已满足，[curation 评估 v1](../benchmarks/context-intelligence-working-context-curation-v1.md)） |
| W4 | Memory Promotion：Curator 产生 Memory candidate，仍经 `MemoryConsolidator` 既有纪律 | W2 关闭；Working Context 与长期 Memory 边界测试冻结 |
| W5 | Subagent fork / merge：快照 fork、局部 delta、curated result、parent merge policy | W4 关闭；多 Agent 工作流场景冻结 |

每 Stage 进入实现前创建里程碑文件（不预分配编号），测试矩阵至少覆盖：正常完成、
组件缺席降级、提交五元组竞态、终态后迟到结果、shutdown、同水位冲突与恢复重建。

## 14. 候选技术与供应链

> 2026-09-14 [DEC-036](../decisions/DEC-036-consolidation-model-supply.md) 修订：W2
> Curator 不要求专用小模型，直接使用既有 Provider 已配置的可用源模型（含生成原输出
> 上下文的主模型），不新增供应链项；本地小模型（Qwen ~0.6B 级等）降级为宿主可选的
> 成本优化，若引入新的捆绑模型仍按 DEC-032 §14 供应链复核并登记
> [直接依赖与许可证](../supply-chain/direct-dependencies.md)。任何候选不得成为
> Mira ABI/API 的一部分。W1 无新依赖。

## 15. 关联文档

- [DEC-035](../decisions/DEC-035-context-curator-working-context.md)
- [Context Intelligence 设计](context_intelligence_design.md)
  （Layer 0–3 既有语义与 Stage A–D 基线）
- [Context 与 Memory 架构设计](context_and_memory_design.md)
- [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)、
  [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)
- [Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)
