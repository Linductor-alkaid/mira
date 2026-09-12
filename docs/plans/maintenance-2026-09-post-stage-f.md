# 阶段 F 后续：验收补齐与产品闭环

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M8–M13 已合入的实现；跨平台验收缺口见下文
> 建议发布点：先恢复 Workflow learning alpha 验收，再按 demo 证据定义下一发布点
> 更新日期：2026-09-12

## 1. 目标与边界

依据当前源码、测试、构建配置和仓库验收记录确认阶段 F 的实际状态，把遗留能力与证据缺口
转为有负责人、依赖和退出条件的任务。本轮交付状态审计与计划更新；后续实现由下列任务
跟踪。新增公开契约先冻结专项设计与 DEC，再创建对应里程碑，不预分配 M14 等编号。

依据：[项目管理规范](../project/project_management_and_documentation.md) §5/§10、
[双路径架构](../design/agent_harness_and_workflow_architecture.md)、
[Workflow 专项设计](../design/workflow_runtime_design.md) §11–14、
[DEC-011](../decisions/DEC-011-demo-first-external-validation.md)、
[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
[DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)、
[DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)（本计划 `MNT-202609-23`
产出）。
M5/M6 保持 Cancelled，M7 保持 Blocked；本计划不批准恢复原范围或扩大 v1 平台承诺。

## 2. 状态核对与缺口

审计基线：`cd89384`，阶段 F 实现合并提交 `db2e814`。初始工作树干净。

| 范围 | 已有证据与实际边界 | 尚缺内容 / 跟踪项 |
| --- | --- | --- |
| 阶段 A–F | M8–M13 实现与测试已合入；历史记录为 Linux/Windows、sanitizer、quality 通过；`BUG-20260909-001` 已由 `MNT-202609-22` 修复，两 ABI 实际编译 `mira_workflow` 并完成安装包 consumer 交叉链接（PR #35） | Android 设备运行与宿主消费证据仍缺，`MNT-202609-27` |
| F 学习闭环 | 域映射、Episode/Lesson、失败查询与 `relevant_lessons` 已实现；M13 测试以三个 Run 验证记住、恢复、再次检索；恢复编排设计与决策已由 `MNT-202609-23` 冻结（DEC-031） | AgentLoop 未消费 continuation；测试由宿主直接 resume/record lesson，不能证明模型采纳有效，实现由 `MNT-202609-24` 承载 |
| 长期资产与恢复 | M4 有 SQLite Memory；M9 Library/Run 与 M12 App Model 为进程内投影 | 跨重启学习持久化已由 25 取证（SQLite 通过）；事件重建配方载荷缺口登记 `BUG-20260909-002`，DEC-030 §5 修订待立项；Library/Run/App Model 跨进程持久化仍缺，`MNT-202609-26` |
| 自动化资产复用 | M11 有轨迹捕获、编译、归纳、DryRun 入库；M12 有图与导航规划 | 缺 Procedure 索引消费者、按目标选 Workflow，以及真实 UI 到 `ScreenStateProvider` 的消费侧验证，`MNT-202609-27/31` |
| 真实平台与 Provider | Android ABI 文档已有 miracle P1 截图及 lease 路径证据；Provider 矩阵逐 capability 记录 | UI tree、转码后视觉闭环、决策修复、输入/权限/Takeover/宿主销毁完整矩阵仍有外部未结项，`MNT-202609-27` |
| 评估与发布 | 有单元/集成/consumer 测试与 M4 benchmark；BuiltIn 工具闭环已交付 | 缺 Workflow 任务级统一评估、学习增益/成本基线、soak；ToolModule 签名/协商/OOP 仍未交付，`MNT-202609-28/29/30` |
| 文档入口 | 总计划已记录 M13 关闭 | README 与架构总览仍只报 M0–M4 或 Workflow 未实现；本轮修正，`MNT-202609-21` |

### 2.1 BUG-20260909-001：Android CI 未覆盖 Workflow 目标

- 证据：[CI 配置](../../.github/workflows/ci.yml) 的 Android build 使用显式 `--target`
  列表，仅包含 Core、Simulator、Android Adapter、transport、state store、stateful consumer。
  [CMakeLists.txt](../../CMakeLists.txt) 中 `mira_workflow` 是独立目标；
  `mira_stateful_consumer` 仅链接 Core/state store/Executor，不会间接构建 Workflow。
  Android presets 同时关闭测试，因此 consumer tests 也不会补上这一覆盖。
- 影响：M8–M13 的历史 Android job 成功不能证明这六阶段新增模块可交叉编译。现有功能与
  Linux/Windows 历史通过记录保留，但矩阵取证工作项及相应退出条件重新打开，状态恢复
  `In Progress`。依赖方复用已冻结契约与实现，跨平台关闭统一等待本缺口。
- Owner：Mira Maintainers。解除条件：两种 Android ABI 实际编译 Workflow，并保留准确
  commit、NDK、命令、日志与结果；按各里程碑退出条件复核后逐项关闭。属于 Mira CI
  覆盖问题，无证据表明是 Executor 能力缺口。
- 修复（2026-09-09）：PR #35（`65c79a8`，合并提交 `8a5bd53`）补入 `mira_workflow`
  目标并新增 `tests/cmake/RunAndroidConsumerLink.cmake` 安装包 consumer 交叉链接
  门禁；合并提交 CI 12/12 通过，两 ABI 编译与链接证据已回填六个里程碑与平台矩阵，
  重开项逐项关闭。设备运行单列，仍由 `MNT-202609-27` 跟踪。

### 2.2 BUG-20260909-002：DEC-030 §5 重建配方事件载荷缺口

- 证据：`MNT-202609-25` 取证（新增 `tests/m13/m13_learning_persistence_test.cpp`，
  严格按 DEC-030 §5 五类事件——`WorkflowRunStarted` + `WorkflowStepSettled` +
  `WorkflowRunSettled` + `WorkflowPatchApplied` + 两员审计事件——实现重建并与直接
  记录路径写入 SQLite 的原记录逐字段比较）。**可恢复字段全部一致**：Episode 的
  run_id/workflow_id/ir_digest/policy/outcome/failed_step_id，Lesson 的
  lesson_id/recovered_run_id/failure.workflow_id/failure.step_id/
  resumed_without_patch/recovery[].patch_id，以及全部确定性派生 ID（MemoryId/
  MutationId）与 provenance（`WorkflowRunSettled` 事件 ID 锚点）。**缺口字段**
  （原记录有值、重建为默认，测试以冻结断言固化）：
  - Episode：`failure_reason_code`（`WorkflowStepSettled` 载荷无失败原因码）、
    `escalations`（无事件携带升级计数）、`checkpoint_handoffs`（同前；本轮场景无
    正样本，载荷缺失为同构缺口）、`recorded_at_ms`（只能以 `WorkflowRunSettled`
    envelope 时间戳代位，是另一次时钟读数，仅同源近似不逐位相等）。
  - Lesson：`failure.reason_code`、`failure.step_kind`（配方事件不含；`WorkflowStepStarted`
    有 kind 但不在配方五类内）、`recovery[].patch_digest`（`WorkflowPatchApplied`
    仅 patch_id/run_patch_epoch；`WorkflowPatchProposed` 有 digest 但同样不在配方内）、
    `recovery[].targets`（无配方事件携带逐 entry 目标）、`recorded_at_ms`。
  - 因此 `workflow_episode_digest`/`recovery_lesson_digest` 均无法由配方事件复现；
    审计事件中的 digest 只能由原记录内容复现（直接路径自洽已断言通过）。
- 影响：DEC-030 §5「足以确定性重建 Episode/Lesson 记忆记录；Memory 损坏后可从
  事件流重放恢复」的声明**未获证实**。ID、provenance 与身份字段可重建（重放落点
  幂等已验证），但记录内容与 digest 不具备逐字段可重建性；依赖「从事件流恢复同一
  学习记录」的消费者（MNT-202609-26 的恢复工具、MNT-202609-24 编排对 lesson 的
  patch 摘要关联）当前无充分数据源。
- 修订提案（待专项立项，不因登记而视为已批准）：方案 A——扩展事件载荷
  （`WorkflowStepSettled` 增有界 reason_code；`WorkflowRunSettled` 增
  escalations/checkpoint_handoffs 有界计数；`WorkflowPatchApplied` 增 patch_digest
  与目标摘要；两员审计事件携带 recorded_at_ms），闭集载荷扩展须按 DEC-022 §2 的
  fail-closed 原则走 schema 版本决策并评估既有消费者；方案 B——修订 DEC-030 §5 为
  「可恢复字段子集重建」，显式声明不参与重建与 digest 的字段，放弃审计 digest 与
  重建产物的一致要求。建议与 MNT-202609-24 联合评估（恢复编排消费 lesson 时
  patch 摘要是关键关联键）。
- Owner：Mira Maintainers。解除条件：选定方案并按「先专项设计与 DEC」流程冻结修订
  后实施，复跑本测试的缺口断言（字段补齐时冻结断言翻转，须同步更新本条与差异
  清单）。属于事件载荷设计缺口，非 Executor 能力缺口。

### 2.3 能力边界

`relevant_lessons` 是数据，`record_recovery_lesson` 是宿主专用写入门面；采纳经验、生成
修复、重新验证和决定是否沉淀资产仍缺 Harness 编排。学习记录默认还要求宿主安装
IMemory 与 EventStore；缺事件锚点的 Episode 会跳过（`record_episode`），并非开箱即持久学习。
该编排的设计与决策已由 `MNT-202609-23` 冻结
（[恢复编排设计](../design/workflow_recovery_orchestration_design.md)、DEC-031）：独立 Core
编排器、宿主通知触发、恢复请求归属载体 Task、四动作决策闭集、lesson 三层过滤、
`WorkflowRecoveryAttempted` 审计事件；实现与证据仍待 `MNT-202609-24`，冻结不等于能力已交付。

M13 已按范围推迟 Procedure 自动索引、向量召回、User Model 扩展、TTL/retention、训练导出。
这些属于新增能力，不据此重开已完成的功能项。跨进程 Library/Run/App Model 持久化亦为
M9/M12 显式非目标；M4 恢复能力不能直接外推到它们。
DEC-030 §5 的重建配方已由 `MNT-202609-25` 取证：ID/provenance/身份字段可确定性重建且
重放落点幂等，但内容字段与 digest 存在载荷缺口（`BUG-20260909-002`），“足以确定性
重建”未证实；修订实施前不将事件可解析等同于可恢复工具。

## 3. 工作项与顺序

所有任务负责人均为 Mira Maintainers；外部验证由 Maintainers 汇总 miracle 宿主维护者证据。
`Planned` 只表示任务定义可执行，不表示依赖已满足；`Proposed` 实现项须先完成对应设计任务。

### 3.1 P0：恢复验收可信度

- [x] `MNT-202609-21`（Completed）完成阶段 F 状态审计、缺口归类及后续计划；同步总计划、
  M8–M13、M7 跟进入口、README、架构现状与平台证据边界，校验文档链接与结构。
- [x] `MNT-202609-22`（Completed）修复 `BUG-20260909-001`：Android CI 显式编译
  `mira_workflow`，增加安装包 consumer 对 `Mira::workflow` 的交叉链接检查，防止只产出
  静态库而遗漏链接闭包。依赖：完整 pinned 子模块、NDK 26.3.11579264/API 24。
  验收：arm64-v8a/x86_64 两配置实际编译全部 Workflow 源文件并完成 consumer 链接；
  原有 Linux/Windows/quality 回归通过；回填六个里程碑与平台矩阵。设备运行单列，不能用
  交叉链接成功代替。证据见第 5 节 2026-09-09 第二条记录及 PR #35。

### 3.2 P1：让 Agent 使用经验并可验证地恢复

- [x] `MNT-202609-23`（Completed）冻结 Harness 恢复编排设计与决策。依赖：DEC-023/024/030。
  验收产物明确 `WaitingAgent -> continuation -> 有界模型请求 -> 结构化 patch/决策 ->
  resume -> Observe/Verify -> 宿主记录 lesson` 的所有者、事件关联与失败出口；定义无
  lesson、拒绝采纳、失效 lesson、取消/Takeover/迟到响应、升级预算及敏感信息处理。
  结果（2026-09-10）：产出
  [Workflow 恢复编排设计](../design/workflow_recovery_orchestration_design.md) 与
  [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)。七阶段管线逐段定义
  所有者/输入输出/失败出口（设计 §4），失败出口矩阵覆盖验收列举的全部场景（§8），
  全链路关联键为新增 `WorkflowRecoveryAttempted` 审计事件（§5.5）。证据见第 5 节
  2026-09-10 记录。
- [x] `MNT-202609-24`（Completed）实现上述编排与公共 consumer。依赖：22 的验收补齐（已完成）、
  23 冻结（已完成，DEC-031）；已按 §1 规则立项 [M14](m14-recovery-orchestration.md)
  承载 DEC-031 验证方式并交付（PR #38，合并提交 `bb77a0a`，CI 全绿；端到端主场景、
  五组矩阵与安全负向逐项取证，证据与实现澄清见 M14 验证记录）。
  验收：recorded Provider 真正收到检索上下文并生成可验证修复；首次
  失败、恢复、再次命中全链路可追踪；恶意 lesson 不提升权限，取消/接管后无新增动作，
  终态不复活，正常/异常/拒绝/超时/shutdown 均有测试；真实 Provider 证据由 27 补齐。
- [x] `MNT-202609-25`（Completed）验证学习持久化与事件重建配方，形成字段级证据及差异清单。
  依赖：现有 M4/M13 接口及可构建环境。验收：用真实 SQLite IMemory 写 Episode/Lesson，
  关闭并重建 owner 后同 scope 查询一致、跨 scope 不可读；从持久事件重建时逐字段比较
  ID、timestamp、failure signature、patch 摘要、provenance 与 digest；缺字段则登记
  实现缺陷或提出 DEC-030 修订，不标为成功。另覆盖慢/失败 store 对取消与 shutdown 的影响。
  结果（2026-09-09）：SQLite 持久化与慢/失败 store 组通过；事件重建配方取证**未通过**
  ——载荷缺口按验收登记 `BUG-20260909-002`（§2.2，含 DEC-030 §5 修订提案），
  未宣称配方验证成功；修订实施另行立项。证据见第 5 节 2026-09-09 第三条记录。
- [ ] `MNT-202609-26`（Proposed）交付有需求证据支持的 Workflow Library/Run/App Model
  持久化与恢复。依赖：25 的差异清单、27 的跨重启需求证据、专项存储/迁移 DEC。
  验收：旧版本 Run 固定原 digest、crash window 重建幂等、损坏/未知版本拒绝、未确认
  副作用先 Observe/Verify，OfflineReplay 不执行输入/网络/Tool；恢复工具与离线展示分离。

### 3.3 P1：真实任务证据与统一评估

- [ ] `MNT-202609-27`（Blocked）汇总 miracle 在固定 Mira 版本上的消费与需求报告。
  阻塞：本仓库相关外部验收项尚未回填，本轮未取得完整设备/宿主报告。
  补跑条件：宿主实现、受支持设备、受控 Provider profile 与脱敏记录可用。验收包括
  `mira.host.tree.v1`、转码后图像请求、决策编译修复同任务复验，以及旋转/前后台/
  权限撤销/Takeover 输入释放/宿主销毁；补充 A–F 组合任务与跨重启需求，记录 Mira 与
  miracle commit、设备/API、Provider/model、样本数、失败归因、成本和成功率。
  报告中的 VLM 坐标定位失败语料同时是 [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)
  视觉 grounding 方向的直接需求证据（2026-09-12 登记）。
  原未结项保留在 [Host ABI 维护计划](maintenance-2026-09-host-abi-feedback.md)、
  [图像传输维护计划](maintenance-2026-09-transport-and-image-media.md)、
  [决策修复维护计划](maintenance-2026-09-decision-compile-repair.md)，以其为验收源；
  此项统一索引，不重复计算完成数。
- [x] `MNT-202609-28`（Completed）定义当前离散动作 + Workflow 范围的最小 Eval profile。
  依赖：M8–M13 契约、[评估设计](../design/evaluation_and_benchmark_design.md)。验收：
  固定 case/fixture/baseline digest、失败分类、重复样本方法与预算，覆盖直接 Agent、
  Strict Workflow、恢复编排、启用/禁用 lesson 四种对照；指标包含任务成功率、恢复率、
  模型调用/token/cost、尾延迟、人工介入、重复副作用、内存/句柄与 shutdown 时间。
  阈值在跑结果前冻结；不得沿用已取消的 ONNX/realtime 准入要求。
  结果（2026-09-12）：产出
  [离散动作与 Workflow 最小评估 Profile](../design/discrete_workflow_eval_profile.md)
  （v1.0）与 [DEC-034](../decisions/DEC-034-minimal-eval-profile.md)。四臂对照、
  17 个 EvalCase（R/W/L/F 四家族 + 臂适用矩阵）、七类失败闭集、seed 方法与预算护栏、
  指标来源映射与跑前冻结阈值（硬门禁 G1–G6、绝对护栏、基线锚定回归规则、学习增益只
  报告不判定）逐项落实；能力缺口（lesson 细粒度开关、内存/句柄与 shutdown 计量、
  A 臂无 lesson 组合）如实声明并由 29 承载。证据见第 5 节 2026-09-12 记录。
- [ ] `MNT-202609-29`（Planned）实现并运行最小任务评估与 soak。依赖：28 冻结（已完成，
  [profile](../design/discrete_workflow_eval_profile.md) + DEC-034）；恢复组依赖 24
  （已完成）；真实平台组依赖 27（仍 Blocked，该组保持未完成）。验收：公共 API 驱动、
  recorded 回归确定、live canary 单独报告、所有适用 fault/cancel/Takeover/rejection/
  shutdown 场景有结果；学习收益依据对照与分布报告，不能凭命中一条 lesson 宣称成功率
  或成本改善。实现规范与阈值以 profile v1.0 为准。
- [ ] `MNT-202609-30`（Planned）产出 M7 重定义提案与任务迁移映射。依赖：27 的需求报告、
  28 的 profile。验收：逐项映射 `M7-01`–`M7-28` 到保留/缩减/推迟的建议及证据，明确
  release profile、ToolModule/OOP 是否必要、平台等级与退出条件；提交专项 DEC 后再
  修改 M7 范围，原项在批准迁移前保持未勾选。
  [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)（视觉 grounding，
  Issue #25）已冻结目标契约并显式以本项为实现入口；其里程碑范围随本项提案一并裁决。

### 3.4 P2：由消费者与失败语料驱动的扩展

- [ ] `MNT-202609-31`（Proposed）建立 Procedure 资产索引及目标到 Workflow 的检索消费者。
  依赖：27 证明复用需求、资产版本/删除/撤销契约冻结。验收：只索引已验证版本，绑定
  workflow ID/digest/scope；失效版本拒绝、删除可传播，检索命中后重新校验准入与 Verify。
- [ ] `MNT-202609-32`（Proposed）评估失败检索向量腿与 retention 扩展。依赖：25/29 的
  持久化与召回基线、真实失败语料。先测 exact+FTS 的漏召回/误召回、成本与延迟，再决策
  是否实现；任何实现需验证 ACL、版本漂移、删除传播和回退。无收益证据时保留现有方案。
  2026-09-12 起，31/32 的检索语义在 [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)
  Context Intelligence 框架（[专项设计](../design/context_intelligence_design.md)）内
  立项；两项的证据条件不变，不因方向冻结自动就绪。

建议先执行 22；23、25、28 可独立准备，27 持续回收外部证据。随后按证据推进 24/26/29，
由 30 收敛 M7。P2 不阻塞验收补齐。该顺序是本维护计划的任务优先级，不替代 DEC-011 的
产品范围决策，也不把缺少外部证据的任务置为已就绪。2026-09-10 状态：22/23/25/24 已完成
（24 由 M14 承载，PR #38）。2026-09-12 状态：28 完成（profile + DEC-034 冻结），29 的
设计前置满足转 `Planned`，recorded 基线轮可开工；其真实平台组与 live canary 仍分别等
27 的外部证据与受控凭据，26 依 25 差异清单与 27 需求推进。

## 4. Executor、风险与退出条件

本轮仅改文档，无新增执行路径。后续模型/有限编排任务使用 Executor 并消费 future；
持久化由后端受管 store worker 承载；设备事件通过 Adapter 外部循环协调；评估 owner
管理所有任务句柄。停止生产者、请求取消、回收模型/存储/平台工作、结算有限任务，最后
由非 worker 外部 owner `shutdown(true)`。具体容量、deadline、解阻塞与拒绝语义必须在
23/26/28 的设计中写明。若有 Executor 缺口，先核对公开接口并登记反馈台账。

- 风险：慢 IMemory 同步门面可能拖延驱动/结算（沿用 M13 `RISK-2026-049`）；由 25 取证。
- 风险：内存 fixture、事件解析与真实恢复语义不等价；由 25/26 检验，不扩大现有承诺。
- 外部阻塞负责人：Mira Maintainers；设备与 Provider 补跑条件见 27。

- [x] 本轮审计事实、计划与文档入口一致，历史失败及验收记录保留。
- [x] 22 完成并恢复 M8–M13 的适用跨平台验收（PR #35，`8a5bd53`，run 34353919141
  12/12；两 ABI 编译全部 Workflow 源文件并完成安装包 consumer 交叉链接；六个里程碑
  与平台矩阵已回填；设备运行明确不在本项范围）。
- [x] 23/25/28 产物齐全，后续实现已获正式里程碑或有理由的延期记录（23/25/24/28 已完成，
  24 由 M14 承载并关闭；28 产出 profile 与 DEC-034，29 据此转 `Planned`）。
- [ ] 27 外部证据归档；30 完成 M7 范围决策及任务映射。
- [ ] 本计划内 Proposed 实现项已正式迁移或通过决策明确取消/推迟，不能因文档更新完成
  而将整个维护计划关闭。

## 5. 验证记录

2026-09-09：`MNT-202609-21` 审计基于 `cd89384`。静态核对 `CMakeLists.txt`、
`.github/workflows/ci.yml`、Workflow/AgentLoop 公共 API 与实现、M13 三组测试及
installed-consumer，发现 `BUG-20260909-001`。CI 历史结果引用原里程碑记录，本轮未重新
查询远端 CI 或调用真实 Provider，不作为新的运行证据。

本轮环境 Linux x86_64、Ubuntu GCC 11.4.0、CMake 4.4.0。尝试
`cmake --build --preset debug -j2` 因无 Makefile 失败；随后 `cmake --preset debug`
因 `third_party/executor`、`third_party/mbedtls` 缺 CMakeLists 失败。
`git submodule status` 确认二者未初始化（分别固定 `4fd8e609`、`068ff080`）。
故 C++ 构建、CTest、sanitizer 本轮未执行成功，不沿用旧计数作为本轮结果。
Owner：Mira Maintainers；补跑条件：初始化 pinned 子模块、满足项目 C++20 工具链后，执行
configure/build/CTest；Android 按 22 使用 NDK 与两 ABI 取证。

文档验证：本轮 12 个新增/修改 Markdown 的相对链接、唯一一级标题、标题层级与代码围栏
经逐文件脚本检查通过；`git diff --check` 通过。全仓 `python3 tools/check_docs.py .`
仍失败，四处均为既有文档指向未初始化子模块中的 Executor API/集成指南/LICENSE 与
Mbed TLS LICENSE；未修改这些引用或将缺失伪造为通过。负责人 Mira Maintainers，补跑
条件为初始化上述 pinned 子模块后重新执行该命令。

2026-09-09：`MNT-202609-22` 完成（PR #35，`65c79a8`，合并提交 `8a5bd53`）。
变更：`.github/workflows/ci.yml` Android job 目标列表补入 `mira_workflow`，matrix
增加 preset→toolchain 映射；新增 `tests/cmake/RunAndroidConsumerLink.cmake`——安装
Android 构建到独立 prefix，再以同 ABI toolchain 配置 `tests/consumer` 对安装包链接
`Mira::workflow`（含 core/state_store/net_transport/mbedtls_transport、executor、
sqlite3 闭包），install 步骤对 MiraTargets 导出成员未构建即失败；仅链接不运行，
设备执行由 `MNT-202609-27` 跟踪。交叉编译下包查找需 `CMAKE_FIND_ROOT_PATH` 与
`CMAKE_PREFIX_PATH` 并用。CI 证据：合并提交
[run 34353919141](https://github.com/Linductor-alkaid/mira/actions/runs/34353919141)
12/12 job 通过——
[arm64-v8a](https://github.com/Linductor-alkaid/mira/actions/runs/34353919141/job/102473681503)、
[x86_64](https://github.com/Linductor-alkaid/mira/actions/runs/34353919141/job/102473681333)
均以 NDK 26.3.11579264、API 24 编译全部 9 个 Workflow 源文件（clang 17/libc++、
warnings-as-errors）并完成 consumer 交叉链接；PR 检查全绿（含两事件各一轮）。
本机复现（Ubuntu 24.04 x86_64，同一 pinned NDK）：两 ABI 安装包 consumer 链接产出
真实 Android ELF；负向用例（移除 `libmira_workflow.a`）按预期在 install 步骤失败；
Linux gcc 13.3 Debug 66/66 ctest（含 `mira_installed_consumer_test`）。回填：M8–M13
六个里程碑重开项逐项关闭并恢复 `Completed`、平台矩阵 Workflow 双 ABI 行升级
`Build verified`（设备运行保持未验证）、总计划 4.1 与里程碑索引同步。限制：本轮
不覆盖 Android 设备运行、宿主消费与真实 Provider 证据（27），不外推为运行支持；
Windows/quality 由合并提交 run 原样通过，未额外增加组合。另：上一条记录中因未初始
化子模块而登记的 `tools/check_docs.py` 补跑条件已满足——子模块初始化后本轮
docs/sbom/platform-boundary 门禁全绿。

2026-09-09：`MNT-202609-25` 完成。新增 `tests/m13/m13_learning_persistence_test.cpp`
（`mira_add_m13_persistence_test`，额外链接 `Mira::state_store`，标签 integration/m13），
三组取证共享同一驱动场景：run A（Strict 终态失败）记 Episode；run B（Recoverable
失败升级 → skip patch → resume 完成）记 Episode 并由宿主 `record_recovery_lesson`
记 Lesson；事件经 `FileEventStore` 落盘。1) 持久化组：按文档顺序关闭整个 owner 栈
（runtime shutdown → store close → MiraRuntime stop → `executor.shutdown(true)`）后以
全新 executor 与 `SqliteMemoryStore` 重建同库文件——同 ID 记录逐字段一致（statement、
kind、scope、verification、confidence、status、version=1、provenance），同 scope
`failure_retrieval_query` 找回逐字节一致，跨 scope（Agent 异 subject、Application
kind）零结果，确定性 mutation id 重放 `idempotent_replay` 且版本不涨；审计事件
`WorkflowEpisodeRecorded`×2 / `WorkflowLessonRecorded`×1 均 outcome=recorded。
2) 重建配方组（`BUG-20260909-002` 证据）：严格按 DEC-030 §5 五类事件重建，可恢复
字段全部一致（身份/策略/ir_digest/outcome/failed_step_id、lesson 的
resumed_without_patch 与 recovery[].patch_id、全部确定性 ID 与 provenance 锚点）；
缺口字段以冻结断言固化（episode 的 failure_reason_code/escalations/
checkpoint_handoffs/recorded_at_ms，lesson 的 failure.reason_code/step_kind/
recovery[].patch_digest/targets/recorded_at_ms，timestamp 仅 RunSettled envelope
代位且偏差 ≤60s 同源近似），episode/lesson digest 均无法由重建复现而原记录 digest
与审计事件一致（直接路径自洽）。3) 慢/失败 store 组：query 慢 200ms 不阻塞升级至
WaitingAgent 且 `relevant_lessons` 为空、cancel 收敛 Cancelled 且幂等；apply 慢
200ms 的异步 drive 与 `shutdown()` 并发，drain 在预算内 clean；失败 store（注意
fixture 的 fail_applies 为一次性旋钮，已重新武装）终态不被 un-settle、审计
outcome=failed×2、cancel/shutdown 闭合、零记录落库。本机环境 Ubuntu 24.04 x86_64、
gcc 13.3.0、CMake 3.28.3：Debug 全量 ctest 67/67（原 66 + 本测试）、ASAN/UBSAN/TSAN
（`setarch -R`）m13 4/4、`format-check` 通过；Release、Windows、Android 编译与
quality 由 PR CI 补齐后回填。未覆盖：Android 设备运行与真实 Provider（27）、
Library/Run/App Model 跨进程持久化（26）。

2026-09-09：`MNT-202609-25` CI 证据回填。PR
[#36](https://github.com/Linductor-alkaid/mira/pull/36)（`ac73386`，合并提交
`20a58f6`）检查全绿：PR pipeline run
[34361479640](https://github.com/Linductor-alkaid/mira/actions/runs/34361479640)
12/12 与合并提交 master pipeline run
[34363464570](https://github.com/Linductor-alkaid/mira/actions/runs/34363464570)
12/12 均通过——Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、
Android arm64-v8a 与 x86_64（NDK，编译级）、ASAN/UBSAN/TSAN、quality（clang-tidy +
format + docs/sbom/platform-boundary）。新增测试目标仅进入 Linux/Windows 测试与
sanitizer 矩阵；Android 仍为编译与 consumer 链接取证，设备运行归 27。本条与上一条
共同构成 25 的完整验证记录，任务关闭；`BUG-20260909-002` 与 DEC-030 §5 修订提案
保持待立项。

2026-09-10：`MNT-202609-23` 完成（基线 `909f6e2`，仅文档变更）。产出
[Workflow 恢复编排设计](../design/workflow_recovery_orchestration_design.md)（v1.0）与
[DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)（Accepted）。设计依据
静态核对源码：`WorkflowRuntime` 公开面（`agent_continuation`/`patch_run`/`resume_run`/
`cancel_run`/`run_snapshot`/`record_recovery_lesson`）、`workflow_events.hpp` v1 闭集
（确认无「进入 `WaitingAgent`」事件）、`WorkflowRunView`/`WorkflowAgentContinuation`
（确认载体 TaskId/epoch 未公开，需增量字段）、`runtime.cpp` `begin_operation`
准入（放行 `Recovering`、拒绝 `Paused`/`SuspendedForTakeover`/终态）、
`workflow_learning.hpp`（lesson `recovery[]` 无参数值，采纳只能是模型合成）、
`ModelGateway`/`TaskAdmissionGate`/`OperationContext` 探针纪律。验收对照：七阶段管线
所有者/输入输出/失败出口（设计 §4 表）、事件关联键（§5.5
`WorkflowRecoveryAttempted`）、失败出口矩阵（§8，覆盖无 lesson、拒绝采纳三层、失效
lesson、取消、Takeover、迟到响应、两侧升级/恢复预算、模型不可用、shutdown）、敏感信息
（§9：参数投影缺省无值、rationale 不入事件、lesson 是数据非授权）、Executor 路由与关闭
顺序（§7）。同步：DEC-023/030 增加前向链接（不改语义）、Workflow 设计 §14.3/§15、
架构设计 §7.6/§16/§17、总计划 §4.1/§5 决策索引、README 能力表（并修正 MNT-22 后
失实的「Android 构建验收重新打开」表述）；`MNT-202609-24` 转 `Planned`。
验证（Ubuntu 24.04.4 x86_64、Python 3.14.6）：`python3 tools/check_docs.py .` 通过
（"Markdown links and fences: OK"）；9 个改动文件逐文件脚本检查唯一一级标题、标题层级
连续、代码围栏配对与相对链接可达通过（README 第 5 行无语言标注的围栏为 2026-09-05
既有内容，仓库门禁接受，未改动）；`git diff --check` 通过。限制：本轮无代码变更，
未执行 C++ 构建/CTest；恢复编排能力本身未实现，本记录只证明设计与决策冻结，不构成
「Agent 采纳 lesson」的能力证据；PR CI（quality 含 docs 门禁）结果合并后回填。

2026-09-10：`MNT-202609-24` 实现完成（详见 [M14 验证记录](m14-recovery-orchestration.md)）。
新增 `WorkflowRecoveryOrchestrator`（七阶段管线、四动作决策闭集、lesson 三层过滤、
提交前重核、三层预算、协作取消与 shutdown drain）、`WorkflowRecoveryAttempted` 审计
事件（v1 闭集扩展）与 `WorkflowAgentContinuation` 四个增量字段；修复
`enter_waiting_agent` 未刷新载体 task epoch 的记账缺口（透出的
`carrier_task_epoch` 曾过期一拍）。本地：Debug 68/68、ASAN/UBSAN/TSAN 目标组、
format/platform-boundary/docs 门禁、Android 两 ABI 编译 `mira_workflow` 通过；
实现澄清（episode 检索语义、lesson 四元计数、`epoch-advanced` 守卫定位）记录于
M14 文档与恢复编排设计 v1.0.1。任务保持未勾选，等待 PR CI（Release/Windows/quality/
完整 sanitizer）回填后关闭；设备运行与真实 Provider 证据仍归 27。

2026-09-10：`MNT-202609-24` CI 证据回填并关闭（PR
[#38](https://github.com/Linductor-alkaid/mira/pull/38)，head `89fae06`，合并提交
`bb77a0a`）。PR push/pull_request 两 pipeline 各 12 项与合并提交 master pipeline run
[34393602561](https://github.com/Linductor-alkaid/mira/actions/runs/34393602561) 全绿：
Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android 双 ABI
（编译级 + consumer 链接）、ASAN/UBSAN/TSAN、quality；首轮 quality 一处 clang-tidy
违例（未用局部变量）修复后复验。验收对照：recorded Provider 收到检索上下文并生成
可验证修复（端到端主场景，含 used_lessons 审计往返）；首次失败、恢复、再次命中全链路
事件关联键逐条断言；越权 patch 确定性拒绝、参数投影缺省无值、rationale 不入事件；
取消/接管/迟到响应/预算/shutdown 矩阵闭合，终态不复活。[M14](m14-recovery-orchestration.md)
退出条件逐项复核后关闭。真实 Provider 与设备运行证据仍归 27；恢复收益声明待 29 对照。

2026-09-12：维护者指示「阅读 issue、分析需求与架构改动方案、结合项目实际更新架构」，
据此评审两个开放 issue 并冻结方向决策（本轮仅文档变更，无实现）。范围：新增
[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 与
[Context Intelligence 设计](../design/context_intelligence_design.md)（Issue #39，
现状核对：Layer 0 已由 M4 `StandardContextManager` 交付、检索三腿齐备但 embedding
供给方缺失、无会话级语义固化）；新增 [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)
与[视觉 Grounding 设计](../design/visual_grounding_design.md)（Issue #25，现状核对：
`PerceptionEvidence`/`ElementRef{Ocr,Detector,Fused}` 占位已预留、坐标变换链与
accessibility 全链已交付、`ObservationPipeline` 无 perception 挂钩且仓库无推理依赖）；
同步架构设计 v0.3、Context 设计 v0.5、Observation/Host 设计 v1.1、M5 假设记录注记
v1.2、总计划 §4.1/§4.2/§5 与本计划 27/30/31/32 交叉引用。依据：DEC-011（#25 实现重入
以 27 证据经 30 立项，不重开 M5/M6）、DEC-005/DEC-013（#25 契约基座）、DEC-016/DEC-029/
DEC-030（#39 会话与记忆基座）、RULE-07/08/09。验证（Ubuntu 24.04.4 x86_64、
Python 3.14.6）：`python3 tools/check_docs.py .` 通过（"Markdown links and fences:
OK"）；10 个改动文件逐文件检查唯一一级标题、代码围栏配对与相对链接可达通过；
`git diff --check` 仅报告新增/改动设计文档头部元数据行的行尾双空格——与既有设计文档
blockquote 硬换行惯例一致（已提交文件同模式），无其他空白问题。限制：无代码变更，
未执行 C++ 构建/
CTest；两方向能力均未实现，设计内代码片段标注为契约草案；28/27 证据条件与全部里程碑
状态不变。

2026-09-12：PR CI 证据回填。PR
[#40](https://github.com/Linductor-alkaid/mira/pull/40)（head `67d38c9`，合并提交
`c8b41bf`）push pipeline run
[`34686235300`](https://github.com/Linductor-alkaid/mira/actions/runs/34686235300)
与 pull_request pipeline run
[`34686244497`](https://github.com/Linductor-alkaid/mira/actions/runs/34686244497)
各 12 项共 24/24 全绿：Linux GCC/Clang（Debug/Release）、Windows MSVC
（Debug/Release）、Android arm64-v8a 与 x86_64（NDK 编译级）、ASAN/UBSAN/TSAN 与
quality（clang-tidy 18 + clang-format + docs/sbom/platform-boundary 检查）全部通过，
零修复复验。本地 master 已同步至合并提交，工作分支（本地与远端）已删除。

2026-09-12：`MNT-202609-28` 完成（基线 `9fac585`，仅文档变更，工作树干净）。产出
[离散动作与 Workflow 最小评估 Profile](../design/discrete_workflow_eval_profile.md)
（v1.0）与 [DEC-034](../decisions/DEC-034-minimal-eval-profile.md)（Accepted）。设计前
对能力边界做代码级核实：`AgentLoop` 公开面（`run`/`LoopOutcome`/`AgentLoopConfig`
预算、`ModelDoneVerifier` 仅测试用）、`WorkflowRuntime` 公开面（Strict/Recoverable
policy、`set_learning_context` 缺省 NoOp 语义）、`WorkflowRecoveryOrchestrator` 预算
配置与 `WorkflowRecoveryAttempted` 载荷、`ModelUsage`/`BudgetLedger` 计量、事件闭集与
monotonic 双时钟、`SimulatorEnvironment` 可编程 fixture 与 `executed_inputs()` 回读、
m9/m13/m14 fixture 旋钮。据此冻结：四臂对照（A 臂无 lesson 组合、C 臂以不装 learning
context 实现 no-memory，均如实声明而非新增契约）；17 case 四家族 + 臂适用矩阵；七类
失败闭集；seed {0,1,2} + seed-0 重复的确定性与串行执行；指标来源映射（内存/句柄仅
Linux `/proc`、shutdown 时长 harness 墙钟，运行时无计量为已声明缺口，归 29）；跑前
冻结阈值（G1–G6 零容忍、五项绝对护栏、基线锚定回归规则、学习增益只报告不判定）；
显式排除 M5/M6 已取消的 ONNX/realtime 准入维度。同步：`MNT-202609-29` 转 `Planned`
（设计前置满足，recorded 基线轮可开工；真实平台组仍等 27）、本计划推进注记与退出条件
括注更新、评估设计 §15 与总计划 §4.1/§5 增加交叉引用。验证（Ubuntu 24.04 x86_64、
Python 3.14）：`python3 tools/check_docs.py .` 通过；新增/修改 Markdown 逐文件检查唯一
一级标题、标题层级、代码围栏与相对链接通过；`git diff --check` 无空白问题。限制：本轮
无代码变更，未执行 C++ 构建/CTest；profile 是规范冻结，评估能力本身未实现、未运行，
任何成功率/恢复率/成本结论须待 29 产出；PR CI（quality 含 docs 门禁）结果合并后回填。

2026-09-12：`MNT-202609-28` CI 证据回填并关闭（PR
[#41](https://github.com/Linductor-alkaid/mira/pull/41)，head `ad192be`，合并提交
`8523e9e`）。PR push pipeline run
[`34689917454`](https://github.com/Linductor-alkaid/mira/actions/runs/34689917454) 与
pull_request pipeline run
[`34689929556`](https://github.com/Linductor-alkaid/mira/actions/runs/34689929556) 各
12 项，合并提交 master pipeline run
[`34690567829`](https://github.com/Linductor-alkaid/mira/actions/runs/34690567829) 12
项，三轮共 36/36 全绿：Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/
Release）、Android arm64-v8a 与 x86_64（NDK 编译级）、ASAN/UBSAN/TSAN、quality
（clang-tidy + clang-format + docs/sbom/platform-boundary）全部通过，零修复复验。本条
与上一条共同构成 28 的完整验证记录，任务关闭。评估能力本身未实现、未运行，harness 与
基线轮归 `MNT-202609-29`（`Planned`，recorded 基线轮可开工）。本地 master 已同步至
合并提交，工作分支（本地与远端）已删除。
