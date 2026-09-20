# M7：Tool 模组体系（DEC-042 重定义）

> 状态：Planned（2026-09-16 依 [DEC-042](../decisions/DEC-042-m7-scope-redefinition.md)
> 重定义；TM0/TM1/TM2、MCP 准入阶段（DEC-039）与 DEC-040 的 TR0（稳定引用与
> 兼容投影）、TR1（Skill 发布生命周期与 Procedure 索引投影）已交付关闭；
> TR2（Runtime 接线与执行）随其立项冻结细项，不预分配编号）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M4（已完成）；[DEC-042](../decisions/DEC-042-m7-scope-redefinition.md)
> 建议发布点：Tool module alpha（分阶段锚点，非发布物）
> 更新日期：2026-09-21（TR1 交付关闭）

## 1. 目标

按 [DEC-009](../decisions/DEC-009-tool-module-boundary.md) 分阶段落地 Tool 模组
体系：CapabilityCatalog 与确定性协商（TM0）、ModuleRegistry 生命周期（TM1）、
LLM 暴露投影（TM2），其后衔接 [DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md)
MCP 工具模组准入与 [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md) 稳定引用
与 Skill 层级的实现阶段。范围只含纯 Core，不依赖真实设备证据；每阶段内部 gate
独立验收，不以并行未结项关闭总里程碑。

2026-09-16 重定义（DEC-042）：原「评估体系、真实平台验证、v1.0 发布加固」范围
按映射表推迟或缩减——真实平台项保持 `MNT-202609-27` 证据门禁，发布范围待未来
发布立项承接；最小评估 profile 与 harness 已由 [M15](m15-eval-harness-and-baseline.md)
交付。

## 2. 范围与非目标

### 2.1 范围（分阶段）

- **TM0 契约与协商**：`CapabilityCatalog` 受治理词汇与 digest、
  `EnvironmentCapabilities -> env.*` 纯函数派生、ToolModule manifest 契约
  （`mira.tool_module.manifest.v1`）与 fail-closed 校验（结构、schema 子集、
  能力词汇、资源上限、ABI）、`negotiate_modules` 确定性协商纯函数与协商 digest。
- **TM1 Registry 生命周期**：模组状态机（`Discovered -> Verified -> Staged ->
  Active -> Deprecated -> Revoked`，失败 `Quarantined`）、不可变 snapshot、
  初始化/部署期注册、运行期只降级、revoke tombstone、生命周期事件、来源信任
  （BuiltIn digest / HostProvided allowlist / OutOfProcess 包签名）验证、协商
  触发挂接（session 建立、能力变化、模组状态变化）。
- **TM2 LLM 暴露投影**：协商结果 → ToolRegistry view → per-request
  `ExposedToolSpec`，模组级/任务级排除理由，`tool_snapshot_digest` 绑定 module
  与 ToolSpec digest，`wire_name` 跨模组规则冻结，Simulator 参考模组，Replay
  module digest 绑定。
- **MCP 准入阶段（DEC-039）**：MCP server tool listing 受控子集 →
  `out_of_process` 模组 manifest 的确定性转换、会话生命周期映射（部署窗准入、
  运行期只降级）、宿主 transport 执行适配与 DEC-015 同源门禁、在途调用取消/
  deadline/shutdown 闭合、不可信数据脱敏纪律（[MCP 准入设计](../design/mcp_tool_admission_design.md)）。
- **TR0 稳定引用与兼容投影（DEC-040 首阶段）**：引用语法 v1 冻结（钉住 spec
  digest / 跟随最新可用版本，受治理词表字符集跨源同命名空间）、Workflow 引用
  清单提取工件（`mira.workflow.tool_refs.v1`）、引用解析矩阵与兼容状态投影
  （`Runnable`/`Degraded`/`Invalid` 确定性重算）、`Invalid` 准入拒绝决策与
  `Degraded` 审计投影产物（[Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md)）。
- **TR1 Skill 发布生命周期与 Procedure 索引投影（DEC-040；2026-09-21 跑前冻结
  细项，进入实施）**：Skill 描述符（`mira.skill.descriptor.v1`，钉住源
  Workflow id + `ir_digest`）与暴露面确定性派生、`SkillPublicationRegistry`
  宿主显式发布/升级/撤销生命周期（runnable 门禁、只降级、部署窗）、Procedure
  索引投影（`mira.skill.procedure_index.v1`，以显式发布为界、无时钟、可重建）
  （[Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md) §17）。
- **其余后续阶段**：TR2 WorkflowRuntime 接线与执行（DEC-040：库存储挂载
  tool_refs 清单、`create_run` 准入消费、`Degraded` 事件发射、Skill 经 Tool
  通道的子 Workflow 调用执行适配、IR 引用表达加法演进）随其立项在 M7 内增补
  工作项与门禁，不预分配编号。

### 2.2 非目标

- 运行中动态注册/热插拔、远端模组分发（DEC-009/DEC-039 否决不变）。
- 真实平台 Adapter、设备矩阵、平台支持等级声明（推迟，`MNT-202609-27` 门禁）。
- ModelPackage policy 绑定（无消费者，待 DEC-011 证据重入）、完整 OOP 沙箱、
  L0–L5 评估全家桶、soak/safety 矩阵、v1.0 发布门禁（推迟，见 DEC-042 映射表）。
- 模组准入构成授权——capability、Policy、confirmation 语义不变（DEC-004/009）。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M4 完成（已满足）；[DEC-042](../decisions/DEC-042-m7-scope-redefinition.md) 冻结。
- 每阶段进入实施前，本文件内冻结该阶段工作项与门禁。

### 3.2 设计与决策依据

- [工具模组设计](../design/tool_module_design.md)（§4–§8、§15 阶段划分）
- [MCP 准入设计](../design/mcp_tool_admission_design.md)（MCP 阶段规范：
  §4 受控子集、§5 转换矩阵、§6 生命周期映射、§7 执行适配、§8 不可信数据）
- [Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md)
  （TR0 阶段规范：§4 引用语法、§5 提取、§6/§7 解析与兼容投影、§8 准入与留痕、
  §9 与 DEC-015 边界；TR1 阶段规范：§17 Skill 描述符与发布生命周期、Procedure
  索引投影）
- [Model Provider 与 Tool 扩展设计](../design/model_provider_and_tool_design.md)
- [DEC-009](../decisions/DEC-009-tool-module-boundary.md)、
  [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md)、
  [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)、
  [DEC-042](../decisions/DEC-042-m7-scope-redefinition.md)
- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)

## 4. 工作项

### 4.1 TM0：契约与协商（2026-09-16 交付关闭）

- [x] `M7-TM0-01` 冻结并实现 `CapabilityCatalog`：核心词表固定收录设计 §4.2 的
  九个 `env.*` 能力与设计 §4.1 示例的 `host.*`/`tool.*` 基础设施能力（共 13 条，
  `env.perception.sources` 为 Counted），条目含 kind 与单行说明；目录 digest
  确定；目录外 capability 一律 fail closed。
- [x] `M7-TM0-02` 实现 `derive_environment_capabilities` 纯函数：设计 §4.2 映射
  表逐字段派生（`perception_sources >= 1` → `env.perception.sources`；质量字段
  不进入目录）；同输入同输出，输出排序。
- [x] `M7-TM0-03` 冻结并实现 ToolModule manifest 契约 `mira.tool_module.manifest.v1`
  与校验：schema_version、module_id 字符集与长度、SemanticVersion、origin
  （built_in/host_provided/out_of_process）、成员工具模组内唯一、参数/结果 schema
  过 `gate_schema_subset` 同源子集校验（拒绝错误归一 `mira.tool_module` domain）、
  能力引用全部在目录内、聚合资源上限有界、`min_mira_module_abi` 上限；canonical
  manifest digest 与输入键序/空白无关；任何失败整组拒绝，不产生部分状态。签名
  字段作为不透明字符串保留，验证归 TM1。
- [x] `M7-TM0-04` 实现 `negotiate_modules` 纯函数：Active snapshot ×
  `EnvironmentCapabilities` × catalog → 逐模组 `Available`/`Unavailable`（附
  missing 清单）/`Conflict`/`Revoked` 结论；缺失能力、未知能力（目录外词表即使
  环境宣称也不可用）、跨模组成员 wire 名冲突、重复 module_id 输入整组不可用或
  Conflict（fail closed，不降级为部分成员可用）；输出按 module_id 排序且与输入
  顺序无关；协商 digest 同输入同输出；不执行 I/O、不读时钟。ToolId 级全局唯一
  随 TM2 身份分配落地，TM0 以成员 wire 名跨模组唯一为协商门禁。
- [x] `M7-TM0-05` fake 模组与 fail-closed 契约测试矩阵：`tests/m7/
  m7_tool_module_test.cpp` 覆盖 G1–G5 全部门禁与设计 §16 的 manifest 校验矩阵
  （29 例负向）、协商 golden、fail-closed 负向；`--report` 跨进程协商 digest
  逐字节一致。

### 4.2 TM1：Registry 生命周期（2026-09-19 跑前冻结细项，进入实施）

