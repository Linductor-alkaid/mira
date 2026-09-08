# DEC-027：App Model 契约与置信度（阶段 E）

> 状态：Accepted
> 日期：2026-09-09
> 负责人：Mira Maintainers
> 冻结里程碑：[M12](../plans/m12-app-model-and-navigation.md)
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §9
> 「App Model 与 GUI 导航」的前半冻结；感知层级（§9.6）按
> [DEC-011](DEC-011-demo-first-external-validation.md) 不在本决策范围）

## 背景与问题

DEC-014 把 GUI 应用建模为带标注的有向图（Node = UI State、Edge = UI Transition、
Guard = Preconditions、Effect = State Change），作为长期环境知识（记忆）而非任务私有
数据。M8–M11 交付了 Workflow 契约、执行闭环、介入与资产生产端，但环境侧知识仍是空白：

- Navigate 步骤在派发策略下于准入时 fail closed（`navigate-unresolvable`，M9 的阶段 E
  占位），Workflow 只能表达「到哪」而不能解析「怎么到」。
- `screen_state:<name>` 谓词自 M8 起保留在封闭信号集中但不可求值（设计 §4.1 注记
  「阶段 E 落地」），验证分层停留在 receipt/结构化谓词两层。
- 环境事实没有契约形式：没有 UI 状态图 schema、没有迁移观测的置信度语义，App 更新
  导致的图失效无从表达（DEC-014 风险「App Model 维护成本」）。

阶段 E 需要冻结三件事：(1) App Model 的版本化公共契约（状态节点、迁移边、guard、
代价向量、置信度记录）；(2) 置信度的确定性更新与衰减语义（观测、验证、失效、再探索
标记）；(3) 感知边界——UI 状态识别由宿主供给，Core 不做感知（DEC-011 边界的显式化）。
Navigation Planner 与 Navigate 步骤解析由
[DEC-028](DEC-028-navigation-planner-and-navigate-resolution.md) 冻结，复用本决策的
契约与置信度。

## 决策

### 1. App Model 契约（`AppModel`，版本化公开类型）

```text
AppModel
├── schema_version {major=1, minor=0}       版本化公共契约（DEC-002）
├── app_id / name / summary                  命名空间与描述（app_id 为非空字符串，
│                                            多设备/多环境命名空间是 DEC-014 开放问题，
│                                            v1 单命名空间，不承诺分隔规则）
├── states[]                                 UI 状态节点
│   ├── id                                   稳定字符串 ID（图内唯一、非空）
│   ├── page / modal? / context?             层次描述（§9.2：Page + Modal State +
│   │                                        Navigation Context；page 必填，其余可选）
│   ├── summary?                             有界描述（kWorkflowEventMaxSummaryBytes）
│   └── confidence                           置信度记录（§2）
└── transitions[]                            UI 迁移边
    ├── id                                   稳定字符串 ID（图内唯一、非空）
    ├── from_state / to_state                必须引用已声明状态（悬垂引用 fail closed）
    ├── action                               触发动作：JSON 对象，保留成员 "tool"
    │                                        （字符串 wire 名，与 ToolCall 步骤同一约定）
    ├── guard?                               WorkflowPredicate（v1 谓词 DSL 复用，
    │                                        信号封闭集不变）
    ├── costs                                代价向量（见 DEC-028 §1）
    └── confidence                           置信度记录（§2）
```

- **序列化与校验**：JSON 往返无损；未知字段、未知 schema 版本、悬垂引用、重复 ID、
  空串、越界数值与 `kAppModelLimits` 上限（文档 bytes、状态数、边数、字符串长度，
  `RULE-08`）全部 fail closed，语义与 `workflow_definition_from_json` 同源。
  `validate_app_model` 对结构构造的模型执行与 JSON 解码同一套校验。
- **内容寻址**：`app_model_digest(model)` 为规范化 JSON 的 SHA-256（与
  `workflow_definition_digest` 同一模式）；同内容同 digest，图更新即新内容。
- **effect 不设独立字段**：v1 中迁移的效果就是到达 `to_state`（到达判定见 DEC-028
  §3）；更丰富的 effect 表达（局部状态变更、副作用清单）留给后续版本按证据升级，
  不做无消费者的预留字段。

### 2. 置信度记录与确定性更新

每个状态节点与迁移边携带一条置信度记录：

```text
ConfidenceRecord
├── confidence            [0,1]，当前可信度
├── observed_at_ms        最近一次观测时间（epoch 毫秒）
├── last_verified_ms      最近一次验证通过时间
├── verified_count        累计验证通过次数
├── failure_count         累计失败次数
└── source                封闭集：host | agent | trajectory
```

全部更新是**纯函数**（无时钟读取、无副作用；时间由调用方传入），同输入同输出：

- `note_transition_outcome(record, success, now_ms)`：`verified_count+1` 且
  `confidence = (verified+1)/(verified+failures+2)`（拉普拉斯平滑），或
  `failure_count+1` 且 `confidence = (verified+1)/(verified+failures+2)`；
  `observed_at_ms = now_ms`，成功时同步 `last_verified_ms = now_ms`。置信度随验证
  单调不降路径、随失败单调下降路径均由计数公式决定（确定性）。
