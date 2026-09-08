# DEC-024：对话 patch 执行语义与决策点交互（阶段 C）

> 状态：Accepted
> 日期：2026-09-08
> 负责人：Mira Maintainers
> 冻结里程碑：[M10](../plans/m10-workflow-intervention-and-policy-set.md)
> 替代/被替代：无（是 [DEC-022](DEC-022-conversation-patch-semantics.md) §2/§3 语义的
> 执行层冻结；关闭其 §6 的超时与并发开放问题）

## 背景与问题

DEC-022 冻结了对话 patch 的判定规则、幂等键、审计事件、版本边界回退与 `WaitingUser`
决策点的解决语义（ID + digest 匹配、accept/reject/cancel_run 封闭枚举），M8 交付了全部
契约层载荷。但执行层缺失：`patch_workflow` 操作在 M9 保持 schema-only，
patch 如何进入运行时、在哪个边界生效、失败如何拒绝、回退如何提交，以及
`request_user_input` 的 schema（DEC-021 §4 预留席位）都没有实现语义。自然语言 → patch
条目的解释由模型完成，Runtime 的职责是「解释结果如何被校验、应用与审计」的机器侧
闭环。

## 决策

### 1. Patch 准入矩阵（来源 × 策略 × 状态）

patch 有两个入口：模型经 Tool 通道（`patch_workflow` handler）与宿主直达
（`WorkflowRuntime::patch_run`）。两者汇聚同一校验、幂等与审计管线；入口仅在准入
判断上不可区分（宿主直达视为与用户同权）。准入按「Run 有效策略 × Run 状态」判定：

| Run 状态 \ 有效策略 | `Interactive` | `Recoverable`/`AgentAssisted` | `Strict`/`DryRun` |
| --- | --- | --- | --- |
| `Running` | 接受（排队至步边界） | 拒绝 `policy-not-interactive` | 拒绝 `policy-not-interactive` |
| `Paused` / `WaitingUser` | 接受（立即，步边界语义） | 拒绝 `policy-not-interactive` | 拒绝 `policy-not-interactive` |
| `WaitingAgent` | 接受（Agent 修复） | 接受（Agent 修复） | 不可达 |
| 终态 | 拒绝 `run-terminal`（幂等重放按 §2 处理） | 同左 | 同左 |

- 例外通道：决策点 `accept` 携带的提议 patch（§6）不受 `policy-not-interactive` 拒绝
  （`WaitingUser` 决议本身即是确认），但走完整校验管线（`W-04`：用户答复不是免验通道）。
- 排队容量（`RULE-08`）：每 Run 待应用 patch 队列上限 16（可收紧）；超限拒绝
  `ResourceExhausted`。运行时 shutdown 后拒绝（生产者停止）。

### 2. 幂等与审计（DEC-022 §2 的执行化）

- `patch_id` + `patch_digest`（既有纯函数）是幂等键：同 ID 同 digest 重复提交返回
  NoOp 成功（`applied=false`、当前 `run_patch_epoch`）；同 ID 异 digest 拒绝
  `id-conflict`。判定在排队时（写前）与应用时各执行一次，覆盖「排队期间重复提交」。
- 事件序列：每个条目一条 `WorkflowPatchProposed`（patch_id + digest + 该条目 target
  + reason_code）→ 应用成功一条 `WorkflowPatchApplied`（patch_id + 新
  `run_patch_epoch`）；失败一条 `WorkflowPatchRejected`（patch_id + reason_code）。
  拒绝发生在校验阶段时不推进 epoch。
- 应用成功按整 patch 原子生效：任一条目校验失败则整个 patch 拒绝（无部分应用）；
  `run_patch_epoch` 每 patch +1（不是每条目）。
- 回退（DEC-022 §2「显式回退 patch」的执行化）：运行时记录每个已应用 patch 的
  应用前有效状态快照（有效参数表、有效策略、跳过集、步骤实参覆盖表）。
  `rollback_run_patch(run_id, target_patch_id)` 从快照与当前有效状态构造差异条目，
  以新生成的 `patch_id` 走同一管线（Proposed/Applied、epoch+1）；不提供隐式快照恢复。

### 3. 条目应用语义（步边界）

生效点 = 步边界（DEC-022 §2）：`Running` Run 的排队 patch 在驱动循环的每步迭代入口
排水应用，正在执行的步骤不受影响；`Paused`/`WaitingUser`/`WaitingAgent` Run 本就处于
边界，提交即应用。

- `run_parameters` Set/Unset：更新有效参数表后以
  `bind_workflow_parameters(definition, effective)` 语义整体重建绑定——Unset 移除该
  参数的覆盖值并回落声明默认值（可选参数）或触发 `MissingRequired` 拒绝（必选参数）；
  重建后**尚未执行**的步骤实参按新绑定重新解析（`$param` 引用），已结算步骤不回溯。
