# Workflow 恢复编排设计（Agent Harness 恢复闭环）

> 状态：Active（冻结规范，决策载体为
> [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)；实现已随
> [M14](../plans/m14-recovery-orchestration.md) 交付，代码片段均为草案，签名以实现为准）
> 版本：1.0.1
> 更新日期：2026-09-10
> 负责人：Mira Maintainers
> 适用范围：`WaitingAgent` Run 的 Agent 侧恢复编排（Core 组件），不改变
> `WorkflowRuntime` 状态机与既有公开契约的已冻结语义

## 1. 文档目的与效力

本设计冻结阶段 F 后续的头号缺口：`relevant_lessons` 与 `agent_continuation` 只是数据，
Agent 侧缺少一个把「失败升级 → 经验检索 → 模型决策 → 修复 → 续跑」串成有界、可审计、
可取消闭环的编排者（[阶段 F 后续计划](../plans/maintenance-2026-09-post-stage-f.md) §2.3、
`MNT-202609-23`）。本文回答四件事：

1. 编排管线的每个阶段由谁拥有、输入输出是什么、失败出口在哪里。
2. 模型请求如何有界、归属哪个 Task、如何被准入与取消。
3. lesson 的采纳、过滤与拒绝在哪个层面发生，权限与脱敏如何保证。
4. 取消、Human Takeover、迟到响应、预算耗尽与 shutdown 如何闭合。

本文与 [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md) 同时生效；冲突时以
DEC 为准。语义依据 [DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)（升级
与续跑出口）、[DEC-024](../decisions/DEC-024-conversation-patch-execution.md)（patch 管线与准入
矩阵）、[DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)（学习闭环与 lesson 契约）、
[DEC-018](../decisions/DEC-018-takeover-input-release-and-operation-admission.md)（Takeover 准入）与
[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)（Episode/Lesson 结构）。

## 2. 背景、目标与非目标

### 2.1 现状边界

- `WorkflowRuntime` 已交付：失败升级进入 `WaitingAgent`（载体 Task 进入 `Recovering`）、
  `agent_continuation(run_id)` 续跑上下文（含 `relevant_lessons` 增量字段）、
  `patch_run`/`resume_run`/`cancel_run` 出口、`record_recovery_lesson` 宿主专用 lesson 记录。
- 事件闭集没有「Run 进入 `WaitingAgent`」的成员：升级只通过 drive 结果
  （`WorkflowRunResult.state == WaitingAgent`）、`run_snapshot` 与载体 Task 的
  `Recovering` 转换可观察。
- M13 测试由宿主直接 resume/记 lesson；模型从未参与修复，「Agent 采纳经验」无证据
  （`MNT-202609-23/24` 缺口登记）。
- lesson 的 `recovery[]` 只含 patch_id/digest/目标名称摘要，**不含参数值**
  （[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md) 脱敏边界）：采纳
  lesson 只能是模型合成新 patch，不存在机械重放路径。

### 2.2 目标

- 一个 Core 组件 `WorkflowRecoveryOrchestrator`（下称「编排器」），由宿主装配，把一次
  失败升级变成一个有界工作单元：装配上下文 → 一次（含有界修复回合的）模型请求 →
  结构化决策 → 经 DEC-024 管线提交 patch（或直接 resume/cancel/上交）→ 审计。
- 全链路可追踪：模型请求、决策摘要、patch、Run 升级序号由一条审计事件关联。
- 所有失败出口确定性命名并留下证据；任何出口都不改变 Run 终态幂等语义。

### 2.3 非目标

- 不修改 `WorkflowRuntime` 的状态机、转换表与既有 API 语义；仅按 DEC-030「增量字段」
  先例为 `WorkflowAgentContinuation` 增补载体标识字段（§5.4）。
- 不实现通用 Agent Loop（观察-推理-行动循环、工具执行）：编排器是单决策恢复控制器，
  不派发环境动作、不执行工具。
- 不为 `WaitingAgent` 引入向用户提问的通道（DEC-024 §4：修复期不向用户提问）；
  需要人类判断时上交宿主。
- 不改变 lesson 记录的所有权：`record_recovery_lesson` 仍是宿主专用 API（DEC-030 §4）。
- 不做 lesson 排序调优、向量召回、跨版本 lesson 复用开关（随 `MNT-202609-32` 按证据决策）。

## 3. 系统上下文与职责边界

