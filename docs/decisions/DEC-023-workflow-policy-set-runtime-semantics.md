# DEC-023：执行策略全集运行时语义与检查点（阶段 C）

> 状态：Accepted
> 日期：2026-09-08
> 负责人：Mira Maintainers
> 冻结里程碑：[M10](../plans/m10-workflow-intervention-and-policy-set.md)
> 替代/被替代：无（是 [DEC-020](DEC-020-workflow-run-lifecycle.md) §3 策略表的
> 运行时行为冻结；补全其「阶段 C 前不得宣称已实现」的留白）

## 背景与问题

DEC-020 §3 冻结了五种执行策略的语义表（Agent 参与、用户介入、副作用派发、失败处理），
但 [M9](../plans/m9-workflow-runtime-minimal-loop.md) 只实现了 `Strict`/`DryRun` 两条
dispatching 边界内的执行闭环；`Recoverable`/`AgentAssisted`/`Interactive` 在创建 Run 时
被拒绝（`UnsupportedCapability`）。架构设计 §7.5/§7.6 与 DEC-020 §2 预留的
`WaitingAgent`/`WaitingUser` 状态、Agent 续跑上下文与检查点参与因此从未被运行时触达。

阶段 C 需要冻结三件事：(1) 三种策略在步进、失败与升级路径上的精确运行时行为；
(2) `AgentAssisted` 的检查点声明方式与交出语义；(3) 运行中策略切换的准入与确认级别
（[DEC-022](DEC-022-conversation-patch-semantics.md) §6 的开放问题之一）。不冻结这些，
对话介入与策略全集都无法以可测试的方式实现。

## 决策

### 1. 策略准入与失败升级路径

五种策略全部可执行（`allowed_policies` 成员资格与 `AgentEscalation` 钩子的
agent-capable 校验沿用既有契约函数，M9 拒绝门移除）。失败路径按策略分流，
全部发生在步边界（`handle_step_failure` 的恢复链耗尽点或钩子声明点）：

| 策略 | 步骤失败（本地恢复链耗尽后） | 承载 Task 状态 | 出口 |
| --- | --- | --- | --- |
| `Strict`/`DryRun` | `Failed`（M9 行为不变） | `Failed` | 无（终态） |
| `Recoverable` | `Running -> WaitingAgent` | `Recovering` | `resume_run`（修复后续跑）或 `cancel_run` |
| `AgentAssisted` | 同 `Recoverable` | `Recovering` | 同上 |
| `Interactive` | `Running -> WaitingUser`（决策点，见 DEC-024 §6） | `Paused` | 决策决议（accept=跳过续跑 / reject=`Failed` / `cancel_run`） |

- `AgentEscalation` 恢复钩子（失败驱动）：agent-capable 策略下，携带该钩子的步骤失败时
  立即 `Running -> WaitingAgent`，先于本地 `Retry`/`FallbackStep` 链（钩子模式互斥，
  每步只声明一种）。
- 升级提交在步边界完成：失败步骤按 `Failed` 结算并保留游标，不回滚已结算步骤。
- **有界性（`RULE-08`）**：Agent 续跑不重置任何计数器。步级 `max_attempts`、每控制步
  `max_iterations`、整 Run 步执行预算与**整 Run 失败升级预算**（默认 32 次，可收紧）
  跨 `WaitingAgent`/`WaitingUser` 周期累计；升级预算耗尽后按 `Failed` 终态化
  （`escalation-budget-exceeded`），不允许 Agent 与运行时之间形成无界升级环。检查点交出
  不消耗失败升级预算（其有界性由 `max_iterations` 与步预算承载）。
- 载体任务迁移：`WaitingUser` 经既有 `pause_task` 进入 `Paused`（暂停族安全语义：无
  自主动作、输入已释放）；`WaitingAgent` 需要任务进入 `Recovering`，控制面新增
  `MiraRuntime::begin_task_recovery(TaskId)` 作为唯一进入路径（直接状态表边
  `Acting/… -> Recovering`，经单写者串行队列提交，语义与既有 simple task command 同构）。
  进入 `Recovering` 与自其恢复（`resume_task` 返回 `Observing`）均递增 task epoch
  （与 pause 族的进出一致，隔离迟到操作完成）。

