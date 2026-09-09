# M8：Workflow 双路径契约冻结

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M4（已完成）；方向依据 [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)
> 建议发布点：Workflow contract alpha
> 更新日期：2026-09-09

> 2026-09-09 状态复核：实现已交付；`BUG-20260909-001` 发现 Android CI 未构建
> `mira_workflow`，本里程碑的矩阵取证项及相关退出条件重新打开。历史验证记录保留，
> 当前状态以本注记为准；依赖方可复用冻结契约，跨平台关闭共同等待
> [阶段 F 后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-22`。

## 1. 目标

按 [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 与
[Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
第 16 节阶段 A，冻结 Workflow-native 路径的全部公共契约：Workflow IR、WorkflowRun 生命周期
与 Task 状态机映射、Workflow 操作的 Tool 通道表达、对话 patch 语义与 Conversation 工件。
交付形态是可编译、可测试的契约实现（数据结构、序列化、校验、状态转换表）加专项决策与设计
文档；不含运行时执行——执行闭环由后续里程碑（阶段 B）承载。

选择新增 M8 而非重定义 M7 的理由：M7 原范围（Tool 模组、评估、平台与发布加固，
[DEC-009](../decisions/DEC-009-tool-module-boundary.md)）仍按
[DEC-011](../decisions/DEC-011-demo-first-external-validation.md) 等待 demo 证据重定义，
DEC-014 明确不解冻 M7。将 Workflow 方向并入 M7 会加重 `RISK-2026-029` 已识别的集成面
过大问题，且两个方向的价值增量与验收证据不同源。

## 2. 范围与非目标

### 2.1 范围

- 四份专项决策：Workflow IR 公共契约与版本化；WorkflowRun 生命周期、状态映射与执行策略；
  Workflow 操作的 Tool 通道表达；对话 patch 语义与 Conversation 工件。
- Workflow Runtime 专项设计文档，作为阶段 B 起的实施规范。
- `mira-workflow` 契约模块：IR 数据结构、JSON 序列化与 schema 校验、参数 Schema 与绑定、
  WorkflowRun 状态视图与转换表、资产版本化与 digest、EventStore 事件 schema 扩展、
  Workflow 操作 Tool 规格 schema。
- `Mira::workflow` 公共包导出与最小 consumer 验证。

### 2.2 非目标

- Workflow Runtime 执行、导航、验证执行与恢复钩子（阶段 B，后续里程碑）。
- 对话驱动 patch 的运行时交互、执行策略全集与 Takeover 交互（阶段 C）。
- 成功轨迹编译、任务归纳与入库流程（阶段 D）。
- App Model、Navigation Planner 与置信度（阶段 E）。
- Memory 四类组织与学习闭环（阶段 F）。
- AgentLoop 内 ToolProposals 执行闭环（GitHub
  [#8](https://github.com/Linductor-alkaid/mira/issues/8)）：仍按 M7 重定义与 POST-01
  触发条件处理；本里程碑只冻结 schema，不实现执行。
- Workflow Library 的 SQLite 存储实现与投影重建工具：契约与投影规则在本里程碑冻结，
  实现随阶段 B 的持久化需要落地。

## 3. 准入条件与设计依据

### 3.1 准入条件

- DEC-014 已接受（2026-09-07），架构设计文档 Active。
- M0–M4 已完成：Task 状态机、EventStore、Tool Proposal 桥、Memory 与 Context 分区等被
  复用契约均已冻结。
- 本里程碑经维护者评审由 `Proposed` 转 `Planned`；决策中暂不能定死的取值按规范标
  「暂定默认值」，注明负责人与最迟冻结里程碑。

### 3.2 设计与决策依据

- [Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
  （第 4、6、7 节为直接输入）
- [核心公共契约与状态机](../design/core_contracts_and_state_machine.md)
- [Model Provider 与 Tool 扩展设计](../design/model_provider_and_tool_design.md)、
  [工具模组设计](../design/tool_module_design.md)
- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)
- [Agent Harness 参考研究：LangGraph 与 Pi](../design/harness_reference_study.md)
  （`M8-02`/`M8-03`/`M8-04`/`M8-05` 的机制输入；采纳与否由各决策记录逐条冻结）
- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
- [DEC-002](../decisions/DEC-002-public-contract-versioning.md)、
  [DEC-003](../decisions/DEC-003-event-sourced-persistence.md)、
  [DEC-004](../decisions/DEC-004-security-authority-confirmation.md)、
  [DEC-009](../decisions/DEC-009-tool-module-boundary.md)、
  [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)

## 4. 工作项

### 4.1 决策与设计冻结

- [x] `M8-01` 新增决策记录：Workflow IR 公共契约与版本化。冻结 IR 表达范围（参数 Schema、
  步骤图、每步前置条件与验证谓词、导航目标、恢复钩子、执行策略声明）、序列化格式与
  digest、兼容性承诺、Workflow 版本历史不可变及索引为可重建投影的存储边界（对齐
  DEC-002、DEC-003、`W-03`）。
- [x] `M8-02` 新增决策记录：WorkflowRun 生命周期与 Task 状态机映射。冻结
  `Created/Running/Paused/WaitingUser/WaitingAgent/Completed/Failed/Cancelled` 与既有
  `TaskState` 族（`Pausing`/`Paused`/`Recovering`/`SuspendedForTakeover` 等）的精确映射、
  单写者与终态幂等规则、迟到完成隔离（`W-01`/`W-07`）；冻结执行策略
  `Strict/Recoverable/AgentAssisted/Interactive/DryRun` 的语义表、策略影响面与暂定默认值。
- [x] `M8-03` 新增决策记录：Workflow 操作的 Tool 通道表达。冻结
  `run_workflow/patch_workflow/pause_workflow/resume_workflow/cancel_workflow` 经
  ToolIntent/ToolProposal 桥的 wire schema、参数校验、错误语义与权限挂钩点；明确宿主与
  用户命令不经模型即可触发运行控制；明确与 DEC-009 模组边界及 GitHub #8 的关系
  （`update_memory` 沿用既有规则、`request_user_input` 的表达席位预留，均不在本决策核心
  范围）。
- [x] `M8-04` 新增决策记录：对话 patch 语义与 Conversation 工件。冻结「本次运行修改 /
  Workflow 定义修改 / 用户偏好记忆」三类目标的判定与歧义确认规则、patch 幂等/审计/版本
  边界回退、Conversation History 与 Execution Trace 的分离、Conversation 工件的脱敏与
  保留策略（`W-04`/`W-05`、DEC-004、`RULE-07`）。
- [x] `M8-05` 新增专项设计文档 `workflow_runtime_design`：IR/Run/Runtime 分解、验证谓词与
  恢复钩子语义、事件 schema、错误分类、Executor 路由、取消与 shutdown 顺序、测试策略，
  作为阶段 B 起的实施规范；文中接口标注为草案级别。

### 4.2 契约实现与测试

- [x] `M8-06` 建立 `mira-workflow` 模块并实现 Workflow IR 结构、JSON 序列化与 schema
  校验：未知字段与版本不匹配 fail closed、嵌套深度与字节数上限（`RULE-08`）、往返保真。
- [x] `M8-07` 实现参数 Schema 契约与绑定校验：类型、默认值、约束，缺失与越界参数的
  确定性错误码；参数绑定为纯函数。
- [x] `M8-08` 实现 WorkflowRun 状态视图与转换表：按 `M8-02` 冻结的映射提供全部合法与
  非法转换的纯函数判定，表驱动覆盖终态幂等与迟到完成隔离语义。
- [x] `M8-09` 实现 Workflow 资产版本化契约：版本记录（Who/Why/What Changed/Validation
  Result/Timestamp）、不可变历史、内容 digest、旧版本 Run 回放引用创建时版本
  （`W-03`，设计第 7.7 节）。
- [x] `M8-10` 扩展 EventStore 事件 schema：WorkflowRun/Step/Patch/策略切换等版本化载荷
  与脱敏规则；OfflineReplay 识别 workflow 事件且不重放副作用（`W-08`，契约级断言）。
- [x] `M8-11` 实现 Workflow 操作 Tool 规格 schema：五个操作的参数、结果与错误 schema 及
  本地校验，fail closed 语义与 `resolve_tool_calls` 同源；只校验，不执行。

### 4.3 工程与公共包门禁

- [x] `M8-12` `Mira::workflow` 进入安装包，最小 consumer 独立包含、链接与运行通过（对齐
  DEC-011 的公共 API 检验边界与既有 installed-consumer 测试模式）。
- [ ] `M8-13` 契约测试矩阵取证：三平台构建组合、ASAN/UBSAN（TSAN 按本机环境限制记录
  补跑条件）、负向与边界测试全绿；总计划、决策索引、设计与 API 手册同步后关闭本里程碑。

## 5. Executor 路由与关闭

本里程碑全部交付物为同步纯函数契约与测试，不引入新的异步路径、定时器或阻塞 I/O，不改变
Runtime 关闭顺序。Workflow Runtime 的 Executor 路由表（步骤执行、验证、恢复、导航的承载
能力、句柄所有者与结算要求）在 `M8-05` 设计文档中按 `W-01` 预定义，并在阶段 B 里程碑
细化为与 M7 第 5 节同构的路由与关闭表。

## 6. 风险与阻塞

- `RISK-2026-034`：IR schema 在无执行反馈时过早冻结，导致阶段 B 返工。Owner：Mira
  Maintainers。缓解：`M8-01` 至 `M8-04` 中无法定死的取值标「暂定默认值」并设最迟冻结
  里程碑；schema 以阶段 B 最小闭环（Strict/DryRun）反推，超出门类的字段留扩展位、不先验
  承诺。
- `RISK-2026-035`：Agent 发起的 Workflow 调用闭环依赖 GitHub #8（AgentLoop ToolProposals
  不可执行）与 miracle POST-01 证据。Owner：Mira Maintainers。缓解：本里程碑只冻结
  schema；阶段 B 的 Workflow 执行以宿主/用户命令直达 Runtime 验证，不经模型；Agent 发起
  调用的执行闭环随 M7 重定义处理，不阻塞本里程碑。
- `RISK-2026-036`：对话 patch 语义（三类目标区分、歧义确认）范围过大、难以一次冻结。
  Owner：Mira Maintainers。缓解：`M8-04` 冻结语义与 schema，交互实现与策略全集留给
  阶段 C；无法收敛的子项显式列为开放问题，不用含糊措辞掩盖。
- `RISK-2026-037`：契约模块先行造成与既有 `mira_core`/`mira_model` 契约的依赖方向漂移。
  Owner：Mira Maintainers。缓解：`mira-workflow` 仅依赖 Core 抽象与 JSON 支持库，禁止
  反向依赖 Agent 决策层（设计第 13 节）；由头文件独立性与 installed-consumer 测试约束。

## 7. 测试与退出条件

- [ ] `M8-01` 至 `M8-13` 全部完成并有可复现验证记录。
- [x] 四份决策记录 `Accepted`、专项设计文档 `Active`；其中暂定默认值均注明负责人与最迟
  冻结里程碑。
- [x] IR 序列化与校验、参数绑定、状态转换表、版本化、事件 schema、Tool 规格 schema 均有
  正向与负向测试；未知字段、版本不匹配、超限、非法转换与终态复活尝试全部 fail closed。
- [ ] `Mira::workflow` 最小 consumer 在 Linux 基准环境通过；Windows/Android 构建组合按
  M0 基线执行并记录；未运行项保持未勾选并记录补跑条件。
- [x] OfflineReplay 对 workflow 事件不产生副作用的契约断言通过。
- [x] 总计划第 4 节、决策索引、API 手册与本文件同步；阶段 B 里程碑文档可依据 `M8-05`
  进入 `Planned`。

## 8. 验证记录

2026-09-07：依据 DEC-014 与架构设计第 16 节阶段 A 创建本里程碑，状态 `Proposed`，尚无
实现。负责人为 Mira Maintainers；新增里程碑而非重定义 M7 的理由见第 1 节。转为 `Planned`
前不排期任何实现工作。

2026-09-08：维护者评审通过（用户指示依设计与计划推进下一步开发），M8 由 `Proposed` 转
`Planned` 并进入实施（`In Progress`）：准入条件第 3 条满足。同日冻结 `M8-01`–`M8-05`
依赖的决策与设计输入：[DEC-019](../decisions/DEC-019-workflow-ir-contract.md)（IR 契约，
`M8-01`）、[DEC-020](../decisions/DEC-020-workflow-run-lifecycle.md)（Run 生命周期/映射/
策略，`M8-02`）、[DEC-021](../decisions/DEC-021-workflow-tool-channel.md)（操作 Tool 通道，
`M8-03`）、[DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)（对话 patch 语义，
`M8-04`）与 [Workflow Runtime 设计](../design/workflow_runtime_design.md)（`M8-05`）。
暂定默认值均已注明负责人与最迟冻结里程碑（阶段 B 里程碑）。
2026-09-08：契约实现与本地验证（Ubuntu 24.04，x86_64，g++ 13.3.0，CMake 3.28.3，Unix
Makefiles；本机无 clang/clang-tidy，由 PR CI quality job 补验；`clang-format` 使用
miniconda 发行版）。

- 实现：`include/mira/workflow_{ir,run,versioning,events,tools}.hpp` 与
  `src/workflow/*.cpp`（CMake 目标 `Mira::workflow`，仅依赖 `Mira::core`）；
  `core_contracts.hpp` 新增 `WorkflowId/WorkflowRunId/WorkflowPatchId/
  WorkflowDecisionId`。
- 新增测试：`mira_m8_ir_test`（往返保真、未知字段/版本不匹配/深度与容量 fail closed、
  绑定确定性错误码、参数引用、谓词求值、控制流与恢复钩子结构校验、digest 内容寻址）、
  `mira_m8_run_test`（转换表全枚举、终态幂等、迟到完成 Stale、Run↔Task 映射、策略门禁）、
  `mira_m8_versioning_test`（只追加与链式校验、digest 钉住创建时版本、仅验证版本可运行）、
  `mira_m8_events_test`（十类载荷往返、fail closed、脱敏断言、OfflineReplay 重建无副作用）、
  `mira_m8_tools_test`（五操作 schema 子集与保留名检查、正负校验、patch 封闭语义、幂等键、
  错误信封）。
- 安装包：`Mira::workflow` 进入 `install(TARGETS ... EXPORT MiraTargets)`；
  `mira_installed_consumer` 扩展 Workflow 契约用例（独立包含、链接、解析、绑定、校验）；
  `mira_public_headers_test` 覆盖五个新头。
- 结果：Release/Debug/ASAN/UBSAN 各 51/51 通过；TSAN（`setarch x86_64 -R`，mbedtls
  portable 按配置禁用）50/50 通过；`format-check`、`docs-check`、`sbom-check`、
  `platform-boundary-check` 通过。
- 限制：Windows/Android 构建组合与 clang-tidy 由 PR CI 补验后随 `M8-13` 回填；阶段 B
  执行闭环不在本轮（本里程碑范围外）。
- 同步：DEC-019..022、`workflow_runtime_design`、API 手册（index/workflow-contracts）、
  总计划（§4 状态、§5 决策索引）。

2026-09-08：PR [#29](https://github.com/Linductor-alkaid/mira/pull/29) CI 全绿（head
`2f7a095`，push pipeline runs
[`34179512874`](https://github.com/Linductor-alkaid/mira/actions/runs/34179512874)、
[`34179515463`](https://github.com/Linductor-alkaid/mira/actions/runs/34179515463)）：Linux
GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android arm64-v8a 与 x86_64
（NDK）、ASAN/UBSAN/TSAN 与 quality（clang-tidy 18 + clang-format + docs/sbom/
platform-boundary 检查）全部 24 项通过，补齐本机缺失的 clang-tidy 与跨平台验证。首轮
quality 在 `workflow_run.cpp` 报 3 处 `bugprone-branch-clone`（Paused/WaitingUser 同型
分支），合并 case 标签后复验通过；语义不变（转换表、映射与终态幂等测试全绿）。`M8-01`
至 `M8-13` 全部完成，退出条件逐项满足，本里程碑关闭（Completed）。阶段 B 里程碑可依据
[workflow_runtime_design](../design/workflow_runtime_design.md) 创建并进入 `Planned`。

2026-09-09：阶段 F 后状态审计重新打开 `M8-13` 与跨平台退出条件，
里程碑恢复 `In Progress`。`BUG-20260909-001`：Android CI 显式目标列表未包含
`mira_workflow`，且已有 consumer 不依赖该目标，历史 job 全绿不足以支撑本模块
Android 构建声明。已交付功能项及其他历史证据保留；这是验收覆盖缺口，尚无 Android
编译失败证据。负责人 Mira Maintainers；按
[后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-22` 补齐两 ABI
实际编译与安装消费链接证据后，逐项复核并关闭。本轮本地构建因未初始化子模块未能
复跑，环境与补跑条件见后续计划第 5 节。