```text
宿主（装配者/信任边界）
  │ 装配、notify_escalation、观察 Run 结果、record_recovery_lesson
  ▼
WorkflowRecoveryOrchestrator（Core，新增）
  │ 上下文装配、lesson 过滤、有界模型请求、决策解析、patch/resume/cancel 提交、审计事件
  ├─▶ WorkflowRuntime（既有公开 API：agent_continuation / patch_run / resume_run /
  │   cancel_run / run_snapshot）
  ├─▶ ModelGateway（既有：infer + TaskAdmissionGate + 预算台账）
  ├─▶ MiraRuntime（既有：task_snapshot 状态前置检查）
  └─▶ IEventStore（既有：WorkflowRecoveryAttempted 审计事件）
```

依赖方向：编排器 → WorkflowRuntime/ModelGateway/MiraRuntime/IEventStore，全部为 Core 内
既有抽象；编排器不接触平台 API，不接触 `IMemory`（检索已由 runtime 在升级期完成，
lesson 数据经 continuation 透出）。

职责一句话：

| 组件 | 职责 | 明确不做 |
| --- | --- | --- |
| 编排器 | 单次升级的恢复决策与提交、审计、预算 | 不验证修复效果、不写记忆、不派发输入 |
| WorkflowRuntime | 状态转换、patch 校验、resume 后的 Observe/Verify/驱动 | 不发起模型请求、不感知编排器 |
| ModelGateway | 模型调用路由、准入、预算、事件 | 不理解 workflow 语义 |
| 宿主 | 装配、触发通知、终局观察、lesson 记录 | 修复决策本身（可全部交给编排器） |

## 4. 编排管线

一次「恢复尝试（attempt）」对应一次失败升级，七个阶段全部为有界工作：

| # | 阶段 | 所有者 | 输入 | 输出 | 主要失败出口 |
| --- | --- | --- | --- | --- | --- |
| 1 | 触发与准入 | 编排器（宿主通知） | `notify_escalation(run_id)` | 准入判定 | `not-waiting-agent`、`attempt-in-progress`、`attempt-budget-exhausted`、`ResourceExhausted` |
| 2 | 前置状态检查 | 编排器 | `run_snapshot` + `task_snapshot` | 载体状态确认 | `run-state-changed`、`takeover` |
| 3 | 上下文装配与 lesson 过滤 | 编排器 | `agent_continuation` | 模型请求载荷（脱敏投影） | 无外部出口（过滤是数据准备） |
| 4 | 有界模型请求 | 编排器 → ModelGateway | 请求载荷 + 载体 Task 标识 | 结构化决策或失败 | `model-unavailable`、`cancelled`、`shutdown` |
| 5 | 决策解析与校验 | 编排器 | 模型输出 + 决策 schema | 合法决策或修复回合 | `decision-invalid`（修复回合耗尽后） |
| 6 | 决策执行 | 编排器 → WorkflowRuntime | 合法决策 | patch/resume/cancel/上交 | `patch-rejected:<code>`、`resume-rejected`、`run-state-changed`、`epoch-advanced` |
| 7 | 审计与回调 | 编排器 | attempt 结果 | `WorkflowRecoveryAttempted` 事件 + 宿主回调 | 事件失败转诊断计数器 |

### 4.1 触发与准入（宿主通知，无新增 runtime 回调）

- 编排器唯一触发面是 `notify_escalation(run_id)`（幂等通知）：宿主从 drive 结果
  （`start_run` 后 `wait_run`、`execute_run`、`resume_run` 后再 `wait_run` 返回
  `WaitingAgent`）或载体 Task 的 `Recovering` 转换获知升级后调用。运行时不新增回调或
  事件（升级的可观测性维持既有边界）。
- 准入（全部满足才受理，否则确定性拒绝并审计 reason_code）：
  - `run_snapshot(run_id).state == WaitingAgent`（否则 `not-waiting-agent`——迟到或重复
    通知的天然幂等出口）；
  - 该 Run 无在途 attempt（否则 `attempt-in-progress`，重复通知吸收不排队）；
  - 该 Run 已完成 attempt 数 < `max_attempts_per_run`（否则
    `attempt-budget-exhausted`，上交宿主）；
  - 追踪表未超 `max_tracked_runs`（RULE-08；淘汰已终态 Run 后仍超限则
    `ResourceExhausted`）。
