# DEC-039：MCP 工具模组准入——部署时注册的外部 Tool 来源

> 状态：Accepted（方向冻结；实现未开始）
> 日期：2026-09-15
> 决策人：Mira Maintainers
> 需求来源：[Issue #56](https://github.com/Linductor-alkaid/mira/issues/56)
> （MCP 作为外部 Tool 生态入口）
> 上位决策：[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)
> 修订对象：[DEC-009](DEC-009-tool-module-boundary.md) 备选方案第 5 条
> （仅解除「部署时注册的 MCP Tool 来源」的否决；「运行中动态发现与热插拔」的
> 否决维持不变，见修订声明）
> 关联决策：[DEC-004](DEC-004-security-authority-confirmation.md)、
> [DEC-015](DEC-015-builtin-tool-execution-boundary.md)、
> [DEC-019](DEC-019-workflow-ir-contract.md)、
> [DEC-021](DEC-021-workflow-tool-channel.md)、
> [DEC-033](DEC-033-hybrid-visual-grounding.md)、
> [DEC-040](DEC-040-tool-reference-and-skill-layer.md)

## 背景与问题

[DEC-009](DEC-009-tool-module-boundary.md) 冻结了 ToolModule/ModuleRegistry/
CapabilityCatalog 的模组体系方向，同时在备选方案中否决了「MCP 式运行时动态发现与
热插拔」，理由是与「运行中不能由模型新增 native Tool」的安全立场冲突；[工具模组
设计](../design/tool_module_design.md) §17 相应留下「远端模组分发与 MCP 式动态发现
明确不在 v1；如引入需新 DEC」的入口。

[Issue #56](https://github.com/Linductor-alkaid/mira/issues/56) 提出 MCP 作为 Mira
外部 Tool 生态入口：MCP Server 的 tool 经统一 descriptor 注册后，与 Native/Host
Tool 同等成为 Agent 调用对象与 Workflow `ToolCall` 步骤的执行原子。现状是 `mcp`
仅作为模型层的保留 hosted wire 名存在（`is_known_hosted_tool_name`），仓库没有任何
MCP client/协议实现；DEC-009 模组体系本身也尚未落地（M7 `Blocked`，过渡期为
DEC-015 最小 BuiltIn 边界）。

需要决策的是：MCP Tool 以什么身份进入 Mira、在什么时机注册、服从哪些既有门禁，以及
这否决立场需要修订到什么程度。

## 修订声明

本决策**部分修订** DEC-009 备选方案第 5 条：

- **维持否决**：「运行中动态发现与热插拔」——运行中不能由模型或远端 server 新增、
  替换或升级 native Tool 的安全立场不变；DEC-009「运行中只允许状态降级（含
  revoke），不允许动态新增模组」的规则对 MCP 模组同样适用。
- **解除否决**：「MCP 作为**部署与初始化时注册**的外部 Tool 来源」——MCP Server
  的 tool 经转换与校验后以 `ToolModule` 身份进入 Registry，与 BuiltIn/HostProvided
  同规则协商、暴露与执行。

DEC-009 其余条款（manifest 治理、能力协商 fail closed、单一事实源三类投影、
DecisionParser/SafetyPolicy 单一汇合点）全部不变。本决策是 DEC-009 备选方案预留的
「届时另立 DEC」，不改变 M7 的 `Blocked` 状态；DEC-009 模组体系落地（M7 重定义）
是本方向的实现前置。

## 决策

1. **身份：MCP Tool 是 ToolModule 成员，不是第二套注册体系**。MCP Adapter 把
   MCP Server 暴露的 tool 转换为统一 `ToolSpec`（wire 名、版本、参数 JSON Schema、
   副作用声明、描述），挂载于来源为 `OutOfProcess` 的 `ToolModule`（DEC-009 信任
   域），携带 manifest 进入既有注册、校验与状态机
   （`Discovered -> Verified -> Staged -> Active -> Deprecated -> Revoked`）。
   没有 manifest 或未通过校验的 MCP tool 不注册（fail closed）。
2. **注册时机：只发生在 Runtime/Session 初始化与显式部署动作**。MCP 能力发现
   （`tools/list`）是部署与初始化阶段的宿主动作，不是模型可触发的运行时动作；
   模型不能发起 server 连接。MCP Server 会话生命周期事件（连接、断开、能力列表
   变化）映射为模组状态机迁移与审计事件；运行中能力列表变化不静默改写暴露面，
   只允许降级路径（revoke → 重新部署走显式动作）。
3. **校验同源**：MCP tool descriptor 的参数 schema 必须过与
   `resolve_tool_calls`/`gate_schema_subset` 同源的 JSON Schema 子集校验；跨模组
   wire 名冲突、未知能力、缺失能力按 DEC-009 一律 fail closed，不降级为「部分
   可用」。MCP schema 的质量参差导致的拒绝是显式 fail closed，不是缺陷；豁免
   只能经宿主显式配置，不经模型。
4. **单一执行门禁不变**：注册后的 MCP tool 与其他来源 tool 走同一条路径——
   exposure 时 `SafetyPolicy` 决定是否进入该请求的 `ExposedToolSpec`；执行前
   ToolId/wire 名/版本/副作用声明与暴露快照一致校验、参数本地校验、至多一次
   派发（DEC-015 语义）；权限按 DEC-004 capability grant 与 PolicyEngine 逐副作用
   判定（Allow/Deny/RequireConfirmation）。**MCP 来源不构成任何 authority 豁免**；
   Workflow 中引用 MCP tool 的步骤同样经 DEC-021 §4 的权限挂钩点。
5. **Workflow 衔接**：注册后的 MCP tool 与其他 tool 同等成为 DEC-019 `ToolCall`
   步骤的调用对象，引用形态遵守 DEC-040 稳定引用契约；Workflow Core 不依赖
   MCP 协议类型，协议差异只存在于 Registry 解析与 Adapter 执行适配阶段。
6. **进程与传输边界**：MCP client 的 I/O、server 进程生命周期管理、重连与超时
   属宿主/Adapter 侧能力；Core 只消费已注册的 `ToolSpec` 与结构化结果。长阻塞
   的进程外调用必须具有可解除阻塞路径并经 Executor 承载（W-01），在途调用的取消
   与 shutdown 语义在立项里程碑冻结。传输协议选型（stdio/HTTP 等）是 Adapter 实现
   细节，不进入 Core 公共契约。
7. **不可信数据与供应链**：MCP tool 的结果与 server 推送内容是不可信外部数据
   （RULE-09），回填按 provenance 标记，不提升权限、不进入 System/Developer 权威；
   工具描述（server 提供的文本）不得绕过脱敏直接进入事件载荷。第三方 server 依赖
   不入 Core 依赖树；server 的许可/来源审计与 `THIRD_PARTY_NOTICES` 义务按
   DEC-033 §9 同等纪律执行。
8. **能力面范围**：本决策只覆盖 MCP 的 tool 能力面。resources/prompts/sampling
   等其他 MCP 能力不在本决策范围，留待有真实消费者证据时另行评审。

## 非目标

- 不做运行时动态发现与热插拔；不让模型发起或管理 server 连接。
- 不在本决策冻结 descriptor 转换的精确 schema、引用 URI 形态（归 DEC-040 立项
  冻结）与传输选型。
- 不承诺任何特定 MCP server 的可用性、兼容性或质量（RULE-10）。
- 不改变 M7 `Blocked` 状态与 DEC-011 证据门禁。

## 备选方案

- **维持 DEC-009 v1 对 MCP 的完全拒绝**：放弃现成的外部工具生态，Tool 多样性受限，
  且 DEC-009 自身已预留「届时另立 DEC」入口。不采用。
- **MCP tool 绕过模组体系直接进 `BuiltinToolRegistry`**：绕过信任域、签名与生命
  周期治理，制造 BuiltIn 与外部来源的信任混淆，违反 DEC-009 模组边界。不采用。
- **运行中热插拔（server 连接即暴露）**：与「运行中不能由模型新增 native Tool」
  的安全立场冲突，且使 Workflow 的稳定能力引用无法版本化（DEC-040 的前提被破坏）。
  不采用。
- **为 MCP 单独建一套工具/Workflow 节点体系**：DEC-021 已否决过同型的旁路方案；
  两套注册与执行语义的维护成本与漂移风险高于收益。不采用。

## 影响与风险

- 实现前置是 DEC-009 模组体系落地（M7 重定义，`MNT-202609-30`）；在此之前任何
  MCP 实现都属于绕过治理的临时方案，不予立项。
- MCP server 生态的 schema 质量参差：子集校验会拒绝一部分真实 server 的工具；
  这是显式 fail closed，宿主如需豁免必须经显式配置并自担风险，文档披露。
- 进程外调用的取消/超时/shutdown 语义复杂：Adapter 必须提供可解除阻塞路径，
  Executor 能力不足时按台账流程登记，不得私建线程或脱离生命周期的后台任务。
- 供应链审计面扩大：每个接入的第三方 server 都是新的许可与来源审计对象。
- 安全面扩大：prompt injection 经工具描述与工具结果进入系统的通道增加；DEC-004
  的 challenge/response 确认与脱敏规则是既有缓解，立项时补充 MCP 专项负向测试。

## 验证方式

- 方向以本决策为准；立项里程碑内冻结：descriptor 转换矩阵（MCP schema →
  `ToolSpec` 的正负路径，含子集校验拒绝样例）、模组生命周期测试（注册/降级/revoke、
  运行中能力列表变化只降级不扩面）、单一门禁一致性（MCP tool 与 BuiltIn tool 过
  同一 DEC-015 校验的契约测试）、在途调用取消与 shutdown 闭合、工具结果/描述的
  脱敏与不提升权限负向测试、DEC-040 引用解析的组合测试。

## 关联文档和工作项

- [Issue #56](https://github.com/Linductor-alkaid/mira/issues/56)
- [DEC-009](DEC-009-tool-module-boundary.md)（修订对象，原文已加反向链接注记）、
  [DEC-004](DEC-004-security-authority-confirmation.md)、
  [DEC-015](DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-019](DEC-019-workflow-ir-contract.md)、
  [DEC-021](DEC-021-workflow-tool-channel.md)、
  [DEC-033](DEC-033-hybrid-visual-grounding.md)（§9 供应链纪律）、
  [DEC-040](DEC-040-tool-reference-and-skill-layer.md)
- [工具模组设计](../design/tool_module_design.md)（§17 开放项已同步）
- [威胁模型与确认协议](../security/threat_model_and_confirmation.md)
- 专项设计与里程碑文件随首阶段立项交付（实现前置：M7 重定义）。
