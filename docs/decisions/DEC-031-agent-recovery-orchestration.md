# DEC-031：Agent Harness 恢复编排运行时语义（`WaitingAgent` 到 resume 的有界闭环）

> 状态：Accepted
> 日期：2026-09-10
> 负责人：Mira Maintainers
> 冻结里程碑：随 `MNT-202609-24` 立项创建的里程碑（编号按
> [阶段 F 后续计划](../plans/maintenance-2026-09-post-stage-f.md) §1 规则在立项时分配，
> 不预占）
> 替代/被替代：无（补全 [DEC-023](DEC-023-workflow-policy-set-runtime-semantics.md) 明确
> 留白的「Agent 侧续跑编排」与 [DEC-030](DEC-030-learning-loop-runtime-semantics.md) §3
> 「lesson 采纳属 Agent Harness 编排」；不改变二者已冻结语义）

## 背景与问题

M10/M13 交付后，失败驱动的升级把 Run 停在 `WaitingAgent`、载体 Task 停在 `Recovering`，
`agent_continuation` 携带续跑上下文与检索到的 `relevant_lessons`，出口是
`patch_run`/`resume_run`/`cancel_run`。但「谁向模型提问、模型答案如何变成 patch、
失败如何收口」没有所有者：M13 测试由宿主直接 resume 并记 lesson，模型从未参与修复；
`MNT-202609-21` 审计将此登记为阶段 F 的头号能力缺口（`MNT-202609-23/24`）。

约束来自四处：DEC-023 §6 已冻结 `WaitingAgent` 的出口集合与「Agent 判定不可恢复时取消
Run」；DEC-024 §1 已把 `WaitingAgent` 态 patch 纳入准入矩阵并要求走完整校验管线；
DEC-030 §3/§4 规定 lesson 是数据、`record_recovery_lesson` 是宿主专用；`AGENTS.md` 要求
模型原始文本不得直接触达平台能力、所有任务受 Executor 管理且可协作取消。另有两个事实
影响方案：事件闭集没有「进入 `WaitingAgent`」成员；lesson 的 `recovery[]` 不含参数值。

## 决策

### 1. 编排器是独立 Core 组件，由宿主装配与触发

- 新增 `WorkflowRecoveryOrchestrator`（Core，`mira_workflow` 目标内），依赖
  `WorkflowRuntime`/`ModelGateway`/`MiraRuntime`/`IEventStore` 公开接口，不接触平台 API 与
  `IMemory`。不并入 `AgentLoop`（避免引入工具执行与环境动作面），不内建于
  `WorkflowRuntime`（runtime 不得获得模型依赖，维持「Agent 参与由宿主装配」的 DEC-023
  分层）。
- 唯一触发面 `notify_escalation(run_id)`（幂等）。宿主从 drive 结果或载体 Task 的
  `Recovering` 转换获知升级后调用；运行时**不新增**回调或升级事件。
- 同步原语 `attempt_recovery` 在调用线程执行；异步面 `start_recovery`/`wait_recovery`
  经 `submit_auto` 承载，容量 `max_concurrent_attempts`，future 由编排器持有并消费。

### 2. 恢复请求归属载体 Task，准入与 Takeover 复用既有机制

- 模型请求以 `carrier_task_id`/`carrier_task_epoch`（`WorkflowAgentContinuation` 增量
  字段）标识；宿主安装的 `TaskAdmissionGate` 对恢复请求与普通请求施加同一准入，epoch
  隔离迟到响应（DEC-023 §1 `Recovering` 进出递增 epoch）。
- 请求前后经 `MiraRuntime::task_snapshot` 复核载体：`Recovering` 通过；
  `SuspendedForTakeover`/`TakeoverSettling` 出口 `takeover`（DEC-018 精神：被接管任务不
  准入导向新自主动作的工作）；终态出口 `run-state-changed`。
- 模型请求不是环境操作，不经 `begin_operation`；不暴露任何工具
  （`ToolChoice::None`），恢复请求不得成为触达其他能力的旁路（`W-04`）。

### 3. 决策 schema 四动作闭集，patch 走 DEC-024 管线，不新增确认层

- `mira.workflow.recovery-decision.v1`：`action ∈ {patch_and_resume, resume, cancel,
  need_user}`，`patch_entries`（形状同 `patch_workflow`，经
  `parse_workflow_patch_entries`/`validate_workflow_patch_entry` 先期校验）、
  `used_lessons`（审计引用）、`rationale`（有界，不进事件）。
- `patch_and_resume` = `patch_run`（新生成 `patch_id`）成功后 `resume_run`；`resume` =
  直接 `resume_run`；`cancel` = `cancel_run`（DEC-023 §6 出口的执行化；不提供
  「直接判 Failed」）；`need_user` = 上交宿主，Run 保持 `WaitingAgent`——**不**升为
  `WaitingUser` 决策点（DEC-024 §4：修复期不向用户提问）。
- v1 不为恢复 patch 增设确认层：`Recoverable`/`AgentAssisted` 是 Workflow 作者入库前
  对「Agent 修复」的预授权（DEC-024 §1 准入矩阵已体现），patch 条目合法性由 runtime
  确定性校验，高风险动作的确认仍在输入派发层由 SafetyPolicy/DEC-004 承担（同
  DEC-023 §5 理由）。