- 同步原语 `attempt_recovery(run_id)` 在调用线程直接执行阶段 1–7（供确定性测试与同步
  宿主）；异步面 `start_recovery(run_id)` 提交一个 `submit_auto` 任务，容量受
  `max_concurrent_attempts` 约束，future 由编排器持有并在 `wait_recovery`/`shutdown`
  消费（成功入队不等于执行成功，`AGENTS.md` 第 3 条）。

### 4.2 前置状态检查（Takeover/终态 fail closed）

模型请求前，编排器经 `MiraRuntime::task_snapshot(carrier_task_id)` 复核载体状态：

- `Recovering` —— 通过；
- `SuspendedForTakeover`/`TakeoverSettling` —— 出口 `takeover`（DEC-018：被接管任务
  不准入新的自主工作；恢复决策会导向新的自主动作，同样被阻止）；
- 终态 —— 出口 `run-state-changed`（宿主已 cancel 等竞态）；
- 其余活跃态（`Pausing`/`Paused` 等，正常不可达）—— 出口 `run-state-changed`，不猜测。

### 4.3 上下文装配与 lesson 过滤

- 载荷 = `agent_continuation(run_id)` 的脱敏投影：run_id、workflow_id、ir_digest（hex）、
  有效策略、当前步骤（ID、种类、已用尝试数）、失败原因 safe 摘要、已结算步骤历史
  （ID/disposition/verdict）、pending_decision（如有）、`escalations` 计数。
- **有效参数的脱敏投影**：缺省只透出参数名、值类型与内容摘要（SHA-256 前 16 hex），
  不透出值；宿主可经 hooks 提供自定义投影（例如白名单参数透传）。脱敏责任在进入模型
  上下文前完成（DEC-023 §3「明文仅在宿主边界」的执行化）。
- **lesson 三层处理**：
  1. 解析层：逐条以 `workflow_episode_from_json`/`recovery_lesson_from_json` 解析
     statement，解析失败即丢弃并计数（fail closed，数据损坏不进入模型上下文）；
  2. 失效层：`workflow_id != continuation.workflow_id` 或
     `ir_digest != hex(continuation.ir_digest)` 判为失效（版本漂移），丢弃并计数；
     不提供跨版本复用开关（保守默认，`RULE-10`；放宽需 `MNT-202609-32` 证据）；
  3. 预算层：过滤后按 runtime 返回序截取前 `max_lessons_in_context` 条
     （排序信任 M4 检索既有规则，编排器不重排）。
- 过滤计数（offered/stale/unparseable/kept）进入审计事件（§5.5），使「无 lesson」与
  「有 lesson 但全被过滤」可区分。

### 4.4 有界模型请求

- 请求归属**载体 Task**：`task_id = continuation.carrier_task_id`、
  `task_epoch = continuation.carrier_task_epoch`。收益：模型事件按被恢复的 Task 归档；
  宿主安装的 `TaskAdmissionGate`（`admit(task, epoch)`）对恢复请求与普通请求施加同一
  生命周期准入；epoch 隔离迟到响应（载体自 `Recovering` 恢复时 epoch 递增，
  DEC-023 §1）。模型请求不是环境操作，不经 `begin_operation`（那属于输入派发路径）。
- 有界性：每次 attempt 至多 `1 + max_decision_repairs` 次请求；每次请求 deadline =
  `model_call_deadline`（经 `InferOptions`/`OperationContext` 双通道传达）；token 与费用
  经 `ModelBudget` 受既有台账约束（`RULE-08`）。
- 取消：`OperationContext.cancellation_requested` 探针 = attempt 取消标志（`cancel_recovery`
  或 `shutdown` 置位）。探针保持廉价（原子读，不回调 runtime——与
  `environment.hpp` 的探针纪律一致）。检查点在：请求前、每次修复回合前、决策执行前。
- 不暴露工具（`tools` 为空、`ToolChoice::None`）：恢复决策是结构化输出，不是工具调用；
  模型不得经恢复请求绕过工具通道触达其他能力（`W-04` 同源）。

### 4.5 决策解析与校验

决策 schema（`mira.workflow.recovery-decision.v1`，JSON Schema 子集，
`gate_schema_subset` 约束，草案）：

```json
{
  "action": "patch_and_resume | resume | cancel | need_user",
  "patch_entries": [{ "target": "...", "op": "set|unset|skip", "path": "...", "value": {} }],
  "used_lessons": ["<lesson_id 或 episode run_id，有界引用>"],
  "rationale": "<有界脱敏理由，≤2 KiB>"
}
```

