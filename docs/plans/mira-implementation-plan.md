# Mira 实施总计划

> 状态：In Progress
> 负责人：Mira Maintainers
> 更新日期：2026-09-10（`MNT-202609-23` 恢复编排设计冻结，DEC-031）
> 设计依据：[Mira Runtime 设计](../design/mira_runtime_design.md)、[Context 与 Memory 设计](../design/context_and_memory_design.md)、
> [LLM API 协议设计](../design/llm-api-protocol-design.md)、[Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)

## 1. 计划目的

本文是 Mira 从空仓库走向可发布 Runtime 的交付入口，只维护范围、里程碑依赖、发布点和通用
门禁。每个里程碑的工作项、测试、风险和逐轮验证记录放在独立文件中。

勾选规则遵循[项目管理与文档规范](../project/project_management_and_documentation.md)：只有实现、
测试、必要文档和验收证据同时完成，工作项才可标记为 `[x]`。环境不足导致的未运行验证保持
未勾选，并记录环境、负责人和补跑条件。

## 2. v1 交付边界

### 2.1 v1 包含

- C++20 平台无关 Agent Core 和稳定的宿主集成边界，首期构建组合覆盖 Linux、Windows、Android。
- `Observe -> Reason -> Plan -> Act -> Verify` 可中断闭环。
- OpenAI-compatible 外部 LLM/VLM Provider。
- Screenshot 与结构化 UI 组成的 Observation Pipeline。
- 离散输入。
- Task、Session、事件、Checkpoint、Memory 与 Replay。
- Simulator 参考环境及至少一个真实平台 Adapter 的契约验证；首个真实目标为 Android Host/NDK。

2026-09-05 起（[DEC-011](../decisions/DEC-011-demo-first-external-validation.md)），依赖
M5/M6 的交付项（本地 OCR/检测/任务 ONNX 感知、连续轨迹与摇杆控制、Human Takeover 实现、
任务模型数据治理与蒸馏链）按原范围终止；这些能力是否以及以何种范围重新进入交付边界，
由独立 demo 仓库的需求验证证据重新定义。"真实平台 Adapter 契约验证"的里程碑载体随 M7
重定义确定。

### 2.2 v1 不包含

- Core 内本地运行通用 LLM/VLM。
- 具体产品 UI 或完整 Android 应用；能力验证 demo 由独立仓库承载（DEC-011），不进入本仓库。
- 未经目标设备实测的硬实时保证。
- 自动将 Runtime 事件、截图或 Memory 转为训练数据。
- 任意代码执行、任意 shell 或模型绕过 Policy 直接调用平台。
- 分布式 Runtime、跨设备一致性和云端控制面。

## 3. 不可破坏的项目约束

- [ ] `RULE-01` Core 不依赖 Android、Windows、Linux 等平台 SDK；平台能力只经 Adapter 注入。
- [ ] `RULE-02` 所有 Mira 发起的异步、阻塞、定时、串行控制和实时任务由 Executor 管理。
- [ ] `RULE-03` Task 状态只有串行控制面可以提交，迟到 completion 不能复活旧 epoch 或终态。
- [ ] `RULE-04` 模型输出只形成结构化 Decision；Action 在本地经过 capability、freshness、权限和
  SafetyPolicy 校验。
- [ ] `RULE-05` 外部副作用采用至多一次派发语义；不确定结果必须 Observe/Verify，禁止盲目重发。
- [ ] `RULE-06` 连续控制具有最大时长、watchdog 和可验证的 `Up/Cancel` 安全收敛。
- [ ] `RULE-07` EventStore 是已提交事实的权威记录；Checkpoint、Memory 和索引是可重建投影。
- [ ] `RULE-08` 所有队列、缓存、上下文、动作、模型请求和并发 operation 都有容量或预算上限。
- [ ] `RULE-09` Observation、Memory、Tool 和模型响应中的外部内容均是不可信数据，不能提升为
  SystemPolicy 或授权。