- [x] `M7-TM1-01` ModuleRegistry 状态机与不可变 snapshot：状态机
  `Discovered -> Verified -> Staged -> Active -> Deprecated -> Revoked`（验证失败
  `Quarantined`）以受控转换落地——`register_module`（初始化/部署期准入：
  Discovered→Verified，信任失败→Quarantined）、`stage_module`（Verified→Staged）、
  `activate_module`（Staged→Active；跨模组成员 wire 名冲突时后激活者拒绝、先 Active
  者不受影响，设计 §5.3/§11）、`deprecate_module`（Active→Deprecated）、
  `revoke_module`（Active/Staged/Deprecated→Revoked）；升级、终态复活、Quarantined
  出口等非法转换明确拒绝且状态不变。`seal()` 结束注册窗口（运行期只降级）；
  `close()` 后一切变更与协商触发拒绝、snapshot 仍可读供在途结算。
  `active_snapshot()` 返回值语义的不可变 `RegistrySnapshot`（generation 单调、
  module_id 排序、canonical digest），状态变化只影响下一次读取。revoke digest 进
  tombstone，同 digest 重注册拒绝并记事件；tombstone 可经信任配置构造时播种。
  生命周期事件（`mira.tool_module.lifecycle.v1`，State 级，Revoked/Quarantined 为
  Critical）：七种状态迁移 + 拒绝原因；事件 sink 失败计入统计不阻塞控制面。
- [x] `M7-TM1-02` 来源信任验证（DEC-009「宿主显式注入 + allowlist」暂定默认值升格为
  v1 冻结决策）：`ModuleTrustConfig`——BuiltIn 构建钉定 digest 集、HostProvided
  module_id allowlist（宿主部署管线持有，Registry 生命周期内不可变）、OutOfProcess
  信任签名者集 + 注入式 `IModuleSignatureVerifier`（签名绑定 canonical manifest
  digest；Core 不引入密码学依赖，参考实现为确定性绑定校验，真实密码学校验由宿主
  注入）。`verify_module_trust` 纯函数：origin 与信任材料一致才通过；signer/
  signature/algorithm 缺失或失配、digest 未钉定、allowlist 外、签名验证失败或不可用
  全部 fail closed。注册准入集成：验证失败 → Quarantined，无部分状态。
- [x] `M7-TM1-03` 协商触发挂接：`ModuleNegotiationCoordinator` 持 catalog 与当前环境
  （epoch + `EnvironmentCapabilities`），三类触发——`SessionEstablished`、
  `EnvironmentChanged`（epoch 单调：旧 epoch 拒绝、同 epoch 重申报幂等 NoOp）、
  `ModuleStatesChanged`（registry 状态变化）；每次有效触发以当前 Active snapshot 调
  既有 `negotiate_modules`，产出新 generation 的 `NegotiationView`（generation 单调、
  协商 digest、投影）并提交 `ModuleNegotiationDecided` 事件
  （`mira.tool_module.negotiation.v1`，含触发源、epoch、generation、digest）。
  在途请求按旧 generation 结算不受影响：`current()` 返回只读值语义 view，旧 view
  永远可结算，新请求读 current。验证/安装类工作经 `submit_module_verification` 以
  `submit_auto()` 承载，future 必须消费；提交拒绝、任务异常、执行中取消与
  shutdown 转化为明确结果（设计 §10，M7 §6）。

### 4.3 TM2：LLM 暴露投影（2026-09-19 跑前冻结细项，进入实施）

- [x] `M7-TM2-01` 投影纯函数 `project_tool_exposure`：以 NegotiationView
  generation + Active snapshot（成员契约）+ 协商结论 + 任务级选择输入 →
  per-request `ExposedToolSpec` 集合——ToolId 由 (module_id, module_digest,
  成员名) 确定性派生（TM2 身份分配落地，跨进程一致，无随机）；wire_name 为
  成员名原文；`spec_digest` canonical；`ActionRisk != read_only →
  has_side_effects` 映射；输出按 wire_name 排序。两级排除理由
  `ToolExclusion`（模组级：Unavailable 附 missing 清单 / Conflict 附对端 /
  Revoked / ReservedWireName；任务级：TaskPolicy、TaskBudget）。合成
  `snapshot_digest` 绑定 generation + included module digest 集合 +
  (tool_id, spec_digest, wire_name) 集合（DEC-002 加法演进：既有
  `PromptProvenance.tool_snapshot_digest` 字段与非模组纯工具 digest 函数
  不变，M3 空注册路径语义不变）。版本化 JSON 投影
  `mira.tool_module.exposure.v1`（脱敏，供事件/Replay/报告）。
- [x] `M7-TM2-02` `wire_name` 跨模组命名规则 v1 冻结（设计 §17 开放项收口）：
  wire 命名空间为扁平集合，成员名原文进入、不做任何自动前缀或改名；跨模组
  唯一性由协商（TM0 Conflict）与激活（TM1 后激活者拒绝）fail-closed 门禁
  承载；hosted provider 保留名（`is_known_hosted_tool_name`）冲突整组排除
  （ReservedWireName），绝不部分暴露或静默改名；命名空间前缀（如
  `builtin.simulator.env` → `simulator.*`）为非约束性约定，安全性不依赖
  约定；投影层对漏网重名/重复 ToolId 防御性整组拒绝。
- [x] `M7-TM2-03` Simulator BuiltIn 参考模组 `make_simulator_reference_module`
  （经真实解析器构建，manifest digest 为 golden 常量；含 read_only 与
  user_visible 两成员、后者要求 `env.input.discrete`）；Replay 绑定
  `verify_recorded_module_digests`（记录 module digest 集与投影 included
  集合精确匹配，多/少/错 digest 显式错误）；与 `resolve_tool_calls` 既有
  fail-closed 语义的组合测试（投影产物入 `ModelRequest.tools` 后正常解析，
  未暴露名/hosted 名仍拒绝）。

### 4.4 MCP 准入（DEC-039；2026-09-20 跑前冻结细项，同日交付关闭）

- [x] `M7-MCP-01` MCP listing 受控子集与转换纯函数 `convert_mcp_listing_to_module`
  （[MCP 准入设计](../design/mcp_tool_admission_design.md) §4/§5）：
  `McpToolDescriptor`（name、description、input_schema、readOnlyHint/
  destructiveHint）与 `McpAdmissionOptions`（module_id、版本、OutOfProcess 信任
  材料、宿主能力映射、聚合资源、逐成员风险覆盖）→ origin `out_of_process` 的
  `ToolModuleManifest`；经**真实** `parse_tool_module_manifest` 校验（成员字符集、
  重名、`gate_schema_subset` 同源子集、资源上限、ABI、目录内能力），任何
  descriptor 缺陷整组拒绝（错误 domain `mira.tool_module`）。hint→ActionRisk
  确定性映射（readOnlyHint→read_only、destructiveHint→critical、缺省→
  user_visible）；`risk_overrides` 只允许升风险、降风险整组拒绝。同 listing +
  同 options 产出逐字节相同 manifest 与 digest；版本化脱敏投影
  `mira.tool_module.mcp.admission.v1`（不含 description 原文与签名材料）。
- [x] `M7-MCP-02` 会话生命周期映射（设计 §6）：`plan_mcp_session_action` 纯策略
  函数按（事件 × 注册状态 × seal/close）输出 AdmitModule/RevokeModule/
  RejectEvent/NoAction；`McpModuleAdmission` 组件绑定 catalog + registry 落地：
  ServerConnected 仅部署窗准入（转换 → TM1 信任 → `register_module`，晋升仍由
  宿主显式驱动），seal/close 后或同 module_id 已注册时拒绝且状态不变；
  ServerDisconnected 与 ToolListChanged 对非终态模组映射 revoke（运行期只降级
  不扩面，digest 进 tombstone，在途调用按旧代正常结算）；变更后的 listing 是
  新 digest，需下一注册周期。审计复用 `mira.tool_module.lifecycle.v1`，理由
  有界脱敏。
- [x] `M7-MCP-03` 执行适配（设计 §7）：宿主注入 `IMcpToolTransport`（进程外
  I/O 与传输选型归宿主，须轮询 `McpInvocationProbe` 协作取消）；`McpToolDispatcher`
  绑定 TM2 `ToolExposure` 快照落地 DEC-015 同源门禁——曝光内身份一致校验
  （tool_id/wire_name/version/side effects）、OperationId 至多一次（预约-回滚
  语义）、参数本地 schema 校验（模型可归因失败 → failed record）、取消 →
  `Cancelled`、模组聚合并发上限调度层强制拒绝、结果字节上限。调用经
  `submit_mcp_invocation`/`consume_mcp_invocation` 以 `submit_auto()` 承载，
  future 必消费（正常路径即时消费、放弃路径 pending 清单由 `close()` 有界排空）；
  deadline 取 `min(context.deadline, now + max_invocation_duration)`；提交拒绝/
  任务异常/执行中取消/超时放弃/close 后拒绝全矩阵显式结果，无吞掉异常；不可信
  结果纪律（§8：结果不携带 System/Developer authority、错误摘要 512 字节有界）。
- [x] `M7-MCP-04` 契约测试矩阵 `tests/m7/m7_tool_module_mcp_test.cpp`（label
  `contract`）：覆盖 `M7-MCP-G1`–`G6` 全部门禁，`--report` 跨进程字节一致；
  `examples/minimal_consumer.cpp` 追加 MCP 闭包段。测试的编写、运行与
  sanitizer 取证由 Independent-Verification-Agent 独立完成。

