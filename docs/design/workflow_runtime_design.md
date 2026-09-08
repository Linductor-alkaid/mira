# Workflow Runtime 设计

> 状态：Active（阶段 B 起的实施规范；本文接口为草案级别）
> 版本：0.1
> 更新日期：2026-09-08
> 负责人：Mira Maintainers
> 决策依据：[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)、
> [DEC-019](../decisions/DEC-019-workflow-ir-contract.md)、
> [DEC-020](../decisions/DEC-020-workflow-run-lifecycle.md)、
> [DEC-021](../decisions/DEC-021-workflow-tool-channel.md)、
> [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)
> 适用范围：Workflow 双路径的契约层（M8 已交付部分）与执行层（阶段 B 起）

## 1. 文档目的与效力

本文是 Workflow 方向的专项设计，分解 IR / WorkflowRun / Workflow Runtime 三层，定义验证
谓词与恢复钩子语义、事件 schema、错误分类、Executor 路由、取消与 shutdown 顺序和测试
策略。M8 交付的契约（`Mira::workflow` 模块）以本文为规范；阶段 B 的执行闭环实现以本文
为入口规范，实施里程碑将按 `W-01` 细化 Executor 路由与关闭表。

效力约定：

- 标注「**草案**」的接口尚未实现或不随 M8 交付；已实现部分以
  [API 手册](../api/workflow-contracts.md)与代码为准。
- 本文不改变既有里程碑状态；阶段 B 里程碑进入 `Planned` 前可依据本文修订，修订需同步
  本文档版本。

## 2. 三层分解

```text
WorkflowDefinition (IR, DEC-019)      不可变、内容寻址（digest）、版本化资产
        │  创建时钉住 ir_digest
        ▼
WorkflowRun (DEC-020)                 一次执行实例的语义视图：状态、游标、参数、决策点
        │  由 Task 生命周期承载（复用取消上下文、单写者控制面、epoch 结算）
        ▼
Workflow Runtime (阶段 B)             执行引擎：步进、验证、恢复、导航、DryRun 门禁
```

- **IR 层**（M8 已交付，`Mira::workflow`）：强类型值对象 + JSON 序列化 + fail-closed
  校验 + 参数绑定纯函数。无执行语义。
- **Run 层**（M8 已交付）：`WorkflowRunView`（`run_id`、`workflow_id`、`ir_digest`、
  状态、`run_epoch`、步骤游标、有效策略、`pending_decision`、`run_patch_epoch`）+
  状态转换表 + Task 映射 + 版本记录解析。状态提交由控制面独占（`W-01`）。
- **Runtime 层**（阶段 B，草案）：`WorkflowRuntime` 持有 Run 表与执行管线，经既有
  `MiraRuntime` 控制面提交状态（不另起控制线程）。

与 Agent Harness 的关系（统一步原语，参考研究 §5.3）：

```text
可恢复有界工作单元（step）= 事件提交 + 检查点 + 中断点
AgentLoop 步与 WorkflowRun 步共用该原语：单写者提交、步边界 drain 用户消息（DEC-016）、
步边界恢复 + 重新观察（DEC-020）、epoch 结算隔离迟到信号。
```

## 3. IR 结构与绑定（对齐 DEC-019）

已冻结结构见 DEC-019 §1–§3。补充执行层约束（阶段 B 实现，M8 已在契约层校验形状）：

- 步骤执行顺序 = 声明顺序；`precondition` 不满足 → `Skipped`（记事件，非失败）。
- `Control` 后向跳转目标必须是显式 `loop_head` 步骤；Runtime 维护每 loop 的已迭代计数，
  达到 `max_iterations` 后跳转被拒绝，Run 转 `Failed`（错误码 `ResourceExhausted`）。
- `max_attempts` 是单步执行尝试上限（含本地恢复重试）；跨过它必须走恢复钩子或失败。

## 4. 验证谓词与恢复钩子语义

### 4.1 谓词 DSL（v1 封闭子集）

```json
{"signal": "<signal-ref>", "op": "eq|ne|lt|le|gt|ge|contains|exists", "value": <json scalar>}
```

- `signal-ref` v1 封闭集合：`run_parameter:<name>`、`step_result:<step_id>`、
  `screen_state:<name>`。前两者在阶段 B 可完全求值；`screen_state` 绑定在阶段 E 落地，
  求值前 Runtime 对含它的谓词返回「不可求值」→ 按 DryRun 规则只能校验形状，不得宣称
  验证通过（`RULE-10`）。