- [ ] `RULE-10` 性能、实时性、兼容性和跨平台声明必须由目标环境证据支撑。
- [ ] `RULE-11` Executor 能力不足时登记 `docs/executor_feedback/ledger.md`，不得静默引入平行生命周期。
- [ ] `RULE-12` 训练数据导出默认关闭，必须经过授权、脱敏、来源追踪、删除传播和审计。

## 4. 里程碑与发布点

| 里程碑 | 目标 | 前置 | 建议发布点 | 状态 |
| --- | --- | --- | --- | --- |
| [M0](m0-engineering-baseline.md) | 仓库、跨平台构建组合、Executor 集成和质量基线 | 无 | 内部工程基线 | Completed |
| [M1](m1-core-contracts.md) | 公共契约、状态机、持久化和安全边界冻结 | M0 | Core contract alpha | Completed |
| [M2](m2-observation-simulator-android-host.md) | Observation、坐标、Simulator 与 Android Host ABI | M1 | Environment alpha | Completed |
| [M3](m3-model-provider-agent-loop.md) | OpenAI-compatible Provider 和视觉离散闭环 | M2 | Agent loop alpha | Completed |
| [M4](m4-context-memory-recovery.md) | Context/Memory、Replay 和恢复 | M3 | Stateful agent beta | Completed |
| [M5](m5-local-perception-task-models.md) | 本地视觉、任务模型注册与 ONNX 推理（原范围终止） | M3 | 无（见 DEC-011） | Cancelled |
| [M6](m6-realtime-control-takeover.md) | 连续控制、实时路径和 Human Takeover（原范围终止） | M2、M5 | 无（见 DEC-011） | Cancelled |
| [M7](m7-tools-evaluation-platform-v1.md) | Tool 模组（[DEC-009](../decisions/DEC-009-tool-module-boundary.md)）、Tool 隔离、评估体系、生产加固和跨平台验证（范围与前置待重定义） | M4、M5、M6（待重定义） | v1.0（待重定义） | Blocked |
| [M8](m8-workflow-contracts.md) | Workflow 双路径契约冻结（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 A） | M4 | Workflow contract alpha | Completed |
| [M9](m9-workflow-runtime-minimal-loop.md) | Workflow Runtime 最小闭环（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 B：Strict/DryRun 执行、暂停/取消、操作工具闭环） | M8 | Workflow runtime alpha | Completed |
| [M10](m10-workflow-intervention-and-policy-set.md) | Workflow 介入与执行策略全集（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 C：策略全集、对话 patch 执行、决策点交互） | M9 | Workflow intervention alpha | Completed |
| [M11](m11-trajectory-compilation-and-task-induction.md) | 成功轨迹编译与任务归纳（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 D：轨迹采集、编译、归纳、DryRun 入库门禁） | M9（阶段 B；M10 生效态为输入） | Workflow compilation alpha | Completed |
| [M12](m12-app-model-and-navigation.md) | App Model 与导航（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 E：UI 状态图、Navigation Planner、GUI Mapping 数据面、置信度、`screen_state` 谓词） | M9（阶段 B；感知能力按 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)） | Workflow navigation alpha | Completed |
| [M13](m13-memory-and-learning-loop.md) | Memory 与学习闭环（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 F：四类记忆组织、Episode/Lesson 学习契约、失败检索、恢复复用） | M11、M12（阶段 D/E） | Workflow learning alpha | Completed |

### 4.1 当前状态复核与后续入口（2026-09-09）

