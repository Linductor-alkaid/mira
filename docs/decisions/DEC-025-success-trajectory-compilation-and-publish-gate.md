# DEC-025：成功轨迹契约、Workflow 编译与入库门禁（阶段 D）

> 状态：Accepted
> 日期：2026-09-09
> 负责人：Mira Maintainers
> 冻结里程碑：[M11](../plans/m11-trajectory-compilation-and-task-induction.md)
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) 阶段 D
> 「成功轨迹 -> Workflow 编译、任务归纳、版本化」的前半冻结；版本化本体随
> [DEC-020](DEC-020-workflow-run-lifecycle.md) §4 于 M8 交付，本决策补全其入库证据链）

## 背景与问题

DEC-014 确立双路径架构：Agent 成功经验沉淀为版本化 Workflow 资产。M8–M10 交付了 IR
契约、执行闭环、介入与策略全集，但资产的生产端仍缺位：`publish_workflow` 只接受宿主
手工构造的定义并以 `NotValidated` 缺省入库，`DryRunPassed`/`Validated` 证据没有生成
管线；「成功轨迹」没有契约形式，运行中 patch 修复后的有效状态无法固化为新版本；
`W-04`「入库前必须通过验证（含 Dry Run）」停留在版本记录的可选字段层面。

阶段 D 需要冻结三件事：(1) 成功轨迹的契约形式与采集规则；(2) 轨迹到
`WorkflowDefinition` 的确定性编译语义；(3) 编译产物进入 Workflow Library 的门禁管线
（DryRun 证据、幂等、回退与审计事件）。任务归纳（多轨迹参数化）由
[DEC-026](DEC-026-task-induction-and-parameterization.md) 冻结，复用本决策的轨迹契约
与入库门禁。

## 决策

### 1. 成功轨迹契约（`WorkflowTrajectory`）

轨迹是一次**真实成功执行**的结构化快照，作为编译输入的公共契约（版本化公开类型，
随 `workflow_compiler.hpp` 交付）：

```text
WorkflowTrajectory
├── workflow_id / source_digest / source_run_id     溯源：来源 Workflow、钉住版本、来源 Run
├── effective_parameters                             采集时点的有效参数表（patch 后）
├── effective_policy                                 采集时点的有效策略
└── steps[]                                          生效步骤序列（见下）
```

每个 `WorkflowTrajectoryStep` 携带：来源步骤 ID、名称、种类（`ToolCall`/`Navigate`/
`Verify`/`Control`）、**生效实参**（参数绑定 + 运行中 step_arguments 覆盖后的解析值，
不含 `$param` 引用）、**原始实参**（含 `$param` 引用，作为归纳溯源的 provenance）、
谓词与恢复钩子、循环标注与预算。

采集规则（`WorkflowRuntime::capture_trajectory(run_id)`）：

- **只采集 `Completed` 且策略实际派发副作用的 Run**（`workflow_policy_dispatches_
  side_effects` 为真）。DryRun 完成的 Run 从未真实执行，编译它将违反 `RULE-10` 诚实
  声明（`capture-not-executed` fail closed）；非终态、`Failed`/`Cancelled`、未知 Run
  同样拒绝。
- 生效步骤 = 钉住定义的步骤序列 **减去跳过集**（`Skipped` 步骤未产生贡献，不进入编译
  产物）；每步实参取 patch 平面维护的生效值（覆盖优先于创建时解析值）。
- 有效参数表与有效策略取 Run 记录的当前快照（含全部已应用 patch 的累积效果）。
- Control 步的 `jump_to` 指向被跳过步骤时，编译期而非采集期 fail closed（§2）。

除 Run 采集外，宿主可**直接构造**轨迹（Agent 工具调用序列路径：harness 把成功任务
的工具调用按序填入 steps，实参为字面量，无 provenance）。构造与编译共用同一契约；
Agent 侧的轨迹抽取编排（从事件流重建工具序列）不属本决策范围。

### 2. 编译语义（纯函数，fail closed）

`compile_workflow(trajectory, options) -> Result<WorkflowDefinition>`，无副作用、
无注册表访问、可在任意线程同步调用：