- `apply_confidence_decay(record, now_ms, half_life_ms)`：自 `last_verified_ms` 起
  按指数衰减 `confidence *= 0.5^(Δt/half_life)`；单调不增，`Δt<=0` 时不变；衰减不
  改动任何计数与时间戳。半衰期是调用方配置（Runtime 配置缺省 7 天），不是硬编码。
- `needs_exploration(record, threshold)`：`confidence < threshold` 的判定纯函数；
  阈值是配置。低置信触发**探索标记**，不自动触发探索行为（探索循环属 Agent Mapper
  编排，DEC-014 §9.5，不在本决策）。
- **源不可提升（`RULE-09`）**：`source` 是溯源标注，不构成授权；`trajectory` 源的
  记录与 `host` 源在更新语义上无差别，消费方（未来的 Memory 组织）自行解释。

### 3. 感知边界与投影语义

- **UI 状态识别（relocalization）由宿主供给**：Core 定义
  `ScreenStateProvider = std::function<std::optional<ScreenStateSnapshot>()>`，
  快照为 `{state_id, observed_at_ms}`。宿主负责用什么感知（Accessibility/OCR/VLM 或
  人）得出状态名；Core 不做任何识别（DEC-011：本地感知能力是否回归由 demo 证据
  决定）。Provider 必须廉价、非阻塞、可在任意调用线程被同步调用；未安装或返回空时，
  一切依赖当前状态的判定 fail closed（`navigate-no-screen-state`、谓词
  `NotEvaluable`）。
- **App Model 是投影（`W-03`/`RULE-07`）**：宿主经 `WorkflowRuntime::set_app_model`
  安装的内容寻址模型是进程内投影；Runtime 导航执行中的迁移观测以
  `WorkflowNavigationObserved` 事件留痕（DEC-028 §4），事件序列 + 安装内容足够重建
  置信度演化。持久化载体（SQLite 等）沿袭 `RISK-2026-038` 推迟。
- **模型不可信内容不提升权限**：guard、action 与代价均不可绕过 SafetyPolicy 与
  工具注册表校验（`RULE-04`/`W-02` 既有门禁对边动作同样适用，见 DEC-028 §3）。

## 备选方案

- **在 Observation 契约中内嵌 UI 状态图**：Observation 是单次快照，App Model 是跨
  任务长期知识，生命周期与更新主体都不同；混入会污染快照语义（DEC-005 边界）。
  不采用。
- **置信度由 Runtime 内部时钟驱动自动衰减**：违反投影可重建（`W-03`）与确定性测试
  诉求；衰减必须可从事件 + 显式时间参数重放。不采用。
- **节点/边级 source 开放字符串**：开放集会让投影重建无法校验溯源；v1 封闭三值，
  扩展走 schema 版本。不采用。
- **把 guard 表达为独立 DSL**：谓词 DSL（v1 封闭子集）已覆盖前置条件表达，复用同一
  信号引用与求值语义（含 `NotEvaluable` fail closed）避免第二套语义。不采用。

## 影响与风险

- `Mira::workflow` 新增 `workflow_navigation.hpp`（App Model 契约 + 置信度纯函数 +
  规划器）；公开面扩大，installed-consumer 需覆盖。
- 模型规模上限（`RULE-08`）与维护成本是 DEC-014 已登记风险：v1 以限制 + 置信度 +
  事件重建兜底，真实 App 的维护性度量（图规模、失效重建成本）按 DEC-011 由 demo
  证据决定是否扩展。
- 置信度公式（拉普拉斯平滑 + 指数衰减）是可解释的保守估计，不是统计推断；小样本下
  「高置信」可能仍不可靠（与 `RISK-2026-046` 同族），文档与 API 手册披露。
- 半衰期与探索阈值的缺省值是暂定默认值（7 天 / 0.5），冻结点为首个以真实 App 数据
  校准的里程碑；不得宣称经过目标环境验证（`RULE-10`）。

## 验证方式

- 契约矩阵：JSON 往返无损、未知字段/悬垂引用/重复 ID/上限越界 fail closed、
  `validate_app_model` 与解码同源、digest 确定性（同内容两次计算相等、一次改动即变）。
- 置信度矩阵：成功/失败更新的计数与数值（逐值断言，不用近似比较）、衰减单调性与
  `Δt<=0` 不变、`needs_exploration` 阈值边界、source 封闭集负向。
- 纯函数性：全部更新函数不读时钟、同输入同输出（重复调用断言）。

## 关联文档和工作项

- [M12](../plans/m12-app-model-and-navigation.md)：工作项承载
- [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §9.1/§9.2/§10.4、
  [DEC-011](DEC-011-demo-first-external-validation.md)、
  [DEC-019](DEC-019-workflow-ir-contract.md)（谓词 DSL 复用）、
  [DEC-028](DEC-028-navigation-planner-and-navigate-resolution.md)
- [Workflow Runtime 设计](../design/workflow_runtime_design.md)（阶段 E 章节）
- [API 手册 workflow-contracts](../api/workflow-contracts.md)
