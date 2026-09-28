# 实体词表设计——跨投影实体引用词表 v1

> 状态：Active（冻结承载；v1 条目与等价规则随 [M27](../plans/m27-world-state-projection-core.md)
> 立项跑前冻结）
> 版本：1.0
> 更新日期：2026-09-28
> 负责人：Mira Maintainers
> 上位决策：[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)（收录范围
> 与效力）、[DEC-002](../decisions/DEC-002-public-contract-versioning.md)（版本化）
> 适用范围：[DEC-038](../decisions/DEC-038-unified-behavior-trace.md) L1 行为投影、
> [DEC-041](../decisions/DEC-041-session-world-state-projection.md) 会话 World State、
> [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md) 资产层及其他需要
> 引用环境实体、页面状态、工具身份或源事件的公开契约面

## 1. 目的与效力

本文是跨投影实体词表的**唯一冻结承载**（[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)
决策 1）：定义四组身份域的引用形态、有效性域与等价规则，登记非基线成员的映射规则。
[DEC-038](../decisions/DEC-038-unified-behavior-trace.md) 与
[DEC-041](../decisions/DEC-041-session-world-state-projection.md) 的首阶段里程碑文件
引用本表，不得复制或另立冻结面；新增消费方先在本表登记再写契约（与
[术语表](../project/glossary.md) §1「新增公共概念先登记」同型纪律）。

本文不改变任何既有契约的 schema 与语义：四组基线条目全部指向已冻结、已实现、有
契约测试的公开面；本文冻结的是「跨投影引用这些实体时必须使用的同一词表」。

## 2. 词表身份域（v1 基线）

### 2.1 ElementRef 族——环境实体证据引用

| 项 | 冻结值 |
| --- | --- |
| 契约出处 | `ElementSource`/`ElementRef`/`PerceptionEvidence`/`AppContext`，`include/mira/observation.hpp:341-375`；权威冻结定义 [Observation 坐标设计](observation_coordinate_android_host.md) §4.3（:196-211） |
| 引用形态 | `ElementRef{observation_id, environment_epoch, source, stable_hint, bounds, space, evidence_digest}` |
| 有效性域 | 单一 Observation + environment epoch + freshness policy；**是证据，不是永久身份** |
| 等价规则 | 七字段全等（含 `evidence_digest`）为同一引用；字段不同即为不同引用，即使指向同一可见元素 |
| 来源闭集 | `UiTree / Ocr / Detector / Fused / Unknown`（`ElementSource`，observation.hpp:341）。`Fused` 生产者随 [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md) 落地，当前无供给——v1 只冻结契约形状，不宣称来源语义已实证（RULE-10） |
| 消费者 | DEC-041 World State 存活实体表（M27）；DEC-038 L1 实体引用（随其首阶段） |

### 2.2 App Model state_id 族——页面状态假设引用

| 项 | 冻结值 |
| --- | --- |
| 契约出处 | `AppModelState.id` 与 `AppModelTransition.from_state`/`to_state`（`include/mira/workflow_navigation.hpp:111-133`）；宿主识别边界 `ScreenStateSnapshot.state_id`（:228-231，[DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md) §3）；导航观测事件载荷 `WorkflowNavigationPlannedEvent`/`WorkflowNavigationObservedEvent` 的 `from_state`/`to_state`（`include/mira/workflow_events.hpp:165-187`） |
| 引用形态 | 字符串 `state_id`；跨文档表达为 `(app_id, state_id)` 二元组（`app_id` 取 `AppContext.package_name` 同域） |
| 有效性域 | 单一 App Model 文档命名空间（DEC-027 v1 单命名空间同源约束）；跨文档不宣称同一性 |
| 等价规则 | 同文档内字符串身份相等；`(app_id, state_id)` 二元组相等为跨文档同一假设 |
| 生产者边界 | 状态识别由宿主 `ScreenStateProvider` 提供，Core 不做识别（DEC-027 §3）；导航观测来自 `mira.workflow.navigation-observed.v1` 事件（词表 2.4） |
| 消费者 | DEC-041 World State 页面假设（M27）；DEC-038 L1 行为的页面状态引用（随其首阶段） |