### 4.5 TR0：稳定引用与兼容投影（DEC-040 首阶段；2026-09-21 跑前冻结细项，同日交付关闭）

- [x] `M7-TR0-01` 引用语法 v1 冻结与解析（
  [Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md) §4）：
  `toolref:<wire-name>`（跟随最新）/ `toolref:<wire-name>@<64 位小写十六进制>`
  （钉住成员 spec digest，内容寻址）；wire 名字符集与模组词表同源，三来源
  （BuiltIn/HostProvided/OutOfProcess）同命名空间；fail-closed 解析（scheme、
  字符集、digest 形态、长度、空白、大写十六进制全矩阵拒绝），规范形态唯一、
  往返无损；错误 domain `mira.tool_reference`。基于版本约束的钉住不进 v1
  （设计 §4.3 裁决），未来引入按加法演进处理。
- [x] `M7-TR0-02` 引用清单提取工件（设计 §5）：`extract_workflow_tool_references`
  从经校验的 `WorkflowDefinition` 的 ToolCall 步骤（IR v1 `arguments["tool"]`）
  确定性提取 `mira.workflow.tool_refs.v1` 清单——绑定 `workflow_id` +
  `definition_digest`；提取选项默认模式 + 逐 wire 名覆盖（重复/空名拒绝）；
  钉住条目记录当时视图观察到的 spec digest，两模式均要求发布期可解析（引用
  不存在工具 fail closed）；视图重复 wire 名防御性拒绝；无 ToolCall 步骤产出
  确定空清单；JSON 严格往返无损；IR v1 schema 零改动。
- [x] `M7-TR0-03` 解析矩阵与兼容状态投影（设计 §6/§7/§8）：
  `project_workflow_tool_compatibility` 以清单 × 当前暴露视图逐条目产出
  `Resolved`/`EvolvedCompatible`/`EvolvedIncompatible`/`Unresolved`（钉住
  digest 失配经「占位符按型实例化 + 既有严格 schema 校验器」做骨架可绑定判定，
  准入期与执行期同一校验器零漂移）；Workflow 级聚合 `Runnable`/`Degraded`/
  `Invalid`；清单与定义错配、视图重名整组拒绝；同输入同投影同 digest（无时钟、
  无随机）；`admit_workflow_run_by_tool_compat` 全函数产出准入决策（`Invalid`
  → 拒绝，reason 确定性；`Degraded` → 放行 + 要求留痕）；留痕投影
  `mira.workflow.tool_compat.v1` 版本化 JSON 脱敏（无 schema 体、描述原文与
  secret）；
  投影不进入执行路径，DEC-015 执行期校验语义不变。
- [x] `M7-TR0-04` 契约测试矩阵 `tests/m7/m7_tool_reference_test.cpp`（label
  `contract`）：覆盖 `M7-TR0-G1`–`G6` 全部门禁，`--report` 跨进程字节一致；
  `examples/minimal_consumer.cpp` 追加 TR0 闭包段。测试的编写、运行与
  sanitizer 取证由 Independent-Verification-Agent 独立完成。

### 4.6 TR1：Skill 发布生命周期与 Procedure 索引投影（DEC-040；2026-09-21 跑前冻结细项，同日交付关闭）

- [x] `M7-TR1-01` Skill 描述符与暴露面派生（
  [Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md) §17.1）：
  `mira.skill.descriptor.v1`——`name`（词表字符集 wire 身份）、显式 `version`、
  `source_workflow_id` + `source_ir_digest` 钉住、派生暴露面
  `SkillSurface{description, parameters_schema, has_side_effects}` 与 canonical
  digest；`derive_skill_surface` 确定性派生——description 取 `definition.summary`
  （非空、有界），参数 schema 从 `WorkflowParameterSpec` 全类型映射（约束与
  enum 传递、`additionalProperties=false`、逐项过 `gate_schema_subset`），
  `has_side_effects` 由 TR0 引用清单 × 视图推导（全部引用发布期可解析）；
  输入绑定与视图重名 fail closed。
- [x] `M7-TR1-02` 发布生命周期（§17.2）：`SkillPublicationRegistry` 宿主显式
  发布/升级/撤销——publish 要求源 `WorkflowVersionRecord` runnable（
  `workflow_version_is_runnable`）且 `content_digest`/`workflow_id` 与定义
  一致、name 撞宿主保留名或已存在拒绝（同 name + 同 descriptor digest 幂等
  NoOp）；upgrade 显式换钉（新版本严格递增、同 digest 幂等 NoOp、旧版本进
  superseded 轨迹）；revoke 只降级不可逆、重复撤销幂等；`seal()`/`close()`
  部署窗纪律与拒绝计数；版本化事件 `mira.skill.publication.v1` 脱敏（仅身份
  /digest/有界理由，无 description 与 schema 体），sink 失败计数不阻塞。
- [x] `M7-TR1-03` Procedure 索引投影（§17.3）：
  `project_skill_procedure_index` 以宿主显式发布为界——未发布的 Workflow 库
  资产不自动索引（DEC-029 否决的自动写入面不复活）；statement 固定 canonical
  JSON（`mira.skill.procedure_index.v1`：身份/digest/钉住/副作用，不含
  description）；输出按 name 排序、无时钟（时间戳归 TR2 接线）、不写
  `IMemory`；statement 严格反解析可重建、重放字节一致。
- [x] `M7-TR1-04` 契约测试矩阵 `tests/m7/m7_tool_skill_test.cpp`（label
  `contract`）：覆盖 `M7-TR1-G1`–`G6` 全部门禁，`--report` 跨进程字节一致；
  `examples/minimal_consumer.cpp` 追加 TR1 闭包段。测试的编写、运行与
  sanitizer 取证由 Independent-Verification-Agent 独立完成。

### 4.7 其余后续阶段（立项时增补工作项与门禁）

- TR2 WorkflowRuntime 接线与执行（[DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)
  §验证方式的消费者接线：库存储挂载 tool_refs 清单、`create_run` 准入消费、
  `Degraded` 事件发射、Skill 经 Tool 通道的子 Workflow 调用执行适配、IR 引用
  表达加法演进；方向见
  [Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md) §15）。

## 5. 阶段门禁

### 5.1 TM0 门禁（2026-09-16 跑前冻结；同日交付取证）

- [x] `M7-TM0-G1` 目录与派生 golden：核心词表 digest 固定且与设计 §4.2 条目一
  一对应；`EnvironmentCapabilities` 布尔字段全组合 × `perception_sources`
  多取值的派生结果与期望集一致（2^8 布尔组合 × {0,1,3} = 768 组合，与测试内
  独立手写映射逐一比对）；目录外 ID 查询 fail closed。
- [x] `M7-TM0-G2` manifest fail-closed 矩阵：缺字段、坏 `module_id`、坏版本、
  非法 origin、未知 capability、模组内重名、schema 超子集/超限、资源上限缺失
  或越界、ABI 超限各有负向用例（29 例）；全部整组拒绝且无部分状态，拒绝错误
  domain 统一为 `mira.tool_module`。
- [x] `M7-TM0-G3` 协商 fail-closed：缺失能力 → `Unavailable` 附完整 missing
  清单；跨模组 wire 名冲突 → 双方 `Conflict`；revoked 输入 → `Revoked`；输出
  按 module_id 排序且与输入构造顺序无关（打乱输入结果与 digest 不变）。
- [x] `M7-TM0-G4` 确定性：同输入同协商 digest；golden 协商报告 digest 跨进程
  断言一致（`--report` 两次运行 cmp 逐字节一致，报告 md5
  `0b25496af889d106e5ecbfcb9b874d45`；无时钟、无随机、canonical JSON）。
- [x] `M7-TM0-G5` 有界性：模组数、成员数、能力数、清单字节数上限生效，越界
  明确拒绝（`RULE-08`）；空 Active 集与全不可用路径行为有定义。
- [x] `M7-TM0-G6` consumer 闭包：新公开头可被最小外部 consumer 独立包含并链接
  （`examples/minimal_consumer.cpp` 追加 TM0 闭包段并注册
  `mira_minimal_consumer_test` 进入 ctest；补齐该 consumer 此前未注册的缺口）。

### 5.2 TM1 门禁（2026-09-19 跑前冻结）

- [x] `M7-TM1-G1` 状态机与只降级矩阵：七状态每条合法转换生效且生命周期事件
  （kind、module_id、version、digest、origin）正确；非法转换（升级、终态复活、
  Quarantined 出口、seal 后注册、close 后变更）100% 拒绝且状态不变、拒绝有事件或
  明确错误。
- [x] `M7-TM1-G2` 来源信任 fail-closed：三 origin 的通过/拒绝矩阵——BuiltIn digest
  钉定命中/未钉定、HostProvided allowlist 命中/在外、OutOfProcess 签名者信任/不
  信任/verifier 拒绝/verifier 不可用、签名或算法字段缺失——全部整组拒绝 →
  Quarantined + 事件、无部分状态；tombstone 同 digest 重注册（含换 module_id 同
  digest）拒绝。