- **步骤映射**：种类、谓词、恢复钩子、循环标注与预算原样保留；生效实参作为字面量
  步骤实参。ToolCall 步生效实参必须是携带保留成员 `"tool"` 的对象
  （`compile-tool-binding-invalid`）。
- **默认值固化**（「本次 -> 默认」目标，DEC-022 §4 的定义级落地）：来源定义的参数
  规格全部保留，`default_value` 重置为轨迹有效参数表中观测到的值；原本 `required`
  的参数因获得默认值转为可选（契约要求默认值仅存在于可选参数）。无来源定义的宿主
  构造轨迹没有参数规格。
- **跳过步剔除**：被跳过步骤不进入编译产物；Control 步 `jump_to` 指向被剔除步骤时
  fail closed（`compile-jump-target-skipped`），不做隐式重定向。
- **策略**：`allowed_policies` 沿用来源定义集合；`default_policy` 取 options 指定值
  （缺省回落来源默认值），必须落在允许集内（`compile-policy-not-allowed`）。
- **目标身份**：options 指定目标 `workflow_id`/`name`/`summary`。同 ID 编译即版本
  链上的新版本（固化/归纳修复）；新 ID 编译即派生新 Workflow。
- 编译产物立即经 `validate_workflow_definition` 同源校验（与 JSON 解码同一套 fail
  closed 语义），任何结构违规以 `compile-*` 确定性错误码拒绝。
- **确定性**：相同轨迹 + 相同 options 产出逐字节相同的定义与 digest（归纳候选作为
  options 的一部分参与确定性）。

### 3. 入库门禁与发布（`WorkflowRuntime::publish_validated`）

编译产物（或宿主编辑后的草稿）进入 Workflow Library 的**唯一门禁化路径**：

```text
publish_validated(definition, actor, reason, source_run_id?)
  1. 结构校验（validate_workflow_definition，fail closed）
  2. DryRun 门禁：以空参数创建 Run（策略 DryRun）→ 调用线程同步驱动（复用 execute_run
     宿主同步路径）→ 要求终态 Completed；否则 fail closed（publish-dryrun-failed），
     库不发生任何变更
  3. 证据摘要：对 {workflow_id, ir_digest, 逐步结算 (step_id, disposition),
     unevaluable_verifications} 的规范化 JSON 取 SHA-256。摘要因此是**内容派生**的
     （同定义 + 同 DryRun 结算序列 => 同证据，§4 的幂等 NoOp 依赖该性质）；源 Run 与
     门禁 Run 的绑定经事件审计承载（`WorkflowPublishProposed` 的 source_run_id、
     `WorkflowPublishApplied` 的 dry_run_id）。unevaluable 计数如实入摘要（DryRun 是
     结构门禁，执行证据在源 Run；两者共同构成 W-04 证据链）
  4. 幂等：若当前 head 记录的 content_digest 与产物相同且同为 DryRunPassed、证据
     相同，则 NoOp 返回既有 digest（不追加记录）；非 head 的历史同容重复仍按新版本
     追加（版本单调递增，历史不可变）
  5. 追加版本记录：validation=DryRunPassed + 证据 digest，经 append_workflow_version
     的全部既有校验（版本递增、parent 链、证据形状）
```

- **回退语义（归纳回退的库侧不变量）**：门禁失败的发布**不触碰库**——不追加
  `Rejected` 记录、不修改既有 head，`latest_runnable` 不变；失败仅以事件与错误结果
  可见。既有版本因此始终是可回退的稳定基线（DEC-014「验证失败回退」）。
- **审计事件（v1 事件闭集扩展三员，State 类）**：
  `WorkflowPublishProposed`（进入门禁：workflow_id、ir_digest、source_run_id）/
  `WorkflowPublishApplied`（version 落库：workflow_id、ir_digest、evidence、dry_run_id）/
  `WorkflowPublishRejected`（门禁或追加失败：ir_digest、机器可读 reason_code）。
  schema 为 `mira.workflow.publish-*.v1`，载荷只含 ID、digest 与原因码（DEC-022 §5
  脱敏规则）。`publish_workflow` 原始路径维持宿主信任边界语义，不发事件——门禁化路径
  才是模型侧/编排侧产物的入口，审计义务随之。