### 4. lesson 三层处理：解析 fail closed、版本失效过滤、预算截取

- 解析层：statement 逐条以学习契约解析函数解析，失败即丢弃并计数。
- 失效层：`workflow_id` 或 `ir_digest` 与当前 Run 不一致判为失效（版本漂移），丢弃并
  计数；v1 无跨版本复用开关（保守默认，`RULE-10`）。
- 预算层：按 runtime 返回序截取 `max_lessons_in_context`（信任 M4 排序，不重排）。
- lesson 是数据不是授权（`RULE-09`）：不参与任何准入；不做机械重放（lesson 无参数值，
  采纳 = 模型合成新 patch 并接受全量校验）。恶意 lesson 最多导致一次 `patch-rejected`
  或一次被步验证否决的修复，无权限提升路径。

### 5. 有界性：三层预算与失败计数

- 编排器侧：`max_attempts_per_run`（默认 8）、每 attempt `1 + max_decision_repairs`
  （默认 1）次模型请求、`model_call_deadline`（默认 30 s）；决策解析失败与 patch 拒绝
  共用修复回合预算；模型拒答不消耗修复回合直接 `decision-invalid`。
- runtime 侧：`max_escalations_per_run` 不变（耗尽 `Failed`，DEC-023 §1）；两者独立，
  编排预算耗尽只 `DeferredToHost`，不替宿主终态化 Run。
- attempt 进入模型请求阶段即计入预算，失败亦计数（防止失败重试绕过上限）；追踪表
  受 `max_tracked_runs` 约束，超限优先淘汰已终态 Run，否则 `ResourceExhausted`。
- 全部默认值为暂定（`RULE-10`），随 `MNT-202609-28/29` 校准。

### 6. 迟到响应与竞态：提交前重核，编排器不改变 Run 状态的两类出口

- 每次向 runtime 提交前重取 `run_snapshot`：`run_epoch` 变化或状态非 `WaitingAgent`
  即丢弃决策（`run-state-changed`/`epoch-advanced`）；迟到模型响应不可能作用到已
  变化的 Run（`RULE-03`）。
- `DeferredToHost`（need_user、decision-invalid、patch-rejected 耗尽、attempt 预算
  耗尽）与 `Aborted`（cancelled、takeover、shutdown、状态漂移、model-unavailable、
  resume-rejected、重复通知）共同不变量：**Run 保持 `WaitingAgent`**，宿主经既有出口
  处置。重复通知在准入即吸收（`attempt-in-progress`/`not-waiting-agent`），不排队。

### 7. 审计事件与 continuation 增量字段

- v1 事件闭集新增 `WorkflowRecoveryAttempted`（State；`mira.workflow.recovery-attempted.v1`）：
  run_id、workflow_id、ordinal、task_id、outcome 枚举、reason_code、可选
  decision_digest/patch_id/model_request_id、lessons_offered/stale/kept 计数。载荷纪律同
  DEC-030 §5（ID/digest/枚举/有界计数器，未知字段 fail closed）。它是全链路关联键：
  ModelRequest 事件（request_id）→ 决策（digest）→ patch 三事件（patch_id）→ Run 升级
  （run_id + ordinal）。OfflineReplay 识别为审计事实，不重发请求、不重复 resume
  （`W-08`）。
- `WorkflowAgentContinuation` 增量字段：`carrier_task_id`、`carrier_task_epoch`、
  `run_epoch`、`escalations`（既有 RunRecord 内部值透出；源码兼容，同 DEC-030
  `relevant_lessons` 先例）。

### 8. Verify 归运行时，lesson 记录归宿主

- `resume_run` 内部重新观察并提交驱动，此后每步验证归 WorkflowRuntime（M9）；编排器不
  新增验证路径，不因模型/lesson 置信度跳过验证；修复是否有效由步验证与 Run 终局判定。
- 编排器职责止于审计事件与宿主回调；Run `Completed` 后由宿主调用
  `record_recovery_lesson`（DEC-030 §4 不变）；再次升级由宿主再次通知（预算递减）。

### 9. Executor 路由与关闭

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| 同步 attempt | 调用线程（模型调用经 gateway 既有路由） | 无新任务 | `Result` 返回 |
| 异步 attempt | `submit_auto` 有界任务 | 编排器持有 future，`wait_recovery`/`shutdown` 消费 | 容量超限 `ResourceExhausted`；异常入结果不丢弃 |
| 取消 | 协作式（attempt 标志 + `OperationContext` 探针，探针不回调 runtime） | — | `cancelled`，不强制终止 |
| shutdown | 拒绝新通知 → 置位取消 → 有界 drain → 报告 | 宿主非 worker 线程 | 先于 `WorkflowRuntime::shutdown()`（编排器是其生产者） |

不新增 timer、worker 池或私有并发；无 Executor 能力缺口。

## 备选方案

