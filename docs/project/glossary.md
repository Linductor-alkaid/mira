# Mira 公共术语表

> 状态：Active
> 版本：1.7
> 更新日期：2026-10-29
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

**Supply Seam（供给缝）**:
Agent Loop 的可选只读快照供给依赖（DEC-045/M25）：宿主注入回调，请求
装配时经 Layer 0 转换消费当前会话已提交快照；只读、零自动化，写侧
纪律归既有提交管线。
_Avoid_: 自动接线（Loop 内自动化须上位决策）、第二写路径

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

**Claim Key（claim 键）**:
长期记忆 statement 归一化后的「主语」：形如 `subject=value` 的 statement 在第一个
`=` 处切分，不含 `=` 时 statement 自己就是主语；主语去首尾空白、折叠内部空白、折叠
大小写。两条记录讲同一件事，当且仅当主语相同且断言值相同（DEC-061）。
_Avoid_: 语义相似（claim key 是词法的，不做推理）、statement 相等（措辞不同可以
同 claim）、主键/primary key（存储标识，不是语义身份）

**行为轨迹（Behavior Trace）**:
DEC-038 统一的任务执行事实轨迹平面，是 Working Context 等投影的
唯一事实源。
_Avoid_: 日志轨迹

**World State（会话世界状态）**:
DEC-041 会话级「Runtime 当前 believed 环境认知」的确定性投影
（前台应用、页面假设、存活实体、最近变化）：从 L0 事件纯函数重建，
`Believed`/`Stale`/`Unknown` 显式三态；只是数据，不构成动作授权。
_Avoid_: 世界模型（无跨设备承诺）、环境事实源（EventStore 才是事实源）、
当前状态（与 App Model 长期图知识语义混淆）

**实体词表（Entity Vocabulary）**:
DEC-046 冻结的跨投影共享引用词表 v1：ElementRef 证据引用、App Model
`state_id` 页面身份、`toolref:` 工具引用、L0 事件引用四组身份域的
单一冻结承载（实体词表设计）；Memory `subject_id` 与 Temporal Policy
`entity_key` 以映射规则登记、不进 v1 基线。
_Avoid_: 各投影自定义引用形态（第二套词表）、永久实体 ID（ElementRef
是证据非身份）

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

权威来源：根 [AGENTS.md](../../AGENTS.md)（Executor 强制约束）、[kairo API 文档](../../third_party/kairo/docs/API.md)、[架构治理](architecture_governance.md)。

**Executor（执行器）**:
`third_party/kairo`（上游 Executor 已于 v0.6.0 更名 kairo）提供的强制并发与生命周期基础设施；所有异步、
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

## 9. Temporal Policy（DEC-037 / M26）

权威来源：[Temporal Policy 设计](../design/temporal_policy_design.md)、
[M26 里程碑](../plans/m26-temporal-policy-stage-t1.md) §4 冻结契约。

**Temporal Policy（时序条件策略）**:
把 Agent 低频探索中经重复验证的模式固化为本地确定性条件策略的机制
（DEC-037 Stage T1 由 M26 交付）；与 `PolicyEngine`（安全授权决策）、
`WorkflowPolicy`（Workflow 执行策略闭集）是不同层的三个概念，无继承、
替代或豁免关系。
_Avoid_: 策略引擎（授权语义）、实时性策略（实时性归 T6 门禁）

**ReactiveRule（反应式规则）**:
`mira.policy.rule.v1` 的闭集谓词规则资产：provenance 必备（RULE-07）、
经 Candidate → Testing → Runtime 证据晋升（RULE-10）、触发不是授权
（RULE-09 延伸）。
_Avoid_: 手写 if 分支（无 provenance 与生命周期的裸规则）

**PolicyRuntime（策略运行时）**:
`ReactivePolicyRuntime` 承载的确定性评估与宿主显式生命周期（归纳/采纳/
测试/晋升/降级/退役）；每 tick 有界同步、不读系统时钟、不建线程。
_Avoid_: 状态机引擎（HSM/BT 归 T5）、自治治理（晋升无自动面）

## 10. 统一交互与容量（DEC-068 / DEC-069）

权威来源：[DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)、
[DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)、
[M28 计划](../plans/m28-interaction-coordinator-first-stage.md)。