- 求值是纯函数：`(predicate, bindings) -> Satisfied | NotSatisfied | NotEvaluable`。
  类型不匹配（如对 string 用 `lt`）= `NotEvaluable`，按配置视为失败（fail closed），
  不是静默通过。
- 既有验证分层（receipt → 结构化谓词 → screen diff → 本地感知 → VLM）在 Workflow 路径
  的映射：`step_result` 谓词对应 receipt/结构化层；更高级别证据经 `Verify` 步骤的
  arguments 声明（阶段 C 细化）。**每次外部副作用步骤后必须有验证**（`W-02`）；验证
  不可跳过，除非该步声明为无副作用且策略为 DryRun。

### 4.2 恢复钩子

```text
RecoveryHook
├── mode:      Retry | FallbackStep | AgentEscalation | None
├── max_retries / fallback_step_id / allow_agent
```

- `Retry`：同步骤重试，受 `max_attempts` 与策略约束；副作用不确定的结果禁止盲目重发，
  必须先重新观察（`RULE-05`/`W-02`）。
- `FallbackStep`：跳到声明的前进目标（必须晚于当前步，禁止后向逃逸出循环结构）。
- `AgentEscalation`：Run → `WaitingAgent`，Agent 获得 DEC-020 §7.6 定义的续跑上下文。
  IR 声明 `allow_agent=false` 或策略为 `Strict`/`DryRun` 时，该钩子在 IR 校验阶段被
  拒绝（策略与 IR 的一致性在创建 Run 时复核）。
- 恢复链消耗完仍失败 → `Failed`（或按策略 `WaitingAgent`）。

## 5. 事件 schema（M8-10 已交付契约层）

全部载荷自带 `schema` 字符串（`mira.workflow.*.v1`），未知字段 fail closed，`EventClass`
固定如下；载荷只含 ID、digest、枚举、原因码与受限摘要（脱敏规则见 DEC-022 §5）：

| 事件类型 | Class | 关键字段 |
| --- | --- | --- |
| `WorkflowRunStarted` | Critical | run_id、workflow_id、ir_digest、parameters_digest、policy |
| `WorkflowStepStarted` | State | run_id、step_id、kind、attempt |
| `WorkflowStepSettled` | State | run_id、step_id、disposition（`Completed/Skipped/Failed/Stale`）、verification 谓词结果摘要 |
| `WorkflowRunSettled` | Critical | run_id、终态、run_epoch、safe_summary |
| `WorkflowPatchProposed/Applied/Rejected` | State | patch_id、patch_digest、target、原因码 |
| `WorkflowPolicySwitched` | State | run_id、from、to |
| `WorkflowDecisionRaised/Resolved` | State | decision_id、payload_digest、resolution（accept/reject/cancel_run） |

OfflineReplay 语义（`W-08`）：以上事件在回放中被识别并重建投影（对话视图、Run 视图），
不派发输入、不调用工具、不发网络请求；已记录的 patch/决策结果显示为已发生事实。

## 6. 错误分类

Workflow 域错误沿用 `Error{domain="mira.workflow", domain_code}`，`ErrorCode` 取既有枚举；
`domain_code` 稳定命名（契约层已定义，随实现只增不改）：

| 类别 | domain_code 示例 | 语义 |
| --- | --- | --- |
| 契约错误 | `ir-unknown-field`、`ir-version-mismatch`、`ir-limit-exceeded`、`bind-*`（DEC-019 确定性绑定码） | 解码/校验/绑定失败，fail closed，不可重试 |
| 状态错误 | `run-invalid-transition`、`run-terminal`、`run-stale-epoch` | 非法转换/终态幂等外操作/迟到信号（Stale） |
| 资源错误 | `loop-budget-exceeded`、`step-attempts-exceeded` | RULE-08 预算耗尽 |
| 验证错误 | `verify-not-evaluable`、`verify-failed` | 谓词不可求值（按失败）/验证未通过 |
| 版本错误 | `version-not-validated`、`version-digest-mismatch` | 未验证版本不可被 Run 引用；digest 不符 |

错误对模型可见时一律经 `safe_message` 脱敏信封（DEC-021 §2）。