- **模型不可直达**：`publish_validated` 是宿主 API，不注册为模型工具。模型输出的
  任何定义草稿必须经宿主（或后续 Agent Harness 编排里程碑的受控通道）提交本门禁
  （`W-04`）。

### 4. Executor 路由

不新增任务类别：捕获是互斥下快照拷贝（调用线程）；编译是纯函数（调用线程）；门禁
驱动复用 `execute_run` 的宿主同步路径（调用线程），门禁 Run 占用 Run 表容量与普通
Run 相同（`RULE-08`）。关闭顺序不变（M9 §5）。

## 备选方案

- **编译产物直接以 `NotValidated` 入库、运行时再验证**：违反 `W-04`「入库前必须通过
  验证」的字面与精神，且 `NotValidated` 版本不可被 Run 引用，库会积累不可用记录。
  不采用。
- **门禁失败追加 `Rejected` 版本记录**：版本历史承载失败尝试会稀释「版本=可用内容」
  的语义，且失败记录本身需要证据形状豁免；审计诉求由事件承载已足够。不采用。
- **允许编译 DryRun 完成的 Run（复用规划结果）**：DryRun Run 从未派发副作用，其
  「成功」不是执行证据；编译它等于把规划冒充经验（`RULE-10`）。不采用。
- **在 IR 中新增「来源轨迹」字段携带溯源**：IR 是执行契约，溯源属于版本记录与事件
  载荷（source_run_id、证据 digest 已覆盖）；IR 加字段引入 reader 升级义务
  （DEC-019）。不采用。
- **捕获时展开循环为线性序列**：丢失循环结构会把「重复到达」固化为重复步骤，编译
  产物膨胀且语义退化为单次快照。保留 Control/loop_head 结构。不采用。

## 影响与风险

- `Mira::workflow` 新增 `workflow_compiler.hpp`（轨迹契约 + 编译纯函数）与
  `workflow_events.hpp` 三员事件；`WorkflowRuntime` 新增 `capture_trajectory` 与
  `publish_validated`。公开面扩大，installed-consumer 需覆盖。
- `WorkflowRunResult.unevaluable_verifications` 进入发布证据摘要：DryRun 对
  `step_result` 谓词必然不可求值（工具未派发），计数如实记录而非视作失败——该取舍
  已在 §3 证据设计显式化，文档披露。
- 门禁在调用线程同步驱动：宿主在持有自身关键锁的上下文内调用会自锁（与 `execute_run`
  同一约束，文档披露）。
- `RISK-2026-038`（沿袭）：库仍为进程内投影；publish 事件为未来跨进程重建提供了事件
  基础，但持久化载体仍推迟。

## 验证方式

- 捕获矩阵：Completed+派发 Run 的生效态快照（覆盖实参、跳过剔除、有效参数）；DryRun
  完成、非终态、Failed/Cancelled、未知 Run 的负向拒绝。
- 编译矩阵：默认值固化（required -> 可选+默认）、跳过步剔除、`jump_to` 指向剔除步
  fail closed、tool 绑定形状、策略门禁、确定性 digest（同输入两次编译 digest 相等）。
- 门禁矩阵：好草稿 -> `DryRunPassed` 入库 -> 新版本可 Run 且按固化默认值完成；坏
  草稿（DryRun 不达 Completed）-> 库不变、head 不变、`WorkflowPublishRejected`；
  幂等重放 NoOp；版本链 parent 正确；历史不可变回归（M8 既有）。
- 事件矩阵：三事件序列与载荷形状、未知字段 fail closed；OfflineReplay 对新事件类型
  无副作用（M8 既有回放负向测试扩展）。

## 关联文档和工作项

- [M11](../plans/m11-trajectory-compilation-and-task-induction.md)：工作项承载
- [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §8/§14/§16、
  [DEC-019](DEC-019-workflow-ir-contract.md)、
  [DEC-020](DEC-020-workflow-run-lifecycle.md) §4、
  [DEC-022](DEC-022-conversation-patch-semantics.md) §4/§5、
  [DEC-026](DEC-026-task-induction-and-parameterization.md)
- [Workflow Runtime 设计](../design/workflow_runtime_design.md)（阶段 D 章节）
- [API 手册 workflow-contracts](../api/workflow-contracts.md)
