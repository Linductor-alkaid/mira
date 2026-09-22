# Mira 公共术语表

> 状态：Active
> 版本：1.1
> 更新日期：2026-09-23
> 适用范围：Mira 全部文档、代码命名、事件与日志、评审和协作沟通

## 1. 目的与效力

本文统一 Mira 的领域词汇：每个词条给出权威定义和应避免的混用词
（`_Avoid:_`）。文档、代码、事件 schema 与评审意见必须按本表用词；
设计文档中的术语与本表冲突时，以设计文档的冻结定义为准并回改本表。

新增公共概念时先在本表登记，再写设计与代码。词条不承载完整语义——
每条链接到权威设计文档或 API 手册。

## 2. 运行时与生命周期

权威来源：[核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)、[Runtime 设计](../design/mira_runtime_design.md)。

**Runtime（运行时）**:
进程内 Agent 能力的顶层协调者，拥有 Session 的创建与串行提交语义。
命令遵循 submission → receipt → settlement 三阶段。
_Avoid_: "引擎"、"框架"、"daemon"

**Session（会话）**:
一次有界交互的容器，聚合 Task、事件流与上下文预算；拥有稳定
`SessionId` 与单调 `SessionSequence`。
_Avoid_: 对话（Conversation 是事件投影，不是会话本体）、连接

**Task（任务）**:
一个可取消、可观测、有终态幂等保证的目标执行单元，挂在 Session 下。
_Avoid_: 作业（job）、run（Workflow 专用）、线程同义词

**Step（步）**:
Task 内一次 Observe → Reason → Plan → Act → Verify 的有界推进单元。
_Avoid_: iteration、回合（Turn 不是 Mira 术语）

**Operation（操作）**:
可接纳、可取消的最小执行请求（命令的动作面），拥有 `OperationId`
与 admission 语义。
_Avoid_: action（模型决策语义）、command（命令是请求面，操作是执行面）

**终态幂等（Terminal Idempotency）**:
`Completed`/`Failed`/`Cancelled` 等终态一旦落定，迟到结果不得使实体
重新进入活动状态。
_Avoid_: "最终一致"（两者无关）

**Human Takeover（人类接管）**:
阻止新的自主动作进入环境、取消或安全收敛在途连续控制，并在恢复前
强制重新观察的运行时转换。
_Avoid_: 暂停（Pause 不含控制收敛语义）、人工审核

**EnvironmentEpoch（环境纪元）**:
环境侧维护的单调计数：拓扑、权限或宿主会话任何不连续都会递增并使
旧坐标与旧观察失效。
_Avoid_: 版本号、时间戳

## 3. Observation、模型与动作

权威来源：[Runtime 设计](../design/mira_runtime_design.md)、[实时控制设计](../design/realtime_control_design.md)、[模型层与 Agent Loop API](../api/model-agent-loop.md)。

**Observation（观察）**:
环境的可扩展快照：截图、UI/Accessibility Tree、前台应用、设备状态、
时间戳与历史动作；不假设所有平台有结构化 UI 信息。
_Avoid_: 截图（只是其一种载荷）、状态（状态是派生概念）

**Decision（决策）**:
模型输出经解析和验证后的结构化结果；原始模型文本不得直接驱动平台
输入。
_Avoid_: 回复（response）、意图（Intent 是其中一类）

**ActionIntent（动作意图）**:
高层动作语义（如 `JoystickIntent`），由 Native Controller 转换为有
时间边界的轨迹后才可交给 `IInputProvider`。
_Avoid_: 直接把意图当输入事件下发

**Verify（验证）**:
动作执行后的重新观察或低成本检测信号比对；输入 API 返回成功不构成
验证。
_Avoid_: 确认（Confirmation 是安全授权语义，见第 5 节）

**AgentLoop（智能体闭环）**:
Session 内托管"步推进 + 工具调用 + 用户介入注入"的离散闭环组件。
_Avoid_: run loop、主循环（暗示无界循环，违反有界步进）

## 4. Tool 模组体系（DEC-009 / DEC-039 / DEC-040）

权威来源：[Tool 模组设计](../design/tool_module_design.md)、[Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md)、[MCP 准入设计](../design/mcp_tool_admission_design.md)。

**ToolModule（工具模组）**:
带 manifest（`mira.tool_module.manifest.v1`）的工具打包与准入单元，
经 fail-closed 验证与确定性协商后进入 Registry。
_Avoid_: 插件（plugin 未在 Mira 语境定义）、动态库

