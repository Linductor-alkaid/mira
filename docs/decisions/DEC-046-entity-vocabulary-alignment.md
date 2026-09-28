# DEC-046：跨投影实体词表对齐——ElementRef / App Model state_id / toolref / L0 事件引用的共享词表 v1

> 状态：Accepted
> 日期：2026-09-28
> 决策人：Mira Maintainers
> 需求来源：[DEC-038](DEC-038-unified-behavior-trace.md) §影响与风险与
> [DEC-041](DEC-041-session-world-state-projection.md) §决策 5 共同点名的
> 「实体词表对齐」立项前置；总计划 §4.1 第 19 条「DEC-038/DEC-041 首阶段须先做
> 实体词表对齐并补专项设计」；
> [M26](../plans/m26-temporal-policy-stage-t1.md) §4.2 留痕「`entity_key` 为阶段
> 局部标识、词表对齐点随 DEC-038/DEC-041 首阶段冻结处理」
> 上位决策：[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)
> 关联决策：[DEC-002](DEC-002-public-contract-versioning.md)、
> [DEC-003](DEC-003-event-sourced-persistence.md)、
> [DEC-005](DEC-005-observation-coordinate-host-boundary.md)、
> [DEC-019](DEC-019-workflow-ir-contract.md)、
> [DEC-027](DEC-027-app-model-contract-and-confidence.md)、
> [DEC-033](DEC-033-hybrid-visual-grounding.md)、
> [DEC-038](DEC-038-unified-behavior-trace.md)、
> [DEC-040](DEC-040-tool-reference-and-skill-layer.md)、
> [DEC-041](DEC-041-session-world-state-projection.md)
> 关联计划：[M27](../plans/m27-world-state-projection-core.md)（DEC-041 首阶段）
> 冻结里程碑：M27

## 背景与问题

两个已冻结方向的投影即将进入实现：DEC-038 的 L1 语义行为单元要携带「源事件引用 +
实体引用」，DEC-041 的会话 World State 要维护「页面假设 + 存活实体引用」。两份决策
都把实体引用指向同一组既有契约语义（`ElementRef`、App Model `state_id`），并把
「词表对齐」列为共同立项前置（DEC-038 §影响与风险「否则两个投影会各自漂移」、
DEC-041 §影响与风险「任何一侧先行都必须预留对齐点」）。同时，M26 已交付的
Temporal Policy T1 在 `TrackedEntity.entity_key`（`include/mira/temporal_policy.hpp:124-135`）
落下了第一处阶段局部实体标识，`PolicyWorldView` 是 T1 局部契约
（[M26 文件](../plans/m26-temporal-policy-stage-t1.md) §4.2 草案偏差注记）。

若不先冻结一份对齐载体就立项任一投影，会出现第三处局部词表；若由两份首阶段
里程碑文件分头冻结同一词表，则同一词表出现两个冻结主体——这正是 DEC-038 §备选方案
要消除的「多个互不一致的语义表示」漂移形态。

需要决策：词表由哪份文档承载、v1 基线收录哪些既有契约实体集、谁不进基线而以映射
规则登记、按什么纪律演进、经何种评审冻结。

## 决策

1. **单一承载文档 + 专项决策，不分散冻结**。跨投影实体词表由
   [实体词表设计](../design/entity_vocabulary_design.md)作为唯一冻结承载（身份域、
   有效性域、等价规则、版本化纪律），本决策冻结其收录范围与效力；DEC-038/DEC-041
   各自首阶段里程碑文件**引用**该词表，不得复制或另立冻结面。本决策按项目管理
   规范 §8 建立（跨模块公开契约词汇变化必须建 DEC）；词表的具体结构属设计文档
   职责，随本决策同批交付并随 M27 立项跑前冻结。

