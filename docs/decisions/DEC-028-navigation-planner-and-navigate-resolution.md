# DEC-028：Navigation Planner 与 Navigate 步骤解析（阶段 E）

> 状态：Accepted
> 日期：2026-09-09
> 负责人：Mira Maintainers
> 冻结里程碑：[M12](../plans/m12-app-model-and-navigation.md)
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §9.3–§9.5
> 「Navigation Planner、导航代价、GUI Mapping」与 M9 阶段 E 占位
  （`navigate-unresolvable`）的冻结）

## 背景与问题

[DEC-027](DEC-027-app-model-contract-and-confidence.md) 冻结了 UI 状态图契约与置信度，
但没有回答「怎么从当前状态到目标状态」：

- 导航搜索（BFS/Dijkstra/A* 候选，DEC-014 §9.3）没有算法、代价模型与确定性语义；
  代价权重是配置而非硬编码（§9.4）的要求需要落地形式。
- Navigate 步骤（IR 既有，`arguments["target"]`）在派发策略下准入即拒；M9 留下的
  阶段 E 挂钩需要闭合：规划 → 边动作派发 → 到达验证 → 置信度回写的完整语义。
- `screen_state:<name>` 谓词求值（设计 §4.1 注记）需要绑定到宿主供给的当前状态
  （DEC-027 §3 的 Provider）。
- 探索升级（搜索失败按策略升级到 Agent，DEC-014 §9.3）与 GUI Mapping 循环属 Agent
  编排侧，本决策只冻结数据面与运行时语义。

## 决策

### 1. 代价向量、CostProfile 与规划器（纯函数）

每条迁移边的代价向量（`NavigationCosts`，随 [DEC-027](DEC-027-app-model-contract-and-confidence.md)
契约校验）：

```text
latency_ms >= 0            估计耗时（无实时性声明，RULE-10）
failure_probability [0,1]  估计失败率
risk [0,1]                 风险权重（宿主标注，如不可逆操作）
energy [0,1]               能耗权重
model_cost >= 0            模型调用成本估计
agent_required bool        边需要 Agent 探索（true 时工作流导航不可用该边）
vision_required bool       边需要视觉（v1 仅标注，不触发任何 Core 行为）
```

`NavigationCostProfile`（权重集合，全非负）：`cost(edge) = w_latency·latency_ms +
w_failure·failure_probability + w_model·model_cost + w_risk·risk + w_energy·energy`。
权重是配置（Runtime 配置缺省全 1），不得硬编码进规划器；目标平台校准前不宣称任何
路径最优性或性能（`RULE-10`）。

`plan_navigation(model, from, to, profile, context, options)` 是**纯函数**（Dijkstra，
`allow_agent_edges=false` 缺省）：

- **确定性**：同输入同输出；等代价路径按状态 ID 字典序打破平局（路径比较逐边按
  `(cost, edge_id, to_state)` 序）。
- **guard 语义**：边 guard 以调用方传入的谓词上下文（与运行时同一信号封闭集：
  `run_parameter:*`、`step_result:*`、`screen_state:<name>`）求值；`NotSatisfied` 记
  `guards_blocked`，`NotEvaluable` 记 `guards_unevaluable`——两者都使边不可用，但
  分开计数（fail closed 且诚实披露，`RULE-10`）。无 guard 的边恒可用。
- **预算（`RULE-08`）**：`max_edge_evaluations`（缺省 4096）与 `max_path_edges`
  （缺省 64）超限即 `nav-budget-exceeded`，不是无限搜索。
- **确定性错误码**（`nav-*` 家族）：`nav-unknown-from-state`、`nav-unknown-to-state`
  （任一端不在图中）、`nav-same-state`（from == to，空路径合法返回）、
  `nav-no-path`、`nav-budget-exceeded`。
- 结果 `NavigationPlan`：有序边 ID 与状态序列、总代价、`guards_blocked`/
  `guards_unevaluable` 计数；无路径时错误可分辨「图不连通」与「guard 全挡」。

### 2. `screen_state` 谓词绑定（设计 §4.1 兑现）

Runtime 在谓词求值时把宿主快照并入上下文：

- 快照存在时注入两个条目：`screen_state:<state_id>` → `true`（仅当前状态这个键），
  以及 `screen_state:current` → 当前状态 ID 字符串。
- 由此 `{"signal":"screen_state:chat","op":"eq","value":true}` 与
  `{"signal":"screen_state:chat","op":"exists"}` 都按「当前是否处于 chat」求值；
  其他状态名不在上下文中，`eq` 为 `NotEvaluable`（fail closed 不变）。