**CapabilityCatalog（能力目录）**:
受治理能力词汇的集合；模组 manifest 引用的 capability 必须存在于
Catalog，未知 ID 即验证失败。
_Avoid_: 权限列表（权限是安全域语义）

**协商（Negotiation）**:
`EnvironmentCapabilities` 与 CapabilityCatalog 之间的确定性纯函数
匹配，产出协商 digest；同输入必同输出。
_Avoid_: 动态发现（运行期动态注册在 DEC-009 被否决）

**模组生命周期（Module Lifecycle）**:
`Discovered -> Verified -> Staged -> Active -> Deprecated -> Revoked`，
失败进入 `Quarantined`；运行期只降级、revoke 留 tombstone。
_Avoid_: 热插拔（非目标）

**ExposedToolSpec（暴露工具规格）**:
协商结果经投影得到的 per-request LLM 工具暴露面，绑定
`tool_snapshot_digest` 与跨模组唯一的 `wire_name`。
_Avoid_: 工具清单（无绑定语义）

**ToolReference（工具稳定引用）**:
Workflow 内对工具的版本化引用语法 v1：钉住 spec digest 或跟随最新
可用版本，受治理词表字符集约束。
_Avoid_: 版本号引用（钉的是 digest 而非版本号）

**兼容状态投影（Compatibility Projection）**:
引用的 `Runnable` / `Degraded` / `Invalid` 三态确定性重算；`Invalid`
阻断准入，`Degraded` 留审计投影。TR2 起 `create_run` 在派发策略下强制
消费该投影（挂载清单或 IR v1.1 引用表达触发）。
_Avoid_: 状态机（投影是可重建的派生视图，不是状态机）

**Tool Refs 挂载（Tool Refs Mount）**:
宿主显式锚定到库版本（workflow_id + content digest）的引用清单工件，
由 `WorkflowRuntime` 保存并在 Run 准入时消费；同版本重挂同 digest 幂等、
异 digest 拒绝，容量有界。
_Avoid_: 兼容状态存储事实（挂载的是清单工件，状态仍按需重算）、自动挂载

**Skill 调用（Skill Invocation）**:
Skill 经同一 `BuiltinToolRegistry` 与 DEC-015 门禁暴露为 Tool，其执行是
经库路径创建的子 Workflow run（runnable 门禁、兼容门与逐步校验无豁免，
嵌套深度有界）。
_Avoid_: 第二执行通道、已发布即豁免门禁

**Skill（技能）**:
以显式发布为界、钉住源 Workflow id + `ir_digest` 的可复用过程资产
（`mira.skill.descriptor.v1`），经 `SkillPublicationRegistry` 的
发布/升级/撤销生命周期管理。
_Avoid_: 宏、脚本、模板

**Procedure 索引（Procedure Index）**:
以显式发布为界、无时钟、可重建的 `mira.skill.procedure_index.v1`
投影。
_Avoid_: 搜索索引（不涉相关性排序）

**部署窗（Deployment Window）**:
只允许在部署边界发生模组准入与 Skill 发布变更的阶段划分；运行期
只降级。
_Avoid_: 热更新窗口

## 5. 安全与确认（DEC-004 / DEC-018）

权威来源：[威胁模型与确认](../security/threat_model_and_confirmation.md)、[安全与权限 API](../api/security.md)。

**Principal（主体）**:
安全域中发起动作的已认证实体，是能力授权与风险分级的对象。
_Avoid_: 用户（UserId 只是 Principal 的一种）

**Human Confirmation（人类确认）**:
高风险动作的安全授权机制（challenge/response），与 Verify 的结果
校验语义无关。
_Avoid_: 与 Verify、Takeover 混用

**PolicyEngine（策略引擎）**:
对 `PolicyInput` 做确定性 `PolicyDecision` 评估的组件；模组准入不
构成授权。
_Avoid_: 权限管理器（授权语义更宽）

**脱敏（Redaction）**:
凭据、授权头、密码与输入法敏感内容不得进入日志与事件的强制规则；
`Error.safe_message` 已脱敏可直接入日志。
_Avoid_: 加密（加密不等于不出现在日志）

**操作准入（Operation Admission）**:
已接受的 busy/running 输入由串行 admission 语义裁决；迟到的用户
输入不得复活已取消操作。
_Avoid_: 消息队列（无 admission 语义）

## 6. 事件、存储与恢复（DEC-003 / DEC-038 / DEC-041）

