# M9：Workflow Runtime 最小闭环（阶段 B）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：[M8](m8-workflow-contracts.md)（实现已交付；Android 验收重开）
> 建议发布点：Workflow runtime alpha
> 更新日期：2026-09-09

> 2026-09-09 状态复核：实现已交付；`BUG-20260909-001` 发现 Android CI 未构建
> `mira_workflow`，本里程碑的矩阵取证项及相关退出条件重新打开。历史验证记录保留，
> 当前状态以本注记为准；依赖方可复用冻结契约，跨平台关闭共同等待
> [阶段 F 后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-22`。

## 1. 目标

依据 [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 B 与
[Workflow Runtime 设计](../design/workflow_runtime_design.md)，交付 Workflow 数据平面的
执行闭环：`WorkflowRuntime` 以 `Strict`/`DryRun` 两种策略执行已冻结的 Workflow IR，
覆盖步进、前置条件跳过、受限循环、验证谓词、恢复钩子、暂停/恢复/取消、终态幂等与
迟到完成隔离，并将四个 Workflow 操作（run/pause/resume/cancel）注册进 BuiltIn 工具
闭环（DEC-021 §5）。Run 由既有 Task 生命周期承载（DEC-020 §2 映射），控制面复用
`MiraRuntime`，不新增控制线程。

本里程碑同时冻结 M8 遗留的暂定默认值与阶段 B 反推出的契约补全（见第 4 节
`M9-01`）：IR 缺省策略、ToolCall 步骤的工具绑定约定、Navigate 步骤在阶段 B 的
可执行性边界。

选择新增 M9 而非扩展 M8 的理由：M8 已按其退出条件关闭（契约冻结）；执行闭环是
独立的能力增量，验收证据（运行时行为测试）与契约测试不同源，按规范应使用新里程碑
文件承载。不对阶段 C–F 预分配编号。

## 2. 范围与非目标

### 2.1 范围

- `WorkflowRuntime` 执行体（`include/mira/workflow_runtime.hpp`）：Run 表、库
  （`WorkflowVersionHistory` 按 `WorkflowId` 组织）、宿主直达与工具发起两条同源创建
  路径、步进驱动、事件发射。
- `Strict` 策略执行：ToolCall 步骤经 BuiltIn 注册表派发（含 W-02 副作用步骤强制
  验证与先观察后验证）、Verify 谓词求值、Control 有界循环、前置条件跳过。
- `DryRun` 策略执行：全部步骤只做规划与可求值谓词断言，不派发任何工具或环境动作；
  不可求值验证计数披露，不宣称验证通过（`RULE-10`）。
- 恢复钩子：`Retry`（受 `max_attempts` 约束、副作用步骤重试前必须重新观察）、
  `FallbackStep`、`None`；`AgentEscalation` 在 Strict/DryRun 下按既有契约在创建时
  拒绝。
- Run 级控制：`pause`（步边界安全点生效）、`resume`（重新观察 + 从游标继续）、
  `cancel`（幂等）；暂停/取消与步执行的竞态按 `run_epoch` 隔离（Stale）。
- Workflow 操作 BuiltIn 注册：`run_workflow`/`pause_workflow`/`resume_workflow`/
  `cancel_workflow` 四个执行 handler，经 `BuiltinToolRegistry` 闭环执行；
  `patch_workflow` 保持 schema-only（阶段 C）。
- WorkflowRuntime 生命周期：容量上限（活跃 Run 数、并发异步驱动数、整 Run 步执行
  预算）、shutdown 顺序（停止生产者 → 取消活动 Run → 有界回收驱动 future → 不自行
  关闭 Executor）。
- 契约补全：`validate_workflow_definition`（结构体定义与 JSON 解码同源校验）、
  ToolCall 工具绑定约定、暂定默认值冻结。

### 2.2 非目标

- 对话驱动 patch 的运行时交互、执行策略全集（`Recoverable`/`AgentAssisted`/
  `Interactive` 的升级与检查点行为）、运行中策略切换与 Takeover 交互细节（阶段 C）。
- 成功轨迹编译、任务归纳与 DryRun 证据入库链（阶段 D；本里程碑只暴露
  DryRun 运行结果与不可求值计数，证据语义由阶段 D 冻结）。
- App Model、Navigation Planner 与 `screen_state` 谓词求值（阶段 E；阶段 B 对含
  `screen_state` 的谓词一律不可求值）。
- Memory 四类组织与学习闭环（阶段 F）。
- SQLite Workflow Library 存储：Run 表与库在本里程碑为进程内投影，EventStore 仍是
  已提交事实的权威记录；跨进程续跑的持久化载体随首个需要它的阶段立项（沿用 M8 对
  该项的推迟记录，责任人与触发条件见第 6 节 `RISK-2026-038`）。
- 连续控制步骤（实时路径）：Workflow 步骤种类 v1 无连续控制；`RULE-06` 路径随相关
  步骤种类扩展另行立项。
- 子 Workflow、并行步骤、通用图（DEC-019 扩展位）。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M8 `Completed`：`Mira::workflow` 契约、五组契约测试、DEC-019..022 与
  `workflow_runtime_design` 已冻结（PR #29 CI 全绿）。
- 本里程碑经维护者评审由 `Proposed` 转 `Planned`（用户指示依设计与计划推进下一步
  开发，与 M8 同一授权模式）；暂定默认值的冻结结论记入 `M9-01`。

### 3.2 设计与决策依据

- [Workflow Runtime 设计](../design/workflow_runtime_design.md)（入口规范；本里程碑
  细化其 §7 路由草案，见第 5 节）
- [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)、
  [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)、
  [DEC-018](../decisions/DEC-018-takeover-input-release-and-operation-admission.md)、
  [DEC-019](../decisions/DEC-019-workflow-ir-contract.md) 至
  [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)
- [核心公共契约与状态机](../design/core_contracts_and_state_machine.md)、
  [Mira Runtime 设计](../design/mira_runtime_design.md)（Task 状态机与控制面单写者）
- [Agent Harness 参考研究](../design/harness_reference_study.md) §5.3（步边界恢复）

## 4. 工作项

### 4.1 契约补全与暂定值冻结

- [x] `M9-01` 冻结 M8 暂定默认值与阶段 B 契约补全，并同步 DEC-019/DEC-020 与设计
  文档：(a) IR 未声明 `default_policy` 时缺省 `Strict` 由暂定转正式冻结；
  (b) ToolCall 步骤的 `arguments` 必须为对象且携带保留成员 `"tool"`（字符串，
  BuiltIn wire 名），其余成员构成工具输入并经注册表 schema 校验（与 Navigate 的
  `arguments["target"]` 同属按步骤种类的实参约定）；(c) dispatching 策略下 Navigate
  步骤在导航解析落地（阶段 E）前于创建 Run 时 fail closed（`navigate-unresolvable`），
  DryRun 下按已验证形状规划结算；(d) 新增 `validate_workflow_definition`：对结构体
  定义执行与 JSON 解码同源的结构校验（经规范化往返实现，语义单一来源）。

### 4.2 执行闭环实现

- [x] `M9-02` 实现 `WorkflowRuntime` 骨架与 Run/Task 绑定：构造注入 Executor、
  `MiraRuntime`、Session 与 `IEnvironment`；每个 Run 创建一个承载 Task；Run 视图
  变更全部经 `apply_workflow_run_transition` 提交并在互斥下原子完成；Run 表容量与
  终态清理；事件发射（`WorkflowRunStarted`/`WorkflowStepStarted`/
  `WorkflowStepSettled`/`WorkflowRunSettled`，M8 载荷构建器）。
- [x] `M9-03` 实现 Run 创建准入：结构校验、策略门禁（M9 仅承载 `Strict`/`DryRun`，
  其余策略 `UnsupportedCapability` 并注明阶段 C）、`allowed_policies` 成员资格、
  `AgentEscalation` 拒绝、参数绑定（确定性错误码）、ToolCall 绑定形状与注册表命中、
  W-02（dispatching 策略下副作用工具步骤必须声明验证谓词）、Navigate 边界
  （见 `M9-01`）；库路径按 digest 解析创建时版本且仅 `DryRunPassed`/`Validated`
  版本可运行（`W-03`/`W-04`），宿主直达路径按宿主信任边界接受定义。
- [x] `M9-04` 实现 `Strict` 步进执行：声明顺序推进；前置条件（`NotSatisfied` 跳过、
  `NotEvaluable` 失败）；ToolCall 经注册表派发（`submit_auto` 有限任务，future 被
  消费，异常转步骤失败）；副作用步骤完成后先经 `submit_auto` 重新观察再求值验证
  谓词（`W-02`）；Verify 步骤求值（`Satisfied` 完成、`NotSatisfied`/`NotEvaluable`
  失败，fail closed）；Control 后向跳转按每控制步 `max_iterations` 计数，超限转
  `Failed`（`loop-budget-exceeded`）；整 Run 步执行预算（`step-budget-exceeded`）。
- [x] `M9-05` 实现 `DryRun` 规划执行：任何步骤不派发；前置条件跳过同 Strict；
  可求值验证谓词必须满足（`NotSatisfied` 即 `Failed`），不可求值（含
  `screen_state` 与 DryRun 下必然缺失的 `step_result`）按「已规划、未宣称验证」
  结算并在 Run 结果中计数披露；Navigate 步骤按规划结算。
- [x] `M9-06` 实现恢复钩子：`Retry`（`max_attempts` 内，副作用步骤重试前必须完成
  一次重新观察）；`FallbackStep`（前进目标）；钩子耗尽或缺失转 `Failed`；
  `AgentEscalation` 于准入阶段拒绝（Strict/DryRun 非 agent-capable）。

### 4.3 Run 控制与生命周期

- [x] `M9-07` 实现 `pause`/`resume`/`cancel`：控制面命令（Task 侧）与 Run 视图提交
  的安全点顺序；在执行步骤的有界完成即安全点（阶段 B 无连续控制）；被暂停截断的
  步骤按 Stale 结算且游标不推进；`resume` 重新观察后处理中断步骤（无副作用工具或
  DryRun → 重新执行；副作用工具 → 验证谓词复评，满足则按已完成恢复；不满足或
  不可求值时仅允许 `FallbackStep` 前进，否则以 `ExecutionUncertain` 失败，
  `RULE-05` 禁止盲目重发）；`cancel` 幂等，Run/Task 终态一致。
- [x] `M9-08` 实现竞态隔离与终态语义的运行时级落地：步级完成信号携带 `run_epoch`
  经 `admit_workflow_run_completion` 结算；暂停/取消/接管期间的迟到完成按 Stale
  落事件且不改变状态；终态幂等（重复 cancel 同终态 NoOp、冲突终态拒绝）；Task 终态
  ⇒ Run 终态的组合不变量。
- [x] `M9-09` 实现监控与协作取消：驱动期间监控任务轮询承载 Task 状态并以原子旗标
  发布撤回（不在持有环境锁的上下文内回调 Runtime，对齐 agent harness 的锁序先例）；
  步骤 `OperationContext` 携带截止时间与撤回探针。
- [x] `M9-10` 实现 shutdown：拒绝新的 run/resume/start 提交 → 经控制面取消活动
  Run → 有界等待驱动 future 结算（超时记诊断）→ 全部 future 被消费；不自行
  `shutdown` Executor（非 owner）；宿主关闭顺序文档化
  （`WorkflowRuntime::shutdown` → `MiraRuntime` 关闭 → Executor `shutdown(true)`）。

### 4.4 工具闭环与门禁

- [x] `M9-11` 注册并执行四个 Workflow 操作 BuiltIn handler（run/pause/resume/
  cancel）：参数经 `validate_workflow_operation` 同源校验；`run_workflow` 创建并
  异步驱动 Run（受并发驱动上限约束）；控制操作汇聚同一控制面；`cancel_workflow`
  对终态 Run 幂等成功并携带 `already_terminal`；结果按 output/details 拆分；
  `patch_workflow` 不注册（阶段 C）。
- [x] `M9-12` 端到端取证：AgentLoop 经模型响应发起 `run_workflow` 工具调用 →
  Run 创建并驱动至终态 → 工具结果回流模型（`RISK-2026-035` 的阶段 B 部分）。
- [ ] `M9-13` 测试矩阵取证与文档同步：本机 Release/Debug/ASAN/UBSAN（TSAN 按环境
  限制记录补跑条件）、quality 门禁、installed-consumer 覆盖 `workflow_runtime.hpp`；
  总计划、设计文档、API 手册与 DEC 同步后关闭里程碑。

## 5. Executor 路由与关闭表（细化设计 §7 草案）

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| Run 驱动（宿主线程同步路径 `execute_run`） | 宿主调用线程（宿主自身的 Executor 任务或主线程） | 调用方 | 结果同步返回 |
| Run 驱动（异步路径 `start_run`、`run_workflow` 工具） | `submit_auto` 普通有限任务，并发上限默认 1（`max_concurrent_async_drives`），上限必须小于 Executor worker 数 | Workflow Runtime（Run 表内 future） | future 由 `wait_run`/shutdown 消费；异常转 `Failed` 结算与诊断 |
| 步骤执行（离散工具派发） | `submit_auto` 普通有限任务 | 驱动任务（逐个消费 future） | 提交拒绝与任务异常转步骤失败（可经恢复钩子） |
| 每步副作用后验证观察 | `submit_auto` 普通有限任务 | 驱动任务 | 不可丢弃；失败即步骤失败 |
| DryRun 谓词求值 | 调用线程同步（纯函数） | 无 | 无 |
| 驱动监控（撤回旗标） | `submit_auto` 有限任务，周期轮询承载 Task 快照 | 驱动任务 | 驱动结束时置停并消费 future |
| 控制面命令（pause/resume/cancel/complete） | 既有 `MiraRuntime` 串行控制面 | Workflow Runtime（CommandHandle） | 有界等待回执；失败对调用方可见 |

理由：步骤与验证观察维持设计 §7 的逐任务派发；Run 驱动本身作为单个 Executor 有限
任务（与 M3 AgentLoop 同构），不在驱动任务内嵌套等待自身驱动的步骤以外的阻塞链；
并发异步驱动上限防止驱动任务占满 worker 造成控制面饥饿。

关闭顺序（设计 §8 的实例化）：`WorkflowRuntime::shutdown()`（停止生产者 → 控制面
取消活动 Run → 有界消费驱动 future）→ 宿主执行 `MiraRuntime::request_shutdown()`/
`finish_shutdown()` → 宿主以非 worker 线程 `executor.shutdown(true)`。
`WorkflowRuntime` 不是 Executor owner，任何路径不得自行关闭 Executor。

## 6. 风险与阻塞

- `RISK-2026-038`：Workflow 库与 Run 表为进程内投影，跨进程续跑需要持久化载体。
  Owner：Mira Maintainers。缓解：EventStore 保持权威记录（`W-08` 回放可重建投影）；
  持久化 Library 随首个需要跨进程续跑的阶段立项，未立项前不宣称恢复能力。
- `RISK-2026-039`：暂停/恢复对「被截断的副作用步骤」的判定在阶段 B 只有
  结构化谓词一层证据（无屏幕级验证），判定保守（`ExecutionUncertain` 失败优先），
  可能造成可恢复场景被判失败。Owner：Mira Maintainers。缓解：行为在测试中锁定并
  在 API 手册披露；阶段 E 感知落地后扩展证据层。
- `RISK-2026-040`：异步驱动占用 worker 的容量权衡。缓解：并发异步驱动上限
  （默认 1）与文档约束（上限 < worker 数）；同步 `execute_run` 路径不占用池内
  worker 等待。
- `RISK-2026-041`：四个操作 handler 生命周期与 `WorkflowRuntime` 实例绑定（handler
  捕获 `this`）。缓解：注册 API 文档要求 Runtime 生命周期覆盖注册表条目；
  shutdown 后 handler fail closed。

## 7. 测试与退出条件

- [x] `M9-01` 至 `M9-12` 全部完成并有可复现验证记录。
- [x] 执行闭环：`Strict` 全路径（多步骤、跳过、循环、验证、恢复钩子全模式）与
  `DryRun` 全路径（规划、可求值断言、不可求值计数披露）正向/负向测试通过；未知
  字段、非法转换、超限、缺失验证声明全部 fail closed。
- [x] 竞态与终态：步执行中暂停/取消的 Stale 结算与游标保持；迟到完成不复活终态；
  终态幂等；Task 终态 ⇒ Run 终态不变量；resume 重新观察与中断步骤三分支
  （重执行/谓词恢复/`ExecutionUncertain`）覆盖。
- [x] shutdown 顺序测试：生产者拒绝、活动 Run 取消、future 清算、Executor 未被
  Runtime 关闭。
- [x] 工具闭环：四操作经 `BuiltinToolRegistry` 执行与错误信封；库版本门禁
  （`NotValidated` 不可运行、`DryRunPassed` 可运行）；模型发起 `run_workflow`
  端到端（`M9-12`）。
- [ ] 门禁：ASAN/UBSAN 全绿；TSAN 在本机限制下按 M8 模式取证（53/53）；
  quality（clang-format、docs、sbom、platform-boundary）通过；Windows/Android 构建
  组合与 clang-tidy 由 PR CI 补验全绿；installed-consumer 覆盖新公共头。
- [x] 总计划第 4 节、`workflow_runtime_design`（路由与限制章节）、API 手册、
  DEC-019/020（暂定值冻结注记）与本文件同步。

## 8. 验证记录

2026-09-08：依据 M8 退出条件与总计划阶段 B 规则创建本里程碑（`Proposed`）。同日
经维护者评审（用户指示依设计与计划推进下一步开发，与 M8 同一授权模式）转 `Planned`
并进入实施（`In Progress`）：M8 契约、DEC-019..022 与 `workflow_runtime_design`
满足准入条件。

2026-09-08：实现与本地验证（Ubuntu 24.04，x86_64，g++ 13.3.0，CMake 3.28.3，Unix
Makefiles；本机无 clang/clang-tidy，由 PR CI quality job 补验；`clang-format` 使用
miniconda 发行版）。

- 实现：`include/mira/workflow_runtime.hpp` 与 `src/workflow/workflow_runtime.cpp`
  （CMake 目标 `Mira::workflow` 增加 PRIVATE `executor::executor` 依赖，安装面不变）；
  `workflow_ir.hpp/cpp` 新增 `validate_workflow_definition`（规范化往返，与解码同源）。
  `WorkflowRuntime` 提供 Run 表/库、宿主直达与库解析创建、同步/异步驱动、pause/resume/
  cancel、监控撤回、shutdown 报告与四操作 BuiltIn handler；Run 视图全部经
  `apply_workflow_run_transition` 提交；步骤与副作用验证观察经 `submit_auto` 并逐个消费
  future。
- 新增测试：`mira_m9_execution_test`（Strict/DryRun 全路径、前置条件跳过、有界循环两种
  出口、恢复钩子 Retry/FallbackStep/耗尽、副作用重试先观察、不可求值 fail closed、步/表
  预算、准入负向全集）、`mira_m9_control_test`（暂停 Stale + 游标保持、resume 重新观察
  与中断步骤三分支、取消幂等与迟到隔离、Task⇔Run 终态一致、Takeover 挂起与恢复、
  shutdown 取消/清算/生产者拒绝/Executor 存活、未启动 Run 的暂停/取消边界）、
  `mira_m9_tools_test`（四操作经 BuiltIn 注册表闭环、库版本门禁与错误信封、同 digest
  重复发布的解析语义、模型经 AgentLoop 发起 `run_workflow` 端到端）。
- 安装包：`mira_installed_consumer` 增加 M9 用例（installed 包上以宿主 Executor + 控制
  面板执行一次 DryRun 闭环并按文档顺序关闭）；`mira_public_headers_test` 覆盖
  `workflow_runtime.hpp`。
- 结果：Release/Debug/ASAN/UBSAN 各 54/54 通过；TSAN（`setarch x86_64 -R`，mbedtls
  portable 按配置禁用）53/53 通过；`format-check`、`docs-check`、`sbom-check`、
  `platform-boundary-check`、`consumer-check` 通过。
- 实现中发现并修复的缺陷（均有测试锁定）：取消中的步骤结算曾被误记为 Completed
  （`dispatch_step` 取消返回路径）；`wait_run` 曾优先返回被新驱动覆盖前的旧结果；
  `pause_run`/`cancel_run` 曾在持内部互斥的作用域内再次加锁（死锁）；
  `workflow_definition_to_json` 曾对空 `summary` 输出被解码端拒绝的空串，导致无
  summary 定义无法通过 `validate_workflow_definition` 往返（修复为按可选键省略，
  已含 summary 的定义 digest 不变）。
- 限制：Windows/Android 构建组合与 clang-tidy 由 PR CI 补验后随 `M9-13` 回填（见下条）；
  阶段 C–F 不在本轮（范围外）。
- 同步：DEC-019/020（默认策略冻结注记）、`workflow_runtime_design`（v0.2：路由定稿、
  M9 契约补全、实现补注）、API 手册（workflow-contracts 增 M9 节）、总计划（§4）。

2026-09-08：PR [#30](https://github.com/Linductor-alkaid/mira/pull/30) CI 全绿（head
`8febcf2`，push pipeline runs
[`34192800547`](https://github.com/Linductor-alkaid/mira/actions/runs/34192800547)、
[`34192803826`](https://github.com/Linductor-alkaid/mira/actions/runs/34192803826)）：Linux
GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android arm64-v8a 与 x86_64
（NDK）、ASAN/UBSAN/TSAN 与 quality（clang-tidy 18 + clang-format + docs/sbom/
platform-boundary 检查）全部 24 项通过，补齐本机缺失的 clang-tidy 与跨平台验证。首轮
quality 在 `workflow_runtime.cpp` 报 1 处 `performance-noautomatic-move`（const 返回值阻止
自动移动），去除 const 后复验通过；语义不变（控制与执行测试全绿）。PR 已合并
（merge `9d80524`）。`M9-01` 至 `M9-13` 全部完成，退出条件逐项满足，本里程碑关闭
（`Completed`）。阶段 C（对话驱动 patch 与执行策略全集）里程碑可依据
[workflow_runtime_design](../design/workflow_runtime_design.md) 创建并进入 `Planned`。

2026-09-09：阶段 F 后状态审计重新打开 `M9-13` 与跨平台退出条件，
里程碑恢复 `In Progress`。`BUG-20260909-001`：Android CI 显式目标列表未包含
`mira_workflow`，且已有 consumer 不依赖该目标，历史 job 全绿不足以支撑本模块
Android 构建声明。已交付功能项及其他历史证据保留；这是验收覆盖缺口，尚无 Android
编译失败证据。负责人 Mira Maintainers；按
[后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-22` 补齐两 ABI
实际编译与安装消费链接证据后，逐项复核并关闭。本轮本地构建因未初始化子模块未能
复跑，环境与补跑条件见后续计划第 5 节。