- Provider 未安装或返回空：不注入任何条目，全部 `screen_state` 谓词维持
  `NotEvaluable`（M8 以来的行为，DryRun 与 Strict 同样 fail closed）。谓词语义本身
  （`evaluate_workflow_predicate`）零改动。

### 3. Navigate 步骤解析与执行（Runtime 语义）

**准入**：派发策略下含 Navigate 步骤的定义，当且仅当导航上下文（App Model + Screen
State Provider，`WorkflowRuntime::set_navigation_context`）已安装时通过准入；未安装
维持 `navigate-unresolvable` fail closed（M9 语义不变）。policy patch 切入派发策略的
门禁同条件放松。DryRun 不要求导航上下文（形状规划语义不变，见下）。

**执行（派发策略，每 Navigate 步）**：

```text
1. 读屏：Provider 快照；无快照 -> 步失败 navigate-no-screen-state（fail closed）
2. 解析目标：resolved arguments["target"]（参数绑定/patch 后）必须是模型已声明状态
   （nav-unknown-to-state 语义 -> navigate-target-unknown）
3. 规划：plan_navigation（配置 CostProfile；run 谓词上下文 + screen_state 并入）
   失败 -> 步失败，原因码透出（navigate-no-path / navigate-budget-exceeded / ...）
4. 逐边执行（上界 = 计划边数）：
   a. 派发边动作：action 经工具注册表派发（与 ToolCall 步骤同一通道、同一
      schema 校验（保留成员 "tool"）、同一 submit_auto 路由与取消探针；每次派发
      计入 run 的 step 执行预算）
   b. 到达验证（W-02，不可跳过）：重读 Provider 快照；state == edge.to_state 即
      到达；否则步失败 navigate-arrival-unverified（禁止盲目重发边动作，RULE-05；
      恢复钩子照常适用）
5. 步级 verification 谓词（如声明）在到达目标状态后求值（含 screen_state 信号）
6. 全边到达 -> 步 Completed；发布 WorkflowNavigationPlanned 与逐边
   WorkflowNavigationObserved（成功）事件
```

**置信度回写**：每条边按到达成败以 [DEC-027](DEC-027-app-model-contract-and-confidence.md)
§2 纯函数更新 Runtime 持有的模型投影（调用线程、互斥下），成功与失败都发
`WorkflowNavigationObserved`；投影演化可从安装内容 + 事件序列重建（`W-03`）。
宿主可随时以 `set_app_model` 安装新内容（如衰减后模型），下次导航以新内容规划。

**DryRun**：不派发边动作。有导航上下文时执行真实规划（步骤 1–3）并以
`WorkflowNavigationPlanned` 留痕——规划失败即步失败（规划无副作用，诚实失败是对的，
且让 `publish_validated` 门禁对导航可达性有约束力）；无导航上下文时维持 M9 的
形状规划结算（`planned navigation`）。DryRun 不回写置信度（无真实观测，
`RULE-10`）。

**取消与暂停**：边动作派发复用步骤执行通道，取消探针、暂停安全点与 Stale 结算语义
照常；Navigate 步内被截断按 Stale 结算、游标不推进（M9 补注不变）。

**安全**：边动作与 ToolCall 同门禁——capability/freshness/SafetyPolicy 校验、脱敏
信封、`W-02` 验证义务一个不少；`agent_required` 边在工作流导航中不可用（规划器
`allow_agent_edges=false`），探索升级到 Agent 属 Agent 编排（DEC-014 §9.3），本决策
不实现自动升级。

### 4. 事件（v1 闭集扩展两员，State 类）

- `WorkflowNavigationPlanned`（schema `mira.workflow.navigation-planned.v1`）：
  run_id、step_id、from_state、to_state、edge_count、plan_digest（边 ID 序列规范化
  JSON 的 SHA-256）、total_cost、guards_blocked、guards_unevaluable。DryRun 与派发
  策略的规划都发。
- `WorkflowNavigationObserved`（schema `mira.workflow.navigation-observed.v1`）：
  run_id、step_id、transition_id、from_state、to_state、success、confidence（回写后
  边置信度）。仅派发策略真实派发后发。
- 载荷只含 ID、digest、枚举、机器数值与原因码（DEC-022 §5 脱敏规则；状态 ID 与边
  ID 是宿主契约标识符，不是用户文本）。OfflineReplay 识别两员并重建投影，无副作用。

### 5. Executor 路由

