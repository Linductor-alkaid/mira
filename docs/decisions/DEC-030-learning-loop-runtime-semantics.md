# DEC-030：学习闭环运行时语义（阶段 F 后半）

> 状态：Accepted
> 日期：2026-09-09
> 负责人：Mira Maintainers
> 冻结里程碑：[M13](../plans/m13-memory-and-learning-loop.md)
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §14「长期
> 学习闭环」的运行时半冻结；契约层见
> [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)）

## 背景与问题

DEC-029 冻结了四类域组织、Episode/RecoveryLesson 契约与失败检索查询的构建。剩下的是
运行时语义：记录与检索发生在 Run 生命周期的哪个点、由哪个线程承载、失败如何降级，
以及恢复经验如何进入 Agent 续跑上下文。约束来自三处：

- AGENTS.md/`W-01`：不新增私有并发；IMemory 是同步门面，其后端 I/O 由宿主按 M4 §16/§17
  的 store worker 路由，WorkflowRuntime 不得自建存储 worker。
- `W-03`/`RULE-07`：EventStore 是事实源；学习记录是投影，写入失败不得影响已结算的 Run，
  投影必须可从事件重建。
- 设计 Context/Memory §18：「MemoryStore 暂时不可用 → 无长期 Memory 继续当前 Task」：
  检索失败降级为空结果，不得阻塞升级路径。

## 决策

### 1. 学习上下文（`WorkflowRuntime::set_learning_context`）

```text
set_learning_context(shared_ptr<IMemory> memory, MemoryScope scope,
                     WorkflowLearningConfig config = {})
```

- 安装即启用学习闭环；未安装时一切学习路径为 NoOp（Run 行为与 M12 完全一致，回归
  锁定）。`memory` 为 null 或 scope 非法时 fail closed 返回错误。
- **scope 准入**：`MemoryScopeKind::User` 被拒绝——Episode/Lesson 是自动写入的运行经验，
  不是用户偏好；User 域按 DEC-014 §10.5 与 M4 策略保持人工审批通道（`Preference` 的
  approval 默认不变）。允许 `Agent`/`Application`/`Environment`/`Session`（宿主按产品
  边界选择；缺省建议 `Agent`）。
- `WorkflowLearningConfig`：检索条数上限（缺省 8）、检索 token 预算（缺省 1024）、检索
  deadline（缺省 250ms）、statement 上限（缺省 `WorkflowLearningLimits`）。全部为暂定
  默认值（`RULE-10`）。
- 配置校验失败（零/负 deadline、零条数等）在安装时 fail closed，不留给运行期。

### 2. 结算期 Episode 记录（settlement hook）

- **时机**：`settle_terminal` 提交终态、发射 `WorkflowRunSettled` 并完成载体 Task 结算
  之后，在同一结算线程（驱动 worker 或同步调用线程）上同步执行：
  有效策略为 **DryRun 的 Run 不记录**（规划演练不是环境经验，`RULE-10` 同源的诚实边界）；
  其余终态（Completed/Failed/Cancelled）按 DEC-029 §2 构建 Episode 并 `IMemory.apply`。
- **幂等与确定性**：MemoryId/MutationId 按 DEC-029 §5 从 run_id 派生；重复结算请求
  （终态幂等路径之外的防御性重放）落到 mutation-id 幂等，不产生重复记录。
- **失败语义**：memory 写失败**不得影响 Run 终态**（Run 已结算，事实在事件流里）；失败
  递增诊断计数器并发射 `WorkflowEpisodeRecorded`（outcome=`failed`，含原因码）。写成功
  发射 outcome=`recorded`（含 episode digest）。DryRun 跳过是设计行为，不发事件（Run
  事件已含 policy）。
- **锁纪律**：episode 构建只读 RunRecord 快照字段；`IMemory.apply` 在 runtime 互斥锁外
  调用（与事件发射同一纪律，互斥锁保持叶子）。

### 3. 失败检索进入 Agent 续跑上下文（escalation hook）