### 2.3 工具引用族——资产层工具逻辑身份

| 项 | 冻结值 |
| --- | --- |
| 契约出处 | [Tool 引用与 Skill 设计](tool_reference_and_skill_design.md) §4（:96-124，权威冻结面在该文档）；`ToolCall` 步骤 `arguments["tool"]` 保留成员（:27、:379）；[DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md) §影响与风险「Trace L1 的工具调用引用应使用同一引用词表」（:128-129） |
| 引用形态 | `toolref:<wire-name>`（跟随）/ `toolref:<wire-name>@<64 位小写十六进制>`（钉住），:104-105 |
| 受治理字符集 | 小写字母数字段、段内 `_`/`-`、单点分隔、段非空、无首尾点；与模组成员名/`module_id` 同字符集、三来源同命名空间（:108-109）；引用总长 ≤ 256 字节（:116-122 解析矩阵拒绝阈） |
| 有效性域 | Registry 暴露视图 + 钉住 digest 的内容寻址；解析 fail closed（:116-122） |
| 解析器归属 | 权威解析器 `parse_tool_reference`/`tool_reference_to_string`（`include/mira/tool_reference.hpp:76`、:80）实现在 workflow 模块（`src/workflow/tool_reference.cpp`，编入 `mira_workflow`——`CMakeLists.txt:153`；`mira_workflow` 链接 `Mira::core`——:163）。core 侧消费者**不复刻**语法解析（core 反向依赖 workflow 被依赖方向禁止）；core 契约只做登记级检查（非空 + 总长 ≤ 256 字节），语法校验由持有权威解析器的一侧（调用方、workflow 面或链接 workflow 的测试目标）执行 |
| 等价规则 | 规范形态字符串相等（钉住为小写十六进制，往返无损）；跟随引用按解析时点视图判定，不宣称跨时点同一 |
| 消费者 | DEC-038 L1 工具调用引用（随其首阶段）；DEC-041 World State 工具结算条目（M27，登记级检查） |

### 2.4 L0 事件引用族——源事件引用

`mira.workflow` 事件族的引用目录由两层构成，**两层都在本表冻结**（此前仓库中
schema 名散落于各 `to_event_payload` 内联字符串、无逐名清单，本表即其单一冻结
承载；`mira.policy.*.v1` 九类已由 [M26](../plans/m26-temporal-policy-stage-t1.md)
§4.4 事件表逐名冻结，本表回指）：

**(a) 类型名闭集**——`is_workflow_event_type`（声明
`include/mira/workflow_events.hpp:28`、闭集
`src/workflow/workflow_events.cpp:207-219`）接受的 20 个 CamelCase 事件类型名；
闭集外加法扩展按 DEC-002 版本化并回登本表 minor。

**(b) wire schema 名清单**——每类事件的载荷 `schema` 成员精确串
（`mira.workflow.<kebab>.v1`，逐一产生于 `src/workflow/workflow_events.cpp`
各 `to_event_payload`）；类型名与 schema 名一一对应：