阶段 A–F（M8–M13）实现已合入，阶段 F 已具备记忆域、Episode/Lesson 与失败检索。
本轮发现 `BUG-20260909-001`：Android CI 未构建独立的 `mira_workflow` 目标，
六个里程碑的跨平台取证项与退出条件一度重新打开。同日 P0 `MNT-202609-22` 修复
合入（PR #35，`8a5bd53`）：Android CI 显式编译 `mira_workflow` 并新增安装包
consumer 交叉链接门禁，合并提交 CI 12/12 通过，两 ABI 均实际编译全部 Workflow
源文件并完成安装闭包链接；六个里程碑据此逐项复核后恢复 `Completed`，已交付功能和
历史验证记录保留。Android 设备运行证据仍缺，由 `MNT-202609-27` 跟踪。

[阶段 F 后续计划](maintenance-2026-09-post-stage-f.md) 为下一轮执行入口：

1. P0 `MNT-202609-22`（Completed）：Android 两 ABI Workflow 编译与安装消费链接
   证据已补齐并回填六个里程碑与平台矩阵。
2. P1 `MNT-202609-23`–`26`：冻结并实现 Agent 采纳 lesson 的恢复编排——23 已于
   2026-09-10 完成（[恢复编排设计](../design/workflow_recovery_orchestration_design.md)
   与 [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)），24 转
   `Planned` 待立项里程碑实现；
   `MNT-202609-25` 已完成——真实 SQLite 学习后端跨 owner 重建取证通过，慢/失败
   store 下取消与 shutdown 闭合，DEC-030 §5 事件重建配方缺口登记
   `BUG-20260909-002`（修订提案待立项）；26 依证据推进 Library/Run/App Model
   跨进程持久化。
3. P1 `MNT-202609-27`–`30`：回收 miracle 真机/Provider 证据，建立任务评估基线，形成
   M7 重定义提案；外部证据缺失项保持未完成。
4. P2 `MNT-202609-31`–`32`：Procedure 检索消费者、语义召回与 retention 按实际需求立项。

M5/M6 保持 Cancelled，M7 保持 Blocked。#8 的最小 BuiltIn 工具闭环已由 DEC-015 和
维护轮交付；M7 剩余的是模组治理、隔离、评估及发布范围重定义，不再把 #8 列作未实现。
本轮不新增产品范围决策；具体依赖、负责人、验收与补跑条件见上述计划。

### 4.2 历史实施与验收记录

`M0 -> M1 -> M2 -> M3 -> M4` 已完成。2026-09-05 起（
[DEC-011](../decisions/DEC-011-demo-first-external-validation.md)），`M3 -> M5 -> M6`
交付链终止、M7 挂起：后续能力需求由独立仓库 demo 产品的验证证据重新定义，产出新的或
重定义的里程碑后恢复交付；在此之前不设关键路径。任何里程碑都不得以“后续再补取消、安全
或验证”关闭。2026-09-06 起，miracle 第一轮真机反馈经
[维护计划 maintenance-2026-09-host-abi-feedback.md](maintenance-2026-09-host-abi-feedback.md)
与 [DEC-012](../decisions/DEC-012-host-adapter-feedback-round1.md) 落地（GitHub #7–#13）；
其中 #8（AgentLoop ToolProposals）当时为方向登记；其最小闭环已于 2026-09-07
由 DEC-015 交付，M7 模组体系仍为 Blocked，待 POST-01 证据重定义。
同日第二轮反馈（GitHub #14、#15）经
[维护计划 maintenance-2026-09-transport-and-image-media.md](maintenance-2026-09-transport-and-image-media.md)
与 [DEC-013](../decisions/DEC-013-transport-export-and-image-media.md) 落地：官方网络
传输头随安装包导出，模型图像 wire 媒体类型改为工件记录驱动并由宿主负责编码。

2026-09-07 起（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)），
Mira 的长期架构方向确立为 Agent Harness 控制平面 + Workflow 数据平面双路径
（[Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)）。
该决策不改变既有里程碑状态、不解冻 M7、不自动恢复 M5/M6，也不改变 v1 交付边界现状；
其落地以 M7 重定义或新增里程碑承载，工作项、Executor 路由、测试矩阵与退出条件在对应
阶段文档定义，阶段划分以设计文档第 16 节为输入。