- **时机**：仅**失败驱动的升级**（`escalate_waiting_agent`，即恢复链耗尽点进入
  `WaitingAgent`）触发；检查点让渡（`enter_waiting_agent`，AgentAssisted 到达驱动）不是
  失败，不检索。
- **流程**：升级转换成功后，在驱动线程上以最后失败步骤构建 `WorkflowFailureSignature`
  （失败原因取 `domain_code` 稳定名，经 DEC-029 §4 消毒），用
  `failure_retrieval_query` 同步查询 IMemory；结果（Episode 与 RecoveryLesson 记录的
  statement 摘要，有界截断）存入 RunRecord，`agent_continuation()` 以
  `relevant_lessons` 字段透出（每项含 kind、statement、confidence）。查询在互斥锁外
  执行；结果写入持锁。
- **降级**：查询失败（后端不可用、超时）→ 空结果 + 诊断计数器，升级路径照常完成
  （无记忆继续当前任务）。检索结果只是**数据**：进入模型上下文时按既有脱敏规则处理，
  不提升权限（`RULE-09`），Agent 对 lesson 的采纳是 Agent Harness 编排，不在本决策。
- **有界性**：每次升级至多一次查询；结果条数与 token 预算受 §1 配置约束（`RULE-08`）；
  RunRecord 只保留最后一次升级的检索结果（覆盖式，不累积）。

### 4. 恢复经验记录（`WorkflowRuntime::record_recovery_lesson`）

```text
record_recovery_lesson(run_id) -> Result<WorkflowRecoveryLesson>
```

- **准入**（全部满足，否则确定性拒绝）：Run 处于 `Completed` 且有效策略非 DryRun；
  失败驱动升级计数 > 0（本次完成是恢复的结果，不是顺路完成）；学习上下文已安装。
- **派生**：失败签名取 RunRecord 记录的最后一次失败升级签名；恢复动作取该升级之后应用
  的 patch（patch_id、digest、目标摘要，无参数值）；两者皆无时以
  `resumed_without_patch` 标记。产物经 DEC-029 §3 转 MemoryRecord 并 `IMemory.apply`。
- **幂等**：MutationId 从 run_id 派生（同 Run 至多一条 lesson；再次调用走 mutation-id
  幂等返回既有内容，不报错）。
- **审计**：成功/失败发射 `WorkflowLessonRecorded`（recorded/failed + 原因码）。
- **宿主专用**：这是宿主边界 API，不注册为模型工具（`W-04`：模型不可直达记忆写入；
  Agent 判断「值得记录」后由宿主调用，与 `publish_validated` 同一信任模式）。

### 5. 事件面扩展与离线回放

新增两员入 v1 事件闭集（载荷只含 ID、digest、枚举名、原因码与有界计数器）：

| 事件 | Class | 关键字段 |
| --- | --- | --- |
| `WorkflowEpisodeRecorded` | State | run_id、workflow_id、episode digest、outcome（recorded/failed）、原因码 |
| `WorkflowLessonRecorded` | State | run_id、workflow_id、lesson digest、outcome（recorded/failed）、原因码 |

- OfflineReplay（`W-08`）：两员被识别并重建学习投影视图；回放不调用 IMemory、不派发
  输入（回放中的「已记录」显示为已发生事实）。
- **重建配方**（`W-03`）：`WorkflowRunStarted`（身份与策略）+ `WorkflowStepSettled`
  （失败步骤）+ `WorkflowRunSettled`（终态与摘要）+ `WorkflowPatchApplied`（恢复动作）
  + 两员审计事件，足以确定性重建 Episode/Lesson 记忆记录；Memory 损坏后可从事件流
  重放恢复。

### 6. Executor 路由与关闭

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| Episode 记录（结算期 `IMemory.apply`） | 结算线程同步门面（后端自路由 store worker，M4 §17） | 无新任务 | 失败转诊断计数器与审计事件，不影响终态 |
| 失败检索（升级期 `IMemory.query`） | 驱动线程同步门面、有界 deadline | 无新任务 | 失败/超时降级为空结果 + 诊断计数器 |
| Lesson 记录（宿主调用） | 调用线程同步门面 | 无新任务 | 失败对调用方可见（Result 错误）+ 审计事件 |

