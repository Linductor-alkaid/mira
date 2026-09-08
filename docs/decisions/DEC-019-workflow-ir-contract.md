# DEC-019：Workflow IR 公共契约与版本化

> 状态：Accepted
> 日期：2026-09-08
> 负责人：Mira Maintainers
> 冻结里程碑：[M8](../plans/m8-workflow-contracts.md)（`M8-01`/`M8-06`/`M8-07`）
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §7.1 IR
> 表达的契约冻结）

## 背景与问题

[Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)§7.1
要求 Workflow 以中间表示（IR）存储与执行，IR 的具体 schema、序列化格式与兼容性承诺由
专项 DEC 冻结。设计 §17 留有开放问题「Workflow IR 的具体表达（图 vs 线性 + 条件）」。
`RISK-2026-034` 要求 schema 以阶段 B 最小闭环（`Strict`/`DryRun` 执行）反推，超出门类的
字段留扩展位、不先验承诺。

本决策冻结 IR 的表达范围、序列化格式、digest、兼容性承诺与存储边界。运行时如何执行
IR 由 [DEC-020](DEC-020-workflow-run-lifecycle.md) 与
[workflow_runtime_design](../design/workflow_runtime_design.md) 承载。

## 决策

### 1. 表达范围：v1 为「有序步骤序列 + 受限控制」，不做通用图

`mira.workflow.ir.v1` 冻结以下顶层结构：

```text
WorkflowDefinition
├── schema_version            major.minor（见第 4 条）
├── workflow_id               稳定 128 位 ID
├── name / summary            受限长度的描述字段
├── parameters                参数 Schema 列表（见第 2 条）
├── steps                     有序步骤列表（见下）
├── default_policy            默认执行策略（DEC-020 策略枚举）
├── allowed_policies          本 Workflow 允许的策略子集
└── metadata                  受限键值（预留扩展位，v1 仅允许声明过的键）
```

步骤（step）结构：

```text
WorkflowStep
├── step_id                   Workflow 内稳定 ID（`StepId`）
├── kind                      ToolCall | Navigate | Verify | Control（封闭枚举）
├── arguments                 步骤实参，可含参数引用（见第 3 条）
├── precondition              可选前置谓词，不满足则跳过该步
├── verification              可选验证谓词（Verify 步骤必填）
├── recovery                  可选恢复钩子声明
└── max_attempts              本步执行尝试上限（默认 1，上限 8）
```

- `ToolCall`：调用一个 Primitive Tool 或 Skill（两者经同一 Tool 通道，见
  [DEC-021](DEC-021-workflow-tool-channel.md)）；`arguments` 必须通过该工具暴露的参数
  schema 校验的绑定结果。
- `Navigate`：声明目标 UI 状态引用，不写死点击序列；v1 中目标是不透明字符串标记
  （非空、受限长度），其解析语义属于阶段 E（App Model），本决策不承诺。
- `Verify`：执行一次验证谓词；`verification` 必填。
- `Control`：受限控制步骤。v1 只允许一种形式：`jump_to` 指向序列中**更早**且被显式标注
  为 `loop_head` 的步骤，并携带 `max_iterations`（1..64）；禁止任意前向跳转、跨 Workflow
  跳转与非 loop_head 后向跳转。通用图结构（分支合并、并行、子 Workflow 步骤）是扩展位，
  本决策不承诺。

步骤执行顺序即声明顺序；前置条件不满足的步骤被跳过（记 `Skipped`），不视为失败。

### 2. 参数 Schema 与绑定是纯函数

参数声明（`ParameterSpec`）冻结为封闭字段：
`name`（Workflow 内唯一）、`type`（`string | integer | number | boolean`）、`required`、
`default_value`（仅可选参数允许，且必须通过自身约束）、约束
（`minimum/maximum/min_length/max_length/pattern/enum_values`，按类型适用）、`summary`。

绑定（`bind_workflow_parameters`）是纯函数：输入参数声明与调用方实参 JSON，输出
「应用默认值后的有效参数表」或确定性错误码（`UnknownParameter`、`MissingRequired`、
`TypeMismatch`、`ConstraintViolated`、`InvalidDefault`）。多余实参、类型不符与越界值
全部拒绝，不做静默修正。约束子集与既有 `JsonSchema` 校验器（`model_schema.hpp`）的关键
词同源，不新造第二套校验语义。

### 3. 步骤实参中的参数引用

`arguments` 中的参数引用使用唯一封闭形式 `{"$param": "<name>"}`；绑定后的实参由
`resolve_step_arguments`（纯函数）把引用替换为有效参数值。引用未声明参数、嵌套引用
（`$param` 的值本身是引用）与未知键（`$param` 对象内出现其他成员）全部 fail closed。
不含引用的实参原样通过。

### 4. 序列化、版本化与兼容性承诺

- 序列化格式为 JSON（严格 RFC 8259 子集，复用 `parse_json`/`to_json_string`），文档
  自带 `schema_version{major,minor}`，与 DEC-002 的 major.minor 规则对齐。