- [x] `M7-TM1-G3` 协商触发与 generation：三类触发各自产生新 generation 与
  `ModuleNegotiationDecided` 事件（触发源/epoch/generation/digest 齐全）；epoch
  回退拒绝、同 epoch 重申报幂等 NoOp；同输入协商 digest 确定性（`--report` 跨进程
  字节一致）。
- [x] `M7-TM1-G4` 不可变 snapshot 与在途结算：状态变化后旧 snapshot 值（模块集与
  digest）不变、generation 单调递增；旧 generation view 正常结算、新请求读新
  digest；跨模组 wire 名冲突激活拒绝且先 Active 者成员不受影响。
- [x] `M7-TM1-G5` Executor 路由与关闭：验证任务经 `submit_auto()` 且 future 必被
  消费；正常完成、任务异常、提交拒绝、执行中取消、shutdown 全矩阵转化为明确
  结果，无吞掉的异常；`close()` 后注册/转换/协商触发全部拒绝且统计可见。
- [x] `M7-TM1-G6` consumer 闭包与事件 schema：新公开头可被最小外部 consumer 独立
  包含链接；两事件 payload 为版本化 JSON（`mira.tool_module.lifecycle.v1` /
  `mira.tool_module.negotiation.v1`），字段脱敏（不含 signature 原文与 secret）。

### 5.3 TM2 门禁（2026-09-19 跑前冻结）

- [x] `M7-TM2-G1` 投影与身份分配：Available 模组成员进入 per-request
  `ExposedToolSpec`（ToolId 派生确定性跨进程一致；spec_digest canonical；
  wire_name 为成员名原文；`ActionRisk != read_only → has_side_effects`）；
  输出按 wire_name 排序；同输入同 snapshot_digest。
- [x] `M7-TM2-G2` 两级排除矩阵：模组级（Unavailable 附完整 missing 清单 /
  Conflict 附对端 / Revoked / ReservedWireName——成员撞 hosted 保留名整组排除，
  不部分暴露、不改名）；任务级（TaskPolicy 显式排除附调用方理由、TaskBudget
  预算耗尽按 module_id 序确定性整组排除，模组原子不拆成员）；每条排除含
  level/reason/module_id/明细。
- [x] `M7-TM2-G3` 一致性 fail-closed：active 集与协商结论错配（缺 verdict、
  module digest 不符、verdict 指向 active 外模组、active 集内重复 module_id、
  达到投影层的跨模组重名/重复 ToolId）整组拒绝返回错误，无部分投影；空集
  （无注册模组）投影为空视图且有确定 digest（§14 空 allowlist 兼容）。
- [x] `M7-TM2-G4` digest 绑定与确定性：snapshot_digest 绑定 generation +
  included module digest 集合 + (tool_id, spec_digest, wire_name) 集合；
  `tool_exposure_to_json` 为版本化 JSON（`mira.tool_module.exposure.v1`，
  脱敏）；`--report` 跨进程字节一致（无时钟、无随机、canonical JSON）。
- [x] `M7-TM2-G5` Simulator 参考模组与 Replay 绑定：参考模组经真实解析器
  构建且 manifest digest 为 golden 常量；全能力环境协商 Available 且投影
  两成员（副作用映射正确）；缺 `env.input.discrete` 时模组级 Unavailable
  附 missing；`verify_recorded_module_digests` 精确匹配通过，多/少/错
  digest 全部显式拒绝。
- [x] `M7-TM2-G6` 组合与 consumer 闭包：投影产物填入 `ModelRequest.tools`
  后 `resolve_tool_calls` 正常解析（wire_name/tool_id 匹配、digest 通过），
  未暴露名与 hosted 名 fail closed 语义不变；新公开头可被最小外部 consumer
  独立包含链接。

### 5.4 MCP 准入门禁（2026-09-20 跑前冻结；同日交付取证）

- [x] `M7-MCP-G1` 转换矩阵与确定性：合法 listing（多 descriptor、hint 组合）→
  manifest 经真实解析器全绿、成员映射与 §5.1/§5.2 冻结口径一致；负路径（空/字符集
  外成员名、重名、空/非对象/子集外 input_schema、越界描述/计数/资源、降风险
  override、未知成员 override、OutOfProcess 信任字段缺失）逐例整组拒绝且无部分
  状态；hint 映射三分支与宿主升风险允许/降风险拒绝；空 listing 整组拒绝（TM0
  manifest 既有 1..256 成员上界优先，`M7-TM0-G5` 冻结契约不为本阶段放宽——
  2026-09-20 IVA 首轮发现原「零成员合法模组」口径与 TM0 冲突后修订）；同输入
  manifest digest 与 admission 投影跨进程字节一致。
- [x] `M7-MCP-G2` 生命周期只降级：部署窗 admit 全链（转换 → 信任 → register →
  宿主 stage/activate → 协商 Available → TM2 投影含成员）；seal 后
  ServerConnected 拒绝且状态不变；close 后一切事件拒绝；ServerDisconnected 与
  ToolListChanged 对 Active 模组 → Revoked（digest tombstone、后续协商收敛、
  暴露面缩小不扩大）；终态/未注册模组上的事件 NoAction 幂等；在途请求按旧
  generation view 结算不受影响；lifecycle 事件字段脱敏（无 signature 原文、
  无 description 原文）。
- [x] `M7-MCP-G3` 单一门禁一致性（DEC-015 对照）：同一正/负矩阵分别驱动 MCP
  dispatcher 与 `BuiltinToolRegistry`——身份失配（wire/version/side effects）、
  重复 OperationId、参数 schema 违例 → failed record、handler/transport 错误 →
  failed record、取消 → `Cancelled`——两者接受/拒绝面逐条一致。
- [x] `M7-MCP-G4` 取消、deadline 与 shutdown 闭合：提交拒绝（executor
  stopping/capacity、未初始化）显式错误且 dispatched 预约回滚；transport 抛
  异常折叠为失败记录；执行中取消经探针传播（transport 轮询 stop 后返回
  Cancelled）；deadline 超时 → 置探针 → 有界宽限 → DeadlineExceeded 失败记录，
  放弃的 future 由 close 排空；close 后新派发 `InvalidState` 拒绝且计数可见；
  聚合并发上限达到 → `ResourceExhausted` 明确拒绝不排队；结果超限 → 失败记录；
  全矩阵无吞掉的异常、无未消费 future。
- [x] `M7-MCP-G5` 不可信数据与脱敏：descriptor 描述超限整组拒绝（不静默截断进
  manifest）；admission 投影与事件不含 description 原文与签名材料；transport
  结果经 `build_tool_result_input` 回填项不携带 System/Developer authority；
  `safe_error_summary` 512 字节有界；跨进程报告字节一致。
- [x] `M7-MCP-G6` consumer 闭包：新公开头可被最小外部 consumer 独立包含链接
  （`examples/minimal_consumer.cpp` 追加 MCP 闭包段）。

### 5.5 TR0 门禁（2026-09-21 跑前冻结；同日交付取证）

- [x] `M7-TR0-G1` 引用语法与解析矩阵：两种模式 golden 形态与规范往返（含真实
  spec digest 的钉住形态）；负矩阵（scheme 缺失/错误、空串、词表字符集违规、
  digest 缺失/非 64 位/大写/非十六进制、尾部字符、内嵌空白、超长、首尾点）逐例
  fail closed，错误 domain `mira.tool_reference`；模式名闭合集。
- [x] `M7-TR0-G2` 提取与钉住观察：以真实 `BuiltinToolRegistry` 曝光视图驱动
  提取——ToolCall 步骤逐条目提取、钉住条目记录视图 spec digest、逐工具覆盖与
  默认模式生效；负路径（`arguments["tool"]` 缺失/非字符串/字符集违规、视图外
  wire 名、未过结构校验的定义、选项重复/空覆盖名、视图重复 wire 名）整组拒绝
  无部分清单；清单绑定 `definition_digest` 且 `verify_workflow_tool_refs`
  通过/错配显式失败；无 ToolCall 定义产出确定空清单；JSON 往返无损。
- [x] `M7-TR0-G3` 解析矩阵与状态聚合：钉住/跟随 × 存在/消失/digest 演进全矩阵
  条目结论正确；聚合规则（任一 Unresolved/EvolvedIncompatible → Invalid，孤立
  EvolvedCompatible → Degraded，全 Resolved → Runnable，空清单 → Runnable）
  逐分支断言；清单-定义错配（workflow_id、definition_digest）整组拒绝。
- [x] `M7-TR0-G4` 骨架可绑定判定与确定性：占位符按型实例化语义（`$param` 位置
  类型变化仍兼容、新增必填属性 → 不兼容、具体值类型收紧 → 不兼容、enum 收窄
  违例 → 不兼容、数组/嵌套对象递归）；detail 含首个违例 path/keyword 且有界；
  同输入同投影 digest，`--report` 跨进程字节一致（无时钟、无随机、canonical
  JSON）；投影对视图重复 wire 名 fail closed。
- [x] `M7-TR0-G5` 准入决策、留痕脱敏与 DEC-015 组合：`Invalid` → 拒绝且 reason
  指向首个未通过条目；`Degraded` → 放行 + 留痕投影 `mira.workflow.tool_compat.v1`
  仅含身份/digest/结论与有界 detail（无 schema 体、无描述原文、无 secret）；
  `Runnable` → 放行无留痕要求；组合负向——准入 `Degraded` 的引用在执行期
  `BuiltinToolRegistry` 身份校验下仍被拒绝（投影不豁免执行期门禁）。