2. **v1 基线 = 四组既有已冻结契约实体集**（全部已实现、有契约/golden 测试的公开面；
   本决策只做对齐登记，不改变任何既有契约的 schema 与语义）：
   - **① ElementRef 族（环境实体证据引用）**：`ElementSource`/`ElementRef`/
     `PerceptionEvidence`/`AppContext`（`include/mira/observation.hpp:341-375`）。
     冻结定义以 [Observation 坐标设计](../design/observation_coordinate_android_host.md)
     §4.3（:196-211）为准：只在单一 Observation/epoch 与 freshness policy 内有效，
     是证据、不是永久身份；[DEC-033](DEC-033-hybrid-visual-grounding.md) 决策 2
     已确认 `GroundingRegion` 以 `PerceptionEvidence`/`ElementRef` 为基座。
   - **② App Model state_id 族（页面状态假设引用）**：`AppModelState.id`、
     `AppModelTransition.from_state`/`to_state`
     （`include/mira/workflow_navigation.hpp:111-133`）、宿主识别边界
     `ScreenStateSnapshot.state_id`（:228-231，DEC-027 §3）与导航观测事件载荷的
     `from_state`/`to_state`（`include/mira/workflow_events.hpp:165-187`——
     DEC-041 §决策 1 点名的重建输入）。身份域为 App Model 文档单命名空间
     （DEC-027 v1）；跨文档不宣称同一性。
   - **③ 工具引用族（资产层工具逻辑身份）**：`toolref:<wire-name>[@digest]`
     引用语法 v1 与受治理词表字符集
     （[Tool 引用与 Skill 设计](../design/tool_reference_and_skill_design.md) §4.1，
     :96-115，权威冻结面在该文档，本词表只登记对齐关系）；`ToolCall` 步骤
     `arguments["tool"]` 保留成员（:27、:379）。[DEC-040](DEC-040-tool-reference-and-skill-layer.md)
     §影响与风险已冻结「Trace L1 的工具调用引用应使用同一引用词表」。
   - **④ L0 事件引用族（源事件引用）**：`mira.workflow` 事件族 20 类，目录
     两层并都在词表 §2.4 逐名冻结——(a) CamelCase 类型名闭集
     （`is_workflow_event_type`，声明 `include/mira/workflow_events.hpp:28`、
     闭集 `src/workflow/workflow_events.cpp:207-219`）与 (b) wire schema 名
     清单（20 个 `mira.workflow.<kebab>.v1` 串，逐一产生于各
     `to_event_payload`）；加 T1 子集九类 `mira.policy.*.v1`
     （`include/mira/temporal_policy.hpp:228-242`、
     [M26 文件](../plans/m26-temporal-policy-stage-t1.md) §4.4）与 EventStore
     信封定位（`event_id` + 会话序列帧，
     `include/mira/event_store.hpp:48-60`）。它是 L1「源事件引用」与投影重建的
     引用基座（RULE-07/W-03：引用必须可经 EventStore 重放定位，载荷内容以
     digest/摘要引用、不内联复制）。

3. **不进 v1 基线、以映射规则登记**（词表设计 §3 承载映射规则；收录为 v1.1 加法
   候选，DEC-002 minor 纪律，最迟冻结点 = 各自消费者立项里程碑）：
   - **Memory 域**：`MemoryScope.subject_id`/`MemoryKind` 闭集
     （`include/mira/memory_contracts.hpp:37-52`）——subject_id 是记忆访问控制
     域，不是环境实体身份；`MemoryRecord.provenance` 本就以 `EventId` 引用 ④ 族
     （:84-104），无需二次身份。
   - **Temporal Policy `entity_key`**（`include/mira/temporal_policy.hpp:124-135`）：
     M26 已冻结的阶段局部标识。对齐规则：T1 契约不改写；T2+ Temporal Policy 消费
     World State 时经显式映射函数把投影实体引用映射为 `entity_key` 供给，映射规则
     随该消费立项冻结；禁止反向把 `entity_key` 提升为跨投影身份。
   - **`WorkflowTrajectoryStep`**（`include/mira/workflow_compiler.hpp:46-59`）：
     契约不含环境实体引用字段，不承载词表；DEC-038 抽取编排产出该契约即可
     （DEC-038 §决策 3），不为其改 schema。

4. **等价规则与身份边界**：词表内「同一引用」的判定按各身份域冻结（设计文档 §2）：
   ElementRef 按字段全等（含 `evidence_digest`）；state_id 按文档内字符串身份；
   toolref 按规范形态（钉住为小写十六进制）字符串身份；事件引用按
   （schema 名，信封定位）二元组。**词表不宣称跨帧实体同一性**——那是感知侧职责
   （DEC-041 §非目标；DEC-033 未交付前 `Fused` 来源无生产者，词表只冻结契约形状）。

5. **版本化纪律**：词表是公开契约面，按 [DEC-002](DEC-002-public-contract-versioning.md)
   major/minor 演进：新增身份域或条目 = minor；改变既有条目的身份域、有效性域或
   等价规则 = major（旧 reader 按 DEC-002 reader 规则 fail closed）。词表变更走
   DEC 评审 + 设计文档更新 + [术语表](../project/glossary.md)同步。

## 备选方案