2026-09-07 起，Workflow 方向的阶段 A 由新增里程碑
[M8](m8-workflow-contracts.md)（`Proposed`）承载；选择新增里程碑而非重定义 M7 的理由见
该文件第 1 节。后续阶段 B–F（Runtime 最小闭环、介入与策略、编译与抽象、App Model 与
导航、Memory 与学习闭环）依次由新增里程碑承载：每个阶段进入 `Planned` 前，先依据
前一阶段冻结的契约完成对应专项设计与决策，再创建里程碑文件；不对未创建的里程碑预分配
编号。M7 状态不因该方向改变，GitHub
[#8](https://github.com/Linductor-alkaid/mira/issues/8) 最小闭环随后由 DEC-015
提前交付；模组化范围仍随 M7 重定义。
2026-09-08，M8 经维护者评审转 `Planned` 并进入实施（`In Progress`）；其契约决策
（[DEC-019](../decisions/DEC-019-workflow-ir-contract.md) 至
[DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)）与专项设计
（[Workflow Runtime 设计](../design/workflow_runtime_design.md)）已冻结。
2026-09-08，`Mira::workflow` 契约模块与五组契约测试随同交付，PR
[#29](https://github.com/Linductor-alkaid/mira/pull/29) CI 全绿（三平台 + sanitizer +
quality 矩阵），M8 关闭（`Completed`）。同日，阶段 B（Runtime 最小闭环）里程碑
[M9](m9-workflow-runtime-minimal-loop.md) 依据 `M8-05` 设计创建：经维护者评审（用户指示
依设计与计划推进下一步开发，与 M8 同一授权模式）转 `Planned` 并进入实施（`In Progress`）；
其范围、契约补全（`M9-01` 冻结 M8 暂定默认值与 ToolCall 工具绑定约定）、Executor 路由
定稿与测试矩阵见该文件。同日交付 `WorkflowRuntime`（Strict/DryRun 执行闭环、暂停/恢复/
取消、恢复钩子、shutdown、run/pause/resume/cancel 四操作 BuiltIn 闭环与模型发起
`run_workflow` 端到端），PR [#30](https://github.com/Linductor-alkaid/mira/pull/30) CI
全绿（三平台 + sanitizer + quality 矩阵 24 项），M9 关闭（`Completed`）；阶段 C（对话
驱动 patch 与执行策略全集）里程碑可依据该设计进入 `Planned`。同日，阶段 C 专项决策
[DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)（策略全集运行时
语义与检查点）与 [DEC-024](../decisions/DEC-024-conversation-patch-execution.md)（对话
patch 执行语义与决策点交互）冻结，`workflow_runtime_design` 升至 v0.3（§11 阶段 C 实施
规范）；里程碑 [M10](m10-workflow-intervention-and-policy-set.md) 依据其创建，经维护者
评审（用户指示依设计与计划推进下一步开发，与 M8/M9 同一授权模式）转 `Planned` 并进入
实施（`In Progress`）。同日交付策略全集运行时（失败升级、检查点、升级预算）、对话
patch 执行闭环（准入矩阵、幂等双检、步边界生效、审计、回退、策略切换）与决策点交互
（`WaitingUser` 两类来源、`resolve_decision` 唯一出口、`request_user_input` 工具、
模型发起端到端），五操作与决策工具 BuiltIn 闭环齐活；PR
[#31](https://github.com/Linductor-alkaid/mira/pull/31) CI 全绿（三平台 + sanitizer +
quality 矩阵 24 项；首轮 quality 1 处 clang-tidy `performance-move-const-arg` 修复后
复验），M10 关闭（`Completed`）；阶段 D（成功轨迹编译与任务归纳）里程碑可依据该设计
创建。同日，阶段 D 专项决策
[DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md)
（成功轨迹契约、编译与入库门禁）与 [DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md)
（任务归纳与参数化提议）冻结，`workflow_runtime_design` 升至 v0.4（§12 阶段 D 实施
规范）；里程碑 [M11](m11-trajectory-compilation-and-task-induction.md) 依据其创建，经
维护者评审（用户指示依设计与计划推进下一步开发，与 M8/M9/M10 同一授权模式）转
`Planned` 并进入实施（`In Progress`）。同日交付轨迹契约与采集（`capture_trajectory`）、
字面量编译与默认值固化、结构 diff 任务归纳与参数化重写（`workflow_compiler` 模块）、
DryRun 入库门禁（`publish_validated`：内容派生证据、幂等 NoOp、失败库零变更）与
publish 三员审计事件；本机全矩阵（Debug/Release/ASAN/UBSAN 60/60、TSAN 59/59、
quality）通过，PR
[#32](https://github.com/Linductor-alkaid/mira/pull/32) CI 全绿（三平台 + sanitizer +
quality 24 项；首轮 quality 1 处 clang-tidy `performance-move-const-arg` 修复后复验），
M11 关闭（`Completed`）；阶段 E（App Model 与导航）里程碑可依据该设计创建（前置为
阶段 B；感知能力按 DEC-011 重定义）。同日，阶段 E 专项决策
[DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)（App Model 契约
与置信度）与 [DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md)
（Navigation Planner 与 Navigate 步骤解析）冻结，`workflow_runtime_design` 升至 v0.5
（§13 阶段 E 实施规范）；里程碑 [M12](m12-app-model-and-navigation.md) 依据其创建，经
维护者评审（用户指示依设计与计划推进下一步开发，与 M8–M11 同一授权模式）转 `Planned`
并进入实施（`In Progress`）。同日交付 `workflow_navigation` 模块（`AppModel` 契约与
置信度纯函数、确定性 Dijkstra 规划器）、导航两员事件与 `WorkflowRuntime` 集成
（导航上下文安装、Navigate 准入条件化与逐边执行、`screen_state` 谓词绑定、置信度
回写；DryRun 真实规划使 `publish_validated` 门禁对导航可达性有约束力）；本机全矩阵
（Debug/Release/ASAN/UBSAN 63/63、TSAN 62/62、本地门禁）通过，PR
[#33](https://github.com/Linductor-alkaid/mira/pull/33) CI 全绿（三平台 + sanitizer +
quality 24 项；两轮 quality 各 1 处 clang-tidy 违例——`performance-inefficient-
string-concatenation` 与 `bugprone-branch-clone`——修复后复验），M12 关闭
（`Completed`）；阶段 F（Memory 与学习闭环，前置 D/E）里程碑可依据架构设计 §10/§16
创建（先专项设计与决策，不预分配编号）。同日，阶段 F 专项决策
[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)
（Memory 四类组织与 Workflow 学习契约）与
[DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)
（学习闭环运行时语义）冻结，`workflow_runtime_design` 升至 v0.6（§14 阶段 F 实施
规范）；里程碑 [M13](m13-memory-and-learning-loop.md) 依据其创建，经维护者评审
（用户指示依设计与计划推进下一步开发，与 M8–M12 同一授权模式）转 `Planned` 并进入
实施（`In Progress`）。同日交付 `workflow_learning` 模块（`MemoryDomain` 四类组织、
`WorkflowEpisodeRecord`/`WorkflowRecoveryLesson`/`WorkflowFailureSignature` 契约、
纯转换与失败检索查询构建）与 `WorkflowRuntime` 学习集成（`set_learning_context`、
结算期 Episode 记录、失败驱动升级的检索进入 `agent_continuation().relevant_lessons`、
`record_recovery_lesson` 恢复复用闭环、两员学习审计事件）；本机全矩阵
（Debug/Release/ASAN/UBSAN 66/66、TSAN 65/65、本地门禁）通过；PR
[#34](https://github.com/Linductor-alkaid/mira/pull/34) CI 全绿（head `bff2fe3`，push
pipeline run [`34310027223`](https://github.com/Linductor-alkaid/mira/actions/runs/34310027223)、
pull_request pipeline run
[`34310029920`](https://github.com/Linductor-alkaid/mira/actions/runs/34310029920)）：
Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android arm64-v8a 与
x86_64（NDK）、ASAN/UBSAN/TSAN 与 quality（clang-tidy 18 + clang-format + docs/sbom/
platform-boundary 检查）全部 24 项通过（两轮修复后复验：MSVC 需要 `<numeric>` 提供
`std::accumulate`；clang-tidy `optin.performance.Padding` 要求 RunRecord 按对齐分组
重排字段），M13 关闭（`Completed`）。DEC-014 阶段 A–F 至此全部交付；后续方向（真实
平台 Adapter 契约验证与 M7 重定义、阶段 F 显式非目标中的 Procedure 索引/失败检索
向量腿/Agent 采纳 lesson 的编排）按证据另行立项，不设隐式关键路径。

M4–M7 的范围、稳定工作项、Executor 路由、测试矩阵、风险、退出条件和验证记录已拆入各自阶段
文档。`Planned` 仅表示范围和验收方式已明确，不表示前置已满足或实现已开始。M3 已于 2026-09-02
完成跨平台 TLS、upload fixture 与 MiniMax-M3 Responses 分能力互操作验收；支持声明严格限于兼容性
矩阵中标记 `InteropVerified` 的字段，image=`Failed` 和其他 `Unknown` 能力不被外推。

## 5. 首批架构决策

| 决策 | 主题 | 状态 | 冻结点 |
| --- | --- | --- | --- |
| [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md) | Runtime 的 Executor 所有权与串行控制面 | Accepted | M0 |
| [DEC-002](../decisions/DEC-002-public-contract-versioning.md) | 公共契约、结果和版本化边界 | Accepted | M1 |
| [DEC-003](../decisions/DEC-003-event-sourced-persistence.md) | EventStore 事实源与副作用日志协议 | Accepted | M1 |
| [DEC-004](../decisions/DEC-004-security-authority-confirmation.md) | 权限、能力授权和 Human Confirmation | Accepted | M1 |
| [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md) | Observation 坐标与 Android Host 边界 | Accepted | M2 |
| [DEC-006](../decisions/DEC-006-local-perception-task-models.md) | 本地感知和任务 ONNX 模型边界 | Accepted | M5 |
| [DEC-007](../decisions/DEC-007-llm-api-protocol-strategy.md) | LLM API 规范契约与协议方言策略 | Accepted | M3 |
| [DEC-008](../decisions/DEC-008-transport-dependency-strategy.md) | 历史 M3 传输基线 | Superseded by DEC-010 | M3 |
| [DEC-009](../decisions/DEC-009-tool-module-boundary.md) | 工具模组边界与能力协商 | Accepted | M7 |
| [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md) | 锁定 Mbed TLS、受管代理与远端文件生命周期 | Accepted | M3 |
| [DEC-011](../decisions/DEC-011-demo-first-external-validation.md) | Demo 优先验证、M5/M6 终止与外部消费边界 | Accepted | M4 后 |
| [DEC-012](../decisions/DEC-012-host-adapter-feedback-round1.md) | Host Adapter 第一轮反馈契约修订（artifact 注入、输入时长、UI 树线格式、lease 统计） | Accepted | M4 后维护轮 |
| [DEC-013](../decisions/DEC-013-transport-export-and-image-media.md) | 传输头文件导出与模型图像 wire 媒体类型（宿主负责编码） | Accepted | M4 后维护轮 |
| [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) | Agent Harness 控制平面与 Workflow 数据平面双路径架构 | Accepted | M7 重定义（暂定） |
| [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md) | BuiltIn 工具执行边界与 AgentLoop 工具闭环（GitHub #8） | Accepted | M4 后维护轮 |
| [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md) | 对话事件、会话投影与步边界用户消息 | Accepted | M4 后维护轮 |
| [DEC-017](../decisions/DEC-017-complete-task-command.md) | 任务终态完成命令 `complete_task` | Accepted | M4 后维护轮 |
| [DEC-018](../decisions/DEC-018-takeover-input-release-and-operation-admission.md) | Takeover 平台输入释放与暂停态操作准入 | Accepted | M4 后维护轮 |
| [DEC-019](../decisions/DEC-019-workflow-ir-contract.md) | Workflow IR 公共契约与版本化 | Accepted | M8 |
| [DEC-020](../decisions/DEC-020-workflow-run-lifecycle.md) | WorkflowRun 生命周期、Task 状态映射与执行策略 | Accepted | M8 |
| [DEC-021](../decisions/DEC-021-workflow-tool-channel.md) | Workflow 操作的 Tool 通道表达 | Accepted | M8 |
| [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md) | 对话 patch 语义与 Conversation 工件 | Accepted | M8 |
| [DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md) | 执行策略全集运行时语义与检查点（阶段 C） | Accepted | M10 |
| [DEC-024](../decisions/DEC-024-conversation-patch-execution.md) | 对话 patch 执行语义与决策点交互（阶段 C） | Accepted | M10 |
| [DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md) | 成功轨迹契约、Workflow 编译与入库门禁（阶段 D） | Accepted | M11 |
| [DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md) | 任务归纳与参数化提议（阶段 D） | Accepted | M11 |
| [DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md) | App Model 契约与置信度（阶段 E） | Accepted | M12 |
| [DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md) | Navigation Planner 与 Navigate 步骤解析（阶段 E） | Accepted | M12 |
| [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md) | Memory 四类组织与 Workflow 学习契约（阶段 F） | Accepted | M13 |
| [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md) | 学习闭环运行时语义（阶段 F） | Accepted | M13 |
| [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md) | Agent Harness 恢复编排运行时语义（阶段 F 后续） | Accepted | `MNT-202609-24` 立项的里程碑 |

“Accepted”表示架构方向已生效，不表示对应实现工作项已经完成。具体实现仍由里程碑复选框和
验证记录证明。DEC-006 与 DEC-009 的架构方向保留；其落地里程碑（M5、M7）分别被 DEC-011
终止与挂起，落地范围待 demo 证据重定义。

## 6. 通用质量门禁

- [ ] 所有公开头文件可由最小外部 consumer 独立包含和链接。
- [ ] 状态转换、取消、shutdown、背压、提交拒绝、异常和不确定副作用有自动化测试。
- [ ] 每个 Provider/Adapter 通过共同 contract test，capability 与实际行为一致。
- [ ] OfflineReplay 不执行网络、Tool 或输入副作用。
- [ ] ASAN/UBSAN 常规运行；TSAN 在支持环境运行，不能运行时保留未完成门禁和补跑条件。
- [ ] 敏感字段、截图、UI Tree、Memory、模型请求和训练导出通过脱敏与权限负向测试。
- [ ] Android 真机或受支持模拟环境覆盖旋转、前后台、权限撤销、宿主销毁和输入释放。
- [ ] benchmark 记录硬件、OS、构建、Executor/模型配置、样本量和百分位。
- [ ] 依赖锁定、许可证和 SBOM 可重复生成。
- [ ] 文档、决策、计划状态和验证记录与实现同步。

## 7. 计划维护规则

- 总计划只更新里程碑状态、依赖、范围和通用门禁；实施细节写入阶段文件。
- 新增里程碑或改变关键路径时，更新受影响的决策、设计和阶段前置条件。
- 里程碑只能在全部工作项和退出条件完成、且验证记录可复现后改为 `Completed`。
- 若某项被拆到后续阶段，原项保持未完成，除非范围变更经决策记录批准并明确迁移编号。