- 四动作闭集：`patch_and_resume`（必须携带非空且通过
  `parse_workflow_patch_entries` + `validate_workflow_patch_entry` 的条目）、`resume`
  （不带 patch 重试）、`cancel`（判定不可恢复，DEC-023 §6「Agent 判定不可恢复时取消
  Run」的执行化）、`need_user`（上交宿主）。
- `used_lessons` 仅是审计引用（模型自述采纳了哪些经验），不构成任何权限或正确性声明；
  `rationale` 不进入事件（事件只带 decision digest），完整决策文本只存在于模型
  Request/Response 元数据事件的既有脱敏边界内。
- 解析失败（Malformed/Ambiguous/NoExecutableOutput/Refused…）时进入**修复回合**：携带
  violation 摘要重新请求（≤ `max_decision_repairs` 次）；耗尽后出口
  `decision-invalid`（`DeferredToHost`）。`Refused`（模型拒答）不消耗修复回合，直接
  `decision-invalid`——拒答不是可修复的格式问题。

### 4.6 决策执行

按决策动作分流，全部经既有公开 API（编排器无直达内部状态的路径）：

| 决策 | 执行序列 | 成功出口 | 失败出口 |
| --- | --- | --- | --- |
| `patch_and_resume` | `patch_run(generated patch_id, entries)` → 成功即 `resume_run` | `PatchedAndResumed`（含 patch_id） | patch 拒绝 → 修复回合（见下）；resume 拒绝 → `resume-rejected` |
| `resume` | `resume_run` | `ResumedWithoutPatch` | `resume-rejected` |
| `cancel` | `cancel_run`（幂等） | `CancelRequested` | 不可达（幂等 API） |
| `need_user` | 不动 Run | `DeferredToHost`（`decision-need-user`） | — |

- **patch 拒绝的修复回合**：`WorkflowPatchRejectedEvent.reason_code` 回喂模型重新决策
  （计入同一 attempt 的模型请求预算）；耗尽后 `DeferredToHost`
  （`patch-rejected:<code>`）。`WaitingAgent` 态 patch 立即应用（DEC-024 §3 边界语义），
  无排队歧义。
- **提交前重核**：每次向 runtime 提交（patch/resume/cancel）前重取 `run_snapshot`，若
  `run_epoch` 与阶段 3 采样不一致或状态已非 `WaitingAgent`，出口 `run-state-changed`
  （丢弃决策，不提交）。迟到模型响应因此不可能作用到已变化的 Run（`RULE-03`）。
- patch 条目合法性最终由 runtime 判定（未知 path、绑定失败、策略成员资格、Navigate
  门禁等确定性拒绝，DEC-024 §3）；编排器只做 schema 与 entry 形状的先期校验，不复制
  runtime 语义。

### 4.7 Observe/Verify 与宿主 lesson 记录

- `resume_run` 内部完成重新观察并提交新的异步驱动（既有契约）；此后每步验证归
  WorkflowRuntime（M9 每步 Verify），**编排器不新增验证路径，也不因模型或 lesson 的
  置信度跳过验证**。模型修复是否有效由运行时步验证与 Run 终局判定。
- 编排器职责止于阶段 7。Run 后续有三种走向，均不需要编排器参与：
  - 终态 `Completed`（恢复成功）——宿主观察到后调用 `record_recovery_lesson(run_id)`
    （DEC-030 §4 宿主专用；准入已含「失败升级计数 > 0」，恢复路径天然满足）；
  - 再次失败升级——宿主再次 `notify_escalation`，进入新 attempt（预算递减）；
  - `Failed`/`Cancelled`——结算期 Episode 记录照常（DEC-030 §2）。
- 宿主装配配方（consumer 示例随 `MNT-202609-24` 交付）：
  `start_run → wait_run → (WaitingAgent? notify_escalation → wait_recovery →) wait_run …`。

## 5. 数据模型与接口（草案）

以下接口为冻结草案：签名以实现 PR 为准，语义以本文与 DEC-031 为准。

### 5.1 配置

```cpp
struct WorkflowRecoveryConfig final {
    std::uint32_t max_attempts_per_run = 8;       // 每 Run 恢复尝试上限（RULE-08）
    std::chrono::milliseconds model_call_deadline{30'000};
    std::uint32_t max_decision_repairs = 1;       // 每 attempt 修复回合（决策/patch 共用预算）
    std::size_t max_lessons_in_context = 8;       // 与 DEC-030 检索上限对齐
    std::size_t max_rationale_bytes = 2'048;
    std::size_t max_concurrent_attempts = 1;      // 异步 attempt 容量
    std::size_t max_tracked_runs = 32;            // 追踪表上限（对齐 max_active_runs）
    ModelProfileId profile_id;                    // 恢复决策模型 profile（必填）
    // 校验失败在构造时 fail closed（零/负值、空 profile）。
};
```