| # | 类型名（闭集 a） | wire schema 名（清单 b） |
| --- | --- | --- |
| 1 | `WorkflowRunStarted` | `mira.workflow.run-started.v1` |
| 2 | `WorkflowStepStarted` | `mira.workflow.step-started.v1` |
| 3 | `WorkflowStepSettled` | `mira.workflow.step-settled.v1` |
| 4 | `WorkflowRunSettled` | `mira.workflow.run-settled.v1` |
| 5 | `WorkflowPatchProposed` | `mira.workflow.patch-proposed.v1` |
| 6 | `WorkflowPatchApplied` | `mira.workflow.patch-applied.v1` |
| 7 | `WorkflowPatchRejected` | `mira.workflow.patch-rejected.v1` |
| 8 | `WorkflowPolicySwitched` | `mira.workflow.policy-switched.v1` |
| 9 | `WorkflowDecisionRaised` | `mira.workflow.decision-raised.v1` |
| 10 | `WorkflowDecisionResolved` | `mira.workflow.decision-resolved.v1` |
| 11 | `WorkflowPublishProposed` | `mira.workflow.publish-proposed.v1` |
| 12 | `WorkflowPublishApplied` | `mira.workflow.publish-applied.v1` |
| 13 | `WorkflowPublishRejected` | `mira.workflow.publish-rejected.v1` |
| 14 | `WorkflowNavigationPlanned` | `mira.workflow.navigation-planned.v1` |
| 15 | `WorkflowNavigationObserved` | `mira.workflow.navigation-observed.v1` |
| 16 | `WorkflowEpisodeRecorded` | `mira.workflow.episode-recorded.v1` |
| 17 | `WorkflowLessonRecorded` | `mira.workflow.lesson-recorded.v1` |
| 18 | `WorkflowRecoveryAttempted` | `mira.workflow.recovery-attempted.v1` |
| 19 | `WorkflowToolCompatDegraded` | `mira.workflow.tool-compat-degraded.v1` |
| 20 | `WorkflowProceduresSynced` | `mira.workflow.procedures-synced.v1` |

| 项 | 冻结值 |
| --- | --- |
| 引用目录 | 上表 (a)+(b) 与 `mira.policy.*.v1` T1 子集九类（`include/mira/temporal_policy.hpp:228-242`，逐类精确名见 [M26](../plans/m26-temporal-policy-stage-t1.md) §4.4 事件表） |
| 定位形态 | （schema 名，`EventEnvelope` 信封定位）二元组；信封含 `event_id`、`runtime_id`/`session_id`/`session_sequence` 帧（`include/mira/event_store.hpp:48-60`） |
| 有效性域 | EventStore 事实源（`RULE-07`/[DEC-003](../decisions/DEC-003-event-sourced-persistence.md)）；引用必须可经事件重放定位 |
| 等价规则 | schema 名与信封定位均相等为同一源事件 |
| 载荷纪律 | 引用只携带定位与 digest/有界摘要，不内联复制载荷内容（DEC-022 §5 摘要纪律延伸）；闭集外加法扩展按 DEC-002 版本化并回登本表 minor |
| 消费者 | DEC-038 L1 `source_refs`；DEC-041 World State 更新条目的源事件摘要与重建输入（M27） |

## 3. 非基线成员的映射规则

以下成员**不进 v1 基线**（[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)
决策 3），只登记映射规则；收录为 v1.1 加法候选（DEC-002 minor），最迟冻结点 =
各自消费者立项里程碑。

- **Memory 域**：`MemoryScope.subject_id`/`MemoryKind` 闭集
  （`include/mira/memory_contracts.hpp:37-52`）。subject_id 是记忆访问控制域，
  不是环境实体身份；`MemoryRecord.provenance` 以 `EventId` 引用 2.4 族
  （:84-104），记忆域对环境实体的引用经 provenance 溯源，不需要第二身份。
- **Temporal Policy `entity_key`**（`include/mira/temporal_policy.hpp:124-135`，
  [M26](../plans/m26-temporal-policy-stage-t1.md) §4.2 阶段局部标识）：T1 契约
  不改写；T2+ Temporal Policy 消费 World State 时经**显式映射函数**把投影实体引用
  （2.1/2.2）映射为 `entity_key` 供给，映射规则随该消费立项冻结并回登本表；
  禁止反向把 `entity_key` 提升为跨投影身份。
- **`WorkflowTrajectoryStep`**（`include/mira/workflow_compiler.hpp:46-59`）：契约
  不含环境实体引用字段，不承载词表；[DEC-038](../decisions/DEC-038-unified-behavior-trace.md)
  §决策 3 的抽取编排产出该契约即可，不为其改 schema。

## 4. 版本化与演进

- 词表整体按 [DEC-002](../decisions/DEC-002-public-contract-versioning.md) 演进：
  v1.0 为本表；**新增身份域或条目 = minor**（不改变既有条目语义，旧消费者不受
  影响）；**改变既有条目的身份域、有效性域或等价规则 = major**（旧 reader 按
  DEC-002 reader 规则 fail closed）。