- `step_arguments` Set：value 作为该步骤的新实参（可含 `$param` 引用，经当前绑定解析；
  ToolCall 步骤必须满足「对象 + 保留 `tool` 成员 + 注册表命中 + 副作用步骤必须已有验证
  谓词」的既有绑定约定，否则整 patch 拒绝）。Unset：清除该步骤全部覆盖（实参覆盖与
  跳过标记），回落定义实参。Skip：将步骤加入跳过集；驱动到达跳过步骤时按 `Skipped`
  结算（summary 标注 run patch 跳过）并推进游标，不派发。
- `execution_policy` Set：见 [DEC-023](DEC-023-workflow-policy-set-runtime-semantics.md)
  §5（成员资格、Navigate 门禁、`WorkflowPolicySwitched`）。
- 条目 path 必须指向真实参数名或步骤 ID（runtime 检查，M8 schema 注明的边界）；
  未知 path 拒绝 `unknown-target`。
- 同一 patch 内多条目按数组顺序应用；后条目可覆盖前条目（同一 patch 内对同一 path 的
  重复写以最后一条为准，digest 覆盖该组合）。

### 4. `request_user_input` 工具 schema（关闭 DEC-021 §4 席位）

- arguments：`{workflow_id, run_id, prompt, proposal?}`——`prompt` 为已脱敏摘要
  （≤2 KiB，入队文本脱敏责任在宿主，DEC-016/022 §5 不变）；`proposal` 为可选
  patch 条目数组（形状同 `patch_workflow` 的 `patch_entries`，≤32 条）。
- 结果：`{decision_id, state:"waiting_user"}`。决策点是异步对象：工具调用只确认
  「已提出」，决议经宿主直达 API（§5）完成，结果经事件与对话视图回流。
- 准入：目标 Run 必须非终态、有效策略为 `Interactive`、当前无未决议决策点
  （每 Run 至多一个 `pending_decision`，DEC-022 §6 的 v1 上限维持），且不在
  `WaitingAgent`（Agent 修复期不向用户提问，先解决修复）。提交经
  `Running -> WaitingUser` 转换；载体任务走 pause 族。
- payload digest = canonical JSON digest of `{prompt, proposal}`；决策身份 =
  `decision_id` + 该 digest（DEC-022 §3 匹配语义）。
- 注册：`WorkflowRuntime::decision_tool_registrations()` 提供
  `request_user_input` 的 BuiltIn 注册（捕获 `this`，生命周期约束同四操作 handler）。

### 5. 决策点决议 API（宿主直达）

`resolve_decision(run_id, decision_id, payload_digest, resolution)`：

- 身份匹配：`decision_id` 与 `payload_digest` 都必须与当前 `pending_decision` 一致；
  不一致拒绝（`decision-mismatch`）并按当前决策点重新询问（不误配，DEC-022 §3）。
- `accept`：应用提议（`StepFailure` 类为跳过失败步骤；`AgentPrompt` 类为模型提议的
  patch 条目）→ 发出 `WorkflowDecisionResolved` → `WaitingUser -> Running` →
  异步续跑（与 `resume_run` 同驱动路径与容量约束）。
- `reject`：`StepFailure` 类终态 `Failed`——冻结转换表没有 `WaitingUser -> Failed`
  边，运行时先经合法边提交 `WaitingUser -> Running` 再自 `Running` 终态化（不新增
  转换边，两步提交在同一决议内完成）；`AgentPrompt` 类不经 patch 直接续跑
  （`WaitingUser -> Running` 异步驱动）。发出 `WorkflowDecisionResolved`。
- `cancel_run`：终态 `Cancelled`。
- **`WaitingUser` 的唯一出口是决议**：`resume_run` 对 `WaitingUser` 拒绝
  （`InvalidState`，指引先决议）；`pause_run` 幂等 NoOp。相比 M9 的接受分支这是收紧
  （fail closed：未决议的问题不得被静默跳过；M9 无法到达 `WaitingUser`，无既有依赖）。
- **超时策略（关闭 DEC-022 §6 开放问题）**：v1 不自动超时，决策点保持等待。
  理由：`WaitingUser` 承载 Task `Paused`（无自主动作、输入已释放），等待本身是安全
  状态；自动降级（取消或转 `Failed`）会替用户处置其未回答的问题。宿主可随时
  `cancel_run`；若未来需要超时，以新决策扩展，不改变本默认。

### 6. 决策点两类来源

| 类别 | 提出者 | 提议 | `accept` | `reject` |
| --- | --- | --- | --- | --- |
| `StepFailure` | Runtime（`Interactive` 失败，DEC-023 §4） | 跳过失败步骤 | 应用跳过 + 续跑 | `Failed` |
| `AgentPrompt` | 模型（`request_user_input`） | 模型提议 patch（可为空） | 应用提议 + 续跑 | 直接续跑 |

两类共用 `pending_decision` 结构、决议 API 与事件（`WorkflowDecisionRaised` /
`WorkflowDecisionResolved`）；类别在宿主可读的决策请求结构中区分，不进事件载荷
（事件只携带 ID 与 digest）。

