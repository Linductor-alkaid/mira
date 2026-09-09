# M14：Workflow 恢复编排（Agent Harness 恢复闭环）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：[M13](m13-memory-and-learning-loop.md)（学习闭环数据面已交付）、
> [M10](m10-workflow-intervention-and-policy-set.md)（patch 管线与准入矩阵）、
> 设计与决策已由 `MNT-202609-23` 冻结（DEC-031）
> 建议发布点：Workflow recovery alpha
> 更新日期：2026-09-10

## 1. 目标

依据 [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md) 与
[Workflow 恢复编排设计](../design/workflow_recovery_orchestration_design.md)（v1.0），
交付 `WaitingAgent` Run 的 Agent 侧恢复编排：新增 Core 组件
`WorkflowRecoveryOrchestrator`，把一次失败升级变成一个有界工作单元——装配脱敏上下文与
lesson 三层过滤 → 一次有界模型请求（归属载体 Task）→ 四动作闭集决策解析 → 经既有
`patch_run`/`resume_run`/`cancel_run` 出口提交 → `WorkflowRecoveryAttempted` 审计事件。
使 `MNT-202609-21` 审计登记的头号缺口（「Agent 采纳经验」无证据、模型从未参与修复）
获得实现载体：recorded Provider 真正收到检索上下文并生成可验证修复，首次失败、恢复、
再次命中全链路可追踪。

