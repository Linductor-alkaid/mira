# DEC-026：任务归纳与参数化提议（阶段 D）

> 状态：Accepted
> 日期：2026-09-09
> 负责人：Mira Maintainers
> 冻结里程碑：[M11](../plans/m11-trajectory-compilation-and-task-induction.md)
> 替代/被替代：无（是 [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) 阶段 D
> 「任务归纳」的冻结；依赖 [DEC-025](DEC-025-success-trajectory-compilation-and-publish-gate.md)
> 的轨迹契约与入库门禁）

## 背景与问题

单条成功轨迹编译（DEC-025）产出**字面量** Workflow：一切观测值被固化，复用面窄。
DEC-014 的「Workflow 参数化复用」要求把多条相似轨迹中的**变化量**提升为参数（如
收件人、消息文本），把**常量**保持为字面量；同时 DEC-014 §风险明确「归纳错误：从
少量样本抽象出错误参数化。缓解：归纳是提议而非事实，验证失败回退」。

不冻结归纳的输入、算法边界、参数规格生成与失败语义，编译产物就只能是死脚本，
「相似任务归纳」也无法以可测试的方式实现。

## 决策

### 1. 归纳候选（`WorkflowParameterCandidate`）

候选是**提议**，不是事实：它描述「轨迹步实参树中某个标量叶值得提升为参数」。

```text
WorkflowParameterCandidate
├── name            参数名（^[a-z][a-z0-9_]{0,63}$）
├── step_index      目标步（轨迹 steps 下标）
├── pointer         RFC 6901 JSON Pointer，定位实参树内标量叶
├── provenance      "provenance"（原 $param 引用继承名）| "structural"（结构 diff）
│                   | "explicit"（宿主/模型显式提议）
└── observed_values 每条轨迹在该叶的观测值（有序，首条为 anchor）
```

两种生成方式，共用同一校验：

- **结构 diff（多轨迹归纳）**：输入 2..16 条**同骨架**轨迹（步数、种类、名称逐一
  相等，实参树同形——键集合与内部节点类型相同）。以首条为 anchor，逐叶比较：类型
  相同但值不同的标量叶生成候选；全部相同的叶保持字面量。命名优先取 provenance
  （该叶在原始实参中的 `$param` 名——同一参数在多个步引用时合并为一个候选，观测值
  聚合）；无 provenance 时按稳定路径序生成 `param_1`、`param_2`…（宿主可在草稿上
  改名）。骨架不同形 fail closed（`induct-skeleton-mismatch`）。
- **显式候选（宿主/模型提议）**：宿主或模型直接给出 `{name, step_index, pointer}`
  列表，标注 `explicit`。**模型提议没有特殊通道**：经与宿主提议完全相同的确定性
  校验（`W-04`），未通过即拒绝（`induct-candidate-invalid`）。

校验规则（全 fail closed，确定性错误码）：名字符合模式且不与既有参数或其他候选
冲突（`induct-name-conflict`）；pointer 在目标步生效实参树内解析到**标量**叶
（string/integer/number/boolean；对象与数组叶 v1 不可参数化，
`induct-leaf-not-scalar`）；pointer 不得命中 ToolCall 保留成员 `"tool"`
（`induct-reserved-member`）。

### 2. 参数化编译

`compile_workflow(trajectory, options, candidates)` 在 DEC-025 字面量编译之上执行
重写：

- 候选叶替换为唯一封闭形式 `{"$param": "<name>"}`（复用 DEC-019 §3 绑定语义，编译
  后的引用解析走既有纯函数，无第二套替换机制）。
- 每个候选生成 `WorkflowParameterSpec`：`type` 按观测值 JSON 类型推断
  （string->String、integer->Integer、number->Number、boolean->Boolean）；
  `required=false`；`default_value` 取 anchor 观测值（编译产物保持可独立运行）；
  不生成约束子句（最小规格，宿主可在草稿上补）。
- 类型不一致的观测值（同候选在不同轨迹呈现不同 JSON 类型）fail closed
  （`induct-type-mismatch`）；integer 与 number 视为不同类型（保守）。
- 其余编译语义（默认值固化、跳过剔除、策略门禁、确定性）与 DEC-025 完全一致；
  来源定义同名既有参数与候选冲突按 `induct-name-conflict` 拒绝（不允许静默遮蔽）。