- [x] `M7-TR0-G6` consumer 闭包：新公开头可被最小外部 consumer 独立包含链接
  （`examples/minimal_consumer.cpp` 追加 TR0 闭包段）。

### 5.6 TR1 门禁（2026-09-21 跑前冻结；同日交付取证）

- [x] `M7-TR1-G1` 描述符与派生矩阵：`WorkflowParameterSpec` 四类型 × 全约束
  （min/max、minLength/maxLength、pattern、enum、required、default 不进
  schema）映射与 `gate_schema_subset` 通过；`has_side_effects` 推导（全部
  read_only → false、任一带副作用 → true）；负路径（summary 空/超限、refs
  未绑定或错配、视图缺引用、视图重名、未过结构校验的定义、hosted 保留名）
  整组拒绝；descriptor digest 确定性。
- [x] `M7-TR1-G2` 发布生命周期矩阵：publish 正路径（钉住 ir_digest 与暴露面
  digest 一致）与负矩阵（非 runnable 版本、digest/id 错配、保留名、重名、
  sealed/closed 后变更）逐例拒绝且状态不变；同 name + 同 digest 幂等 NoOp；
  upgrade 版本不递增拒绝、同 digest NoOp、superseded 轨迹正确；revoke 只
  降级、重复幂等、撤销后发布拒绝；统计可见。
- [x] `M7-TR1-G3` 事件与确定性：`mira.skill.publication.v1` 字段闭集与脱敏
  （无 description/schema 体/secret）；sink 失败计数不阻塞；`--report` 跨
  进程字节一致（无时钟、无随机、canonical JSON）。
- [x] `M7-TR1-G4` Procedure 投影与重建：同输入同投影同排序；statement 严格
  反解析回等价条目且重放字节一致；撤销/升级后投影演进正确（Revoked 条目
  显式状态、升级后钉住新 digest）；未发布 Workflow 资产不入索引（以显式
  发布为界）；无 description 泄漏。
- [x] `M7-TR1-G5` 组合边界：TR1 全程无执行面——`BuiltinToolRegistry` 曝光面
  与 `BuiltinToolRegistry::execute` 不受发布影响（负向断言）；发布经 TR0
  `extract_workflow_tool_references` + `derive_skill_surface` 组合链路成立；
  DEC-030 学习路径语义不变。
- [x] `M7-TR1-G6` consumer 闭包：新公开头可被最小外部 consumer 独立包含链接
  （`examples/minimal_consumer.cpp` 追加 TR1 闭包段）。

## 6. Executor 路由与关闭

TM0 全部为串行控制面内的同步纯计算（协商有界、无 I/O），不新增异步路径；TM1
的验证/安装类工作按工具模组设计 §10 以 `submit_auto()` 承载，future 必须消耗；
TM2 投影同样是串行控制面内的同步纯计算（有界、无 I/O、无时钟），不新增异步
路径。MCP 准入阶段（设计 §13）：转换、策略与 admission 组件为串行控制面纯
计算/簿记，不新增异步路径；在途 invocation 经 `submit_mcp_invocation()` 以
`submit_auto()` 承载，future 必须消费（正常路径即时消费、被放弃的 pending 由
dispatcher `close()` 有界排空），取消为三层协作探针（调用方 context →
dispatcher 探针 → transport 轮询），close 归宿主 shutdown 序列第 3 步。本里程碑
不引入脱离 Executor 生命周期的线程或定时器；能力缺口先登记
`docs/executor_feedback/ledger.md`。

## 7. 测试矩阵

| 层级 | 必测内容 |
| --- | --- |
| Contract | catalog/digest、派生 golden、manifest 校验矩阵、协商 golden 与 fail-closed 负向；MCP 转换矩阵与 hint 映射 |
| Component（TM1 起） | 状态机、签名/allowlist 验证、tombstone、事件；MCP 会话生命周期映射 |
| Integration（TM2 起） | 暴露投影、`resolve_tool_calls` 组合、Replay digest；MCP dispatcher 与 DEC-015 单一门禁对照、取消/deadline/shutdown |
| 边界 | 输入规模上限、空集、全不可用、同输入跨进程 digest 一致；MCP 不可信描述/结果脱敏 |

## 8. 退出条件

- 分阶段：每阶段的工作项与门禁全部完成、本地门禁（ctest、三 sanitizer、
  format/docs/platform-boundary/sbom、clang-tidy、NDK 两 ABI 预演）通过且 PR
  CI 全绿后，该阶段方可关闭。
- 总里程碑：TM0–TM2 及后续已立项阶段全部关闭；总计划、工具模组设计、决策
  状态同步；`M7-01`–`M7-06` 的重定义映射项全部有验证记录（`M7-07`–`M7-28` 按
  DEC-042 映射表保持推迟/缩减状态，不在本里程碑退出条件内）。

## 9. 验证记录

2026-09-01：依据总计划、DEC-009、ToolModule/评估/平台设计创建详细计划；状态为
`Planned`，尚未实现 ToolModule、真实平台 Host 或 v1 eval harness，未执行发布
门禁。负责人为 Mira Maintainers；准入条件为 M4、M5、M6 完成以及 v1 profile、
设备、账号、签名和测试环境就绪。

2026-09-05：按 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)
状态改为 `Blocked`。阻塞原因：前置 M5/M6 已终止，本文件声明的范围（含"M5 本地
模型、M6 控制进入统一 eval 报告"等退出条件）失去前提，不能按原文交付。负责人
为 Mira Maintainers；解除条件为外部 demo 仓库产出需求验证报告，并据此完成 M7
范围、前置与发布点的重定义提案且经决策记录批准。重定义前本文件保持冻结，
`M7-01` 至 `M7-28` 与退出条件不适用；DEC-009 的 Tool 模组架构方向保留。

