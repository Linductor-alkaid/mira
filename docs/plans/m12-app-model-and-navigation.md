# M12：App Model 与导航（阶段 E）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：[M9](m9-workflow-runtime-minimal-loop.md)（实现已交付；Android 验收重开；阶段 E 的直接前置是阶段 B，
> 阶段 C/D 的策略与轨迹语义是输入）
> 建议发布点：Workflow navigation alpha
> 更新日期：2026-09-09

> 2026-09-09 状态复核：实现已交付；`BUG-20260909-001` 发现 Android CI 未构建
> `mira_workflow`，本里程碑的矩阵取证项及相关退出条件重新打开。历史验证记录保留，
> 当前状态以本注记为准；依赖方可复用冻结契约，跨平台关闭共同等待
> [阶段 F 后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-22`。

## 1. 目标

依据 [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 E、
[DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md) 与
[DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md)，交付
Workflow 数据平面的环境知识端：App Model 契约（UI 状态图：层次状态节点、迁移边、
guard、代价向量、置信度记录）、确定性置信度更新（观测、衰减、再探索标记）、导航
规划器（多维代价 Dijkstra、权重配置、guard 语义、预算）与 Runtime 集成（导航上下文
安装、Navigate 步骤解析与逐边执行、`screen_state` 谓词绑定、置信度回写与两员导航
事件），使 Workflow 的 Navigate 步骤从「准入即拒」变为「可解析、可执行、可验证、
可回写」（`W-02`/`W-03`），并兑现设计 §4.1 的 `screen_state` 谓词承诺。感知能力
（识别本身）按 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md) 保持
在宿主侧。

选择新增 M12 而非扩展 M11 的理由：M11 已按其退出条件关闭（资产生产端）；App Model
与导航是独立能力增量（环境知识端 vs 经验编译端），验收证据（契约/置信度/规划器/
导航执行矩阵）与 M11 测试不同源，按规范使用新里程碑文件承载。不对阶段 F 预分配编号。

## 2. 范围与非目标

### 2.1 范围

- App Model 契约：`AppModel`（状态节点 + 迁移边 + 代价向量 + 置信度）、
  `AppModelLimits`、JSON 往返、fail-closed 校验（`validate_app_model` 与解码同源）、
  内容寻址 digest。
- 置信度：`ConfidenceRecord`（confidence/observed_at/last_verified/verified_count/
  failure_count/source 封闭集）与纯函数更新（`note_transition_outcome`、
  `apply_confidence_decay`、`needs_exploration`）；时间由调用方传入、同输入同输出。
- 导航规划器：`plan_navigation`（确定性 Dijkstra、`NavigationCostProfile` 权重、
  guard 分计数、字典序平局、`agent_required` 边排除、`max_edge_evaluations`/
  `max_path_edges` 预算、`nav-*` 错误码、计划 digest）。
- Runtime 集成：`set_navigation_context`/`set_app_model`；Navigate 步准入条件（有
  上下文才放行派发策略）、执行（读屏 -> 目标校验 -> 规划 -> 逐边工具派发 -> 到达
  验证 -> 步级谓词）、置信度回写；`navigate-*` 运行时错误码；policy patch 门禁同
  条件放松。
- `screen_state` 谓词绑定：快照注入（当前状态 + `screen_state:current`）、缺席全
  `NotEvaluable`（回归锁定）。
- 事件：`WorkflowNavigationPlanned/Observed` 两员入 v1 闭集（构建/解析、fail
  closed、DryRun 只发 Planned、离线回放无副作用）。
- 端到端取证：带 Navigate 步的 Workflow 在导航上下文下完整运行（含失败边置信度
  回写与再规划、DryRun 真实规划入 publish 门禁）；installed-consumer 覆盖新公共面。

### 2.2 非目标

- UI 状态识别（Accessibility/OCR/CV/VLM）与 §9.6 感知层级本体（DEC-011：宿主/deno
  侧证据决定回归；Core 只消费快照）。
- 探索循环（Agent Mapper 编排）、规划失败的自动 Agent 升级（经恢复钩子显式声明）。
- Verify 步骤更高级证据层（screen diff/本地感知/VLM；M10 §11.4 推迟维持）。
- 多设备/多环境命名空间、图自动失效检测、Memory 四类组织与学习闭环（阶段 F）。
- App Model 持久化与跨进程重建（`RISK-2026-038` 沿袭推迟；导航事件为重建留痕）。
- 代价权重/置信度参数（半衰期、探索阈值）的目标平台校准（`RULE-10`；缺省值为
  暂定默认值）。
- A* 启发式、到达验证的有界等待轮询（DEC-028 备选已拒绝；升级留证据）。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M9 `Completed`（阶段 B 冻结：执行闭环、谓词求值、预算与恢复语义；Navigate 的
  阶段 E 挂钩即 `navigate-unresolvable` 占位）；M10/M11 `Completed`（策略全集与
  轨迹语义是输入）。PR #30/#31/#32 CI 全绿。
- 本里程碑经维护者评审由 `Proposed` 转 `Planned`（用户指示依设计与计划推进下一步
  开发，与 M8–M11 同一授权模式）；专项设计与决策（DEC-027/028、设计 v0.5 §13）
  随本里程碑创建并冻结。

### 3.2 设计与决策依据

- [Workflow Runtime 设计](../design/workflow_runtime_design.md) §13（阶段 E 实施规范）
- [DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)、
  [DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md)
- [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) §9/§16、
  [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)、
  [DEC-019](../decisions/DEC-019-workflow-ir-contract.md) 至
  [DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md)
- [核心公共契约与状态机](../design/core_contracts_and_state_machine.md)、
  [Mira Runtime 设计](../design/mira_runtime_design.md)

## 4. 工作项

### 4.1 App Model 契约与置信度

- [x] `M12-01` 冻结并实现 `AppModel` 契约与置信度纯函数：JSON 往返、fail-closed
  校验（悬垂引用、重复 ID、上限、未知字段）、内容寻址 digest；成功/失败更新、
  指数衰减（单调不增、Δt<=0 不变）、再探索阈值判定；source 封闭集。

### 4.2 导航规划器

- [x] `M12-02` 实现 `plan_navigation` 纯函数：确定性 Dijkstra（权重线性和、字典序
  平局）、guard `NotSatisfied`/`NotEvaluable` 分计数且均不可用、`agent_required`
  边排除（`allow_agent_edges` 宿主保留口）、双预算、`nav-*` 错误码、计划 digest、
  无路径 vs guard 全挡可分辨。

### 4.3 Runtime 集成

- [x] `M12-03` 实现导航上下文与 Navigate 步骤解析：`set_navigation_context`/
  `set_app_model`；准入条件（无上下文维持 `navigate-unresolvable`，M9 回归）、
  逐边工具派发（`"tool"` 保留成员、注册表校验、step 预算计入）、每边到达验证
  （`navigate-arrival-unverified` 禁盲目重发、恢复钩子照常）、`navigate-no-screen-state`、
  `navigate-target-unknown`；policy patch 切入派发策略门禁放松。
- [x] `M12-04` 实现 `screen_state` 谓词绑定与置信度回写：快照注入（当前状态 +
  `screen_state:current`）、Provider 缺席全 `NotEvaluable`（回归）；逐边成败回写
  投影（纯函数、互斥下更新、事件锁外发射）；DryRun 真实规划（失败即步失败、不发
  Observed、不回写），无上下文 DryRun 维持形状规划。
- [x] `M12-05` 实现 `WorkflowNavigationPlanned/Observed` 两员事件（构建/解析/闭集
  扩展）与审计序列；DryRun 只发 Planned；离线回放无副作用。

### 4.4 端到端与取证

- [x] `M12-06` 端到端取证：导航上下文下含 Navigate 步的 Workflow 完整运行（多边
  路径、失败边置信度回写与重规划、恢复钩子路径、取消 Stale）；`publish_validated`
  门禁对含 Navigate 定义的行为（有上下文 DryRun 规划约束）；installed-consumer
  用例。
- [ ] `M12-07` 测试矩阵取证与文档同步：本机 Release/Debug/ASAN/UBSAN（TSAN 按环境
  限制记录补跑条件）、quality 门禁、installed-consumer；总计划、设计文档（v0.5）、
  API 手册与 DEC-019 注记同步后关闭里程碑。

## 5. Executor 路由与关闭表（细化设计 §7/§13）

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| 导航规划（plan_navigation） | 调用线程同步（纯函数） | 无 | 无 |
| 读屏（ScreenStateProvider） | 驱动/调用线程同步回调 | 宿主 | 宿主保证非阻塞；缺席 fail closed |
| 导航边动作派发 | `submit_auto`（复用步骤执行通道） | 驱动任务 | 计入 Run 步预算；异常转步骤失败 |
| 置信度回写 | 调用线程（互斥下投影更新） | 无 | 纯函数；事件锁外发射 |

理由：阶段 E 不引入新任务类别；边动作与 ToolCall 步骤同一通道（同一取消探针、
deadline 与预算计数）。关闭顺序不变（M9 §5）：`WorkflowRuntime::shutdown()` ->
`MiraRuntime` 关闭 -> 宿主非 worker 线程 `executor.shutdown(true)`。

## 6. 风险与阻塞

- `RISK-2026-047`：读屏回调在驱动线程同步执行，宿主 Provider 阻塞即阻塞驱动（与
  工具派发同一约束）。Owner：Mira Maintainers。缓解：DEC-027 §3 契约披露 + API
  手册注明 + 测试用非阻塞 Provider。
- `RISK-2026-048`：小样本置信度不可靠（与 `RISK-2026-046` 同族）；缺省半衰期/阈值
  未经目标平台校准（`RULE-10`）。Owner：Mira Maintainers。缓解：暂定默认值标注 +
  文档披露；校准随 demo 证据。
- `RISK-2026-039`（沿袭并部分收敛）：`screen_state` 谓词层落地后，验证分层扩展到
  receipt/结构化谓词 + 宿主快照状态；screen diff/本地感知/VLM 层仍推迟。
- `RISK-2026-038`（沿袭）：App Model 为进程内投影；导航事件为重建留事件基础，
  持久化载体仍推迟。

## 7. 测试与退出条件

- [x] `M12-01` 至 `M12-06` 全部完成并有可复现验证记录。
- [x] 契约矩阵：JSON 往返无损；未知字段/悬垂引用/重复 ID/空串/上限 fail closed；
  `validate_app_model` 与解码同源；digest 确定性（同内容两次相等、改动即变）；
  source 封闭集负向。
- [x] 置信度矩阵：成功/失败更新计数与数值逐值断言；衰减单调不增、`Δt<=0` 不变、
  计数与时间戳不被衰减改动；`needs_exploration` 阈值边界；纯函数性（重复调用同
  输出）。
- [x] 规划器矩阵：确定性（重复调用同结果）；权重改变改变路径选择；guard
  `NotSatisfied`/`NotEvaluable` 分计数且边不可用；等代价字典序平局；
  `agent_required` 排除与 `allow_agent_edges`；预算超限 `nav-budget-exceeded`；
  未知端点；from == to 空路径；无路径 vs guard 全挡可分辨。
- [x] Runtime 导航矩阵：无上下文 `navigate-unresolvable` 准入拒绝（M9 回归）与
  policy patch 门禁拒绝回归；有上下文准入放行；逐边派发与到达验证成功路径；
  `navigate-arrival-unverified` 失败与恢复钩子（Retry/FallbackStep/升级）；
  `navigate-no-screen-state`；目标未声明 `navigate-target-unknown`；边动作工具
  缺失/校验失败；step 预算计入；执行中取消 Navigate 步 Stale 结算；置信度回写
  （成功/失败逐值）与投影再规划吃到回写。
- [x] 谓词矩阵：`screen_state:<name>` eq/exists 求值；非当前状态 NotEvaluable；
  Provider 缺席全部 NotEvaluable（M8/M9 回归）；DryRun 完成的 unevaluable 计数
  行为（有快照后可求值路径）。
- [x] DryRun 矩阵：有上下文真实规划（Planned 事件、规划失败即步失败、无 Observed、
  无回写）；无上下文形状规划回归；`publish_validated` 对含 Navigate 定义的门禁
  行为。
- [x] 事件矩阵：两员载荷往返、未知字段 fail closed、DryRun 只发 Planned、闭集
  扩展回归；离线回放无副作用。
- [ ] 门禁：ASAN/UBSAN 全绿；TSAN 在本机限制下按 M9–M11 模式取证；quality
  （clang-format、docs、sbom、platform-boundary）通过；Windows/Android 构建组合与
  clang-tidy 由 PR CI 补验全绿；installed-consumer 覆盖新公共面。
- [x] 总计划第 4/5 节、`workflow_runtime_design` v0.5、API 手册、DEC-019 注记与
  本文件同步。

## 8. 验证记录

2026-09-09：依据 M11 退出条件与总计划阶段 E 规则创建本里程碑（`Proposed`）：先完成
专项决策（DEC-027、DEC-028）与设计更新（`workflow_runtime_design` v0.5 §13），再
创建本文件。同日经维护者评审（用户指示依设计与计划推进下一步开发，与 M8–M11 同一
授权模式）转 `Planned` 并进入实施（`In Progress`）。

2026-09-09：实现与本地验证（Ubuntu 24.04，x86_64，g++ 13.3.0，CMake 3.28.3，Unix
Makefiles；本机无 clang/clang-tidy，由 PR CI quality job 补验；`clang-format` 使用
miniconda 发行版但本机 format 结论以 PR CI 为准）。

- 实现：`workflow_navigation.hpp/cpp`（`AppModel` 契约与 JSON 往返、`ConfidenceRecord`
  与三个纯更新函数、`NavigationCostProfile`/`NavigationCosts`、确定性 Dijkstra
  `plan_navigation`（字典序平局、guard 分计数、双预算、`nav-*` 错误码、计划
  digest）、`ScreenStateProvider` 边界类型；`AppModelError`/`NavigationError` 确定性
  错误码）；`workflow_events` 新增导航两员（构建/解析/闭集扩展）；`WorkflowRuntime`
  新增 `set_navigation_context`/`set_app_model`/`app_model_snapshot` 与导航配置，
  Navigate 准入条件化（无上下文维持 M9 拒绝）、`execute_navigate_step`（读屏 → 目标
  校验 → 规划 → 逐边工具派发（复用 `dispatch_tool_invocation` 共享通道）→ 到达
  验证 → 置信度回写）、谓词求值统一经 `evaluation_context`（screen_state 注入），
  patch 管线维护 Navigate 生效实参与策略门禁放松；`capture_trajectory` 自此取到
  Navigate 步的生效实参（resolved_arguments 维护补全）。
- 实现中的语义修正（相对决策草案，随本轮冻结）：`set_navigation_context`/
  `set_app_model` 以 `Result<void>` 返回校验失败（非异常）；Navigate 目标形状
  （非空字符串 `target`）在准入时对**所有策略**校验（与 patch 平面的既有形状规则
  同源）；规划后模型被重装导致计划边消失时以 `NavigatePlanFailed` fail closed
  （`planned edge ... no longer present`），不派发陈旧动作。
- 新增测试：`mira_m12_contract_test`（往返/悬垂引用/重复 ID/上限/版本/digest、source
  闭集、置信度逐值与纯函数性、权重选路/字典序平局/guard 分计数/agent 边/预算/端点/
  权重校验）、`mira_m12_navigation_test`（无上下文准入与 policy 门禁回归、逐边执行
  与到达、stuck 边回写失败置信度、无读屏/未声明目标/未注册工具、screen_state 谓词
  eq/exists/alias/缺席回归、DryRun 真实规划与不可达失败、Interactive 停决策点后
  策略切换放松、边预算计入、衰减重装再规划、暂停 Stale 与 resume 重规划、publish
  门禁对导航可达性的约束）、`mira_m12_events_test`（两员往返与 fail closed、DryRun
  只发 Planned、失败边 Observed 置信度、离线回放无副作用）。
- 安装包：`mira_installed_consumer` 增加 M12 用例（构造模型 → 安装上下文 → 导航
  执行 → 投影置信度断言 → 规划器与置信度纯函数直接使用）。
- 结果：Release/Debug/ASAN/UBSAN 各 63/63 通过；TSAN（`setarch x86_64 -R`）62/62
  通过；`docs-check`、`sbom-check`、`platform-boundary-check`、`consumer-check`
  通过。
- 限制：Windows/Android 构建组合与 clang-tidy、format-check 由 PR CI 补验后随
  `M12-07` 回填（见下条）；阶段 F 不在本轮（范围外）。
- 同步：DEC-027/028（本轮冻结）、`workflow_runtime_design`（v0.5 §13 与路由/事件/
  模块/测试表）、API 手册（workflow-contracts 增 M12 节与兼容性更新）、总计划
  （§4/§5）、本文件。

2026-09-09：PR [#33](https://github.com/Linductor-alkaid/mira/pull/33) CI 全绿（head
`cc569c9`，push pipeline run
[`34277090112`](https://github.com/Linductor-alkaid/mira/actions/runs/34277090112)、
pull_request pipeline run
[`34277095666`](https://github.com/Linductor-alkaid/mira/actions/runs/34277095666)）：
Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android arm64-v8a 与
x86_64（NDK）、ASAN/UBSAN/TSAN 与 quality（clang-tidy 18 + clang-format + docs/sbom/
platform-boundary 检查）全部 24 项通过，补齐本机缺失的 clang-tidy、format 与跨平台
验证。前两轮 quality 各报 1 处 clang-tidy 违例——`workflow_runtime.cpp:1906`
`performance-inefficient-string-concatenation`（边循环内 call_id 链式拼接，改 `+=`
构造并顺带处理循环内其余诊断串）与 `workflow_navigation.cpp:530`
`bugprone-branch-clone`（`AppModelError` 映射两个连续相同分支合并，与 PR #29 同
模式）——修复后复验全绿；语义不变（本地 m8–m12 套件复跑通过）。PR 已合并（merge
`1b8692b`）。`M12-01` 至 `M12-07` 全部完成，退出条件逐项满足，本里程碑关闭
（`Completed`）。阶段 F（Memory 与学习闭环，前置 D/E）里程碑可依据
`agent_harness_and_workflow_architecture` §10/§16 与 `workflow_runtime_design` 创建并
进入 `Planned`（先专项设计与决策，不预分配编号）。

2026-09-09：阶段 F 后状态审计重新打开 `M12-07` 与跨平台退出条件，
里程碑恢复 `In Progress`。`BUG-20260909-001`：Android CI 显式目标列表未包含
`mira_workflow`，且已有 consumer 不依赖该目标，历史 job 全绿不足以支撑本模块
Android 构建声明。已交付功能项及其他历史证据保留；这是验收覆盖缺口，尚无 Android
编译失败证据。负责人 Mira Maintainers；按
[后续计划](maintenance-2026-09-post-stage-f.md) `MNT-202609-22` 补齐两 ABI
实际编译与安装消费链接证据后，逐项复核并关闭。本轮本地构建因未初始化子模块未能
复跑，环境与补跑条件见后续计划第 5 节。