全部数值为暂定默认值（`RULE-10`），随 `MNT-202609-28/29` 评估基线校准。

### 5.2 尝试结果

```cpp
enum class WorkflowRecoveryOutcome : std::uint8_t {
    PatchedAndResumed,     // patch 已应用并已提交 resume
    ResumedWithoutPatch,   // 模型判定无需 patch，直接 resume
    CancelRequested,       // 模型判定不可恢复，已 cancel
    DeferredToHost,        // 需要人类/宿主判断：need_user、decision-invalid、
                           // patch-rejected 修复耗尽、attempt-budget-exhausted
    Aborted,               // 外部条件终止：cancelled、takeover、shutdown、
                           // run-state-changed、epoch-advanced、model-unavailable、
                           // resume-rejected、not-waiting-agent（重复通知）
};

struct WorkflowRecoveryAttempt final {
    WorkflowRunId run_id;
    std::uint32_t ordinal = 0;          // 编排器侧每 Run 递增
    WorkflowRecoveryOutcome outcome;
    std::string reason_code;            // 闭集原因码，可空
    std::optional<Hash> decision_digest; // 合法决策的 canonical digest
    std::optional<WorkflowPatchId> patch_id;
    std::optional<ModelRequestId> model_request_id; // 关联模型事件
};
```

`DeferredToHost` 与 `Aborted` 的共同不变量：**Run 状态不被编排器改变**（保持
`WaitingAgent`），区别是前者请求宿主决断、后者等待外部条件变化后可重试。

### 5.3 编排器（公开类草案）

```cpp
class WorkflowRecoveryOrchestrator final {
  public:
    WorkflowRecoveryOrchestrator(executor::Executor &executor,
                                 WorkflowRuntime &runtime, MiraRuntime &control,
                                 ModelGateway &gateway,
                                 WorkflowRecoveryConfig config);
    void set_event_store(std::shared_ptr<IEventStore> events);
    void set_hooks(WorkflowRecoveryHooks hooks);   // on_attempt_settled 回调、参数投影

    // 同步原语：调用线程执行整个 attempt。
    [[nodiscard]] Result<WorkflowRecoveryAttempt> attempt_recovery(const WorkflowRunId &run_id);
    // 异步面：submit_auto 任务；future 由编排器持有并在 wait/shutdown 消费。
    [[nodiscard]] Result<void> start_recovery(const WorkflowRunId &run_id);
    [[nodiscard]] Result<WorkflowRecoveryAttempt> wait_recovery(const WorkflowRunId &run_id,
                                                                std::chrono::milliseconds timeout);
    [[nodiscard]] Result<void> cancel_recovery(const WorkflowRunId &run_id); // 协作取消
    [[nodiscard]] WorkflowRecoveryShutdownReport shutdown(); // 见 §7
};
```

### 5.4 `WorkflowAgentContinuation` 增量字段

对 DEC-023 §3 结构的**增量扩展**（既有消费者源码兼容，同 DEC-030 `relevant_lessons`
先例）：`carrier_task_id`、`carrier_task_epoch`、`run_epoch`、`escalations`（失败驱动
升级计数）。四者均已在 RunRecord 内部存在，仅透出；`escalations` 供模型上下文与预算
校验，`run_epoch`/`carrier_task_epoch` 供提交前重核（§4.6）。

### 5.5 审计事件 `WorkflowRecoveryAttempted`（闭集扩展）

v1 事件闭集新增一员（State 类，schema `mira.workflow.recovery-attempted.v1`，未知字段
fail closed，载荷只含 ID、digest、枚举名、有界计数器——与 DEC-030 §5 两员同纪律）：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `run_id` / `workflow_id` | ID | 关联 Run |
| `ordinal` | uint32 | 编排器侧 attempt 序号 |
| `task_id` | ID | 载体 Task（关联模型事件与 Task 事件） |
| `outcome` | 枚举名 | §5.2 五值闭集 |
| `reason_code` | 有界字符串 | 闭集原因码，可空 |
| `decision_digest` | digest（可选） | 模型决策 canonical digest |
| `patch_id` | ID（可选） | 已应用 patch |
| `model_request_id` | ID（可选） | 关联 ModelRequest 元数据事件 |
| `lessons_offered`/`lessons_stale`/`lessons_kept` | uint32 | 过滤计数 |