2026-09-06：登记方向性缺口 GitHub
[#8](https://github.com/Linductor-alkaid/mira/issues/8)（MIR-20260905-002，
AgentLoop 内 ToolProposals 不可执行，`src/model/agent_loop.cpp` 对 ToolProposals
解析结果显式失败）。该缺口属本里程碑范围（Tool 执行闭环），随 M7 重定义一并
处理；触发条件为 miracle `POST-01` 的证据到位，届时按证据重定义验收（见
[维护计划 maintenance-2026-09-host-abi-feedback.md](maintenance-2026-09-host-abi-feedback.md)
`MNT-202609-06`）。在此之前不实现、不排期，本文件状态不变。

2026-09-07：GitHub #8 的**最小闭环**由维护者指令提前落地（依据
[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 的 Harness
审计结论，用户指令优先级高于本文件的触发条件约定）：`BuiltinToolRegistry` 最小
执行边界 + AgentLoop 工具分支见
[DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md) 与
[维护计划 maintenance-2026-09-agent-harness-closure.md](maintenance-2026-09-agent-harness-closure.md)。
#8 随该轮关闭。原 M7 文件范围与范围**不变**：DEC-009 模组体系（manifest/签名/
隔离/协商）、ToolProposals 的模组化执行与评估体系仍属 M7 重定义范围，原触发
条件继续适用。

2026-09-16：依 [DEC-042](../decisions/DEC-042-m7-scope-redefinition.md)
（`MNT-202609-30` 的交付物）完成重定义：范围收敛为 Tool 模组体系分阶段落地
（TM0/TM1/TM2 + DEC-039/DEC-040 后续阶段），前置改为 M4 + DEC-042，发布点改为
Tool module alpha（分阶段锚点）；原 `M7-01`–`M7-28` 按 DEC-042 §4 迁移映射处置
（保留/缩减/推迟），推迟项保持 `MNT-202609-27` 证据门禁或待未来发布立项。状态
`Blocked` → `Planned`；TM0 工作项（`M7-TM0-01`–`05`）与门禁（`M7-TM0-G1`–`G6`）
跑前冻结，TM0 进入实施。

2026-09-16：TM0 交付关闭。交付 `include/mira/tool_module.hpp` +
`src/tool/tool_module.cpp`（入 `mira_core`）：`CapabilityCatalog` 13 条核心词表
与目录 digest、`derive_environment_capabilities` 派生纯函数、
`mira.tool_module.manifest.v1` fail-closed 解析（schema 子集同源校验、目录内
能力引用、origin 信任字段、资源上限、ABI 上限、canonical digest）与
`negotiate_modules` 确定性协商（Unavailable/Conflict fail-closed、排序无关、
同输入同 digest、无 I/O 无时钟）。测试 `tests/m7/m7_tool_module_test.cpp`
（label `contract`，门禁 `M7-TM0-G1`–`G6`；测试的编写、运行与 sanitizer 取证
由 Independent-Verification-Agent 独立完成）与 `examples/minimal_consumer.cpp`
TM0 闭包段（补注册 `mira_minimal_consumer_test`）。本地门禁：全量 ctest
**84/84**、ASAN/UBSAN/TSAN m7 目标零报告（`setarch -R`）、format/docs/
platform-boundary/sbom 四检查通过、clang-tidy 18.1.8 预检库源零违例、本机 NDK
r26.3 两 ABI（arm64-v8a/x86_64）交叉编译 `mira_core`+`mira_workflow` 通过且
`negotiate_modules` 符号在库。实现期修复两处（均经 IVA 发现/复验）：schema
子集拒绝错误 domain 归一为 `mira.tool_module`；两处 clang-tidy
`performance-move-const-arg`（平凡可拷贝 `SemanticVersion` 的 `std::move`）。
附带修复：本文件一处预先存在的坏链（`DEC-040` 链接缺 `../decisions/` 前缀）；
`mira_minimal_consumer` 此前从未注册 ctest 的缺口。
PR [#58](https://github.com/Linductor-alkaid/mira/pull/58)（head `8e2dd62`，
merge `f2d2077`）双 pipeline 各 12 项首轮全绿（push run
[`35125359147`](https://github.com/Linductor-alkaid/mira/actions/runs/35125359147) /
pull_request run
[`35125364698`](https://github.com/Linductor-alkaid/mira/actions/runs/35125364698)），
master 合并提交 run
[`35127320798`](https://github.com/Linductor-alkaid/mira/actions/runs/35127320798)
success。限制与未执行项：签名字段的密码学验证、Registry 状态机与暴露投影分别
归 TM1/TM2（实施前在 §4.2/§4.3 冻结细项）；脚本化确定性口径非语义质量声明
（`RULE-10`）；TSAN 仅覆盖纯计算路径（TM0 无并发）。TM0 关闭后下一阶段为
TM1（Registry 生命周期）。

2026-09-19：TM1 细项冻结并交付。交付 `include/mira/tool_module_registry.hpp` +
`src/tool/tool_module_registry.cpp`（入 `mira_core`）：`ModuleTrustConfig` 与
`verify_module_trust` 三 origin 来源信任纯函数（BuiltIn 构建钉定 digest /
HostProvided allowlist——[DEC-009](../decisions/DEC-009-tool-module-boundary.md)
暂定默认值升格 v1 冻结 / OutOfProcess 注入式 `IModuleSignatureVerifier` 绑定
unsigned manifest digest；`tool_module.hpp` 加法式新增
`tool_module_unsigned_manifest_digest` 作为签名载荷）；`ModuleRegistry` 状态机
（七状态受控转换、`seal()`/`close()` 注册窗口与终态、跨模组 wire 名冲突激活
fail-closed、revoke tombstone 播种与运行期追加、不可变 `RegistrySnapshot`
generation/digest）；`ModuleNegotiationCoordinator` 三类触发（session 一次、
epoch 单调与同 epoch 幂等、模组状态变化；closed registry 拒绝触发）；生命周期
与协商两类版本化事件（`mira.tool_module.lifecycle.v1` /
`mira.tool_module.negotiation.v1`，脱敏、sink 失败计数不阻塞）；
`submit_module_verification`/`consume_module_verification` Executor 路由
（`submit_auto()` + future 经 consume 折叠全部拒绝面为显式 Result）。测试
`tests/m7/m7_tool_module_registry_test.cpp`（20 个 gate 函数、约 500 断言，
label `contract`；测试的编写、运行与 sanitizer 取证由
Independent-Verification-Agent 独立完成，共两轮：首轮抓到提交拒绝路径两处
实现缺陷——shutdown 后 facade 抛普通 `runtime_error` 而非 `ExecutorStopping`
映射失配、容量拒绝经 ready-with-exception future 逃逸 Result 边界——主循环
修复为双层显式拒绝面并补齐 coordinator 的 closed 检查后复验通过）；
`examples/minimal_consumer.cpp` 追加 TM1 闭包段。本地门禁：全量 ctest
**85/85**（原 84 + 本里程碑 1 目标）、ASAN/UBSAN/TSAN（`setarch -R`）m7
目标零报告、docs/platform-boundary/sbom/format 四检查通过（format 由与 CI
同行为的 LLVM 18 二进制校验）、clang-tidy 18.1.8 预检两个被改库源零违例
（一处 `performance-move-const-arg` 修复后复验）、本机 NDK r26.3 两 ABI
交叉编译 `mira_core`+`mira_workflow` 通过且 8 个 TM1 符号在库；TM1 契约
报告 `--report` 跨进程字节一致（md5
`aba09395c81b616f4adc0cd3d75829ab`）。限制与未执行项：OutOfProcess 签名的
密码学校验为宿主注入（Core 仅确定性绑定参考实现，`RULE-10`）；executor
shutdown 进行中并发窗口的提交行为未注入竞态测试；Windows/Android 运行与
Release/quality 由 PR CI 回填后 TM1 方可关闭。
2026-09-19：PR CI 证据回填并关闭。PR
[#59](https://github.com/Linductor-alkaid/mira/pull/59)（head `01de324`，合并提交
`76eec95`）push 与 pull_request pipeline run
[`35425708347`](https://github.com/Linductor-alkaid/mira/actions/runs/35425708347)/
[`35425754169`](https://github.com/Linductor-alkaid/mira/actions/runs/35425754169)
各 12 项首轮全部通过（Linux GCC/Clang Debug/Release、Windows MSVC、Android 两
ABI、ASAN/UBSAN/TSAN、quality），master 合并提交 run
[`35427226722`](https://github.com/Linductor-alkaid/mira/actions/runs/35427226722)
success（12/12）。`M7-TM1-01`–`03` 与 `M7-TM1-G1`–`G6` 关闭；下一阶段为 TM2
（LLM 暴露投影，实施前在本文件 §4.3 冻结细项与门禁）。

2026-09-19：TM2 细项冻结并交付。交付 `include/mira/tool_module_exposure.hpp` +
`src/tool/tool_module_exposure.cpp`（入 `mira_core`）：`project_tool_exposure`
投影纯函数（active 集与协商结论错配——缺 verdict、digest/version 不符、引用集外
模组、重复 module_id、漏网重名/重复 ToolId——整组 fail closed 无部分投影；空集
投影为确定空视图，M3 空 allowlist 兼容）；ToolId 由 (module_id, module_digest,
成员名) 确定性派生（TM2 身份分配落地）；`ActionRisk != read_only →
has_side_effects` 映射；两级排除 `ToolExclusion`（模组级 module_unavailable 附
missing/module_conflict/module_revoked/reserved_wire_name，任务级
task_policy/task_budget，模组原子不拆成员）；合成 `snapshot_digest` 绑定
generation + included module digest 集 + (tool_id, spec_digest, wire_name) 三元组
集（DEC-002 加法演进：`PromptProvenance.tool_snapshot_digest` 字段与非模组
`tool_snapshot_digest` 纯函数不变）；`tool_exposure_to_json`（
`mira.tool_module.exposure.v1`，脱敏）；Replay 绑定
`verify_recorded_module_digests`（集合精确匹配，多/少/错显式拒绝）；Simulator
BuiltIn 参考模组 `make_simulator_reference_module`（`builtin.simulator.env`，经
真实解析器构建，manifest digest 为 golden 常量）；`wire_name` 规则 v1 冻结（设计
§17 收口 + DEC-009 注记：扁平命名空间、成员名原文、唯一性由协商/激活门禁承载、
hosted 保留名整组排除、前缀为非约束性约定）。

实现期修复两处（均经 Independent-Verification-Agent 独立发现/复验）：
(1) 既有 TM0 缺陷——`parse_tool_module_manifest` 成员重名检测持有悬垂
`string_view`（成员名被 move 后栈槽复用，等长 ≤15 字符短名被误判重复），修复为
拷贝语义并在 TM0 测试矩阵新增回归门（等长短名对/三成员解析成功、真重复相邻与
间隔位置仍拒绝）；(2) **format 门禁空转缺陷**——`format-check` 目标在 `VERBATIM`
下 `-DROOT_DIR="..."` 的内嵌引号被字面保留，ROOT_DIR 带引号使 GLOB 匹配零文件、
检查自 M0 基线（`89d88a6`）起静默通过；修复传参与脚本（零文件 FATAL_ERROR
防护 + 文件计数输出），并按 CI 口径（clang-format 18.1.8）整树机械重排 142 个
漂移文件。**更正声明：既往各轮验证记录中"format 检查通过"的证据对 C++ 文件
为空转结果，不构成格式合规证据；本轮起以修复后门禁（214 文件真实检查）为准，
历史记录按规范保留不重写。**

测试 `tests/m7/m7_tool_module_exposure_test.cpp`（16 个 gate 函数、约 360 断言，
label `contract`；测试的编写、运行与 sanitizer 取证由 Independent-Verification-
Agent 独立完成，共三轮：首轮覆盖 G1–G6 全部通过并发现 TM0 解析器缺陷；次轮
复验修复并落地回归门与短名 fixture；末轮重排后最终取证）；TM0 测试矩阵同步
扩至 191 断言；`examples/minimal_consumer.cpp` 追加 TM2 闭包段。本地门禁：全量
ctest **86/86**（原 85 + 本里程碑 1 目标）、ASAN/UBSAN/TSAN（`setarch -R`）m7
两测试 + consumer 9 项零报告、format/docs/platform-boundary/sbom 四检查通过
（format 由修复后门禁真实检查 214 文件；format 重排的"纯机械性"经 IVA 逐文件
去空白 diff 审计：118 文件去空白逐字节一致，其余差异全部归因于 include 排序与
本轮已验证代码变更）、clang-tidy 18.1.8 预检两个被改库源零违例、本机 NDK
r26.3 两 ABI（arm64-v8a/x86_64）交叉编译 `mira_core`+`mira_workflow` 通过且
TM2 符号在库（重排后重建复验）；TM2 契约报告 `--report` 跨进程与跨四个构建树
（debug/asan/ubsan/tsan）字节一致（md5
`a96637323a11eaeea7cd433c0e97e560`），TM0 契约报告 md5 保持
`0b25496af889d106e5ecbfcb9b874d45` 不变（重排无行为影响佐证）。限制与未执行项：
"重复派生 ToolId"防御分支无法经公开输入构造触发（需 sha256 前 16 字节碰撞），
仅静态审查覆盖；executor shutdown 进行中并发窗口未注入竞态测试（与 TM1 一致）；
Windows/Android 运行与 Release/quality 由 PR CI 回填后 TM2 方可关闭。
2026-09-19：PR CI 证据回填并关闭。PR
[#60](https://github.com/Linductor-alkaid/mira/pull/60)（head `21e749d`，合并提交
`0655d65`）pull_request pipeline run
[`35452396490`](https://github.com/Linductor-alkaid/mira/actions/runs/35452396490)
12 项全部通过（Linux GCC/Clang Debug/Release、Windows MSVC、Android 两 ABI、
ASAN/UBSAN/TSAN、quality——format 门禁首次真实检查 214 文件并在 CI 通过）；
push run
[`35452378534`](https://github.com/Linductor-alkaid/mira/actions/runs/35452378534)
11/12 首轮通过，唯一未过项为 TSAN 作业在 9m48s 被**基础设施取消**（日志
`The operation was canceled`，跑至 M16 基线中途被杀，无任何测试失败或 TSAN
报告输出；workflow 无 timeout/concurrency 配置，取消发生在作业级），重跑该
作业后 run conclusion `success`；master 合并提交 run
[`35455012483`](https://github.com/Linductor-alkaid/mira/actions/runs/35455012483)
success（12/12）。`M7-TM2-01`–`03` 与 `M7-TM2-G1`–`G6` 关闭；M7 已立项阶段
（TM0–TM2）全部关闭，后续阶段（DEC-039/DEC-040）立项时增补工作项与门禁。

2026-09-20：MCP 准入阶段细项冻结并交付（DEC-039 首个实现阶段，前置 TM0–TM2
已全部关闭；维护者指令「依设计与计划推进下一步开发」，与 TM0–TM2 同一授权
模式）。立项同步交付专项设计
[MCP 准入设计](../design/mcp_tool_admission_design.md)（DEC-039 关联文档要求的
首阶段产出；[DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md) 状态行
与[工具模组设计](../design/tool_module_design.md) v1.4 §15/§17/§18 同步注记）。
交付 `include/mira/tool_module_mcp.hpp` + `src/tool/tool_module_mcp.cpp`（入
`mira_core`）：`McpToolDescriptor`/`McpServerListing` 受控子集与
`convert_mcp_listing_to_module` 转换纯函数（组装 manifest JSON 过**真实**
`parse_tool_module_manifest`，TM0 全部门禁原样生效；hint→ActionRisk 确定性
映射；`risk_overrides` 只升不降；MCP 专属 fail-closed 前置检查）；脱敏投影
`mira.tool_module.mcp.admission.v1`（不含 description 原文与签名材料）；
`plan_mcp_session_action` 冻结策略矩阵 + `McpModuleAdmission` 部署窗准入与
运行期只降级应用；`IMcpToolTransport`/`McpInvocationProbe`/`McpToolDispatcher`
执行适配（DEC-015 同源门禁：曝光身份一致、OperationId 至多一次（预约-回滚）、
参数本地校验、聚合并发/结果上限；`submit_mcp_invocation`/`consume_mcp_invocation`
以 `submit_auto()` 承载、future 必消费、放弃路径 pending 由 `close()` 有界排空、
deadline 取 `min(context.deadline, 上限)`、三层协作取消）。

实现期裁决一处（IVA 首轮发现）：**空 listing 口径冲突**——本阶段冻结稿曾写
「空 listing 产出零成员合法模组」，与 TM0 冻结契约 `mira.tool_module.manifest.v1`
的 1..256 成员上界（`M7-TM0-G5`）互斥。按上位契约优先原则修订为本设计口径
错误：空 listing 整组拒绝（拒绝理由来自真实解析器），宿主对不暴露 tool 的
server 跳过准入；设计 §5.3/§11、本文件 `M7-MCP-G1` 与公开头注释同变更内修订
（含更正注记），实现零改动。

测试 `tests/m7/m7_tool_module_mcp_test.cpp`（25 个 gate 函数、456 断言，label
`contract`；测试的编写、运行与 sanitizer 取证由 Independent-Verification-Agent
独立完成，共两轮：首轮 24/25 全绿并抓出上述空 listing 缺陷；主循环裁决修订
口径后复验 25/25 全绿）。取证要点：G1 负矩阵约 40 例整组拒绝（domain
`mira.tool_module`）；G2 只降级矩阵（策略纯函数 84 组合 + admit 全链到 TM2 投影
+ revoke/tombstone/钉住旧代结算）；G3 与 `BuiltinToolRegistry` 的 11 行对照表
两引擎逐条一致（DEC-015 单一门禁）；G4 取消/deadline/提交拒绝/transport 异常/
并发与结果上限/close 排空全矩阵显式结果且无未消费 future；G5 脱敏与 authority
负向；G6 consumer 闭包。`--report` 跨进程与跨构建树（debug/asan）字节一致
（md5 `19b8f358f9615d2caa7415eee6b12714`）。本地门禁：全量 ctest **87/87**
（原 86 + 本里程碑 1 目标）、ASAN/UBSAN/TSAN（TSAN 需 `setarch -R`，本机内核
ASLR 已知启动期不兼容，非代码竞争）m7 目标 + consumer 零报告、format/docs/
platform-boundary/sbom 四检查通过（format 真实检查 216 文件）、clang-tidy
18.1.8 预检新库源零违例（一处 `performance-no-automatic-move` 修复后复验）、
本机 NDK r26.3 两 ABI（arm64-v8a/x86_64）交叉编译 `mira_core`+`mira_workflow`
通过且 MCP 符号在库。限制与未执行项：脚本化 transport 桩口径，真实 MCP
server/传输互操作归宿主侧证据（`RULE-10`）；钉住的 Executor v0.5.0 对未初始化
facade 接受提交（无「未初始化拒绝」折叠面可观测，测试按可观测行为钉住并注释）；
DEC-040 引用解析组合测试归后续 DEC-040 阶段；Windows/Android 运行与
Release/quality 由 PR CI 回填后 MCP 阶段方可关闭（下一条记录回填闭环）。

2026-09-20：PR CI 证据回填并关闭。PR
[#62](https://github.com/Linductor-alkaid/mira/pull/62)（head `39884de`，合并提交
`ae7410d`）push 与 pull_request pipeline run
[`35512461239`](https://github.com/Linductor-alkaid/mira/actions/runs/35512461239)/
[`35512472810`](https://github.com/Linductor-alkaid/mira/actions/runs/35512472810)
双 pipeline 各 12 项**首轮全部通过**（Linux GCC/Clang Debug/Release、Windows MSVC
Debug/Release、Android 两 ABI、ASAN/UBSAN/TSAN、quality——format 真实检查 217
文件），零修复复验；master 合并提交 run
[`35513574793`](https://github.com/Linductor-alkaid/mira/actions/runs/35513574793)
success（12/12）。`M7-MCP-01`–`04` 与 `M7-MCP-G1`–`G6` 关闭；M7 已立项阶段为
TM0–TM2 + MCP 准入，后续阶段（DEC-040）立项时增补。



2026-09-21：TR0 细项冻结并交付（DEC-040 首个实现阶段，前置「模组体系落地」已
满足；维护者指令「依设计与计划推进下一步开发」，与 TM0–TM2、MCP 准入同一授权
模式）。立项同步交付专项设计
[Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md)
（引用语法 v1、提取、解析矩阵、兼容投影、准入与留痕、DEC-015 边界、TR1 方向
预告；基于版本约束的钉住不进 v1 的裁决见该文 §4.3）。

交付 `include/mira/tool_reference.hpp` +
`src/workflow/tool_reference.cpp`——**入 `mira_workflow`**：引用层消费
`WorkflowDefinition` + `ExposedToolSpec` 暴露视图，属 Workflow 资产面，依赖
方向 workflow→core 不变（`mira_core` 不含 IR 实现，不能承载）；最小 consumer
闭包相应链接 `Mira::workflow`，仍不触碰 Executor API。契约：`parse_tool_reference`
/`tool_reference_to_string` 引用语法 v1（跟随/钉住 spec digest，词表字符集，
fail-closed，domain `mira.tool_reference`，domain_code 1/2/3）；
`extract_workflow_tool_references` 提取 `mira.workflow.tool_refs.v1` 清单
（绑定 workflow_id + `definition_digest`、发布期可解析 fail closed、钉住观察
digest、逐 wire 名模式覆盖、严格 JSON 往返、`verify_workflow_tool_refs` 绑定
校验、IR v1 零改动）；`project_workflow_tool_compatibility` 解析矩阵与
`Runnable`/`Degraded`/`Invalid` 聚合（骨架可绑定判定 = `$param` 占位符按型
实例化后交既有 M3 严格 schema 校验器 `validate_instance_against_schema`，准入
期与派发路径同一校验器零漂移）；`admit_workflow_run_by_tool_compat` 准入决策
与 `mira.workflow.tool_compat.v1` 脱敏留痕投影；DEC-015 执行期校验不变并组合
负向入测。

实现期修复一处（IVA 首轮发现）：`toolref:<name>@`（`@` 后 digest 缺失）被
误判为跟随模式接受，违反设计 §4.2「`@` 后缺失一律拒绝」；修复为记录
`has_digest_part` 后空 digest 走 `parse_pinned_digest` 失败路径，复验零测试
改动。

测试 `tests/m7/m7_tool_reference_test.cpp`（21 个 gate、约 239 断言，label
`contract`；测试的编写、运行与 sanitizer 取证由 Independent-Verification-
Agent 独立完成，共两轮：首轮 G1 负矩阵抓到上述解析缺陷（其余 39 例负路径与
全部正路径通过），主循环修复后复验 21/21 全绿，交付测试文件 md5
`2733d4b8f1b5246f48aef07dbd235f6c`）。取证要点：G1 含 40 例负矩阵；G2 以真实
`BuiltinToolRegistry` 曝光视图驱动提取并覆盖 JSON 变异拒绝；G3 钉住/跟随 ×
存在/失配/消失全矩阵与聚合逐分支；G4 实例化语义（类型变化兼容、新增必填/
类型收紧/enum 收窄不兼容、detail 与校验器首违例逐字一致、有界）；G5 准入
reason 指向首个未通过条目、留痕 JSON 闭字段集脱敏、DEC-015 组合负向（Degraded
准入后旧身份提案仍被 `BuiltinToolRegistry` 拒绝）。`--report` 跨进程与跨四
构建树（debug/asan/ubsan/tsan）字节一致（md5
`4c4d8c72654e3211d6f9b6e3f7901666`，3773 字节）。本地门禁：全量 ctest
**88/88**（原 87 + 本里程碑 1 目标）、format/docs/platform-boundary/sbom 四
检查通过（format 真实检查 220 文件）、clang-tidy 18.1.8 预检新库源零违例、
本机 NDK r26.3 两 ABI（arm64-v8a/x86_64）交叉编译 `mira_core`+`mira_workflow`
通过且 TR0 符号在库。PR
[#63](https://github.com/Linductor-alkaid/mira/pull/63)（head `4313cc1`，
格式修复 `522bfb3`，合并提交 `a180e88`）：首轮 push run
[`35520589859`](https://github.com/Linductor-alkaid/mira/actions/runs/35520589859)
quality 失败——IVA 交付的测试文件未过 clang-format（唯一违例文件，纯空白
重排，`--report` md5 不变佐证行为中性），`522bfb3` 修复后第二轮双 pipeline
run [`35522450999`](https://github.com/Linductor-alkaid/mira/actions/runs/35522450999)/
[`35522453203`](https://github.com/Linductor-alkaid/mira/actions/runs/35522453203)
各 12 项全部通过；master 合并提交 run
[`35523751427`](https://github.com/Linductor-alkaid/mira/actions/runs/35523751427)
success（12/12）。`M7-TR0-01`–`04` 与 `M7-TR0-G1`–`G6` 关闭；M7 已立项阶段
为 TM0–TM2 + MCP 准入 + TR0，后续阶段（TR1：Skill 生命周期、Procedure 索引
投影、Runtime 接线与 IR 引用表达加法演进）立项时增补。限制与未执行项：清单/
投影的存储挂载、`create_run` 准入消费与 `Degraded` 事件发射接线归 TR1/首个
消费者里程碑（本阶段交付其消费的决策与投影产物，设计 §2.2）；确定性投影口径
非语义质量声明（`RULE-10`）；`pattern` 约束位置按设计 §7.2 保守判不兼容。

2026-09-21：TR1 细项冻结并交付（DEC-040 第二阶段，前置 TR0 已关闭；与 TR0 同一
维护者授权模式、同一日连续交付）。规范随
[Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md) §17
冻结（描述符与暴露面派生、发布生命周期、Procedure 索引投影；TR2 方向收窄为
Runtime 接线与执行）。

交付 `include/mira/tool_skill.hpp` + `src/workflow/tool_skill.cpp`（入
`mira_workflow`）：`derive_skill_surface` 暴露面确定性派生（description 取
summary 非空有界、参数 schema 从 `WorkflowParameterSpec` 全类型映射过
`gate_schema_subset` 且 default 不进 schema、`has_side_effects` 由 TR0 引用
清单 × 视图推导）；`make_skill_descriptor`（词表字符集 + hosted 保留名拒绝 +
canonical digest）与 `mira.skill.descriptor.v1` 严格 JSON 往返；
`SkillPublicationRegistry` 宿主显式发布/升级/撤销（publish fail-closed 链：
descriptor digest 自洽、定义结构校验、workflow_id/ir_digest 绑定、
`resolve_workflow_version` + `workflow_version_is_runnable`（DEC-025 门禁）、
refs 绑定、surface 重派生一致；name + descriptor 幂等 NoOp；升级严格递增 +
superseded 轨迹 + 同 digest NoOp；撤销只降级、重复幂等；seal/close 部署窗与
拒绝计数）；事件 `mira.skill.publication.v1` 脱敏（revoked=Critical、sink
失败计数不阻塞）；`project_skill_procedure_index` +
`skill_procedure_entry_from_statement`（`mira.skill.procedure_index.v1`，以
显式发布为界——未发布库资产不入索引，statement 固定 canonical JSON、无时钟、
严格可重建、本阶段不写 `IMemory`）。TR1 无执行面（Skill 不进
registry/exposure/协商，G5 负向断言）。

实现期修复一处（IVA 首轮发现并经主循环裁决为实现缺陷）：升级后原样重发布当前
descriptor 被误拒 `AlreadyExists`——幂等条件冻结口径为「同 name + 同
descriptor」（M7 `M7-TR1-02`、设计 §17.2），原实现误用整记录相等（superseded
轨迹非空导致失配）；修复为 `status == Published && descriptor ==` 判定，复验
经独立探针与新增断言（含 Revoked 分支回归守卫）双重取证。

测试 `tests/m7/m7_tool_skill_test.cpp`（23 个 gate、430 断言，label
`contract`；测试的编写、运行与 sanitizer 取证由 Independent-Verification-
Agent 独立完成，共两轮：首轮 23/23 全绿并抓出上述幂等缺陷，主循环修复后复验
23/23 全绿，测试文件 md5 `0bb49b3eed6d455104be33402126195a`）。取证要点：G1
四类型 × 全约束映射 + 保留名/边界负矩阵；G2 发布全负矩阵逐例拒绝 + 幂等 +
seal/close 窗口 + 计数；G3 注入 sink 事件闭字段集脱敏 + 失败隔离；G4 投影
确定性/重建/演进（升级钉新 digest、Revoked 显式状态、未发布资产零泄漏）；
G5 registry 曝光与执行不受发布影响 + TR0→TR1 组合链。`--report` 跨进程与跨
四构建树字节一致（md5 `e8a91ca5e030df1584250dcb964fa1a3`，4069 字节）。本地
门禁：全量 ctest **89/89**（原 88 + 本里程碑 1 目标）、format/docs/
platform-boundary/sbom 四检查通过（format 真实检查 223 文件）、clang-tidy
18.1.8 预检新库源零违例、本机 NDK r26.3 两 ABI（arm64-v8a/x86_64）交叉编译
`mira_core`+`mira_workflow` 通过且 TR1 符号在库。PR
[#64](https://github.com/Linductor-alkaid/mira/pull/64)（head `8159e24`，
合并提交 `2833bc4`）双 pipeline run
[`35528677082`](https://github.com/Linductor-alkaid/mira/actions/runs/35528677082)/
[`35528693696`](https://github.com/Linductor-alkaid/mira/actions/runs/35528693696)
各 12 项首轮全部通过，master 合并提交 run
[`35530911707`](https://github.com/Linductor-alkaid/mira/actions/runs/35530911707)
success（12/12）。`M7-TR1-01`–`04` 与 `M7-TR1-G1`–`G6` 关闭；M7 已立项阶段为
TM0–TM2 + MCP 准入 + TR0 + TR1，后续阶段（TR2：WorkflowRuntime 接线与执行——
库存储挂载 tool_refs、`create_run` 准入消费、`Degraded` 事件发射、Skill 经
Tool 通道的子 Workflow 调用执行适配、IR 引用表达加法演进）立项时增补。限制
与未执行项：Procedure statement 落库 `IMemory`（scope/ACL/检索/时间戳）、
Skill 执行面与 `create_run` 接线归 TR2（本阶段无从验证，设计 §17.4）；发布
即索引的口径使索引面限于宿主显式动作（规则性限制，放宽需新 DEC）。