### 3. 提议语义与回退

- **归纳是提议而非事实**：`induce_parameters`（结构 diff）与参数化编译的输出都是
  **宿主可编辑的草稿**——改名、改默认值、删候选、补约束都发生在提交门禁之前。唯一
  入库路径是 DEC-025 `publish_validated`（结构校验 + DryRun 门禁 + 证据），归纳输出
  没有旁路。
- **回退**：归纳草稿门禁失败时库零变更（DEC-025 §3 回退不变量）；既有版本保持
  runnable。归纳参数在后续真实运行中绑定失败（类型/约束）按既有 `bind-*` 确定性
  错误码暴露，调用方换参重试或回退旧版本，无隐式修正。
- **样本量诚实**：候选的 `observed_values` 随草稿保留在宿主侧（不入 IR、不入版本
  记录）；少量样本下「常量」可能是偶然一致，文档披露该风险由宿主编辑与后续运行
  验证消化，不在编译器内做统计推断（v1 不设最小差异样本数阈值）。
- 候选数量受 `WorkflowLimits::max_parameters` 约束（既有 RULE-08 上限，超限
  fail closed）。

### 4. 非目标

- 从事件流自动抽取 Agent 工具调用轨迹（宿主构造入口已留，编排属 Agent Harness
  后续里程碑）。
- 自然语言 -> 候选提议的解释编排（模型可给 explicit 候选，解释与歧义升级仍属
  Agent 侧，M10 §2.2 既定推迟）。
- 对象/数组叶参数化、跨 Workflow 聚类归纳、统计型参数发现（最小样本数、方差阈值）。
- 把候选或观测值写入 IR、版本记录或事件载荷（草稿是宿主侧数据）。

## 备选方案

- **编译器内置启发式（单轨迹自动猜参数）**：无差异证据的猜测必然把常量误固化为
  参数或反之，且不可解释；v1 要求差异证据（多轨迹）或显式提议。不采用。
- **观测值直书入参数默认值之外的约束子句（maxLength/enum 等）**：从一两个样本生成
  约束会把偶然值变成硬边界，放大「归纳错误」风险。约束留给宿主编辑。不采用。
- **允许对象/数组叶参数化**：JSON Pointer 定位与 `$param` 单叶替换可支持，但类型
  推断与绑定校验面显著扩大而无已验证消费者。推迟（记录于非目标）。
- **归纳失败时发布 `Rejected` 版本记录**：与 DEC-025 §3 同一裁决，审计走事件。
  不采用。

## 影响与风险

- `workflow_compiler.hpp` 新增候选结构与 `induce_parameters`、参数化编译重载；
  `WorkflowLimits` 既有上限直接复用，无新配置面。
- 候选命名冲突与保留成员规则使「渠道误固化」类错误（DEC-014 §风险）在编译期
  fail closed；但「常量误判」只能靠宿主编辑与运行验证消化（§3 样本量诚实）。
- `integer`/`number` 分型的保守选择可能让浮点/整数混合的叶被拒绝：这是显式的
  fail closed 而非静默宽化，宿主可编辑草稿统一类型后重提。

## 验证方式

- 结构 diff：同骨架双轨迹单叶差异 -> 单候选（provenance 命名合并多步引用）；常量叶
  保持字面量；auto 命名稳定有序；骨架不匹配、超上限、类型不一致负向。
- 显式候选：合法提升、名字/pointer/保留成员/非标量叶/冲突负向；模型提议同校验。
- 参数化编译：`$param` 引用形状、绑定解析（默认值与显式传参）、`required=false`
  语义、与既有绑定纯函数一致性、digest 确定性。
- 回退：候选诱导草稿 DryRun 失败 -> 库零变更；归纳参数绑定失败暴露 `bind-*` 错误码；
  M8 版本历史不可变回归。

## 关联文档和工作项

- [M11](../plans/m11-trajectory-compilation-and-task-induction.md)：工作项承载
- [DEC-025](DEC-025-success-trajectory-compilation-and-publish-gate.md)（轨迹契约、
  编译与门禁）、[DEC-019](DEC-019-workflow-ir-contract.md) §2/§3（绑定与引用）、
  [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) §14/§16/§17
- [Workflow Runtime 设计](../design/workflow_runtime_design.md)（阶段 D 章节）
- [API 手册 workflow-contracts](../api/workflow-contracts.md)