该事件是全链路关联键：ModelRequest/Response（经 `model_request_id`）→ 恢复决策（digest）
→ patch（`patch_id`，串起 DEC-024 三事件）→ Run 升级上下文（`run_id` + `ordinal`）。
事件发射在编排器互斥锁外、以提交序保证（与 runtime 事件纪律一致）；发射失败转诊断
计数器，不影响 attempt 结果。OfflineReplay 识别该事件为控制面审计事实：回放只重建
投影，不重发模型请求、不派发 resume（`W-08`）。

## 6. attempt 生命周期（状态机）

```text
Notified ──准入失败──────────────────────────▶ (Aborted, 不占预算)
    │受理
    ▼
Preflight ──takeover/状态变化─────────────────▶ (Aborted)
    ▼
Assembling ──continuation 失败────────────────▶ (Aborted, run-state-changed)
    ▼
Requesting ──取消/shutdown/模型失败───────────▶ (Aborted)
    ▼                                    ▲
Parsing ──解析失败(可修复)──修复回合───────────┘
    │合法决策
    ▼
Executing ──patch 拒绝(可修复)──修复回合────────┘
    │        └─耗尽/状态漂移/epoch 变化───────▶ (DeferredToHost / Aborted)
    ▼
Settled ──审计事件 + 宿主回调─────────────────▶ 终态（PatchedAndResumed /
                                               ResumedWithoutPatch /
                                               CancelRequested / DeferredToHost）
```

- attempt 一旦进入 `Requesting` 即占用该 Run 的预算槽位直至 Settled（失败也计数——
  防止反复失败重试绕过预算）。
- attempt 无跨 Run 共享可变状态；每 Run 独立预算与序号，追踪表条目在 Run 终态观察或
  `shutdown` 时回收。

## 7. 所有权、Executor 路由与 shutdown

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| 同步 attempt（`attempt_recovery`） | 调用线程；模型调用经 gateway 既有内部路由 | 无新任务 | `Result` 直接返回 |
| 异步 attempt（`start_recovery`） | `submit_auto` 有界任务（占一 worker 至多 deadline + ε） | 编排器持有 future，`wait_recovery`/`shutdown` 消费 | 容量 `max_concurrent_attempts`，超限 `ResourceExhausted`；异常进 attempt 结果不丢弃 |
| 取消 | 协作式：attempt 标志 + `OperationContext` 探针；模型调用按 deadline 可解除阻塞 | — | 出口 `cancelled`；不强制终止 |
| shutdown | `shutdown()`：拒绝新通知 → 置位全部取消 → 有界等待在途 future（drain）→ 返回报告 | 宿主（非 worker 线程）调用 | 报告含 clean/cancelled/drained 计数 |

- 关闭顺序（`AGENTS.md` 第 6 条）：编排器是 WorkflowRuntime 的任务生产者，必须在
  `WorkflowRuntime::shutdown()` **之前**完成 `WorkflowRecoveryOrchestrator::shutdown()`；
  在途 attempt 结束前不再向 runtime 提交任何 patch/resume。
- 并发不变量：每 Run 至多一个在途 attempt；追踪表与在途表由编排器互斥锁保护（叶子锁，
  runtime/gateway 调用与事件发射在锁外）。
- 无新增 timer、worker 池或私有并发；不涉 Executor 能力缺口，无需登记台账。

## 8. 失败出口矩阵