- **并入 [Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
  新增「实体词表」章节，不新建决策**：该文档是总纲定位（W-01–W-08 架构纪律），
  塞入跨域词表稀释职责；且跨 DEC-038/DEC-040/DEC-041 三个决策面的公开契约词汇
  变化按项目管理规范 §8 必须走 DEC 评审，改总纲绕不开决策环节；还与 DEC-038/DEC-041
  已冻结的「专项设计随首阶段立项交付」安排不一致。不采用。
- **词表分别写进 DEC-038 与 DEC-041 各自首阶段里程碑文件内冻结、互引对齐**：
  同一词表出现两个冻结主体 = 双事实源，谁先立项谁先冻——正是 DEC-038 §备选方案
  否决的漂移形态，也与 DEC-041 §影响与风险「任何一侧先行都必须预留对齐点」冲突；
  M26 的 `entity_key` 局部标识已经表明分头冻结会持续制造局部词表。不采用。
- **等待 DEC-033 感知管线交付跨帧身份后再冻结词表**：DEC-033 实现受
  `MNT-202609-27` 外部证据门禁，等待会把两个无外部依赖的确定性投影一并卡死；
  且 DEC-041 §非目标明确不做跨帧视觉身份——跨帧身份不是本词表的收录对象。不采用。

## 影响与风险

- 词表冻结即公开兼容承诺（DEC-002）：installed-consumer 覆盖面随收录范围增长，
  v1 刻意只收录四组已有冻结定义与测试的契约面，Memory/entity_key 以映射规则登记
  而不进基线，控制首阶段评审面。
- `Fused` 来源 `ElementRef` 当前无生产者（DEC-033 未实现）：v1 只冻结契约形状，
  不宣称来源语义已实证（RULE-10）；词表设计文档与 M27 交付文档须如实披露。
- 实体身份稳定性依赖感知侧：无结构证据场景的实体引用只能来自宿主供给，投影在该
  场景下表达力有限（DEC-041 §影响与风险已披露），词表不增加超出契约的表达承诺。
- `entity_key` 的二元性延续到 T2+：Temporal Policy 后续消费投影需一次显式映射；
  拖延映射会在使用面扩大后抬升对齐成本（映射规则最迟冻结点见决策 3）。
- DEC-038 首阶段尚未立项：其里程碑必须引用本词表（L1 实体引用、源事件引用、
  工具调用引用三处一致性进其门禁），在立项前 L1 不产生第二套引用形态。

## 验证方式

- 词表目录一致性测试随 M27 门禁冻结（`tests/m27/`）：①②④ 族两层断言——
  (a) 类型名闭集（`is_workflow_event_type` 对词表 §2.4 表 20 名逐一接受/闭集外
  拒绝）× (b) schema 名清单（20 个 `mira.workflow.<kebab>.v1` 串逐一经各事件
  序列化产物断言；九类 `mira.policy.*.v1` 与 `policy_event_schema_name` 一致）、
  ElementRef/state_id 校验拒绝路径；③ 族对齐冒烟由链接 `Mira::workflow` 的
  测试目标调用权威 `parse_tool_reference` 断言登记一致（core 消费者不复刻解析
  ——权威解析器实现在 workflow 模块，core 反向依赖被依赖方向禁止；权威解析
  矩阵已在 M7 TR0 测试）。
- DEC-038 首阶段立项评审时核对其门禁引用本词表且无第二套引用形态定义。
- 词表条目的后续演进按 DEC-002 schema 兼容性测试纪律（current/previous fixtures）。

## 关联文档和工作项

- [M27](../plans/m27-world-state-projection-core.md)（DEC-041 首阶段；词表对齐
  共享冻结节随其立项落地，词表目录门禁 `WS-G1`）
- [实体词表设计](../design/entity_vocabulary_design.md)（唯一冻结承载）、
  [World State 投影设计](../design/world_state_projection_design.md)（消费方
  专项设计）
- [DEC-002](DEC-002-public-contract-versioning.md)（版本化纪律）、
  [DEC-003](DEC-003-event-sourced-persistence.md)/`RULE-07`（源事件引用可重放
  定位）、[DEC-027](DEC-027-app-model-contract-and-confidence.md)（state_id 身份
  域）、[DEC-033](DEC-033-hybrid-visual-grounding.md)（ElementRef 基座与 Fused
  来源）、[DEC-040](DEC-040-tool-reference-and-skill-layer.md)（toolref 词表）、
  [DEC-038](DEC-038-unified-behavior-trace.md)/[DEC-041](DEC-041-session-world-state-projection.md)
  （共同前置的两投影）
- [术语表](../project/glossary.md)（World State / 实体词表词条随 M27 落地）