- **未知字段 fail closed**：任何未声明字段（含嵌套）解码失败，错误码
  `UnsupportedVersion`/`InvalidArgument`。理由：IR 是可执行内容（类比代码而非诊断数据），
  未知字段可能在后续执行时被赋予语义（`W-04`/`RULE-09`），宽松跳过等于允许未验证指令
  入库。DEC-002 的「未知字段解码为未声明」不适用于本契约。
- **版本不匹配 fail closed**：reader 声明支持的 `{major,minor}`；文档被接受当且仅当
  同 major 且 `document.minor <= reader.minor`。文档 minor 高于 reader 时 reader 无法
  保证理解全部语义，拒绝并返回 `UnsupportedVersion`。因此 minor 提升要求 reader 升级，
  这是本契约相对通用事件 schema 更强的承诺，写入 API 手册。
- digest：`workflow_ir_digest = canonical_json_digest(definition)`，内容寻址；版本记录
  与 WorkflowRun 均引用 digest 而非内联定义（大内容不复制，`RULE-07` 精神）。

### 5. 容量与深度上限（`RULE-08`）

默认上限（可被调用方收紧，不可放宽超过常量上限）：文档 256 KiB、嵌套深度 16、步骤数
256、参数数 64、步骤实参 64 KiB、字符串字段单条 8 KiB、`enum_values` 64。超限在解码/
校验阶段拒绝，错误码 `ResourceExhausted`/`InvalidArgument`，不进入存储。

### 6. 存储边界：不可变历史 + 可重建投影（`W-03`）

- Workflow 定义是**受版本管理的衍生资产**：每次修改产生新的不可变版本记录
  （Who/Why/What Changed/Validation Result/Timestamp，契约见 `M8-09` 与
  [DEC-020](DEC-020-workflow-run-lifecycle.md) §版本化），内容以 digest 关联；历史只追加，
  不修改、不删除。
- EventStore 仍是已提交事实的唯一权威记录（DEC-003）；Workflow Library 的索引、统计与
  「最新版本」视图是从事件与版本记录可重建的投影。SQLite 存储实现不在本决策范围
  （随阶段 B 落地，M8 只冻结契约）。
- 模型输出不得直接成为 Workflow 定义：入库前必须通过 schema 校验与验证
  （含 Dry Run），验证结果记入版本记录（`W-04`）。

## 备选方案

- **通用步骤图（DAG + 条件边）**：表达力更强，但阶段 B 最小闭环（Strict/DryRun 顺序
  执行 + 跳过 + 有界循环）不需要它；无执行反馈时冻结通用图会放大 `RISK-2026-034` 的
  返工面。不采用；作为扩展位保留。
- **未知字段宽松解码（跳过未知成员）**：对诊断数据合理，对可执行 IR 等于允许未验证
  内容通过校验管线，违反 `W-04`。不采用。
- **IR 中内联子 Workflow**：引入组合语义与验证传递问题，阶段 A 无执行证据可冻结。
  不采用；子 Workflow 经 Tool 通道引用（DEC-021）或未来扩展位。
- **用现有 `JsonSchema` 直接作为 IR 而非强类型结构**：失去编译期约束与 fail-closed
  边界，且 digest/校验语义分裂。不采用；IR 是强类型值对象 + JSON 序列化。

## 影响与风险

- 新增公共契约模块 `mira-workflow`（CMake 目标 `Mira::workflow`），仅依赖 `Mira::core`
  抽象与 JSON 支持库，禁止反向依赖 Agent 决策层（`RISK-2026-037`，由 installed-consumer
  与头文件独立性测试约束）。
- minor 升级要求 reader 同步升级：宿主升级窗口内新旧版本共存时，旧 reader 拒读新文档。
  这是显式承诺，写入 API 手册与兼容性说明。
- `Navigate` 目标在 v1 是不透明标记：阶段 E 之前它只能被 DryRun 校验为「形状合法」，
  不能被解析。不得据此宣称导航能力已实现（`RULE-10`）。
- 默认值：`default_policy` 缺省时为 `Strict`。该值在 M8 为暂定默认值，已于
  2026-09-08 随 [M9](../plans/m9-workflow-runtime-minimal-loop.md)（`M9-01`）冻结为正式
  默认：保守缺省与安全底线一致（策略只能收紧不能放宽）。

## 验证方式

- `mira-workflow` 契约测试：IR 往返保真（to/from JSON 逐字段断言）、未知字段/版本不匹配/
  超深/超限/非法结构全部负向拒绝、参数绑定正负路径与确定性错误码、参数引用解析、
  Control 步骤后向跳转与 loop_head 约束、digest 稳定性（同内容同 digest、变内容变 digest）。
- installed-consumer：`Mira::workflow` 独立包含、链接与运行（`M8-12`）。

## 关联文档和工作项

- [M8：Workflow 双路径契约冻结](../plans/m8-workflow-contracts.md)：`M8-01`、`M8-06`、`M8-07`
- [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)、
  [DEC-002](DEC-002-public-contract-versioning.md)、
  [DEC-003](DEC-003-event-sourced-persistence.md)
- [workflow_runtime_design](../design/workflow_runtime_design.md)
- [Agent Harness 参考研究](../design/harness_reference_study.md) §7（图即代码不采纳）