| 场景 | 判定点 | 出口 | Run 状态 | 宿主可见动作 |
| --- | --- | --- | --- | --- |
| 无 lesson（检索空/未装学习上下文） | 装配层 | 正常进行（空 lesson 列表） | 不变（attempt 继续） | 审计计数 `lessons_offered=0` |
| lesson 全部失效/不可解析 | 装配层过滤 | 正常进行（kept=0） | 不变 | 审计计数区分 stale/unparseable |
| 模型不采纳 lesson | 决策 `used_lessons` 为空 | `PatchedAndResumed` 等 | 按决策 | `used_lessons` 随决策 digest 审计 |
| 编排层拒绝（失效过滤） | 装配层 | 数据不进入上下文 | 不变 | 过滤计数 |
| 运行时拒绝 patch | DEC-024 管线 | 修复回合 → 耗尽 `DeferredToHost` | `WaitingAgent` | `patch-rejected:<code>` |
| 决策不可解析/拒答 | 解析层 | 修复回合 → `DeferredToHost` | `WaitingAgent` | `decision-invalid` |
| 失效 lesson（版本漂移） | 过滤层 | 丢弃 | 不变 | `lessons_stale` |
| 宿主/上游取消 | 探针 | `Aborted` | `WaitingAgent` | `cancelled` |
| Human Takeover | 前置检查/准入门 | `Aborted` | `WaitingAgent`（Takeover 由宿主对 Run 另行处置） | `takeover` |
| 迟到模型响应 | 提交前重核（run_epoch/task_epoch） | `Aborted`，决策丢弃不提交 | 保持宿主已造成的任何状态 | `run-state-changed`/`epoch-advanced` |
| 升级预算（runtime 侧） | `escalate_waiting_agent` | Run `Failed`（`escalation-budget-exceeded`，既有） | 终态 | 既有事件 |
| 恢复预算（编排器侧） | 通知准入 | `DeferredToHost` | `WaitingAgent` | `attempt-budget-exhausted` |
| 模型不可用/超时/熔断 | gateway | `Aborted` | `WaitingAgent` | `model-unavailable`（宿主可再 notify 重试，计数） |
| shutdown | 探针/准入 | `Aborted` | `WaitingAgent`（终局由 runtime shutdown 收敛） | `shutdown` |

矩阵覆盖 `MNT-202609-23` 验收要求列举的全部场景（无 lesson、拒绝采纳、失效 lesson、
取消/Takeover/迟到响应、升级预算、敏感信息）。

## 9. 安全、隐私、可观测性与 Replay

- **lesson 是数据不是授权**（`RULE-09`/DEC-030 §3）：lesson 的 confidence 与内容不参与
  任何准入判定；由 lesson 引导的 patch 仍走 DEC-024 全量校验；策略切换受
  `allowed_policies` 成员资格约束（只能收紧不能放宽，`W-05`）；高风险动作的确认仍由
  全局 SafetyPolicy 与 DEC-004 管线在输入派发层承担。**恶意 lesson 最多诱导一次
  `patch-rejected` 或一次无效修复**，无法提升权限、无法绕过验证、无法直达记忆写入。
- **脱敏**：参数投影缺省不带值（§4.3）；lesson statement 构造即脱敏（DEC-029）；
  `rationale` 不进事件；事件载荷全为 ID/digest/枚举/计数器；凭据与 Authorization 不进
  任何日志（既有规则）。
- **可观测性**：每次 attempt 一条审计事件 + hooks 回调（`on_attempt_settled`）；回调在
  编排器锁外调用、异常隔离转诊断（Observer 纪律）；诊断计数器（事件发射失败、通知
  拒绝原因分布）在 shutdown 报告透出。
- **Replay**：`WorkflowRecoveryAttempted` 为 State 类审计事实；OfflineReplay 重建投影
  但不重发模型请求、不重复 resume/cancel（`W-08`）；模型交互的复现走既有
  ModelReplay（recorded Provider）通道。
- **兼容性**：新增事件为 v1 闭集扩展，按 DEC-022 §2 fail-closed 纪律实施（未知事件
  类型/字段的既有消费者拒绝而非忽略）；`WorkflowAgentContinuation` 增量字段不破坏
  既有消费者源码兼容；不改变任何已冻结 API 语义。

## 10. 备选方案（摘要，详论见 DEC-031）

- **并入 AgentLoop（恢复作为 loop 的一步）**：把单决策恢复塞进通用循环，引入工具执行
  与环境动作面，违反 §2.3 边界且放大取消面。不采用。
- **WorkflowRuntime 内建编排（升级时自动发模型请求）**：runtime 获得模型依赖，破坏
  「Runtime 不感知 Agent 侧」的分层（DEC-023 已明确 Agent 参与由宿主装配）。不采用。
- **新增 runtime 回调/事件自动触发编排**：扩大闭集与回调面，收益仅是省一次宿主通知；
  升级已可经 drive 结果观察。不采用。
- **机械重放 lesson 的 patch（按 patch_digest 找回并重放）**：lesson 不含参数值，且旧
  patch 未必适配当前状态；重放绕过模型判断也无法处理参数化差异。不采用（如未来出现
  「确定性可重放修复」证据，随 `MNT-202609-32` 再评估）。