不新增任务类别：规划是纯函数（调用线程）；读屏是宿主回调（同步、无锁调用，宿主
保证非阻塞）；边动作派发复用步骤执行的 submit_auto 通道（预算计数）；置信度回写是
调用线程互斥下投影更新（与 `capture_trajectory` 同模式）。关闭顺序不变（M9 §5）。

## 备选方案

- **BFS（忽略代价）**：忽略 §9.4 的多维代价与策略差异化选路；Dijkstra 在同等实现
  复杂度下覆盖 BFS（权重全零时行为等价）。不采用。
- **A\* 启发式**：v1 没有可校准的启发函数（无目标平台度量，`RULE-10`），引入即
  引入不可验证的调参面。规划器保持无启发式的确定性 Dijkstra；升级留给证据。
- **Navigate 编译期展开为 ToolCall 步序列**：把图知识固化进 IR 会让 Workflow 绑定
  过期图版本（App 更新即全量失效，DEC-014 风险）；运行期按当前投影规划才能吃到
  置信度回写。不采用。
- **到达验证轮询等待（有界重试）**：宿主快照何时更新不受 Runtime 控制，轮询引入
  不可测时序；v1 单次读屏判定，未到达走失败/恢复钩子路径（确定性、可测）。
  放宽为有界等待留给真实宿主证据。
- **规划失败自动升级 Agent**：升级载体（`begin_task_recovery`/`WaitingAgent`）已
  存在，但「何时升级」是编排策略（DEC-014 §9.3「按策略升级」）；自动升级会把
  交互面悄悄变大。v1 经恢复钩子显式声明。不采用。

## 影响与风险

- `WorkflowRuntime` 公开面新增 `set_navigation_context`、`set_app_model`（重装投影）
  与 `screen_state_provider` 相关类型；配置新增 CostProfile、规划与置信度参数
  （缺省值为暂定默认值，见 DEC-027 影响节）。
- 读屏回调在驱动线程同步执行：宿主 Provider 阻塞会阻塞驱动（与工具派发同一约束，
  文档披露）；Provider 与外部状态的一致性由宿主负责。
- 边动作的副作用不确定性（派发成功但未到达）由「单次读屏 + navigate-arrival-
  unverified 失败」诚实暴露；恢复钩子若声明 Retry 重发边动作，与 ToolCall 步骤的
  `RULE-05` 语义一致（副作用不确定时禁止盲目重发由既有恢复语义约束）。
- 导航执行让一次 Navigate 步的耗时上界 = 边数 ×（工具派发 + 读屏）；预算由
  `max_step_executions_per_run` 与 `max_path_edges` 双重约束（`RULE-08`）。

## 验证方式

- 规划器矩阵：确定性（重复调用同结果）、权重影响路径选择、guard `NotSatisfied`/
  `NotEvaluable` 分计数、平局字典序、`agent_required` 边排除与 `allow_agent_edges`、
  预算超限、未知端点、同状态空路径、无路径 vs guard 全挡可分辨。
- Runtime 矩阵：未装上下文维持 `navigate-unresolvable`（M9 回归）；装上下文后准入
  通过、逐边派发与到达验证、`navigate-arrival-unverified` 失败与恢复钩子、
  `navigate-no-screen-state`、目标未声明、边动作走工具注册表（含工具缺失失败）、
  置信度回写与两员事件、DryRun 真实规划与失败即步失败、无上下文 DryRun 形状回归、
  step 预算计入、取消中 Navigate 步 Stale 结算、policy patch 切入派发策略的门禁
  放松。
- 谓词矩阵：`screen_state:<name>` eq/exists 求值、非当前状态 NotEvaluable、
  Provider 缺席全部 NotEvaluable（既有测试回归 + 新增）。
- 事件矩阵：两员载荷往返、未知字段 fail closed、DryRun 只发 Planned、
  OfflineReplay 无副作用。

## 关联文档和工作项

- [M12](../plans/m12-app-model-and-navigation.md)：工作项承载
- [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §9.3–§9.6、
  [DEC-027](DEC-027-app-model-contract-and-confidence.md)、
  [DEC-019](DEC-019-workflow-ir-contract.md)（谓词与参数绑定复用）、
  [DEC-020](DEC-020-workflow-run-lifecycle.md)（预算与恢复）、
  [DEC-021](DEC-021-workflow-tool-channel.md)（工具通道复用）
- [Workflow Runtime 设计](../design/workflow_runtime_design.md)（阶段 E 章节）
- [API 手册 workflow-contracts](../api/workflow-contracts.md)