- **并入 AgentLoop**：恢复成为通用循环的一步，引入工具执行与环境动作面、放大取消面；
  且 AgentLoop 以 Task 为单位驱动，与「一次升级一次决策」的粒度不合。不采用。
- **WorkflowRuntime 内建（升级时自动请求模型）**：runtime 获得模型依赖，破坏
  DEC-023「Agent 参与由宿主装配」分层，且难以让宿主替换/关闭编排策略。不采用。
- **新增 runtime 升级回调或事件自动触发**：扩大回调面与事件闭集，收益仅是省一次宿主
  通知；升级已可经 drive 结果与 Task 转换观察。不采用（可观测性维持既有边界）。
- **机械重放 lesson 中的 patch**：lesson 不含参数值，旧 patch 未必适配当前状态，重放
  还绕过模型判断与参数化差异。不采用；出现确定性可重放证据后随 `MNT-202609-32` 评估。
- **`need_user` 自动升为 `WaitingUser` 决策点**：与 DEC-024 §4「修复期不向用户提问」
  冲突。不采用；上交宿主，由宿主决定是否人工介入。
- **恢复 patch 增设确认层**：`allowed_policies`/agent-capable 策略即预授权，叠加确认
  无新增安全收益且使自动恢复不可用（同 DEC-023 §5 理由）。不采用。
- **不新增审计事件，靠模型事件 + patch 事件隐式关联**：模型响应到 patch 的映射只存在
  于编排器行为中，`resume`/`cancel`/`DeferredToHost` 路径无任何事件，全链路不可重建。
  不采用；一条 State 事件是最小闭集扩展。

## 影响与风险

- 公开面新增：`WorkflowRecoveryOrchestrator` 及配置/结果类型、`WorkflowRecoveryAttempted`
  事件（闭集扩展，DEC-022 §2 fail-closed 纪律）、`WorkflowAgentContinuation` 四个增量
  字段、`workflow_recovery_decision_schema()`。API 手册与 Workflow 设计随实现同步。
- 宿主责任明确化：装配编排器、传递升级通知、`Completed` 后记 lesson、Executor worker
  数需覆盖异步 attempt 与 runtime 异步驱动（`max_concurrent_attempts` +
  `max_concurrent_async_drives`）。
- 参数投影缺省不带值可能降低模型修复能力：这是脱敏优先的保守默认，宿主可经 hooks
  放开白名单参数；收益/成本由 `MNT-202609-29` 评估组对照。
- 风险 `RISK-2026-052`：模型修复质量不可预测，可能在预算内反复无效修复消耗步预算与
  模型费用。缓解：三层预算、失败计数、`DeferredToHost` 上交、评估基线对照
  （`MNT-202609-28/29`）；无对照证据前不宣称恢复率改善。
- 与 `BUG-20260909-002` 正交：编排器消费直接记录路径的 lesson（含 patch_digest）；
  重建路径成为数据源时方案 A 会强化关联，但不影响本决策的过滤键。

## 验证方式

- 决策矩阵：四动作 ×（合法/格式错/拒答/超时/熔断）；修复回合边界；`used_lessons` 与
  decision digest 审计往返。
- lesson 矩阵：空、全失效、不可解析、混合截断；过滤计数断言；保留 statement 逐字节
  一致。
- 竞态矩阵：请求在途中 cancel/重复通知/Takeover/`run_epoch` 变化；迟到响应丢弃且不复活
  终态；`DeferredToHost`/`Aborted` 后 Run 状态不变。
- 预算矩阵：编排预算耗尽、runtime 升级预算先耗尽、并发容量超限、追踪表淘汰。
- shutdown 矩阵：在途 drain、关闭后拒绝通知、编排器先于 runtime 关闭；事件发射失败
  不影响结果。
- 端到端（`MNT-202609-24` 主场景）：Run A 失败升级（检索空）→ 编排修复 → 完成 →
  宿主记 lesson → Run B 同签名失败升级 → 命中 A 的 Episode+Lesson → 模型合成修复 →
  完成；全链路事件关联键逐条断言；recorded Provider 真正收到检索上下文。
- 安全负向：lesson 引导的越权 patch 确定性拒绝；参数投影缺省无值；`rationale` 不入
  任何事件。

## 关联文档和工作项

- [Workflow 恢复编排设计](../design/workflow_recovery_orchestration_design.md)（本决策
  的设计载体）
- [阶段 F 后续计划](../plans/maintenance-2026-09-post-stage-f.md)：`MNT-202609-23`
  （冻结）、`MNT-202609-24`（实现）、`MNT-202609-28/29`（评估校准）
- [DEC-018](DEC-018-takeover-input-release-and-operation-admission.md)、
  [DEC-022](DEC-022-conversation-patch-semantics.md)、
  [DEC-023](DEC-023-workflow-policy-set-runtime-semantics.md)、
  [DEC-024](DEC-024-conversation-patch-execution.md)、
  [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](DEC-030-learning-loop-runtime-semantics.md)
- [Workflow Runtime 设计](../design/workflow_runtime_design.md) §14、
  [Agent Harness 与 Workflow 架构](../design/agent_harness_and_workflow_architecture.md) §14/§17