**Interaction Coordinator（交互协调层）**:
DEC-068 冻结的主体级协调组件，位于 Runtime 之上、宿主之下：持有主体身份、
接纳输入、裁决输入归属哪个内部 Session/Task、准备该工作的上下文、
归集结果并统一回复。它**不实现第二套状态机**，唯一地使用 Runtime 的既有命令面，
也不创建 Agent Loop。
_Avoid_: 会话路由器（协调层还负责上下文与结果关联）、第二 Runtime、
权限代理（授权语义归 PolicyEngine）

**Input Envelope（输入信封）**:
一次主体输入的接纳载荷：主体身份、宿主提供的稳定 `input_id`、内容与
回复/附件引用。`input_id` 是**幂等键**，不是内容哈希。
_Avoid_: 消息（Message 是 Loop 内的输入单元）、请求

**Input Ledger（输入台账）**:
持久化的输入幂等台账：记录 `input_id`、Mira 内部入口身份
（`ControlIngressId`）、接纳时间、归属与当前阶段，在声明窗口内阻止重复输入
产生重复副作用；窗口外的重复输入得到**明确过期拒绝**。
_Avoid_: Command 去重表（绑定 Task/Session 终态 retention）、缓存、
记忆台账（`memory_mutations` 是记忆变更的幂等台账）

**Routing Decision（路由裁决）**:
协调层对一次输入做出的**确定性**归属决定：延续/补充/新建/恢复/暂停/取消/
澄清/拒绝，并给出目标 Session/Task 与可审计理由。模型只能提出建议，
不得直接裁决。
_Avoid_: 意图（意图是模型的语义产物）、计划、模型路由

**Input Stage（输入阶段）**:
一次已接纳输入的**闭集**阶段：已接纳 / 已路由 / 已回答 / 任务已终态 / 已拒绝。
阶段区分「收到输入」与「任务完成」，且**永不回退**；迁移必须显式登记。
_Avoid_: 任务状态（TaskState 是 Runtime 的任务状态机）、
状态（泛称）、进度

**Route Intent（路由意图）**:
路由裁决的**闭集**语义目标：延续当前工作 / 补充纠正 / 新建 / 恢复 / 暂停 / 取消 /
澄清 / 拒绝。它是**协调层裁决的词汇**，不是模型的语义产物。
_Avoid_: 意图（泛称，模型语义）、GoalPriority、Task Priority

**User Input Sink（用户输入汇）**:
协调层把输入投递给某个**已在运行**的 Loop 的注册面（`task_id → IUserInputSink`）。
协调层**不创建也不运行** Loop；投递经各 Loop 自己的有界入队通道，
溢出以 `ResourceExhausted` 显式拒绝。
_Avoid_: 消息队列（Loop 侧的有界队列）、回调、监听器

**Principal Registry（主体注册表）**:
协调层持有的 `(TenantId, UserId) → Session/Task` 映射，是台账事件流的
**可重建投影**。宿主实例与 grant 是主体**属性**，不是身份维度；
注册表由协调层独占写入。
_Avoid_: 会话表（Runtime 独占的会话状态机）、用户表、
记忆 ACL 域（`MemoryScope.subject_id` 是记忆作用域）

**Interaction Event（交互事件）**:
`mira.interaction.*.v1` 事件族的成员：输入的接纳、路由、澄清、拒绝、回答与
任务终态。事件写在**主体级专用分区**，每个事件**自带版本标记**
（信封的 `schema_version` 是死字段，不得依赖）。
_Avoid_: 会话事件（Session 流的事件）、对话投影、指标

**Capacity Budget（容量预算）**:
DEC-069 冻结的**持久化与驻留资源**总量上限，分层为总量、按主体份额、
软水位、硬水位与回收预留。它与 Context 预算（单次请求的 token 预算）
**互不相干**，两者不可互证。
_Avoid_: Context 预算、请求上限、配额（单对象限额）、
模型预算（`BudgetLedger` 按 Task 的计价账本）

**Forgetting Layer（遗忘分层）**:
DEC-069 冻结的四层遗忘机制：召回降权、去重/抽象巩固、有限归档、物理删除。
召回降权**不释放空间**，不计入「已遗忘」。
_Avoid_: 遗忘（泛称）、TTL 到期（只是物理删除的一条触发）

**Protection Set（保护集合）**:
不得按普通情景淘汰的最小必要状态集合：活动/挂起任务的最低必要状态、
未决副作用、待批准/待澄清事项、当前安全与授权约束。它本身**占用预算且有上限**，
满时限制新工作而非扩大保护集合。
_Avoid_: 固定集、永久保留、豁免