- **恢复期向用户提问（need_user 自动升决策点）**：DEC-024 §4 明确 `WaitingAgent` 修复
  期不向用户提问。不采用；`need_user` 上交宿主由宿主决定是否人工介入。

## 11. 已知限制与分阶段落地

- 编排器不观察 Run 终局：宿主负责在 `Completed` 后调用 `record_recovery_lesson`。这是
  DEC-030 §4 信任边界的直接结果，不是实现省略。
- lesson 的跨版本复用、排序反馈、失败检索向量腿均未开放（`MNT-202609-32`）。
- `BUG-20260909-002`（DEC-030 §5 载荷缺口）不阻塞本设计：编排器消费的 lesson 来自
  直接记录路径（含 `patch_digest`）；若未来重建路径成为数据源，方案 A 载荷扩展会
  强化 `patch_id` 关联，与本设计的过滤键（workflow_id/ir_digest）正交。
- 分阶段：本设计 + DEC-031 冻结（`MNT-202609-23`）→ `MNT-202609-24` 立项实现
  （编排器、增量字段、审计事件、consumer 测试）→ `MNT-202609-29` 评估（恢复组对照）。

## 12. 测试策略（映射 `MNT-202609-24` 验收）

- **决策矩阵**：recorded Provider 四动作 ×（合法/格式错/拒答/超时/熔断），修复回合
  边界（0/1/N），`used_lessons` 审计往返。
- **lesson 矩阵**：空 lesson、全失效（版本漂移）、不可解析 statement、混合截断；
  `lessons_offered/stale/kept` 计数断言；过滤后 statement 与原记录逐字节一致。
- **竞态矩阵**：请求在途中宿主 cancel / 第二次 notify / Takeover / run_epoch 变化；
  迟到响应丢弃（无 patch、无 resume、Run 状态保持宿主造成的终态，不复活）。
- **预算矩阵**：`max_attempts_per_run` 耗尽 `DeferredToHost`；runtime 升级预算先行
  耗尽的组合；`max_concurrent_attempts` 超限 `ResourceExhausted`；`max_tracked_runs`
  淘汰终态 Run。
- **shutdown 矩阵**：在途 attempt 的 drain、shutdown 后 notify 拒绝、编排器先于
  runtime 关闭的顺序断言；事件发射失败不影响结果。
- **端到端**（MNT-24 验收主场景）：Run 0 终态失败（Strict）→ Run A 失败升级（检索
  已见 Run 0 的 Episode）→ 编排修复 → 完成 → 宿主记 lesson → Run B 同签名失败升级 →
  检索命中失败结局的 Episode 与 A 的 Lesson → 模型合成修复 → 完成；全链路事件序列
  （ModelRequestPrepared → RecoveryAttempted → PatchProposed/Applied → StepSettled… →
  RunSettled → EpisodeRecorded/LessonRecorded）逐条断言关联键。实现澄清（与 DEC-030
  §2 一致）：Completed Run 的 Episode 不携带 `failed_step_id`（失败身份仅 failure 结局
  记录），签名检索命中的 Episode 来自失败结局的 Run；成功恢复的经验经其 Lesson 复用。
  原 v1.0 表述「命中 A 的 Episode+Lesson」按此语义理解为「失败结局 Episode + A 的
  Lesson」。
- **安全负向**：lesson 引导的越权 patch（策略越界/未知 path/危险参数形状）被确定性
  拒绝；参数投影缺省不带值；`rationale` 不出现在任何事件载荷。

## 13. 关联文档与后续问题

- 决策：[DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)（本设计载体）、
  [DEC-018](../decisions/DEC-018-takeover-input-release-and-operation-admission.md)、
  [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)、
  [DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)、
  [DEC-024](../decisions/DEC-024-conversation-patch-execution.md)、
  [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)
- 计划：[阶段 F 后续计划](../plans/maintenance-2026-09-post-stage-f.md)
  （`MNT-202609-23/24`）、[M13](../plans/m13-memory-and-learning-loop.md)
- 相关设计：[Workflow Runtime 设计](workflow_runtime_design.md) §14、
  [Agent Harness 与 Workflow 架构](agent_harness_and_workflow_architecture.md) §14
- 后续问题：恢复预算与模型 deadline 的实测校准（`MNT-202609-28/29`）；lesson 跨版本
  复用与确定性重放证据（`MNT-202609-32`）；真实 Provider 消费证据（`MNT-202609-27`）。
