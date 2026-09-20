# Tool 稳定引用与 Skill 层设计（DEC-040 落地规范）

> 状态：Active（TR0 冻结规范，已由
> [M7](../plans/m7-tools-evaluation-platform-v1.md) TR0 阶段交付关闭（PR #63，
> 合并提交 `a180e88`）；TR1 规范见 §17，2026-09-21 随其立项冻结；决策载体为
> [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)。本文代码
> 片段均为契约草案，签名以实现为准）  
> 版本：1.1  
> 更新日期：2026-09-21  
> 负责人：Mira Maintainers  
> 适用范围：Workflow 资产对工具的稳定逻辑引用、引用解析矩阵与兼容状态投影
> （`Runnable`/`Degraded`/`Invalid`）的 Core 侧契约；Skill 生命周期仅登记方向  
> 上位设计：[Agent Harness 与 Workflow 架构设计](agent_harness_and_workflow_architecture.md)、
> [工具模组设计](tool_module_design.md)、
> [Workflow Runtime 设计](workflow_runtime_design.md)  
> 决策依据：[DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)、
> [DEC-009](../decisions/DEC-009-tool-module-boundary.md)、
> [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)、
> [DEC-019](../decisions/DEC-019-workflow-ir-contract.md)、
> [DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md)、
> [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
> [DEC-002](../decisions/DEC-002-public-contract-versioning.md)

## 1. 文档目的与效力

DEC-019 的 `ToolCall` 步骤以实参树内保留成员 `"tool"`（wire 名字符串）引用被调用
工具，DEC-015 提供执行期身份一致性校验。这条链路对「当下执行」闭合，对「长期保
存」有两个缺口：裸 wire 名无法表达「钉住哪个版本」或「跟随哪个演进线」；Registry
无法在准入期回答「这个 Workflow 现在还能不能跑」。本文冻结 DEC-040 首个实现阶段
（TR0）：引用语法、提取契约、解析矩阵与兼容状态投影。回答四件事：

1. Workflow 保存的稳定引用长什么样，钉住与跟随两种模式如何显式声明（§4）。
2. Workflow 定义如何被提取为随版本保存的引用清单，提取时观察什么（§5）。
3. 引用清单 × 当前暴露视图如何确定性重算出逐引用结论与 Workflow 级兼容状态
   （§6/§7），`Invalid` 如何准入拒绝、`Degraded` 如何留痕（§8）。
4. 兼容投影与 DEC-015 执行期校验的边界在哪：投影是准入期前置检查，绝不替代、
   绝不进入执行期身份校验（§9）。

本文与 DEC-040 同时生效；冲突时以 DEC 为准。引用语法（§4）冻结后即为公开兼容
承诺（DEC-002 major/minor 纪律）；TR1 的 Skill 语义在冻结前不构成任何承诺。

## 2. 背景、目标与非目标

### 2.1 目标

- 冻结稳定引用的字符串语法与两种模式（钉住 spec digest / 跟随最新可用版本），
  BuiltIn 纯工具、`HostProvided`、`out_of_process`（MCP）成员处于同一引用命名
  空间（DEC-040 §3.3）。
- 提供从 `WorkflowDefinition`（DEC-019 IR v1）确定性提取引用清单的纯函数；清单
  是随 Workflow 版本保存的版本化工件，不改变 IR v1 schema（DEC-040 非目标：
  IR 表达是加法演进，随首个消费者里程碑冻结）。
- 提供引用清单 × 当前暴露视图的确定性兼容投影：逐引用
  `Resolved`/`EvolvedCompatible`/`EvolvedIncompatible`/`Unresolved`，Workflow 级
  `Runnable`/`Degraded`/`Invalid`；同输入同状态同 digest，不读时钟（RULE-07 投影）。
- 提供 `Invalid` 准入拒绝决策与 `Degraded` 审计投影产物（版本化 JSON），供
  Run 准入与事件留痕消费。

### 2.2 非目标

- 不改变 DEC-019 IR v1 schema；不改写 `arguments["tool"]` 的执行期语义
  （DEC-015 身份一致性校验原样生效）。
- 不做 Skill 发布/升级/撤销生命周期与 Procedure 索引投影（TR1 方向见 §15）。
- 不把兼容状态写入库版本记录（投影 + 事件留痕，DEC-040 备选方案否决第 4 条）。
- 不做 WorkflowRuntime 的库存储接线与 `create_run` 强制门禁（首个消费者里程碑
  冻结 IR 加法演进后接线；本阶段交付其消费的决策与投影产物）。
- 不做自动 Skill 发现、推荐、自动发布；不做跨进程 Workflow 库（RISK-2026-038）。
- 不新增任何权限豁免：投影结论不构成授权（DEC-004/DEC-015 语义不变）。

## 3. 系统上下文与依赖方向

```text
                    ┌────────────────────────────┐
 WorkflowDefinition │ TR0 引用层（本文）          │   ExposedToolSpec 视图
 (DEC-019 IR v1) ──▶│  extract → refs manifest   │◀── BuiltinToolRegistry::exposed_tools()
                    │  project → compat projection│    或 project_tool_exposure().tools
                    │  admit   → 准入决策         │    （两来源同构，DEC-040 §3.1）
                    └──────────┬─────────────────┘
                               │ 纯函数、无 I/O、无时钟
                    ┌──────────▼─────────────────┐
                    │ 消费者（后续接线）：         │
                    │  · Run 准入：Invalid 拒绝   │
                    │  · 事件留痕：Degraded 审计  │
                    │  · 发布门禁：提取时观察快照  │
                    └────────────────────────────┘
```

- TR0 引用层是纯计算层：消费调用方供给的定义、清单与暴露视图，产出清单、投影
  与决策。不做 I/O、不读时钟、不产生随机数；全部 digest 跨进程一致。
- 依赖方向：引用层依赖 `workflow_ir.hpp`（定义与 digest）、`model_contracts.hpp`
  （`ExposedToolSpec`）、`model_schema.hpp`（严格子集校验器）。不依赖 Registry
  状态机、协商协调器或 WorkflowRuntime——暴露视图由调用方折叠为
  `ExposedToolSpec` 集合注入，模组体系与纯 BuiltIn 注册表两个来源同构。
- 执行期仍按 DEC-015 以 wire 名在请求时解析；引用层不参与派发路径。

## 4. 稳定引用语法 v1（冻结）

### 4.1 字符串形态

引用字符串是可嵌入事件、轨迹（DEC-038 L1 对齐）与审计文本的逻辑身份，不内联运
行时对象或会话句柄（DEC-040 §3.3）：

```text
toolref:<wire-name>                    -- 跟随模式（FollowLatest）
toolref:<wire-name>@<64 位小写十六进制> -- 钉住模式（PinnedDigest）
```

- scheme 字面量 `toolref:`；`<wire-name>` 为受治理词表字符集（小写字母数字段，
  段内允许 `_`/`-`，单点分隔，段非空，无首尾点）——与模组成员名、module_id 同一
  字符集，三来源（BuiltIn/HostProvided/OutOfProcess）同命名空间。
- 钉住内容为成员 `spec_digest`（内容寻址）：纯 BuiltIn 工具取注册表曝光 spec 的
  canonical digest，模组成员取 `module_tool_spec_digest`。digest 变化即工具内容
  演进，无论版本号是否变化。
- 两种模式在引用中显式声明：带 `@digest` 即钉住，不带即跟随（DEC-040 §3.3）。

### 4.2 解析规则（fail closed）

- scheme 缺失或错误、空串、字符集违规、`@` 后缺失或非 64 位小写十六进制、大写
  十六进制、尾部多余字符、内嵌空白、超长（引用总长 > 256 字节）一律拒绝。
- 拒绝错误 domain 为 `mira.tool_reference`，错误码确定性。
- 解析产物 `ToolReference{wire_name, mode, pinned_spec_digest}`；
  `tool_reference_to_string` 产出唯一规范形态（钉住为小写十六进制），往返无损。

### 4.3 为什么钉 spec digest 而非版本号

版本字符串是 manifest 声明值，不保证跨来源、跨内容唯一（不同内容可声明同版本，
同内容可升版本）；内容寻址 digest 是 DEC-040 §3.3 的首选钉住形态（「钉住内容以
内容寻址 digest 关联」）。基于版本约束的钉住（如 `>=1.2,<2`）需要来源限定的版本
命名空间与区间代数，v1 不冻结；未来若引入，作为引用语法的加法演进（新形态），
旧 Runtime 按本节规则 fail closed 拒绝（DEC-002 加法演进纪律）。

## 5. 引用清单：提取契约

### 5.1 工件

引用清单 `mira.workflow.tool_refs.v1` 是 Workflow 版本的随行工件（内容寻址独立
digest；不入库为独立事实，随 Workflow 版本记录的保存方式归消费者/宿主——TR1 接
线时冻结其在版本记录中的挂载字段）：

- `workflow_id` + `definition_digest`（提取所对 IR 的
  `workflow_definition_digest`，绑定身份防错配）。
- 逐 `ToolCall` 步骤条目：`step_id`、`wire_name`（读自 IR v1
  `arguments["tool"]` 字符串）、`mode`、钉住条目附提取时观察到的 `spec_digest`。
- 输出按 `step_id` 排序；清单 digest 为 canonical 投影摘要。

### 5.2 提取规则

- 输入：`WorkflowDefinition`（内部先过 `validate_workflow_definition`，结构非法
  fail closed）、当前暴露视图（`ExposedToolSpec` 集合）、提取选项
  （`default_mode` + 逐 wire 名覆盖；覆盖含重复名或空名 fail closed）。
- 每个 `ToolCall` 步骤的 `arguments["tool"]` 必须是符合词表字符集的非空字符串，
  否则整组拒绝（IR v1 约定）。
- 模式解析：逐工具覆盖优先，否则默认模式。钉住与跟随条目都要求 wire 名在视图中
  可解析——发布期 fail closed：引用不存在工具的 Workflow 不产生清单（打错名的
  缺陷在发布时暴露，而不是运行时）。
- 钉住条目记录观察到的 spec digest；跟随条目 digest 为空（解析到什么算什么，
  由 §6 投影判定）。
- 视图中同一 wire 名出现两次（跨来源冲突漏网）fail closed（防御纵深，正常由
  注册/协商/激活门禁挡住）。
- 无 `ToolCall` 步骤的定义产出空条目清单（合法，digest 确定）。
- JSON 序列化/严格反序列化往返无损；`verify_workflow_tool_refs` 校验清单与
  （definition_digest）绑定。

## 6. 解析矩阵

清单条目 × 当前暴露视图，逐条目结论（无时钟、无 I/O）：

| 条目模式 | 视图中 wire 名 | spec digest 对比 | 条目结论 |
| --- | --- | --- | --- |
| 跟随 | 存在 | —（取当前） | `Resolved` |
| 跟随 | 不存在 | — | `Unresolved` |
| 钉住 | 存在 | 相等 | `Resolved` |
| 钉住 | 存在 | 不等 | `EvolvedCompatible` 或 `EvolvedIncompatible`（§7 骨架可绑定判定） |
| 钉住 | 不存在 | — | `Unresolved` |

## 7. 兼容状态投影

### 7.1 Workflow 级状态

- 任一条目 `Unresolved` 或 `EvolvedIncompatible` → `Invalid`（引用无法解析或
  schema 不兼容；Run 准入 fail closed）。
- 否则任一条目 `EvolvedCompatible` → `Degraded`（schema 演进但参数仍可绑定；
  允许启动、事件留痕）。
- 否则 → `Runnable`。
- 空条目清单 → `Runnable`（digest 确定）。

### 7.2 骨架可绑定判定（`EvolvedCompatible` vs `EvolvedIncompatible`）

对 digest 不等的钉住条目，取该条目对应步骤的记录实参（IR `arguments`）与当前
工具 `parameters_schema` 做**骨架可绑定**检查：

1. **占位符按型实例化**：实参树中形如 `{"$param": "<name>"}` 的节点按其所在
   位置的当前 schema 以最小合法实例替换（type 定向：string→串、integer→0、
   number→0、boolean→false、object→递归必填属性、array→单元素（有界）、enum→
   首个成员、const→常量；无 type 视为任意→null）。深度与规模受 schema 子集
   限额约束，超限按不兼容处理。
2. **严格校验复用**：实例化后的实参树交由既有
   `validate_instance_against_schema`（M3 严格子集校验器）校验——准入期与执行
   期参数校验是同一个校验器，语义零漂移。
3. **结论**：无违例 → `EvolvedCompatible`；有违例 → `EvolvedIncompatible`，
   detail 记录首个违例的 path/keyword（有界、脱敏，不含 schema 与描述原文）。

`$param` 占位按所在位置当前 schema 可满足即为「仍可绑定」——真实绑定失败（如
参数声明与绑定值类型不合）仍由执行期绑定与 DEC-015 校验拒绝，本投影不做类型
 unknown 通配（避免与严格校验器语义漂移）。

### 7.3 输入绑定与防御

- 清单的 `workflow_id`/`definition_digest` 与给定定义不一致 → 整组拒绝
  （防错配，fail closed）。
- 视图 wire 名重复 → 整组拒绝（同 §5.2 防御纵深）。
- 输出投影含逐条目结论（`ToolRefCompatEntry`）与 Workflow 级状态；投影 digest
  为 canonical 摘要；同输入同 digest（跨进程一致）。

## 8. 准入决策与事件留痕

- `admit_workflow_run_by_tool_compat(projection)` 是全函数：
  - `Invalid` → `admitted=false`，reason 确定性（含首个未通过条目的 step_id 与
    结论名）；消费者在 Run 准入处据此 fail closed 拒绝启动（DEC-040 §4）。
  - `Degraded` → `admitted=true` 并要求留痕：消费者以
    `workflow_tool_compat_to_json(projection)`（`mira.workflow.tool_compat.v1`，
    脱敏——只含 step_id/wire 名/digest/结论名/有界 detail，不含 schema 体、
    工具描述与任何 secret）为事件载荷留审计事件。
  - `Runnable` → `admitted=true`，无留痕要求。
- 决策函数不产生副作用、不发事件；事件发射接线归消费者（显式，无隐藏后台）。
- 投影/决策**不进入执行路径**：执行期身份校验（DEC-015）照旧。

## 9. 与 DEC-015 执行期校验的边界

| 维度 | 兼容投影（TR0，准入期） | DEC-015（执行期） |
| --- | --- | --- |
| 输入 | 引用清单 + 当时暴露视图 | 当次请求的暴露快照 + ToolProposal |
| 判定 | Workflow 还能不能跑 / 降级到什么程度 | 单次调用的身份一致、至多一次、参数合法 |
| 失败语义 | `Invalid` 拒绝启动；`Degraded` 留痕 | 模型可归因 failed record |
| 时间语义 | 准入时刻的确定性重算 | 派发时刻实时校验 |

组合负向（测试矩阵 G5）：准入为 `Degraded` 的 Workflow 在执行前工具被撤销或
身份漂移，仍然被执行期校验拒绝——投影通过不豁免任何执行期门禁。

## 10. 所有权、并发与 Executor 路由

TR0 全部为串行控制面内的同步纯计算（有界、无 I/O、无时钟），不新增异步路径、
不新增线程或定时器；不持有可变状态，全部输入经 const 引用或 span 注入。清单与
投影的保存归调用方/宿主（RULE-07：EventStore 仍是事实源，本工件是投影）。

## 11. 错误处理

- 错误 domain `mira.tool_reference`，确定性 domain_code（解析失败 / 提取失败 /
  绑定错配三类入口），`safe_message` 只含违规形态摘要与有界标识符，无视图内容
  原文。
- 整组拒绝语义：提取或投影任一条目失败即整体失败，不产生部分清单/部分投影。

## 12. 安全与隐私

- 引用与投影产物只承载身份与 digest（wire 名、step_id、spec digest、结论），
  不承载工具描述、schema 体、参数值或结果内容；脱敏负面清单入测试矩阵（G5）。
- 视图内容是不可信数据（RULE-09）：重复 wire 名、超限 schema 一律 fail closed，
  不静默截断进工件。

## 13. 测试策略（对应 M7 §5.5 门禁）

- 引用解析 golden 与负矩阵（G1）；提取矩阵与钉住观察（G2）；解析矩阵与状态
  聚合（G3）；投影确定性（`--report` 跨进程字节一致）与骨架可绑定语义（G4）；
  准入决策、留痕脱敏与 DEC-015 组合负向（G5）；consumer 闭包（G6）。

## 14. 备选方案（本阶段裁决记录）

- **引用直接写进 IR v1 `arguments["tool"]`**：执行期 wire 名匹配会拒绝
  `toolref:` 形态，除非改动派发路径——违反「不改 DEC-019 v1」非目标。不采用；
  引用清单作为随行工件，IR 加法演进归首个消费者里程碑（DEC-040 非目标条款）。
- **钉住版本号而非 digest**：见 §4.3。不采用。
- **骨架可绑定用自定义宽松校验器**：与严格校验器双实现必然漂移。不采用；
  占位符按型实例化后复用同一校验器（§7.2）。
- **兼容状态落库为版本记录字段**：DEC-040 备选方案已否决（投影 + 事件留痕）。

## 15. 分阶段与 TR2 方向（未冻结）

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| TR0 | 引用语法、提取、解析矩阵、兼容投影、准入决策与留痕产物 | §4–§13 冻结，M7 TR0 交付关闭（PR #63） |
| TR1 | Skill 发布生命周期与 Procedure 索引投影（§17 冻结）：Skill 描述符与暴露面派生、`SkillPublicationRegistry` 发布/升级/撤销生命周期、Procedure 索引投影；纯契约层，不触碰执行路径 | §17 冻结（2026-09-21），M7 TR1 承载 |
| TR2 | WorkflowRuntime 接线与执行（未冻结）：库存储挂载 tool_refs 清单、`create_run` 准入消费 TR0 投影、`Degraded` 事件发射、Skill 经 Tool 通道的子 Workflow 调用执行适配（DEC-040 §3.2）与 IR 引用表达加法演进 | 仅方向；进入实施前随其立项冻结工作项、门禁与本文件增补 |

TR1 约束（§17 冻结）：Skill 复用 Workflow 版本化本体，不建第二套资产体系
（W-03）；编译产物不自动暴露，索引以宿主显式发布为界；Skill 与普通 Tool 同
门禁（DEC-040 §6 Authority 不变）；TR1 无执行面——Skill 不进入
`BuiltinToolRegistry`/exposure，调用执行归 TR2。

## 16. 关联文档

- [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)、
  [DEC-009](../decisions/DEC-009-tool-module-boundary.md)、
  [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-019](../decisions/DEC-019-workflow-ir-contract.md)、
  [DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md)、
  [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)
- [M7 计划](../plans/m7-tools-evaluation-platform-v1.md)（§4.5 工作项、§5.5 门禁）
- [工具模组设计](tool_module_design.md)（§8 投影消费者、§15 阶段表）
- [Mira 实施总计划](../plans/mira-implementation-plan.md)（§4.1 方向登记）

## 17. TR1：Skill 发布生命周期与 Procedure 索引投影（2026-09-21 冻结）

### 17.1 Skill 描述符与暴露面派生

Skill 是「暴露为 Tool 的 Workflow」（DEC-040 §3.2）：描述符携带源 Workflow id
与钉住的 `ir_digest`，Tool 通道暴露面从源定义确定性派生：

- `mira.skill.descriptor.v1`：`name`（词表字符集 wire 身份，Skill 的稳定逻辑
  身份）、`version`（Skill 自身显式版本，升级为显式动作）、
  `source_workflow_id` + `source_ir_digest`（钉住）、派生暴露面
  `SkillSurface{description, parameters_schema, has_side_effects}`、
  canonical descriptor digest。
- `derive_skill_surface(definition, refs, view)` 派生规则（全部确定性）：
  - `description` = `definition.summary`（非空、有界，超限整组拒绝）；
  - `parameters_schema` 从 `WorkflowParameterSpec` 列表映射（String→string +
    minLength/maxLength/pattern/enum，Integer/Number→integer/number +
    minimum/maximum，Boolean→boolean；required 参数进 `required`；
    `additionalProperties=false`；逐项过 `gate_schema_subset`）；
  - `has_side_effects` = TR0 引用清单 × 视图：任一被引用工具
    `has_side_effects` 即真；全部引用须在视图可解析（发布期 fail closed）。
- 输入绑定：refs 必须通过 `verify_workflow_tool_refs` 绑定该定义；视图重名
  fail closed（复用 TR0 纪律）。

### 17.2 发布生命周期

`SkillPublicationRegistry`（串行控制面组件，镜像 TM1 纪律）承载宿主显式发布
动作；状态 `Published -> Revoked`（只降级，无复出口），升级为同名的显式换钉：

- `publish_skill`：宿主显式动作。fail closed——源定义未过结构校验、refs 未
  绑定、源 `WorkflowVersionRecord` 非 runnable（`workflow_version_is_runnable`，
  即未经 `publish_validated` 门禁的 DryRunPassed/Validated 版本）、记录的
  `content_digest`/`workflow_id` 与定义不符、name 撞宿主保留名或已存在（同
  name + 同 descriptor digest 幂等 NoOp，其余 `AlreadyExists`）、超出容量。
- `upgrade_skill`：显式换钉。新版本号必须严格大于当前版本；新 `ir_digest`
  经同一 runnable 校验；同 digest 升级幂等 NoOp；旧版本进 superseded 轨迹，
  已发布描述符被替换（事件留痕）。
- `revoke_skill`：Published -> Revoked，理由有界脱敏；重复撤销幂等；撤销后
  不可恢复（重新发布同 name 需新注册周期——seal 后即为拒绝）。
- 部署窗：`seal()` 后 publish/upgrade 拒绝、revoke 仍可用；`close()` 后一切
  变更拒绝、读取仍可用；拒绝计数可见。
- 版本化事件 `mira.skill.publication.v1`（kind: published/upgraded/revoked）：
  仅 name/version/workflow_id/ir_digest/descriptor digest/有界理由——不含
  description 与 schema 体；sink 失败计数不阻塞控制面。

### 17.3 Procedure 索引投影

承接 DEC-029 推迟项（DEC-040 §7）：**索引以宿主显式 Skill 发布为界**——未发
布的 Workflow 库资产不自动索引（避免 DEC-029 否决的「无验收写入面」）。

- `project_skill_procedure_index(publications)` → 逐发布记录的 Procedure
  statement（canonical JSON，schema `mira.skill.procedure_index.v1`：name、
  version、source_workflow_id、source_ir_digest、descriptor digest、
  has_side_effects；不含 description），对齐 DEC-029「statement 固定
  canonical JSON」纪律；输出按 name 排序。
- 无时钟：时间戳归消费接线（TR2）；本投影只承载身份与钉住事实。
- 可重建：statement 严格反解析回等价条目、重放字节一致（同输入同状态，
  RULE-07 投影）。
- 不写 `IMemory`：写入面（MemoryRecord(kind=Procedure) 的落库、scope/ACL、
  检索）归 TR2 接线里程碑。

### 17.4 明确非目标

- 无执行面：Skill 不进入 `BuiltinToolRegistry`/exposure/协商；子 Workflow
  调用执行适配归 TR2（DEC-040 §3.2，经 Tool 通道既有校验路径）。
- 不做自动发现/推荐/自动发布；编译产物不自动成为 Skill。
- 不改变 `workflow_learning`（DEC-030）既有语义。