本里程碑依据 [阶段 F 后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-24`
立项创建，承载 DEC-031 的验证方式；不修改 `WorkflowRuntime` 状态机与既有公开契约的
已冻结语义。

## 2. 范围与非目标

### 2.1 范围

- `WorkflowRecoveryOrchestrator`（`mira_workflow` 目标内，独立 Core 组件）：依赖
  `WorkflowRuntime`/`ModelGateway`/`MiraRuntime`/`IEventStore` 公开接口，不接触平台
  API 与 `IMemory`。
- 触发与准入：`attempt_recovery`（同步，调用线程执行阶段 1–7）与
  `start_recovery`/`wait_recovery`（异步，`submit_auto` 有界任务，容量
  `max_concurrent_attempts`）；准入四条件（`WaitingAgent`、无在途 attempt、
  attempt 预算、追踪表上限）确定性拒绝并审计 reason_code。
- 前置状态检查：载体 Task 经 `MiraRuntime::task_snapshot` 复核——`Recovering` 通过，
  `SuspendedForTakeover`/`TakeoverSettling` 出口 `takeover`，终态出口
  `run-state-changed`。
- 上下文装配：`agent_continuation` 的脱敏投影（有效参数缺省只透出名称、值类型与
  SHA-256 前 16 hex 摘要，宿主可经 hooks 自定义投影）；lesson 三层处理（解析
  fail closed、版本漂移失效过滤、`max_lessons_in_context` 截取）与过滤计数。
- 有界模型请求：归属载体 Task（`carrier_task_id`/`carrier_task_epoch`）、
  `1 + max_decision_repairs` 次请求上限、`model_call_deadline`、`OperationContext`
  探针取消（请求前/修复回合前/决策执行前三检查点）、不暴露工具（`ToolChoice::None`）。
- 决策 schema `mira.workflow.recovery-decision.v1`（`gate_schema_subset` 子集）：
  四动作闭集（`patch_and_resume`/`resume`/`cancel`/`need_user`）、`patch_entries`
  先期形状校验（`parse_workflow_patch_entries` + `validate_workflow_patch_entry`）、
  `used_lessons` 审计引用、`rationale`（有界，不入事件）；解析失败修复回合；`Refused`
  直接 `decision-invalid`。
- 决策执行：全部经既有公开 API；提交前重核 `run_epoch`/状态（迟到响应丢弃）；
  patch 拒绝修复回合（与决策解析共用预算）；`DeferredToHost`/`Aborted` 不改变
  Run 状态。
- 审计事件 `WorkflowRecoveryAttempted`（v1 闭集扩展，State 类，
  `mira.workflow.recovery-attempted.v1`，fail closed）：全链路关联键
  （run_id + ordinal、task_id、model_request_id、decision_digest、patch_id、
  lesson 过滤计数）；事件发射在编排器锁外，失败转诊断计数器。
- `WorkflowAgentContinuation` 增量字段：`carrier_task_id`、`carrier_task_epoch`、
  `run_epoch`、`escalations`（既有 RunRecord 内部值透出，源码兼容）。
- shutdown：拒绝新通知 → 置位取消 → 有界 drain 在途 future → 报告
  （clean/cancelled/drained 计数与诊断）；编排器先于 `WorkflowRuntime::shutdown()`
  关闭。
- 测试矩阵与 installed-consumer：决策矩阵、lesson 矩阵、竞态矩阵、预算矩阵、
  shutdown 矩阵、端到端主场景、安全负向（设计 §12）。

### 2.2 非目标

- 通用 Agent Loop（观察-推理-行动循环、工具执行）：编排器是单决策恢复控制器。
- 修改 `WorkflowRuntime` 状态机、转换表与既有 API 语义。
- `WaitingAgent` 向用户提问的通道（DEC-024 §4）：`need_user` 上交宿主，不升
  `WaitingUser` 决策点。
- lesson 记录所有权变化：`record_recovery_lesson` 仍是宿主专用 API（DEC-030 §4）。
- lesson 排序调优、向量召回、跨版本 lesson 复用开关（`MNT-202609-32` 按证据决策）。
- 恢复 patch 确认层（DEC-031 §3：`Recoverable`/`AgentAssisted` 即预授权）。
- 恢复率/成本收益声明：无 `MNT-202609-29` 对照证据前不宣称改善
  （`RISK-2026-052`）。
- 真实 Provider 与设备运行证据（`MNT-202609-27`）；本里程碑用 recorded/scripted
  Provider 与 installed-consumer 取证。

## 3. 设计与决策依据

- [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)（本里程碑的决策载体，
  Accepted；冻结里程碑即本文件）
- [Workflow 恢复编排设计](../design/workflow_recovery_orchestration_design.md) v1.0
  （七阶段管线 §4、数据模型与接口草案 §5、attempt 状态机 §6、Executor 路由与
  shutdown §7、失败出口矩阵 §8、测试策略 §12）
- [DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)（升级与
  续跑出口、`Recovering` epoch 语义）、
  [DEC-024](../decisions/DEC-024-conversation-patch-execution.md)（patch 管线与准入
  矩阵、修复期不向用户提问）、
  [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)（学习闭环、lesson
  是数据）、[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)
  （Episode/Lesson 契约与脱敏边界）、
  [DEC-018](../decisions/DEC-018-takeover-input-release-and-operation-admission.md)
  （Takeover 准入）、
  [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md) §2（闭集扩展
  fail-closed 纪律）
- [阶段 F 后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-24`（本里程碑的
  计划来源与验收定义）

## 4. 工作项

- [x] `M14-01` `WorkflowAgentContinuation` 增量字段透出：`carrier_task_id`、
  `carrier_task_epoch`、`run_epoch`、`escalations`，值来自既有 RunRecord；既有消费者
  源码兼容（同 DEC-030 `relevant_lessons` 先例）。
- [x] `M14-02` `WorkflowRecoveryAttempted` 事件入 v1 闭集：结构、构建/解析
  （`gate` 于 `check_exact_keys` fail closed、digest/ID 形状校验）、
  `is_workflow_event_type` 注册；载荷只含 ID/digest/枚举名/有界计数器。
- [x] `M14-03` 决策 schema `mira.workflow.recovery-decision.v1`
  （`workflow_recovery_decision_schema()`）：四动作闭集、patch_entries 形状、
  used_lessons/rationale 界；通过 `gate_schema_subset`。
- [x] `M14-04` `WorkflowRecoveryOrchestrator` 七阶段管线：触发与准入（四条件确定性
  拒绝）、前置状态检查（Takeover/终态 fail closed）、上下文装配与参数脱敏投影、
  lesson 三层过滤与计数、有界模型请求（载体 Task 归属、deadline、探针取消、无工具）、
  决策解析与修复回合、决策执行（提交前重核、四动作分流）。
- [x] `M14-05` 预算、并发与生命周期：每 Run attempt 预算（进入 `Requesting` 即计数，
  失败也计数）、`max_concurrent_attempts`（超限 `ResourceExhausted`）、
  `max_tracked_runs`（优先淘汰已终态 Run）、每 Run 至多一个在途 attempt、协作取消
  （`cancel_recovery`）、shutdown（拒绝新通知、置位取消、有界 drain、报告）。
- [x] `M14-06` 测试矩阵（设计 §12）：决策矩阵（四动作 × 合法/格式错/拒答/超时/熔断、
  修复回合边界、used_lessons 审计往返）、lesson 矩阵（空/全失效/不可解析/混合截断、
  过滤计数、保留 statement 逐字节一致）、竞态矩阵（在途取消/重复通知/Takeover/
  run_epoch 变化、迟到响应丢弃且终态不复活）、预算矩阵（编排预算耗尽、runtime 升级
  预算先行、并发容量超限、追踪表淘汰）、shutdown 矩阵（drain、关闭后拒绝通知、
  编排器先于 runtime 关闭、事件发射失败不影响结果）、端到端主场景（Run A 失败升级→
  编排修复→完成→宿主记 lesson→Run B 同签名命中→合成修复→完成，全链路事件关联键
  逐条断言，recorded Provider 收到检索上下文）、安全负向（lesson 引导越权 patch 确定性
  拒绝、参数投影缺省无值、rationale 不入事件）。
- [x] `M14-07` installed-consumer 与文档同步：公共 API 手册（编排器、事件、continuation
  字段、决策 schema）、Workflow 设计 §14、架构现状、README 能力表、DEC-031 冻结点
  回填、维护计划与总计划状态同步。

## 5. 风险与阻塞

- `RISK-2026-052`（DEC-031）：模型修复质量不可预测，可能在预算内反复无效修复消耗
  步预算与模型费用。缓解：三层预算、失败计数、`DeferredToHost` 上交；无对照证据
  （`MNT-202609-29`）前不宣称恢复率改善。
- `RISK-2026-053`：参数投影缺省不带值可能降低修复质量。这是脱敏优先的保守默认；
  宿主可经 hooks 放开白名单参数；收益/成本由评估组对照。
- 慢模型调用阻塞异步 drive worker：`model_call_deadline` 上界 + 探针取消；
  Executor worker 数需覆盖 `max_concurrent_attempts` 与 runtime 异步驱动。
- 外部阻塞：真实 Provider 消费证据（`MNT-202609-27`）。本里程碑以 recorded/scripted
  Provider 取证，不外推。

## 6. 测试与退出条件

- [x] 全部工作项（`M14-01`–`M14-07`）完成且测试通过。
- [x] 决策矩阵：recorded Provider 四动作 ×（合法/格式错/拒答/超时/熔断）有测试；
  修复回合边界（0/1/N）覆盖；`used_lessons` 与 decision digest 审计往返断言。
- [x] lesson 矩阵：空、全失效（版本漂移）、不可解析 statement、混合截断覆盖；
  `lessons_offered/stale/unparseable/kept` 计数断言；过滤后 statement 与原记录逐字节
  一致。
- [x] 竞态矩阵：请求在途宿主 cancel / 第二次通知 / Takeover / 请求在途宿主 patch（epoch 守卫 fail closed）覆盖；
  迟到响应丢弃（无 patch、无 resume、Run 状态保持宿主造成的终态不复活）；
  `DeferredToHost`/`Aborted` 后 Run 状态不变（保持 `WaitingAgent`）。
- [x] 预算矩阵：`max_attempts_per_run` 耗尽 `DeferredToHost`；runtime 升级预算先行
  耗尽的组合；`max_concurrent_attempts` 超限 `ResourceExhausted`；`max_tracked_runs`
  淘汰终态 Run。
- [x] shutdown 矩阵：在途 attempt drain、shutdown 后通知拒绝、编排器先于 runtime
  关闭的顺序断言；事件发射失败不影响 attempt 结果。
- [x] 端到端主场景（`MNT-202609-24` 验收）：recorded Provider 真正收到检索上下文并
  生成可验证修复；Run A 失败升级（检索空）→ 编排修复 → 完成 → 宿主记 lesson →
  Run B 同签名失败升级 → 命中 A 的 Episode+Lesson → 模型合成修复 → 完成；
  全链路事件序列（ModelRequest → RecoveryAttempted → PatchProposed/Applied →
  StepSettled… → RunSettled → EpisodeRecorded/LessonRecorded）逐条断言关联键。
- [x] 安全负向：lesson 引导的越权 patch（策略越界/未知 path/危险参数形状）确定性
  拒绝；参数投影缺省不带值；`rationale` 不出现在任何事件载荷。
- [x] Linux 本机全量 ctest（Debug）与 ASAN/UBSAN/TSAN 目标测试通过；Release、
  Windows MSVC（Debug/Release）、Android 双 ABI（含 `mira_workflow` 编译与安装包
  consumer 链接）、quality（clang-tidy + format + docs/sbom/platform-boundary）与完整
  sanitizer 矩阵经 PR #38 CI 全绿取证（见下）。
- [x] 文档（API 手册、设计、架构、README、DEC-031 冻结点、计划状态）同步完成。

## 7. 验证记录

（按日期追加；环境限制未执行的验证保持未勾选并记录补跑条件。）

2026-09-10：`M14-01`–`M14-07` 实现完成（分支 `feat/mnt-24-recovery-orchestration`）。
变更：`include/mira/workflow_recovery.hpp` + `src/workflow/workflow_recovery.cpp`（编排器
七阶段管线、配置 fail closed、决策 schema 与语义校验、修复回合、提交前重核、预算/并发/
追踪表、协作取消与 shutdown drain）；`workflow_events.{hpp,cpp}` 新增
`WorkflowRecoveryAttempted`（构建/解析 fail closed、闭集注册）与
`WorkflowRecoveryOutcome` 五值闭集；`WorkflowAgentContinuation` 增量字段
`carrier_task_id`/`carrier_task_epoch`/`run_epoch`/`escalations`；`enter_waiting_agent`
在载体进入 `Recovering` 后刷新缓存 task epoch（否则透出值过期一拍——进入 `Recovering`
本身推进 task epoch）；`mira_workflow` 目标、headers 独立包含检查与 installed-consumer
（M14 段：包消费侧装配编排器 + 网关 + scripted provider，`need_user` 上交路径）同步；
`tests/m14/m14_recovery_test.cpp`（17 场景）与 `tests/support/m14_support.hpp`。

实现要点偏离/澄清（均不改变 DEC-031 语义，语义依据设计正文）：
- 审计事件 lesson 过滤计数为四元 `lessons_offered/stale/unparseable/kept`（设计 §5.5
  草案表列三元，§4.3/§8 正文要求 stale 与 unparseable 可区分，按正文实现）。
- 重复通知的早结算结果不写入 `last_result`（不遮蔽在途 attempt 的真实结果；
  `wait_recovery` 消费 future）。准入拒绝的 ordinal 取自既有追踪条目（未受理过通知的
  Run 为 0，事件仍可审计）。
- 端到端主场景按 DEC-030 §2 实际语义取证：Completed Run 的 Episode 无
  `failed_step_id`，签名命中的 Episode 来自失败结局 Run（Run 0，Strict），Run A 贡献
  Lesson；恢复编排设计 §12 已按此澄清（v1.0.1）。run_epoch 仅在状态转换时推进，
  `epoch-advanced` 出口保留为 fail-closed 守卫（不可由现行公开 API 确定性触发；状态
  漂移测试覆盖可观测竞态），在途宿主 patch 场景取证为「仍有效决策照常执行」。
- 编排器构造签名在草案基础上增加 `SessionId`（事件 AppendRequest 需要；签名以实现
  为准，语义不变）。

本地验证（Ubuntu 24.04.4 x86_64、gcc 13.3.0、CMake 3.28.3、NDK 26.3.11579264）：
Debug 全量 ctest 68/68（原 67 + `mira_m14_recovery_test`）；ASAN/UBSAN
`mira_m14_recovery_test` 通过；TSAN（`setarch -R`）`mira_m14_recovery_test` 与 m13
learning/persistence 4 组通过（runtime epoch 刷新改动的回归面）；`format-check`、
`platform-boundary-check`、`tools/check_docs.py` 全绿；Android arm64-v8a 与 x86_64
以同版本 NDK 实际编译 `mira_workflow`（含新源文件）通过。Release、Windows MSVC、
quality（clang-tidy）与完整 sanitizer 矩阵由 PR CI 执行后回填；Android 设备运行与
真实 Provider 证据仍归 `MNT-202609-27`，不因本轮回填外推。

2026-09-10：PR [#38](https://github.com/Linductor-alkaid/mira/pull/38) CI 证据回填并关闭。
head `89fae06`（含一处 quality 修复：clang-tidy
`bugprone-unused-local-non-trivial-variable`，字节级 digest 比较遗留的未用变量），
合并提交 `bb77a0a`。PR 检查全绿：push pipeline run
[34389297172](https://github.com/Linductor-alkaid/mira/actions/runs/34389297172) 与
pull_request pipeline run
[34389329504](https://github.com/Linductor-alkaid/mira/actions/runs/34389329504) 各 12
项通过——Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release，含
`mira_m14_recovery_test`）、Android arm64-v8a 与 x86_64（NDK 26.3.11579264，编译级 +
安装包 consumer 交叉链接）、ASAN/UBSAN/TSAN、quality（clang-tidy + clang-format +
docs/sbom/platform-boundary）；合并提交 master pipeline run
[34393602561](https://github.com/Linductor-alkaid/mira/actions/runs/34393602561) 同样
全绿（14m56s）。首轮 quality 因上述 clang-tidy 违例失败一次，修复后复验通过（两轮
记录保留）。限制：Android 设备运行与真实 Provider 消费证据仍归 `MNT-202609-27`，
本轮不外推为运行支持；恢复率/成本收益无 `MNT-202609-29` 对照前不作声明
（`RISK-2026-052`）。里程碑退出条件逐项复核后关闭（`Completed`）。