**Archive（归档）**:
会话退出后的有限保留态：释放受管理/驻留会话名额，保留有限摘要、元数据与
必要工件，有自己的总额与保留期。归档**不是**「忘掉」——隐私删除的 hold
不因归档解除。
_Avoid_: 归档文件（文件系统语义）、备份、冷存储

## 11. 注意力与自主行为（DEC-071 / DEC-072）

权威来源：[DEC-071](../decisions/DEC-071-attention-focus-and-safe-switch.md)、
[DEC-072](../decisions/DEC-072-autonomy-and-interruption-budget.md)、
[M32](../plans/m32-attention-focus-and-safe-switch.md)、
[M33](../plans/m33-autonomy-and-interruption-budget.md)。

**Focus（认知焦点）**:
DEC-071 冻结的**任务/活动级**注意力对象，由协调层持有、跨会话、单一提交点。
与感知层的 ROI「焦点」和表现层的显示选择**不是同一契约**。
_Avoid_: 感知焦点、设备焦点、窗口焦点、Session（焦点跨会话）

**Task Origin（任务来源）**:
`TaskOrigin{UserInitiated, Autonomous}`——工作由用户发起还是自主发起。
与 `TaskPriority` **正交**：自主工作可以是高优先级，用户工作也可以是后台优先级。
_Avoid_: GoalPriority（把来源与紧迫度混在一个枚举里）、
用户类型（UserId 是主体，不是来源）

**Task Priority（任务优先级）**:
`TaskPriority{Background, Normal, High}`——同级竞争与抢占**候选资格**。
`High` **不等于**可抢占：实际抢占仍受可中断性判定与授权约束。
用户发起任务的优先级**始终不低于**自主任务。
_Avoid_: Executor 调度优先级（不同概念，禁止混用）、
抢占（抢占是动作，优先级是资格）

**Interruptibility（可中断性）**:
DEC-071 冻结的**纯函数只读**判定：`Interruptible` / `WaitForSettlement` /
`NotInterruptible`。安全点是「在途 operation 结算为 Applied 或明确 Failed」的时刻；
超时**不**构成强制终止。
_Avoid_: 可取消（取消是用户意图，可中断是当前状态）、暂停（暂停是动作）

**Safe Switch（安全切换）**:
`Prepare → Suspend → Release → Commit → Resume` 五阶段协议：
旧焦点先释放、新焦点后提交；失败回滚到原焦点；崩溃后可从持久化状态识别为
「中断的切换」。
_Avoid_: 抢占（抢占只是 Safe Switch 的一种触发）、注意力切换（泛称）

**Autonomous Intent（自主意图）**:
DEC-072 冻结的**第二输入源**：无用户主体、由宿主提出的工作意向
（`IntentSource`、类别、优先级、期限、关联承诺、价值与风险估计）。
它**不走**用户输入路径，但**共用**协调层的预算、租约、注意力、审计与恢复纪律。
_Avoid_: Input Envelope（授权主体、幂等键与接纳判据都不同）、
后台任务（后台任务可以由用户发起）

**Interruption Budget（打断预算）**:
DEC-072 冻结的**九道确定性闸门**：类别开关、全局开关、权限判定、静默期、冷却、
最小间隔、合并窗口、每类别频率上限、每日/每小时配额。
Core **强制执行**闸门；宿主只提供配置与「该不该说话」的策略。
_Avoid_: 焦点稳定性（`min_focus_duration` 等只管切换任务，不管主动发言）、
配额（单项限额，打断预算是多项闸门的合取）

**Autonomy Permission（自主权限三分类）**:
`Allowed`（只读无副作用）/ `NeedsConsent`（有副作用，需用户确认）/
`Forbidden`（高风险，无论用户是否在线）。Core 侧同源判定；
宿主的开关**只能收紧，不能放宽**。
_Avoid_: 风险分级（ActionRisk 是动作维度，这是发起维度）、能力授权

**Class Disable（按类别禁用）**:
用户否决（「不要再…」）后由 Core **强制**持有的持久禁用状态。
这是把宿主 UX 意图提升为 Core 不变量的一条规则。
_Avoid_: 降频（降频只影响频率，禁用是硬拒绝）、
模型判断（否决不是模型裁量）