- 变更路径：DEC 评审（项目管理规范 §8）→ 本文更新（版本号与更新日期）→
  [术语表](../project/glossary.md)同步 → 消费方契约与测试同步。
- 目录条目（2.4 类型名闭集与 schema 名清单）的加法扩展同时受各自事件族的既有
  版本化纪律约束（`mira.workflow` 事件族的类型名闭集函数
  `is_workflow_event_type`、`mira.policy.*.v1` 事件表），本表登记
  不替代其自身门禁。

## 5. 消费者与冻结点

| 消费者 | 消费身份域 | 冻结点 |
| --- | --- | --- |
| DEC-041 World State 首阶段 | 2.1、2.2、2.3、2.4 | [M27](../plans/m27-world-state-projection-core.md)（2026-09-28 立项跑前冻结，门禁 `WS-G1` 词表目录一致性） |
| DEC-038 L1/L2 首阶段 | 2.1、2.2、2.3、2.4 | 首阶段里程碑尚未立项（不预分配编号）；其门禁必须引用本表且不得定义第二套引用形态 |
| DEC-040 资产层（TR0 已交付） | 2.3 | 引用语法的权威冻结面在 [Tool 引用与 Skill 设计](tool_reference_and_skill_design.md) §4，本表只登记对齐关系，不改写其定义 |
| Temporal Policy T2+（消费投影时） | 2.1/2.2 → `entity_key` 显式映射 | 随该消费立项冻结（§3） |

## 6. 已知限制

- **跨帧实体同一性不在词表内**：2.1 是证据引用而非永久身份（observation.hpp:343-344），
  跨帧 identity 归感知侧（[DEC-041](../decisions/DEC-041-session-world-state-projection.md)
  §非目标）。会话内「同一实体」的存活/陈旧语义是消费方（World State）在引用之上
  维护的状态，不是词表宣称的身份。
- **`Fused` 来源无生产者**（DEC-033 未实现，受 `MNT-202609-27` 证据门禁）：含
  `Fused` 来源的引用当前不可能真实出现；测试面只覆盖契约形状与拒绝路径。
- **无结构证据场景表达力有限**：实体引用只能来自宿主供给，词表不为此增加契约外
  表达（DEC-041 §影响与风险已披露，RULE-10）。

## 7. 测试策略

- 词表目录一致性（两层断言）：**(a) 类型名闭集**——`is_workflow_event_type`
  对 §2.4 表 20 个 CamelCase 类型名逐一接受、闭集外名拒绝；**(b) schema 名
  清单**——§2.4 表 20 个 wire schema 名逐一经各事件序列化产物（`schema` 成员）
  精确断言（M26 T1-G1 事件表断言同型），`mira.policy.*.v1` 九类经
  `policy_event_schema_name` 比对。词表 §2.1 校验拒绝路径（关键字段缺失/零值）、
  §2.2 空 `state_id`/`app_id` 拒绝经投影输入路径覆盖；§2.3 冒烟由链接
  `Mira::workflow` 的测试目标调用权威 `parse_tool_reference` 断言登记一致
  （接受/拒绝样例；非复刻实现，权威解析矩阵仍在 M7 TR0 既有测试）。随
  [M27](../plans/m27-world-state-projection-core.md) 门禁 `WS-G1` 在
  `tests/m27/` 落地。
- 后续消费方（DEC-038 首阶段、T2+ 映射）各自门禁引用本表并覆盖其新增形态的
  接受/拒绝路径。

## 8. 关联决策、计划与文档

- [DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)（收录范围与效力）、
  [DEC-002](../decisions/DEC-002-public-contract-versioning.md)（版本化）、
  [DEC-003](../decisions/DEC-003-event-sourced-persistence.md)（源事件可重放定位）、
  [DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)（state_id
  域）、[DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)（ElementRef
  基座）、[DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)
  （toolref 词表）
- [M27](../plans/m27-world-state-projection-core.md)（词表对齐共享冻结节 + 词表
  目录门禁）
- [World State 投影设计](world_state_projection_design.md)（首个消费方专项设计）、
  [术语表](../project/glossary.md)（实体词表词条）