权威来源：[事件与崩溃一致性设计](../design/event_artifact_crash_consistency.md)、[Context 与 Memory 设计](../design/context_and_memory_design.md)、[Context Curator 设计](../design/context_curator_design.md)。

**Event（事件）**:
带顺序号与时间戳的已记录事实，载荷可版本化、可序列化；大体积内容
用稳定引用与摘要。
_Avoid_: 日志行（日志是诊断，事件是事实源）

**EventStore（事件存储）**:
事件事实源的持久化组件；回放与投影均从它派生。
_Avoid_: 数据库泛称

**Replay（回放）**:
能区分已记录外部结果与真实副作用的确定性重演；默认不重新执行输入
动作或网络请求。
_Avoid_: 重跑（重执行副作用）

**Artifact（工件）**:
事件以引用方式挂载的大体积内容，持有 `ArtifactId` 与摘要。
_Avoid_: 附件（无版本与摘要语义）

**Working Context（工作上下文）**:
短期、任务导向、可频繁覆盖、可从 Trace Plane 重建的投影；不是第二
份 Memory。
_Avoid_: 会话记忆、缓存

**Subagent（子代理）**:
父会话旁由宿主创建的子 Session 中运行的推理角色代理（Developer/Test/
Review 等，[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)）；
无专属执行面，环境动作（如需）在其自身会话按既有租约纪律执行。
_Avoid_: 子线程、插件、第二 Runtime

**Snapshot Fork（快照分叉）**:
从父会话已提交快照确定性派生子会话只读基线的投影操作，副本携带 fork
溯源（父快照 id、父会话、fork 点水位）；不复用 epoch 的「作废」语义。
_Avoid_: 分支（Workflow 语义）、复制（无溯源与身份语义）

**Local Delta（局部增量）**:
子代理结束时回传父会话的带 provenance 提炼结果，按继承剔除、血统
supersede、addition 三分类；不是完整探索历史。
_Avoid_: diff（无语义分类）、摘要（无结构化状态语义）

**Merge Policy（合并策略）**:
父快照吸收子 delta 的机械确定性裁决：引用驱动、冲突不静默覆盖、产物
经既有提交管线；rejected 即不合并，子内容只存子会话投影与 Raw Trace。
_Avoid_: 模型自由合并（v1 无语义裁决）、自动同步

**Consolidation（固化）**:
把任务经验沉淀为长期记忆的学习过程（DEC-029/030）。
_Avoid_: 压缩（Context 压缩是预算语义）

**行为轨迹（Behavior Trace）**:
DEC-038 统一的任务执行事实轨迹平面，是 Working Context 等投影的
唯一事实源。
_Avoid_: 日志轨迹

## 7. Workflow 体系（M8–M14）

权威来源：[Workflow Runtime 设计](../design/workflow_runtime_design.md)、[Workflow 契约 API](../api/workflow-contracts.md)。

**Workflow（工作流）**:
经编译器校验的确定性过程定义，持有 `ir_digest`；版本化资产。
_Avoid_: 流程图、脚本

**WorkflowRun（工作流运行）**:
Workflow 的一次有状态执行实例，遵循冻结的转换表。
_Avoid_: Task（Task 是 Runtime 语义，Run 是 Workflow 语义）

**Task Induction（任务归纳）**:
从成功轨迹归纳参数化 Workflow 的学习机制（DEC-026）。
_Avoid_: 录制回放（无泛化语义）

## 8. 基础设施

权威来源：根 [AGENTS.md](../../AGENTS.md)（Executor 强制约束）、[Executor API 文档](../../third_party/executor/docs/API.md)、[架构治理](architecture_governance.md)。

**Executor（执行器）**:
`third_party/executor` 提供的强制并发与生命周期基础设施；所有异步、
延时、周期、阻塞 I/O 与实时任务必须经其公开能力管理。能力缺口按
`docs/executor_feedback/ledger.md` 登记。
_Avoid_: 线程池（实现细节）、自建调度器（禁止）

**Platform Adapter（平台适配器）**:
把 Android、Windows、Linux 等宿主能力接入 Core 接口的边界层；只做
最终映射，不承载规划或验证策略。
_Avoid_: 驱动、插件

**强类型 ID（Strongly-typed ID）**:
`MIRA_DEFINE_ID` 生成的不可互换 128-bit 标识；`TaskEpoch`、
`EnvironmentEpoch` 等单调计数器不是 ID。
_Avoid_: 整数句柄、字符串拼接 ID