### 2. `AgentAssisted` 检查点：以 `AgentEscalation` 钩子为声明

不扩展 IR（不新增 checkpoint 字段）：**检查点 = 声明了 `AgentEscalation` 恢复钩子的
步骤**。该钩子是「Agent 参与点」的唯一声明形式，按策略呈现两种行为：

- `Recoverable`/`Interactive`（及非检查点场景）：失败驱动——步骤失败时升级
  `WaitingAgent`（§1）。
- `AgentAssisted`：**到达驱动（主动检查点）**——驱动到达检查点步骤时，先不执行该步，
  而是 `Running -> WaitingAgent` 交出给 Agent；`resume_run` 重新观察后执行该步。

计数语义：运行时按步骤记录「到达次数」与「已交出次数」，每次到达至多交出一次；
`resume` 消费掉当前到达的交出义务后执行该步。循环回归到同一检查点视为新到达，再次
交出，受该控制步 `max_iterations` 与步预算约束。检查点交出不结算步骤（无
`WorkflowStepSettled`），游标不动；`WaitingAgent` 的进入即是可观察信号。

理由：复用既有 IR 结构避免 v1.1 minor 升级（DEC-019 的 reader 升级义务）；「Agent 在
选定检查点参与」与「失败升级」共享同一参与机制（续跑上下文 + resume 出口），语义单一。

### 3. Agent 续跑上下文（设计 §7.6 的最小落地）

`WorkflowRuntime::agent_continuation(run_id)` 仅对 `WaitingAgent` Run 返回结构化上下文：
`run_id`/`workflow_id`/`ir_digest`（创建时版本）、有效策略、当前步骤（ID、种类、已用
尝试数）、有效参数表（宿主侧 API，明文仅在宿主边界；进入模型上下文时由既有脱敏规则
处理）、已结算步骤历史、最后失败原因（safe 摘要）与 `pending_decision`。「相关记忆」
「可用工具」由 Agent 侧按需装配（Context Manager 分区），不在本结构内复制。

### 4. `Interactive` 的介入准入

- 允许的用户/模型运行中介入：`pause`/`patch`/`resume`/`cancel`（对话驱动 patch 的执行
  语义与准入矩阵由 [DEC-024](DEC-024-conversation-patch-execution.md) 冻结）。
- 其余策略维持 DEC-020 §3 表：介入仅 `pause`/`cancel`（patch 拒绝，错误
  `policy-not-interactive`；`WaitingAgent` 态的 Agent 修复 patch 例外，见 DEC-024 §1）。
- 失败决策点（`StepFailure` 类）：恢复链耗尽后 Run 进入 `WaitingUser`，提议为
  「跳过失败步骤继续」（Skip 条目）；`accept` 应用提议并续跑、`reject` 终态 `Failed`、
  `cancel_run` 终态 `Cancelled`。跳过由用户显式接受（DEC-004 确认精神），不是静默降级。

### 5. 运行中策略切换

- 唯一路径是 `execution_policy` Set 的 patch 条目（经 DEC-024 的校验、幂等与审计管线），
  应用点为步边界；成员资格必须落在创建时版本的 `allowed_policies` 内（复用既有校验），
  应用后发出 `WorkflowPolicySwitched` 事件并更新 Run 视图有效策略。
- **切换到 dispatching 策略时的 Navigate 门禁**：若存在尚未结算的 `Navigate` 步骤，
  该 patch 条目被拒绝（`navigate-unresolvable`，与创建时 fail closed 同源）；阶段 E
  导航解析落地后该门禁随解析能力解除。
- **确认级别（关闭 DEC-022 §6 开放问题）**：v1 不设高于全局 SafetyPolicy 的额外确认
  层。理由：`allowed_policies` 是 Workflow 作者在版本入库前预授权的切换范围，成员资格
  校验即是预授权边界；策略不改变安全底线（DEC-020 §3「只能收紧不能放宽」，`W-05`），
  高风险动作的确认仍由全局 SafetyPolicy 与 DEC-004 管线约束。

### 6. `WaitingAgent` 的解决出口

