# DEC-014：Agent Harness 控制平面与 Workflow 数据平面双路径架构

> 状态：Accepted  
> 日期：2026-09-07  
> 负责人：Mira Maintainers  
> 冻结里程碑：架构方向即日生效；落地范围与关键路径待 M7 重定义（暂定）  
> 替代/被替代：无（作为 [DEC-011](DEC-011-demo-first-external-validation.md) 遗留的
> 交付边界重定义问题提供方向输入）

## 背景与问题

M0–M4 已交付平台无关 Agent Core 与可中断的 `Observe -> Reason -> Plan -> Act -> Verify`
闭环。DEC-011 终止 M5/M6、挂起 M7，规定后续能力是否以及以何种范围进入交付边界，由独立
demo 仓库的验证证据重新定义。

2026-09 的产品与架构思考（固化于
[Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)）
得出结论：Mira 的目标不是纯 GUI Agent（每次任务都由模型逐步决策），也不是纯脚本自动化
工具，而是双路径系统——Agent 负责控制、理解、规划、交互与修复，Workflow 负责高频、
稳定、低成本执行。若不将该方向固化为决策，后续范围讨论会反复在「更强的 Agent」与
「更稳的自动化」之间摇摆，里程碑重定义缺乏裁决依据。

## 决策

1. **定位**：Mira 是 Agent Harness + Workflow Compiler + Automation Runtime +
   Persistent Memory System，面向 GUI 环境与个人设备。
2. **双路径**：保留完整的 Agent-native 路径（会话、上下文、工具调用、Agent Loop、
   用户介入、恢复），新增 Workflow-native 路径（成功经验固化为可参数化、可独立执行的
   Workflow，已知任务优先确定性执行）。
3. **控制平面/数据平面**：Agent Harness 是常驻系统控制平面，不是只在 Workflow 失败时
   出现的临时兜底；Workflow Runtime 是执行数据平面；任务可在两者之间任意切换。
4. **Workflow 是一等公民**：版本化、可编辑、可参数化、可脱离模型独立执行，支持执行
   策略（Strict / Recoverable / AgentAssisted / Interactive / DryRun）与人工介入；
   用户可通过 Conversation 完成 pause / 修改参数 / skip / resume / cancel 与默认值固化。
5. **GUI 导航与任务逻辑解耦**：App Model（UI 状态图）作为长期环境知识；Workflow 表达
   What，App Model 表达 Where/How，Runtime 表达 How to execute；已知图用图搜索，未知
   状态与任务交给 Agent。
6. **Memory 分层**：按环境模型 / 用户模型 / 程序性记忆 / 情景记忆组织（演进自 M4 的
   MemoryScope/MemoryKind，不推翻既有契约）；Agent 成功经验尽可能沉淀为 Workflow、
   Skill、App Model、Recovery Pattern 等长期资产。
7. **资产与不变量**：新增 `W-01` 至 `W-08` 架构不变量（见设计文档第 4 节）；其中
   EventStore 仍是唯一事实源，App Model 与 Workflow 索引是可重建投影；模型输出不得
   直接成为 Workflow 变更，入库前必须验证。
8. **与现状的关系**：现有 M0–M4 资产是 Agent-native 路径的基础，复用而非重写；本决策
   不改变 v1 交付边界现状、不解冻 M7、不自动恢复 M5/M6；落地以 M7 重定义或新增里程碑
   承载。

## 备选方案

- **纯 Agent 路径**（每次任务由模型逐步决策）：否决作为唯一路径。成本高、稳定性差、
  无法把成功经验转化为低成本可复用执行。Agent 路径本身保留，作为探索、恢复与未知任务
  的路径。
- **纯 Workflow/脚本引擎**（Agent 仅离线维护 Workflow）：否决。与「用户随时对话介入」
  「未知情况升级决策」的产品目标冲突；Agent 必须常驻控制平面。
- **Workflow 仅作为普通工具、不设独立 Runtime 层**：否决。执行策略、运行中介入、恢复
  continuation、版本化与数据平面语义超出 Tool 通道职责；但 Workflow 的调用入口仍经
  Tool/Decision 通道（设计文档 6.5 节），不新增绕过校验的模型旁路。

## 影响与风险

- **范围显著扩大**：Workflow IR/编译/运行时、App Model、Memory 演进、对话介入均需
  专项设计与后续 DEC（Workflow IR schema、WorkflowRun 状态映射、执行策略默认值、导航
  代价模型、Conversation 工件脱敏与保留）。
- **Workflow 脆弱性**：selector 与 UI 假设随 App 更新失效。缓解：版本化、验证后入库、
  Dry Run、Agent 恢复 continuation；无验证证据不固化。
- **归纳错误**：从少量样本抽象出错误参数化。缓解：归纳是提议而非事实，验证失败回退
  既有版本。
- **第二事实源风险**：App Model 与 Workflow Library 可能膨胀为平行状态。缓解：`W-03`
  强制投影可重建，EventStore 权威性不变。
- **App Model 维护成本未知**：以真实应用的维护性度量后再扩大投入（设计文档第 16、17
  节阶段化）。
- **对话 patch 的安全与歧义**：patch 经与 Decision 相同的校验与确认管线，不绕过权限
  （DEC-004）。

## 验证方式

- 方向级：本决策与设计文档评审通过。
- 落地级（由后续里程碑退出条件承载）：首个 Workflow 编译 -> Dry Run -> 执行 -> 恢复 ->
  版本化闭环；对话 patch 的幂等、权限与回退测试；导航图搜索与 App Model 投影重建
  测试；WorkflowRun 取消/终态幂等/迟到完成隔离测试；OfflineReplay 不重放副作用。

## 关联文档和工作项

- 设计：[Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
- 计划：[Mira 实施总计划](../plans/mira-implementation-plan.md)（第 4、5 节已同步）
- 决策：[DEC-001](DEC-001-runtime-executor-ownership.md)、
  [DEC-002](DEC-002-public-contract-versioning.md)、
  [DEC-003](DEC-003-event-sourced-persistence.md)、
  [DEC-004](DEC-004-security-authority-confirmation.md)、
  [DEC-009](DEC-009-tool-module-boundary.md)、
  [DEC-011](DEC-011-demo-first-external-validation.md)
- 工作项：M7 重定义或新增里程碑承载（待定；阶段划分见设计文档第 16 节）