### 7. 范围边界

- Workflow 定义修改与用户偏好记忆两个 patch 目标不经 `patch_workflow`：前者走
  `publish_workflow` 版本化管线（新版本须过验证，`W-04`；对话编排随阶段 D 里程碑），
  后者走既有 `update_memory` 规则（DEC-004 User-scope 审批）。`patch_workflow` 的
  条目 target 封闭于 run 范围（run_parameters / step_arguments / execution_policy，
  M8 冻结）。
- 自然语言 → 条目的解释（含三类目标判定与歧义升级 `WaitingUser` 的提示编排）是
  Agent 侧行为，不在 Runtime 范围；Runtime 提供的是解释结果的机器侧闭环。

## 备选方案

- **patch 立即生效（不等步边界）**：违反 DEC-022 §2 冻结的生效点语义，且正在执行
  步骤的实参中途变化会造成派发/验证不一致。不采用。
- **条目级部分应用（失败条目拒绝、其余应用）**：破坏整 patch 原子性与幂等键语义
  （digest 覆盖整组条目）。不采用。
- **回退 = 隐式恢复快照**：违反 DEC-022 §2「显式提交回退 patch」；不可审计。不采用。
- **决策点自动超时转 `Paused` 或取消**：替用户处置未回答问题，且 `WaitingUser` 已具
  备暂停族安全性；超时无新增安全收益。不采用（保持等待，宿主可取消）。
- **`request_user_input` 同步返回决议结果（阻塞工具调用至用户答复）**：把无界人工
  等待塞进工具执行路径，违反有界工作单元与 Executor 生命周期约束。不采用；决议是
  异步对象，经控制面回流。
- **`resume_run` 直接绕过 `WaitingUser`（隐式 reject）**：`StepFailure` 与
  `AgentPrompt` 的 reject 语义不同（`Failed` vs 续跑），隐式绕过会造成类别相关的静默
  分歧。不采用；显式决议是唯一出口。

## 影响与风险

- `WorkflowRuntime` 公开面新增：`patch_run`、`rollback_run_patch`、`resolve_decision`、
  `pending_decision_request`、`decision_tool_registrations`；RunRecord 增加有效状态
  快照、排队与审计结构（内部）。`patch_workflow` 注册进四操作集合（五操作齐活）。
- Run 视图字段 `run_patch_epoch` 与 `pending_decision` 自本决策起有运行时写入者；
  状态转换仍全部经 `apply_workflow_run_transition`，patch 应用与决议在运行时互斥下
  串行提交（控制面单写者语义扩展到 patch 平面，事件先于视图变更不可见——事件发射
  在锁外，顺序由提交序保证）。
- `resume_run` 对 `WaitingUser` 从接受改为拒绝：M9 未触达该状态，无行为依赖；API
  手册披露。
- 模型提议 patch 的质量风险（归纳错误/恶意条目）由既有校验管线兜底：未知 path、
  绑定失败、策略门禁、Navigate 门禁全部确定性拒绝；用户 accept 是第二道确认
  （`W-04`/`W-05`）。
- 每步边界排水的成本：队列为空时是 O(1) 原子检查；上限 16 保证有界。

## 验证方式

- 幂等与审计：同 ID 同 digest NoOp、同 ID 异 digest 拒绝、三事件序列与顺序、
  epoch 语义（拒绝不推进、应用 +1）、排队期间重复提交的写前判定。
- 应用语义：步边界生效（执行中步骤不受影响）、参数重建（Unset 回落默认/必选拒绝）、
  未执行步骤重解析与已结算步骤不回溯、Skip 结算、policy 切换门禁、未知 path 拒绝、
  多条目顺序与整 patch 原子性。
- 回退：快照差异构造、走同一管线（事件/epoch）、回退后再回退（链式）、回退不存在的
  patch 拒绝。
- 决策点：两类来源、ID+digest 匹配与不匹配、accept/reject/cancel_run 全路径、
  `WaitingUser` 唯一出口（resume 拒绝）、并发上限（已有决策点时新请求拒绝）、
  超时默认不动作。
- 工具闭环：`patch_workflow` 与 `request_user_input` 经 BuiltIn 注册表执行、模型经
  AgentLoop 发起 patch 与决策的端到端、错误信封。
- M9 回归：四操作、执行与控制全路径不回归。

## 关联文档和工作项

- [M10](../plans/m10-workflow-intervention-and-policy-set.md)：patch 与决策点工作项
- [DEC-015](DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-016](DEC-016-conversation-events-and-user-messages.md)、
  [DEC-020](DEC-020-workflow-run-lifecycle.md)、
  [DEC-021](DEC-021-workflow-tool-channel.md)、
  [DEC-022](DEC-022-conversation-patch-semantics.md)、
  [DEC-023](DEC-023-workflow-policy-set-runtime-semantics.md)
- [Workflow Runtime 设计](../design/workflow_runtime_design.md)（阶段 C 章节）
