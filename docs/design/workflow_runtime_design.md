# Workflow Runtime 设计

> 状态：Active（阶段 F 实施规范；契约层随 M8、执行层随 M9、介入与策略随 M10、编译与归纳随 M11、导航随 M12 交付，接口以代码与 API 手册为准）
> 版本：0.6
> 更新日期：2026-09-10（§14.3/§15 增补恢复编排设计与 DEC-031 交叉引用）
> 负责人：Mira Maintainers
> 决策依据：[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)、
> [DEC-019](../decisions/DEC-019-workflow-ir-contract.md)、
> [DEC-020](../decisions/DEC-020-workflow-run-lifecycle.md)、
> [DEC-021](../decisions/DEC-021-workflow-tool-channel.md)、
> [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)、
> [DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)、
> [DEC-024](../decisions/DEC-024-conversation-patch-execution.md)、
> [DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md)、
> [DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md)、
> [DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)、
> [DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md)、
> [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
> [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)
> 适用范围：Workflow 双路径的契约层（M8 已交付部分）、执行层（阶段 B/C）、编译层
> （阶段 D）、导航层（阶段 E）与学习层（阶段 F）

## 1. 文档目的与效力

本文是 Workflow 方向的专项设计，分解 IR / WorkflowRun / Workflow Runtime 三层，定义验证
谓词与恢复钩子语义、事件 schema、错误分类、Executor 路由、取消与 shutdown 顺序和测试
策略。M8 交付的契约（`Mira::workflow` 模块）以本文为规范；阶段 B 的执行闭环已随
[M9](../plans/m9-workflow-runtime-minimal-loop.md) 交付（`WorkflowRuntime`，Strict/DryRun），
第 7 节路由表按 M9 定稿；阶段 C（策略全集、对话 patch 与决策点）的实施规范见第 11 节，
由 [M10](../plans/m10-workflow-intervention-and-policy-set.md) 承载；阶段 D（成功轨迹
编译与任务归纳）的实施规范见第 12 节，由 [M11](../plans/m11-trajectory-compilation-and-task-induction.md)
承载；阶段 E（App Model 与导航）的实施规范见第 13 节，由
[M12](../plans/m12-app-model-and-navigation.md) 承载；阶段 F（Memory 与学习闭环）的
实施规范见第 14 节，由 [M13](../plans/m13-memory-and-learning-loop.md) 承载。

效力约定：

- 标注「**草案**」的接口尚未实现；已实现部分以
  [API 手册](../api/workflow-contracts.md)与代码为准。
- M9 交付后的契约补全（`M9-01`）：ToolCall 步骤的 `arguments` 必须为对象且携带保留成员
  `"tool"`（字符串 wire 名，与 Navigate 的 `arguments["target"]` 同属按步骤种类的实参
  约定），其余成员构成工具输入并经注册表 schema 校验；dispatching 策略下 Navigate 步骤
  于创建 Run 时 fail closed（`navigate-unresolvable`），DryRun 按形状规划结算；
  `validate_workflow_definition` 对结构体定义执行与 JSON 解码同源的结构校验。

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
  `screen_state:<state_id>`。前两者在阶段 B 可完全求值；`screen_state` 自阶段 E
  （M12）起由宿主快照绑定求值（DEC-028 §2）：当前状态注入
  `screen_state:<state_id>`（true）与 `screen_state:current`（状态 ID 字符串）两个
  条目，其余状态名不在上下文中（`NotEvaluable` fail closed）；宿主 Provider 缺席或
  返回空时不注入任何条目，全部 `screen_state` 谓词维持不可求值——按 DryRun 规则
  只能校验形状，不得宣称验证通过（`RULE-10`）。
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
  拒绝（策略与 IR 的一致性在创建 Run 时复核）。阶段 C 补全触发时机（DEC-023 §2）：
  失败驱动（`Recoverable`/`Interactive`）与到达驱动（`AgentAssisted` 主动检查点），
  两种时机按策略互斥。
- 恢复链消耗完仍失败 → `Failed`（或按策略升级：agent-capable 策略 → `WaitingAgent`，
  `Interactive` → `WaitingUser` 决策点；DEC-023 §1）。

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
| `WorkflowPublishProposed/Applied/Rejected`（阶段 D） | State | workflow_id、ir_digest、source_run_id、evidence、原因码 |
| `WorkflowNavigationPlanned/Observed`（阶段 E） | State | run_id、step_id、from/to 状态、edge_count、plan_digest、total_cost、guard 计数；transition_id、success、confidence |
| `WorkflowEpisodeRecorded`（阶段 F） | State | run_id、workflow_id、episode_digest、outcome（recorded/failed）、原因码 |
| `WorkflowLessonRecorded`（阶段 F） | State | run_id、workflow_id、lesson_digest、outcome（recorded/failed）、原因码 |

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

## 7. Executor 路由（M9 定稿；M9 里程碑第 5 节为验收载体）

M8 契约层全部为同步纯函数，无异步路径。M9 交付的路由表：

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| 步骤执行（离散工具派发） | `submit_auto` 普通有限任务 | 驱动任务（逐个消费 future） | 提交拒绝与任务异常转步骤失败（可经恢复钩子） |
| 每步副作用后验证观察 | `submit_auto` 普通有限任务 | 驱动任务 | 不可丢弃；失败即步骤失败 |
| Run 驱动（宿主同步路径 `execute_run`） | 宿主调用线程 | 调用方 | 结果同步返回 |
| Run 驱动（异步路径 `start_run`、`run_workflow` 工具、`resume_run` 续跑） | `submit_auto` 普通有限任务，并发上限默认 1 且必须小于 worker 数 | Workflow Runtime（Run 表内 future） | future 由 `wait_run`/shutdown 消费；异常转 `Failed` 结算与诊断 |
| 驱动监控（撤回旗标） | `submit_auto` 有限任务，轮询承载 Task 快照 | 驱动任务 | 驱动结束时置停并消费 future |
| DryRun 谓词求值 | 调用线程同步（纯函数） | 无 | 无 |
| 控制面命令（pause/resume/cancel/complete） | 既有 `MiraRuntime` 串行控制面 | Workflow Runtime（CommandHandle） | 有界等待回执；失败对调用方可见 |
| 升级进入 `WaitingAgent`（`begin_task_recovery`） | 既有 `MiraRuntime` 串行控制面 | Workflow Runtime（CommandHandle） | 有界等待回执；失败转诊断并保持 Run 视图一致 |
| 决议续跑（`resolve_decision` accept/reject 后的驱动） | `submit_auto` 普通有限任务，与 resume 共用并发上限 | Workflow Runtime（Run 表内 future） | future 由 `wait_run`/shutdown 消费 |
| 轨迹捕获（`capture_trajectory`，阶段 D） | 调用线程（互斥下快照拷贝） | 无 | 生效态一致性由互斥保证 |
| 编译与归纳（`compile_workflow`/`induce_parameters`，阶段 D） | 调用线程同步（纯函数） | 无 | 无 |
| 门禁驱动（`publish_validated` 内的 DryRun `execute_run`，阶段 D） | 宿主调用线程（复用 `execute_run` 同步路径） | 调用方 | 门禁 Run 占用 Run 表容量；失败即拒绝发布 |
| 导航规划（`plan_navigation`，阶段 E） | 调用线程同步（纯函数） | 无 | 无 |
| 读屏（ScreenStateProvider 回调，阶段 E） | 驱动/调用线程同步回调 | 宿主 | 宿主保证非阻塞；失败或缺席即 fail closed |
| 导航边动作派发（Navigate 步逐边，阶段 E） | `submit_auto` 普通有限任务（复用步骤执行通道） | 驱动任务 | 计入 Run 步预算；异常转步骤失败 |
| 置信度回写（阶段 E） | 调用线程（互斥下投影更新） | 无 | 更新是纯函数；事件在锁外发射 |
| Episode 记录（结算期 `IMemory.apply`，阶段 F） | 结算线程同步门面（后端自路由 store worker） | 无新任务 | 失败转诊断计数器与审计事件，不影响终态 |
| 失败检索（升级期 `IMemory.query`，阶段 F） | 驱动线程同步门面、有界 deadline | 无新任务 | 失败/超时降级为空结果 + 诊断计数器 |
| Lesson 记录（`record_recovery_lesson`，阶段 F） | 调用线程同步门面 | 无新任务 | 失败对调用方可见（Result 错误）+ 审计事件 |
| patch 应用（步边界排水 / 等待态直达） | 调用线程（控制面提交）或驱动任务（排水） | 无新任务 | 校验失败确定性拒绝；事件在锁外发射 |
| 周期性健康检查 | timer 能力（随首个需要它的阶段立项，M10 非目标） | Runtime 生命周期所有者 | 停止时取消 |
| 实时连续控制（若 Workflow 含连续步骤） | realtime/low-latency 能力 | Runtime，watchdog 约束 | `Up/Cancel` 安全收敛（`RULE-06`） |

取消是协作式的：步执行轮询 `OperationContext` 取消与 deadline；连续控制经环境
`interrupt()` 收敛（DEC-018 同模式）。步骤取消探针只读原子旗标（由监控任务发布），
不在持有环境锁的上下文内回调 Runtime，锁序以 WorkflowRuntime 内部互斥为叶子。

## 8. 取消、暂停与 shutdown 顺序

Run 级操作（阶段 B 实现于 Runtime 控制面）：

1. `pause`：停止新步派发 → 在执行步骤到达安全点（连续控制收敛 + 平台输入释放）→
   提交 `Running -> Paused`（Task 侧 `Pausing -> Paused`）→ 步边界 drain 用户消息。
2. `resume`：重新观察（环境 epoch 变化则先走恢复判定）→ `Paused/Waiting* -> Running`，
   从游标继续。
3. `cancel`：提交取消意图 → 在执行动作安全收敛 → 终态 `Cancelled`（幂等）；迟到完成按
   Stale 结算。

M9 实现补注：被暂停截断的步骤按 `Stale` 结算且游标不推进；`resume` 先重新观察、先提交
`Paused -> Running`（冻结转换表没有 `Paused -> Failed` 边），再处理中断步骤——无副作用
工具或 DryRun 直接重执行；副作用工具复评验证谓词，满足则按已完成恢复，否则仅
`FallbackStep` 可救，缺钩子以 `ExecutionUncertain` 失败（`RULE-05` 禁止盲目重发）。

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
├── workflow_ir.hpp           IR 结构、序列化、校验、参数绑定（M8-06/07；M9 补 validate）
├── workflow_run.hpp          Run 视图、转换表、Task 映射（M8-08）
├── workflow_versioning.hpp   版本记录、不可变历史、digest 钉住（M8-09）
├── workflow_events.hpp       事件载荷构建/解析（M8-10；阶段 D 增 publish 三员）
├── workflow_tools.hpp        五操作 schema 与本地校验（M8-11）
├── workflow_compiler.hpp     轨迹契约、编译与归纳纯函数（阶段 D，DEC-025/026）
├── workflow_navigation.hpp   App Model 契约、置信度与导航规划纯函数（阶段 E，DEC-027/028）
├── workflow_learning.hpp     Memory 四类域、Episode/Lesson 契约与失败检索纯函数（阶段 F，DEC-029/030）
└── workflow_runtime.hpp      执行闭环：Run 表、驱动、控制与四操作 handler（M9）
```

依赖方向：`Mira::workflow` → `Mira::core`（契约、JSON）only。禁止依赖 Agent 决策层
（agent_loop、model_gateway）与任何平台类型（`RISK-2026-037`、`RULE-01`）；阶段 B 的
Runtime 执行体依赖 Workflow 契约，方向不变。

## 10. 测试策略

- **契约层（M8 已交付）**：见 DEC-019/020/021/022 的验证方式；矩阵含正负路径、fail
  closed、幂等、迟到隔离、digest 稳定性、脱敏断言、离线回放无副作用。
- **阶段 B 增量**（已随 M9 冻结为测试矩阵并交付）：执行闭环（Strict/DryRun 全路径）、
  取消竞态（步执行中取消/暂停/接管）、shutdown 顺序、验证不可求值路径、恢复钩子全模式、
  终态幂等 + 迟到完成隔离的运行时级测试、启动与控制类 Workflow 操作（run/pause/resume/
  cancel 四操作；`patch_workflow` 的执行属阶段 C）经 BuiltIn 闭环的执行与权限挂钩。
- **阶段 C 增量**（DEC-023/024 验证方式的里程碑化）：策略矩阵（失败分流、钩子两时机、
  检查点到访计数、跨等待周期计数器累计）、`WaitingUser`/`WaitingAgent` 的载体一致性与
  出口、patch 幂等/审计/原子性/边界生效/参数重建/回退、决策点两类来源与决议全路径、
  `patch_workflow` 与 `request_user_input` 的工具闭环（含模型端到端）、M9 全路径回归。
- **阶段 D 增量**（DEC-025/026 验证方式的里程碑化）：捕获矩阵（生效态快照、DryRun/
  非终态/失败 Run 拒绝）、编译矩阵（默认值固化、跳过剔除、`jump_to` 负向、确定性
  digest）、门禁矩阵（DryRunPassed 入库与可 Run、失败库零变更、幂等 NoOp、版本链与
  不可变回归）、归纳矩阵（结构 diff 命名与常量保持、显式候选校验、参数化重写与
  绑定解析、类型不一致负向）、publish 三事件序列与离线回放无副作用、端到端
  （Run -> 捕获 -> 归纳 -> 编译 -> 入库 -> 新版本运行）。
- **阶段 E 增量**（DEC-027/028 验证方式的里程碑化）：契约矩阵（App Model 往返、
  fail closed、digest 确定性）、置信度矩阵（成功/失败更新逐值、衰减单调、探索阈值、
  纯函数性）、规划器矩阵（确定性、权重选路、guard 分计数、平局字典序、agent 边
  排除、预算、未知端点、无路径 vs guard 全挡）、Runtime 导航矩阵（无上下文
  `navigate-unresolvable` 回归、逐边派发与到达验证、`navigate-arrival-unverified`、
  `navigate-no-screen-state`、目标未声明、工具缺失、置信度回写与两员事件、DryRun
  真实规划、step 预算计入、取消 Stale、policy patch 门禁放松）、谓词矩阵
  （`screen_state` 绑定求值与缺席 fail closed 回归）。
- **阶段 F 增量**（DEC-029/030 验证方式的里程碑化）：域矩阵（`MemoryKind` 全集映射
  逐值、逆映射覆盖、name/parse 负向）、契约矩阵（Episode/Lesson/签名 JSON 往返、
  fail closed、digest 确定性、消毒规则负向）、转换矩阵（`to_memory_record` 逐字段且过
  `MemoryRecord::validate`、lesson 往返与非 canonical fail closed、纯函数性）、查询矩阵
  （同签名同查询、kinds 恒为 Episodic 服务子集、limits 负向）、结算矩阵（三终态记录、
  DryRun/未装上下文零记录、写失败不影响终态、幂等）、升级矩阵（失败驱动检索进入
  `relevant_lessons`、检查点让渡不检索、查询失败降级、条数上限）、Lesson 矩阵（准入
  负向、两形态派生、幂等、失败可见）、事件矩阵（两员往返与闭集扩展、离线回放无
  IMemory 调用）、端到端（失败 -> Episode -> 恢复完成 -> Lesson -> 再失败检索同时命中
  两者）。
- 门禁：既有 ASAN/UBSAN/TSAN 矩阵与 installed-consumer 覆盖 `Mira::workflow`。

## 11. 阶段 C 实施规范（策略全集、对话 patch 与决策点）

规范来源：[DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)（策略
运行时语义与检查点）、[DEC-024](../decisions/DEC-024-conversation-patch-execution.md)
（patch 执行与决策点交互）。本节为里程碑 [M10](../plans/m10-workflow-intervention-and-policy-set.md)
的入口摘要；语义细节以决策为准。

### 11.1 策略全集与升级路径

- 准入：五种策略全部可执行；`AgentEscalation` 钩子要求 agent-capable 策略（既有契约
  校验，M9 的两策略限制门移除）。
- 失败分流（恢复链耗尽点，步边界）：`Strict`/`DryRun` → `Failed`（M9 不变）；
  `Recoverable`/`AgentAssisted` → `WaitingAgent`（载体任务经 `begin_task_recovery` 进入
  `Recovering`）；`Interactive` → `WaitingUser` 决策点（载体任务走 pause 族）。
- 检查点（`AgentAssisted`）：`AgentEscalation` 钩子声明的步骤即检查点；到达驱动，
  每次到达至多交出一次，循环回归再交出，受 loop/步预算约束。
- 有界性：跨 `WaitingAgent` 周期不重置任何计数器（`RULE-08`）；出口 =
  `resume_run`（重新观察 + 有界重试）/ `cancel_run` / 修复期 patch。
- Agent 续跑上下文：`agent_continuation(run_id)` 仅 `WaitingAgent`；有效参数只在宿主
  边界出现，进模型上下文走既有脱敏。

### 11.2 Patch 平面

- 入口：`patch_workflow` 工具（模型）与 `patch_run`（宿主直达），同一管线；准入矩阵、
  幂等键（`patch_id`+`patch_digest`）、三事件审计、整 patch 原子性、每 Run 排队上限
  16（DEC-024 §1–§3）。
- 生效点：步边界排水（Running）或等待态直达；参数条目以绑定纯函数整体重建有效状态，
  未执行步骤重解析、已结算不回溯；Skip 进跳过集按 `Skipped` 结算；policy 条目经
  `allowed_policies` 门禁 + Navigate 剩余步骤门禁 + `WorkflowPolicySwitched`。
- 回退：`rollback_run_patch` 从应用前快照构造显式回退 patch，新生成 `patch_id` 走同一
  管线；无隐式快照恢复。

### 11.3 决策点

- 两类来源（`StepFailure`：Runtime 在 Interactive 失败时提出；`AgentPrompt`：模型经
  `request_user_input` 提出），共用 `pending_decision`（ID+digest）、决议 API 与两事件。
- `resolve_decision(run_id, decision_id, payload_digest, resolution)`：accept 应用提议并
  异步续跑；reject 按类别终态化或直接续跑；cancel_run 取消。`WaitingUser` 唯一出口是
  决议（`resume_run` 拒绝并指引）。
- 超时默认：不自动超时（等待是安全状态；宿主可取消）。
- 并发上限：每 Run 至多一个 `pending_decision`。

### 11.4 显式非目标（阶段 C 内）

- timer 周期健康检查：无已验证消费者，不随 M10 引入（本设计 §7 的标注改为「随首个
  需要它的阶段立项」，与 `RISK-2026-038` 持久化推迟同一模式）。
- Verify 步骤更高级证据声明（screen diff / 本地感知 / VLM 层）：依赖阶段 E 感知落地，
  M10 维持谓词/receipt 两层（§4.1 的「阶段 C 细化」以此显式收敛为推迟）。
- 自然语言 → patch 条目的解释编排与 Workflow 定义/偏好两类目标：Agent 侧与阶段 D。
  （阶段 D 注记：定义级「本次 -> 默认」目标经轨迹编译与门禁入库落地（DEC-025 §2）；
  偏好目标走 `update_memory` 既有规则；两者的对话解释编排仍属 Agent 侧，推迟不变。）


## 12. 阶段 D 实施规范（成功轨迹编译、任务归纳与入库门禁）

规范来源：[DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md)
（轨迹契约、编译与入库门禁）、[DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md)
（任务归纳与参数化）。本节为里程碑 [M11](../plans/m11-trajectory-compilation-and-task-induction.md)
的入口摘要；语义细节以决策为准。

### 12.1 轨迹与编译

- 轨迹（`WorkflowTrajectory`）是一次真实成功执行的结构化快照：溯源三元组
  （workflow、钉住版本 digest、来源 Run）、有效参数表、有效策略与生效步骤序列
  （字面量生效实参 + 含 `$param` 的原始实参作为 provenance）。
- 采集（`WorkflowRuntime::capture_trajectory`）只对 `Completed` 且实际派发副作用的
  Run 开放（DryRun 完成与非终态 fail closed，`RULE-10`）；跳过步剔除，patch 覆盖
  后的生效态入快照。宿主可直接构造轨迹（Agent 工具调用路径）。
- 编译（`compile_workflow` 纯函数）产出草稿：生效实参字面量化、参数默认值固化
  （required -> 可选 + 观测默认）、跳过步剔除（`jump_to` 指向剔除步 fail closed）、
  策略沿用来源允许集、产物过 `validate_workflow_definition` 同源校验、同输入同
  digest。

### 12.2 任务归纳

- 候选（`WorkflowParameterCandidate`）= 提议：`name` + `step_index` + RFC 6901
  `pointer` + provenance（provenance/structural/explicit）+ 逐轨迹观测值。
- 结构 diff（`induce_parameters`，2..16 条同骨架轨迹）：类型同、值不同的标量叶成
  候选；常量叶保持字面量；provenance 命名优先，auto 命名按稳定路径序。
- 参数化编译：候选叶重写为 `{"$param": name}`（复用 DEC-019 绑定纯函数），规格按
  观测类型推断、`required=false`、默认值取 anchor；名字冲突、保留成员 `"tool"`、
  非标量叶、类型不一致 fail closed。
- 归纳回退：草稿唯一入库路径是门禁（下节），失败库零变更；候选与观测值是宿主侧
  草稿数据，不入 IR、版本记录与事件。

### 12.3 入库门禁

- `WorkflowRuntime::publish_validated(definition, actor, reason, source_run_id?)`：
  结构校验 → DryRun Run（空参数、调用线程同步驱动）→ 要求 `Completed` → 证据摘要
  （workflow/ir digest、逐步结算、unevaluable 计数；内容派生，源 Run 与门禁 Run 绑定
  经事件审计）→ 幂等（head 同容同证 NoOp）→ 追加 `DryRunPassed` + 证据 digest 的
  版本记录。
- 回退不变量：门禁失败不触碰库（无 Rejected 记录、head 不变）；审计走三员新事件
  `WorkflowPublishProposed/Applied/Rejected`（§5 表）。模型不可直达该 API（`W-04`）；
  `publish_workflow` 维持宿主信任边界原始路径（无事件）。
- Executor 路由（§7 表新增三行）：捕获 = 调用线程互斥下快照；编译/归纳 = 调用线程
  纯函数；门禁驱动 = 复用 `execute_run` 宿主同步路径。

### 12.4 显式非目标（阶段 D 内）

- Agent 工具调用轨迹的事件流自动抽取与编排（宿主构造入口已留，编排属 Agent
  Harness 后续里程碑）。
- 自然语言 -> 候选提议/patch 条目的解释编排（M10 §2.2 既定推迟不变）。
- 对象/数组叶参数化、跨 Workflow 聚类、统计型参数发现（DEC-026 §4）。
- SQLite Workflow Library 持久化（`RISK-2026-038` 沿袭推迟；publish 事件为重建留了
  事件基础）。
- 阶段 E 能力（App Model、导航解析、`screen_state` 谓词求值）。

## 13. 阶段 E 实施规范（App Model、导航解析与 screen_state 绑定）

规范来源：[DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)（App
Model 契约与置信度）、[DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md)
（规划器与 Navigate 解析）。本节为里程碑 [M12](../plans/m12-app-model-and-navigation.md)
的入口摘要；语义细节以决策为准。

### 13.1 App Model 契约与置信度

- `AppModel`（`workflow_navigation.hpp`）：状态节点（id + 层次描述 page/modal/
  context + summary）与迁移边（id、from/to、action（保留成员 `"tool"`）、可选
  guard（v1 谓词 DSL）、代价向量、置信度记录）；JSON 往返无损、未知字段/悬垂
  引用/上限 fail closed（`app-model-*` 错误码）、内容寻址 digest。
- 置信度（`ConfidenceRecord`）：confidence/observed_at/last_verified/verified_count/
  failure_count/source（封闭集 host|agent|trajectory）。更新全为纯函数（时间由调用方
  传入）：`note_transition_outcome`（拉普拉斯平滑）、`apply_confidence_decay`
  （指数衰减、单调不增、Δt<=0 不变）、`needs_exploration`（阈值判定）。
- 感知边界（DEC-011）：UI 状态识别由宿主经 `ScreenStateProvider` 供给
  （`{state_id, observed_at_ms}` 快照，非阻塞同步回调）；Core 不做识别。App Model
  是可从「安装内容 + 事件序列」重建的投影（`W-03`）；持久化沿袭 `RISK-2026-038`
  推迟。

### 13.2 规划器

- `plan_navigation(model, from, to, profile, context, options)`：确定性 Dijkstra；
  代价 = 权重线性和（`NavigationCostProfile`，配置非硬编码）；guard 以谓词上下文
  求值，`NotSatisfied`/`NotEvaluable` 均使边不可用但分开计数；等代价按状态 ID
  字典序打破平局；`max_edge_evaluations`（缺省 4096）与 `max_path_edges`（缺省
  64）预算（`RULE-08`）；`agent_required` 边缺省不可用（`allow_agent_edges` 为
  宿主保留口）；`nav-*` 确定性错误码区分未知端点、同状态空路径、无路径与预算
  超限。
- 无路径错误携带 `guards_blocked`/`guards_unevaluable` 计数，可分辨「图不连通」
  与「guard 全挡」（`RULE-10` 诚实披露）。

### 13.3 Runtime 集成

- 导航上下文：`set_navigation_context(model, provider)` 与 `set_app_model(model)`
  （重装投影）；未安装时派发策略下 Navigate 步维持 `navigate-unresolvable`
  准入拒绝（M9 语义回归锁定），policy patch 切入派发策略的门禁同条件放松。
- Navigate 执行（派发策略）：读屏（缺席 `navigate-no-screen-state` fail closed）
  → 目标必须是已声明状态（`navigate-target-unknown`）→ 规划（失败原因码透出）→
  逐边派发动作（工具注册表通道、`"tool"` 保留成员、计入 step 预算）→ 每边后读屏
  到达验证（未到达 `navigate-arrival-unverified`，禁盲目重发，恢复钩子照常）→
  步级 verification 谓词在到达后求值。步内被截断（暂停/取消）按 Stale 结算、游标
  不推进；resume 时 Navigate 步从**当前读屏状态重新规划**（规划是状态驱动的，
  不重放旧计划边序列，`RULE-05` 由构造满足）。
- `screen_state` 谓词绑定（§4.1 兑现）：快照注入 `screen_state:<state_id>`（true）
  与 `screen_state:current`（ID 字符串）；缺席时全部 `NotEvaluable`（回归锁定）。
- DryRun：不派发边动作、不回写置信度；有导航上下文时执行真实规划并
  `WorkflowNavigationPlanned` 留痕，规划失败即步失败（门禁因此对导航可达性有约束
  力）；无上下文维持 M9 形状规划结算。
- 置信度回写：逐边按成败纯函数更新 Runtime 持有的模型投影（互斥下、事件锁外
  发射）；`WorkflowNavigationPlanned`（两种策略都发）与
  `WorkflowNavigationObserved`（仅真实派发）入 v1 事件闭集，OfflineReplay 识别
  且无副作用。

### 13.4 显式非目标（阶段 E 内）

- UI 状态识别、Accessibility/OCR/CV/VLM 感知与目标解析优先级（§9.6 层级）：按
  DEC-011 由 demo 证据决定是否回归；本阶段 Core 只消费宿主快照。
- 探索循环与规划失败的自动 Agent 升级（GUI Mapping 的编排侧）：升级经恢复钩子
  显式声明，自动升级属 Agent Harness 编排。
- Verify 步骤更高级证据声明（screen diff / 本地感知 / VLM）：M10 §11.4 的推迟
  维持；本阶段只落地 `screen_state` 谓词层。
- 多设备/多环境 App Model 命名空间、图自动失效检测、Memory 四类组织与学习闭环
  （阶段 F）。
- App Model 持久化与跨进程重建（`RISK-2026-038` 沿袭推迟）。
- 代价权重与置信度参数的目标平台校准（`RULE-10`：缺省值为暂定，未实测）。

## 14. 阶段 F 实施规范（Memory 四类组织、失败检索与学习闭环）

规范来源：[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)
（四类域组织与学习契约）、[DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)
（学习闭环运行时语义）。本节为里程碑 [M13](../plans/m13-memory-and-learning-loop.md)
的入口摘要；语义细节以决策为准。

### 14.1 四类域组织与学习契约

- `MemoryDomain`（environment/user/procedural/episodic 封闭集）是 `MemoryKind` 之上的
  确定性全映射与域级检索视图，不是新存储分层；不改变 `IMemory`、scope/ACL 与 M4
  审批规则。Environment 域的图级载体是 M12 `AppModel` 投影；Procedural 域的版本化
  本体是 M11 Workflow 库。
- `WorkflowEpisodeRecord`（终态 Run 的结构化情景：身份、策略、outcome、失败签名、
  升级/检查点计数）与 `WorkflowRecoveryLesson`（失败签名 + 恢复动作序列 + 恢复完成）
  是版本化公共契约：JSON 往返 fail closed、内容寻址 digest、statement 只含 ID/digest/
  枚举/原因码/计数器（脱敏面同 DEC-022 §5）。
- `episode_to_memory_record`/`recovery_lesson_to_memory_record` 为纯函数
  （verification=Verified、confidence=1.0、provenance=RunSettled 事件）；记录 ID 与
  mutation ID 从 run_id 经 SHA-256 派生（幂等重放友好）。
- `failure_retrieval_query`（签名 -> `MemoryQuery`）：kinds 恒为 `{Episode,
  RecoveryLesson}`、exact_terms 为签名标识、无 embedding 腿；同签名同查询。

### 14.2 运行时集成

- 学习上下文：`set_learning_context(memory, scope, config)` 安装即启用；scope 拒绝
  `User`；未安装时一切学习路径 NoOp（M12 行为回归锁定）。
- 结算期 Episode 记录：`settle_terminal` 完成 Task 结算后在结算线程同步执行；DryRun
  跳过（设计行为，无事件）；写失败不影响终态，转诊断计数器 + `WorkflowEpisodeRecorded`
  （failed）。
- 失败检索：仅失败驱动升级（`escalate_waiting_agent`）在驱动线程同步查询；结果存
  RunRecord 并经 `agent_continuation().relevant_lessons` 透出（增量字段）；检查点让渡
  不检索；查询失败降级为空结果；结果条数/token/deadline 受配置约束（`RULE-08`）。
- Lesson 记录：`record_recovery_lesson(run_id)` 为宿主专用 API（不注册为模型工具，
  `W-04`）；准入 = Completed + 非 DryRun + 失败升级计数 > 0 + 学习上下文已安装；
  恢复动作取最后一次失败升级后应用的 patch（无 patch 时 `resumed_without_patch`）；
  mutation-id 幂等。
- 事件：`WorkflowEpisodeRecorded`/`WorkflowLessonRecorded` 两员入 v1 闭集；
  OfflineReplay 识别且不调用 IMemory；学习投影按 DEC-030 §5 配方从事件流重建
  （`W-03`）。

### 14.3 显式非目标（阶段 F 内）

- Workflow/Skill 版本资产自动索引为 Procedure 记录：无检索消费者，推迟（DEC-029
  备选方案）。
- 检索向量腿、排序调优、召回质量声明（`RULE-10`：exact+FTS 两腿为保守首期）。
- 对话式 `update_memory` 编排、User Model 扩展（M4 既有规则不变）。
- Episode TTL/compaction 策略（宿主 retention 既有能力承载）。
- Agent 自动采纳/执行 lesson 的编排（Agent Harness 侧；`relevant_lessons` 只供数据）。
  该编排已由 [Workflow 恢复编排设计](workflow_recovery_orchestration_design.md) 与
  [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md) 冻结为独立 Core 组件
  `WorkflowRecoveryOrchestrator`，实现随 `MNT-202609-24` 立项的里程碑交付；其对本文
  的唯一触达是 `WorkflowAgentContinuation` 四个增量字段与 v1 事件闭集新增
  `WorkflowRecoveryAttempted`（实现时同步 §5 事件表）。
- 学习记录的专用持久化 schema（宿主 IMemory 后端承载；`RISK-2026-038` 沿袭）。
- 训练数据导出（`RULE-12` 维持默认关闭）。

## 15. 关联文档

- 决策：DEC-014、DEC-019、DEC-020、DEC-021、DEC-022、DEC-023、DEC-024、DEC-025、
  DEC-026、DEC-027、DEC-028、DEC-029、DEC-030（及 DEC-015/016/017/018 既有边界）；
  阶段 F 后续的 Agent 侧恢复编排见
  [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md) 与
  [Workflow 恢复编排设计](workflow_recovery_orchestration_design.md)
- 计划：[M8](../plans/m8-workflow-contracts.md)、
  [M9](../plans/m9-workflow-runtime-minimal-loop.md)、
  [M10](../plans/m10-workflow-intervention-and-policy-set.md)、
  [M11](../plans/m11-trajectory-compilation-and-task-induction.md)、
  [M12](../plans/m12-app-model-and-navigation.md)、
  [M13](../plans/m13-memory-and-learning-loop.md)
- 参考研究：[Agent Harness 参考研究](harness_reference_study.md) §5.3/§6
- API：[workflow-contracts](../api/workflow-contracts.md)
