# DEC-020：WorkflowRun 生命周期、Task 状态映射与执行策略

> 状态：Accepted
> 日期：2026-09-08
> 负责人：Mira Maintainers
> 冻结里程碑：[M8](../plans/m8-workflow-contracts.md)（`M8-02`/`M8-08`/`M8-09`）
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §7.2/§7.3/§7.5/§7.7
> 的契约冻结）

## 背景与问题

架构设计 §7.2 要求 WorkflowRun 由现有 Task 生命周期承载（复用取消上下文、单写者控制面、
epoch 结算与事件序列，`W-01`/`W-07`），§7.3 给出 Run 状态图但将 `WaitingUser`/
`WaitingAgent` 与既有 `TaskState` 族的精确映射留给专项设计；§7.5 列出五种执行策略但
默认值与切换规则未冻结；§7.7 要求版本化记录与旧版本回放引用创建时版本。
[Agent Harness 参考研究](../design/harness_reference_study.md)§5.3/§6 要求显式拒绝
LangGraph 的「resume 即节点重跑」模型，冻结为「步边界恢复 + 重新观察」。

## 决策

### 1. Run 状态集与转换表

`WorkflowRunState` 冻结为封闭枚举：
`Created / Running / Paused / WaitingUser / WaitingAgent / Completed / Failed / Cancelled`。

合法转换（纯函数 `valid_workflow_run_transition`，表驱动）：

```text
Created      -> Running | Cancelled
Running      -> Paused | WaitingUser | WaitingAgent | Completed | Failed | Cancelled
Paused       -> Running | Cancelled
WaitingUser  -> Running | Cancelled
WaitingAgent -> Running | WaitingUser | Failed | Cancelled
终态          -> 无出边
```

规则：

- **终态幂等**：对终态重复提交同一终态是合法 NoOp（视图不变）；提交其他任何目标均为
  非法转换。
- **迟到完成隔离**（`W-01`/`RULE-03`）：Run 视图携带单调 `run_epoch`；状态提交只有
  单写者控制面可以做。步级完成、验证结果、恢复决策等一切迟到信号带旧 epoch 到达时按
  `Stale` 结算（沿用 `CompletionDisposition` 语义），不得改变任何状态。终态不可复活。
- **暂停语义**（对齐 [DEC-018](DEC-018-takeover-input-release-and-operation-admission.md)）：
  `Running -> Paused` 之前必须先安全收敛在执行的连续控制并释放平台输入；暂停态拒绝
  新的自主动作进入环境。
- **恢复 = 步边界 + 重新观察**：`Paused/WaitingUser/WaitingAgent -> Running` 从当前步骤
  游标恢复（不从头重跑），恢复的第一个动作是重新观察环境（takeover/长时暂停后环境可能
  已变化）；拒绝 LangGraph 的「节点整体重跑」模型。步骤游标之前的副作用不重发
  （`RULE-05` 至多一次派发）；重新观察后若环境已偏离预期，按恢复钩子/策略处理，不盲目
  续跑。
- `Created -> Cancelled`：未启动即取消是合法路径（如排队期间用户取消）。

### 2. 与 TaskState 族的精确映射

**不扩展既有 `TaskState` 枚举。** Run 视图与 Task 视图是同一执行的两层投影：Task 状态机
（M1 冻结）承载执行与介入语义；Run 状态视图（本决策）承载 Workflow 语义。映射是纯函数
`task_state_for_run_state`（表驱动）：

| Run 状态 | Task 状态（承载态） | 说明 |
| --- | --- | --- |
| `Created` | `Idle` | Run 已创建未启动 |
| `Running` | `Observing`/`Reasoning`/`Planning`/`Acting`/`Verifying` | 由当前步骤相位决定，Run 侧不新增状态 |
| `Paused` | `Pausing`（收敛中）→ `Paused`（已收敛） | 收敛期间任务处于 `Pausing`，收敛完成才提交 `Paused` |
| `WaitingUser` | `Paused` | 区别只在 Run 侧挂起原因（决策点等待用户），复用暂停族的安全语义（无自主动作、输入已释放） |
| `WaitingAgent` | `Recovering` | 等待 Agent 修复决策；复用恢复族的语义（先观察、不重发） |
| `Completed`/`Failed`/`Cancelled` | 同名终态 | 终态幂等与迟到隔离直接继承 Task 侧规则 |

反方向（Task 事件 → Run 视图更新）由控制面在提交 Task 状态转换时同事务完成；两层视图
不得出现一方终态、另一方活动的组合。`WaitingUser` 与 `Paused` 的区分依据是挂起原因
（Run 侧字段 `pending_decision`：决策点 ID + digest），不是新的 Task 状态。

动作租约（`W-07`）：本映射下「同一 Session 只有一个动作租约持有者」由既有单写者控制面 +
epoch/`OperationState` 结算等价强制（M8 不引入名为 ActionLease 的新构造；如后续需要显式
租约对象，另立决策）。

### 3. 执行策略语义表

策略枚举（封闭，属 [DEC-019](DEC-019-workflow-ir-contract.md) IR 的 `default_policy`/
`allowed_policies` 引用）：

| 策略 | Agent 参与 | 用户运行中介入 | 高风险确认 | 副作用派发 | 失败处理 |
| --- | --- | --- | --- | --- | --- |
| `Strict` | 禁用 | 拒绝（暂停/取消除外） | 按全局 SafetyPolicy | 真实 | 立即 `Failed`，本地恢复钩子仍可用（不得升级 Agent） |
| `Recoverable` | 失败后升级 | 拒绝（暂停/取消除外） | 按全局 SafetyPolicy | 真实 | 本地恢复链 → 仍失败升级 `WaitingAgent` |
| `AgentAssisted` | 声明检查点参与 | 拒绝（暂停/取消除外） | 按全局 SafetyPolicy | 真实 | 检查点交 Agent |
| `Interactive` | 失败后升级 | 允许（pause/patch/resume/cancel） | 阈值不高于全局 | 真实 | 可转 `WaitingUser` |
| `DryRun` | 禁用 | 拒绝（暂停/取消除外） | 不适用 | **禁止**：所有含副作用的步骤只做规划与验证谓词求值，不派发 | 验证失败即 `Failed` |