- 不新增任务类别、timer 或私有并发；学习状态（上下文与检索结果）随 RunRecord 生命周期
  由 runtime 互斥锁保护。关闭顺序不变（M9 §5）：`WorkflowRuntime::shutdown()` 拒绝新的
  学习调用（结算期记录随最后一次终态自然完成，无待排空队列）。

## 备选方案

- **异步学习队列（提交 Executor 任务写记忆）**：终态结算需要额外等待或放弃语义，队列
  还要_own_ 关闭路径；同步门面 + 后端自路由已满足（IMemory 后端本身在 store worker
  上执行 I/O）。不采用。
- **检索结果缓存/预取**：失败升级是低频事件，预取浪费检索预算且引入失效问题。不采用。
- **Lesson 由模型经工具通道提议**：模型输出直达记忆写入违反 `W-04`；提议-审批管线在
  M4 consolidation 已存在（宿主可走 `MemoryConsolidator`），不需要第二通道。不采用。
- **检索失败时阻断升级（fail closed）**：把可选增强置于必需路径，违反「MemoryStore
  不可用时继续当前 Task」的既有降级语义。不采用；降级 + 计数器披露。

## 影响与风险

- `WorkflowRuntime` 公开面扩大（`set_learning_context`/`record_recovery_lesson`；
  `agent_continuation` 增 `relevant_lessons` 字段——**增量字段**，既有消费者源码兼容）。
- 学习写入落在结算热路径上（同步门面）：后端慢会拖长结算；由 M4 后端的 store worker
  有界通道与 §1 deadline 共同兜底，API 手册披露。
- 检索签名只含结构化标识，同 Workflow 的不同根因可能一起召回：DEC-029 已披露的保守
  取舍，排序由 M4 既有规则承担。
- `RISK-2026-038`（沿袭）：学习记录的持久化载体是宿主安装的 IMemory 后端，本决策不新
  增 schema；投影重建依赖事件流（§5 配方）。

## 验证方式

- 结算矩阵：非 DryRun 三终态各记录一条 Episode（字段逐值断言）；DryRun 完成零记录；
  未装学习上下文零记录；memory 写失败不影响终态与 Task 结算、计数器与 failed 事件
  到位；重复结算幂等。
- 升级矩阵：失败驱动升级触发检索且 `agent_continuation.relevant_lessons` 携带先前
  Episode/Lesson；检查点让渡不检索；查询失败降级为空结果且升级完成；结果条数受配置
  上限约束。
- Lesson 矩阵：准入全负向（未完成/DryRun/无升级/未装上下文）；恢复 patch 与
  resumed_without_patch 两形态派生正确；同 run 幂等重放；写失败对调用方可见。
- 事件矩阵：两员载荷往返、未知字段 fail closed、闭集扩展回归；离线回放无 IMemory 调用。
- 端到端：Run A 失败升级（检索空）→ 记录 Episode → Run A 恢复完成 → 记录 Lesson →
  Run B 同签名失败升级时 `relevant_lessons` 同时含 A 的 Episode 与 Lesson。

## 关联文档和工作项

- [M13](../plans/m13-memory-and-learning-loop.md)：工作项承载
- [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)（契约层）、
  [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §10/§14、
  [DEC-023](DEC-023-workflow-policy-set-runtime-semantics.md)（升级与续跑上下文）、
  [DEC-024](DEC-024-conversation-patch-execution.md)（patch 记录）、
  [DEC-025](DEC-025-success-trajectory-compilation-and-publish-gate.md)（宿主信任边界
  同模式）
- [Context 与 Memory 架构设计](../design/context_and_memory_design.md) §17/§18（路由与
  降级）、[Workflow Runtime 设计](../design/workflow_runtime_design.md)（阶段 F 章节）
- API 手册 workflow-contracts