- `resume_run`：重新观察（既有 resume 契约）→ `WaitingAgent -> Running` → 从游标续跑
  （重试当前失败步骤，受 §1 有界性约束）。
- `cancel_run`：终态 `Cancelled`（幂等）。
- 修复期间可提交 run patch（参数/步骤实参/跳过，见 DEC-024 §1）。
- 不提供模型可调用的「直接判 Failed」操作：Agent 判定不可恢复时取消 Run；
  `WaitingAgent -> Failed` 边保留给运行时内部（如 resume 后重试立即再次耗尽预算）。

## 备选方案

- **IR v1.1 新增每步 `checkpoint` 布尔字段**：表达更显式，但引入 reader 升级义务与
  双轨声明（checkpoint 与 AgentEscalation 并存时语义冲突需仲裁）；无执行证据前放大
  `RISK-2026-034` 返工面。不采用；若未来出现「非 Agent 参与的检查点」需求再评估。
- **`WaitingAgent` 升级时不迁移载体任务（保持活跃态）**：违反 DEC-020 §2 冻结映射
  （`WaitingAgent -> Recovering`）与两层视图一致性不变量。不采用。
- **Agent 续跑重置尝试计数（无界修复）**：违反 `RULE-08`；Agent 与运行时之间的
  resume 循环必须有界。不采用。
- **策略切换要求额外高确认级别**：`allowed_policies` 已是入库前预授权范围，再叠一层
  确认没有新的安全收益，反而把对话介入（「这次稳一点跑」）变得不可用。不采用；
  全局 SafetyPolicy 底线不受影响。
- **`Interactive` 失败直接 `Failed`（不设决策点）**：与 DEC-020 §3「可转 `WaitingUser`」
  的冻结语义冲突，且剥夺用户在交互策略下的修复选择。不采用。

## 影响与风险

- `MiraRuntime` 公开控制面新增 `begin_task_recovery`（append-only 命令；`resume_task`
  对 `Recovering` 起点的 epoch 递增是其隔离补全）。设计文档与 API 手册同步。
- `WorkflowRuntime` 新增升级路径、检查点计数与 `agent_continuation`；Run 结果可能以
  `WaitingAgent`/`WaitingUser` 非终态返回（既有字段承载，`WorkflowRunResult.state`
  语义扩展为「驱动停止时的状态」，文档披露）。
- 检查点复用 `AgentEscalation` 钩子在 `AgentAssisted` 下改变钩子的触发时机（失败驱动
  → 到达驱动）：两种时机按策略互斥、同一钩子声明不会同时呈现两种行为；风险是读者
  需意识到「钩子语义按策略分流」，以 API 手册与本决策显式披露。
- Agent 参与依赖宿主装配 Agent 侧（AgentLoop 续跑编排不在本决策范围）；未装配时
  `WaitingAgent` Run 停留等待，出口仍有 `cancel_run`，无死锁路径。

## 验证方式

- 策略矩阵运行时测试：三种策略的失败升级、`AgentEscalation` 钩子两时机、检查点每次
  到达至多交出一次（含循环回归）、跨 `WaitingAgent` 的计数器累计与预算终态化。
- 状态与载体一致性：`WaitingUser -> Paused`、`WaitingAgent -> Recovering`、
  resume 出口、终态组合不变量（Task 终态 ⇒ Run 终态）扩展到新路径。
- 策略切换：边界生效、事件序列（`WorkflowPolicySwitched`）、成员资格与 Navigate
  门禁的负向拒绝、切换后失败路径按新策略分流。
- M9 既有行为回归：`Strict`/`DryRun` 全路径不回归。

## 关联文档和工作项

- [M10](../plans/m10-workflow-intervention-and-policy-set.md)：策略全集与检查点工作项
- [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §7.5/§7.6、
  [DEC-019](DEC-019-workflow-ir-contract.md)、
  [DEC-020](DEC-020-workflow-run-lifecycle.md)、
  [DEC-022](DEC-022-conversation-patch-semantics.md)、
  [DEC-024](DEC-024-conversation-patch-execution.md)
- [Workflow Runtime 设计](../design/workflow_runtime_design.md)（阶段 C 章节）
- [Agent Harness 参考研究](../design/harness_reference_study.md) §5.3/§6