- 策略影响面：介入准入（哪些 Run 操作被接受）、恢复链上限、DryRun 的副作用门禁。
  策略不改变安全底线：全局 SafetyPolicy 与权限确认（DEC-004）在任何策略下不被降低
  （`W-05`）。
- **默认值**：IR 未声明 `default_policy` 时为 `Strict`。该值在 M8 为暂定默认值，已于
  2026-09-08 随 [M9](../plans/m9-workflow-runtime-minimal-loop.md)（`M9-01`）冻结为正式
  默认。Run 启动时可从 `allowed_policies` 中
  选定策略；运行中切换只能切到 `allowed_policies` 内的目标，切换经控制面提交并发出
  `WorkflowPolicySwitched` 事件（M8-10）。`DryRun` 验证通过是 Workflow 新版本入库的前置
  （`W-04`，与 [DEC-022](DEC-022-conversation-patch-semantics.md) 的版本化联动）。
- 「策略影响导航代价权重」（架构 §7.5/§9.4）属阶段 E，本决策不冻结任何权重。
- 2026-09-08 补注：三种策略的运行时行为（升级路径、检查点、等待态出口与切换确认
  级别）已由
  [DEC-023](DEC-023-workflow-policy-set-runtime-semantics.md) 随
  [M10](../plans/m10-workflow-intervention-and-policy-set.md) 冻结并交付；`WaitingAgent`
  的载体进入路径为 `MiraRuntime::begin_task_recovery`（`Recovering` 的唯一控制面
  入口）。

### 4. 版本化契约（`M8-09` 的语义输入）

- WorkflowRun 创建时记录 `workflow_id + ir_digest`（创建时版本），**回放与续跑始终按
  该 digest 解析定义**，不受后续版本修改影响（`W-03`）。
- 版本记录冻结为不可变追加：`version`（`SemanticVersion`，单调递增）、`actor`（Who，
  已脱敏主体标识）、`reason`（Why，受限长度）、`content_digest`（What Changed，新 IR
  digest + 父版本 digest）、`validation`（`NotValidated | DryRunPassed | Validated |
  Rejected` + 证据 digest）、`created_at`。历史不可修改；「最新版本」「按名字索引」是
  可重建投影。
- 归纳/修改产生新版本是提议而非事实：未通过验证（至少 DryRun）的版本不得成为任何 Run
  的创建时版本（`W-04`）。（阶段 D 注记，M11：`DryRunPassed` 证据的生成管线随
  [DEC-025](DEC-025-success-trajectory-compilation-and-publish-gate.md) §3 的
  `publish_validated` 门禁交付——结构校验 + DryRun 驱动 + 内容派生证据摘要 + 幂等
  NoOp；`NotValidated` 原始入库路径仍保留为宿主信任边界。）

## 备选方案

- **扩展 `TaskState` 增加 `WaitingUser` 等**：改动 M1 冻结的核心状态机，波及全部既有
  状态转换测试与 Runtime 准入逻辑；而 Task 侧语义（暂停安全、恢复先观察）已能承载。
  不采用。
- **Run 与 Task 平行生命周期（Run 自带线程与队列）**：违反 `W-01` 单写者与 DEC-001
  Executor 所有权，制造第二套任务设施。不采用。
- **恢复时整步重跑（LangGraph 模型）**：与 `RULE-05` 至多一次派发冲突，副作用责任上移
  给步骤作者。不采用（参考研究 §5.3 的有意分歧）。
- **策略允许降低确认阈值**：以性能/流畅性为由在策略内豁免确认会破坏 `W-05` 与 DEC-004
  的全局安全边界。不采用；策略只能收紧不能放宽。

## 影响与风险

- 公开契约新增 `WorkflowRunState` 转换表、`task_state_for_run_state` 映射、Run 视图
  （含 `run_epoch`、步骤游标、`pending_decision`）与版本记录结构；全部为同步纯函数，
  不引入新的异步路径（M8 §5）。
- 两层视图的一致性由控制面同事务提交保证；需要 Runtime 集成（阶段 B）时以测试锁定
  「Task 终态 ⇒ Run 终态」的组合不变量。
- `Interactive` 策略与对话 patch 的交互细节（歧义确认流程）由
  [DEC-022](DEC-022-conversation-patch-semantics.md) 冻结；执行策略全集的运行时行为
  （导航权重、恢复链深度）在阶段 C 前不得宣称已实现。

## 验证方式

- 表驱动测试：全部合法/非法转换逐格断言；终态幂等；旧 epoch 迟到信号按 `Stale` 结算
  且不改状态；Run↔Task 映射全枚举断言。
- 版本化测试：不可变追加、版本单调、digest 钉住创建时版本、未验证版本不能被 Run 引用、
  历史投影可重建。

## 关联文档和工作项

- [M8](../plans/m8-workflow-contracts.md)：`M8-02`、`M8-08`、`M8-09`
- [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)、
  [DEC-018](DEC-018-takeover-input-release-and-operation-admission.md)、
  [DEC-019](DEC-019-workflow-ir-contract.md)、
  [DEC-022](DEC-022-conversation-patch-semantics.md)
- [Agent Harness 参考研究](../design/harness_reference_study.md) §5.3/§6（恢复模型分歧）
- [workflow_runtime_design](../design/workflow_runtime_design.md)