## 7. Executor 路由（按 `W-01` 预定义；阶段 B 里程碑细化为与 M7 同构的路由与关闭表）

M8 契约层全部为同步纯函数，无异步路径。阶段 B 的路由预定（草案）：

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| 步骤执行（离散动作派发） | `submit_auto` 普通有限任务 | Workflow Runtime（Run 表内句柄） | future 必须被消费；异常转 `Failed` 事件 |
| 每步验证观察 | `submit_auto` 普通有限任务 | 同上 | 不可丢弃 |
| DryRun 谓词求值 | 调用线程同步（纯函数） | 无 | 无 |
| 周期性健康检查（阶段 C 起） | timer 能力 | Runtime 生命周期所有者 | 停止时取消 |
| 实时连续控制（若 Workflow 含连续步骤） | realtime/low-latency 能力 | Runtime，watchdog 约束 | `Up/Cancel` 安全收敛（`RULE-06`） |

取消是协作式的：步执行轮询 `OperationContext` 取消与 deadline；连续控制经环境
`interrupt()` 收敛（DEC-018 同模式）。

## 8. 取消、暂停与 shutdown 顺序

Run 级操作（阶段 B 实现于 Runtime 控制面）：

1. `pause`：停止新步派发 → 在执行步骤到达安全点（连续控制收敛 + 平台输入释放）→
   提交 `Running -> Paused`（Task 侧 `Pausing -> Paused`）→ 步边界 drain 用户消息。
2. `resume`：重新观察（环境 epoch 变化则先走恢复判定）→ `Paused/Waiting* -> Running`，
   从游标继续。
3. `cancel`：提交取消意图 → 在执行动作安全收敛 → 终态 `Cancelled`（幂等）；迟到完成按
   Stale 结算。

Runtime shutdown（融入既有 Runtime 关闭顺序，DEC-001/AGENTS.md 第 6 条）：

```text
停止 Run 生产者（拒绝新的 run/resume 提交）
→ 向活动 Run 发取消/停止请求（协作式收敛）
→ 回收步骤执行任务与 timer 句柄
→ 等待有界步骤任务结算（有限等待，超时记诊断事件）
→ 随宿主 Runtime 的既有 shutdown(true)（非 worker 线程）完成
```

Workflow Runtime 不得自行 `shutdown` Executor；它不是 Executor owner。

## 9. 模块与依赖

```text
mira-workflow (Mira::workflow)
├── workflow_ir.hpp           IR 结构、序列化、校验、参数绑定（M8-06/07）
├── workflow_run.hpp          Run 视图、转换表、Task 映射（M8-08）
├── workflow_versioning.hpp   版本记录、不可变历史、digest 钉住（M8-09）
├── workflow_events.hpp       事件载荷构建/解析（M8-10）
└── workflow_tools.hpp        五操作 schema 与本地校验（M8-11）
```

依赖方向：`Mira::workflow` → `Mira::core`（契约、JSON）only。禁止依赖 Agent 决策层
（agent_loop、model_gateway）与任何平台类型（`RISK-2026-037`、`RULE-01`）；阶段 B 的
Runtime 执行体依赖 Workflow 契约，方向不变。

## 10. 测试策略

- **契约层（M8 已交付）**：见 DEC-019/020/021/022 的验证方式；矩阵含正负路径、fail
  closed、幂等、迟到隔离、digest 稳定性、脱敏断言、离线回放无副作用。
- **阶段 B 增量**（进入实施里程碑时冻结为测试矩阵）：执行闭环（Strict/DryRun 全路径）、
  取消竞态（步执行中取消/暂停/接管）、shutdown 顺序、验证不可求值路径、恢复钩子全模式、
  终态幂等 + 迟到完成隔离的运行时级测试、三个 Workflow 工具经 BuiltIn 闭环的执行与
  权限挂钩。
- 门禁：既有 ASAN/UBSAN/TSAN 矩阵与 installed-consumer 覆盖 `Mira::workflow`。

## 11. 关联文档

- 决策：DEC-014、DEC-019、DEC-020、DEC-021、DEC-022（及 DEC-015/016/017/018 既有边界）
- 计划：[M8](../plans/m8-workflow-contracts.md)；阶段 B 里程碑（依据本文进入 `Planned`）
- 参考研究：[Agent Harness 参考研究](harness_reference_study.md) §5.3/§6
- API：[workflow-contracts](../api/workflow-contracts.md)
