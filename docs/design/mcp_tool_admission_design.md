# MCP 工具模组准入设计（DEC-039 落地规范）

> 状态：Active（冻结规范，决策载体为
> [DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md)；实现载体为
> [M7](../plans/m7-tools-evaluation-platform-v1.md) 的 MCP 准入阶段；本文代码片段均为
> 契约草案，签名以实现为准）  
> 版本：1.0  
> 更新日期：2026-09-20  
> 负责人：Mira Maintainers  
> 适用范围：MCP Server tool 经统一 descriptor 进入 Tool 模组体系的 Core 侧契约
> （转换、准入、会话生命周期映射、执行适配）；不覆盖 MCP client 传输实现与
> server 生命周期管理（宿主/Adapter 职责）  
> 上位设计：[工具模组设计](tool_module_design.md)、
> [Agent Harness 与 Workflow 架构设计](agent_harness_and_workflow_architecture.md)  
> 决策依据：[DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md)、
> [DEC-009](../decisions/DEC-009-tool-module-boundary.md)、
> [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)、
> [DEC-002](../decisions/DEC-002-public-contract-versioning.md)

## 1. 文档目的与效力

TM0–TM2 交付后，模组体系的「目录/协商（TM0）→ 生命周期（TM1）→ 暴露投影（TM2）」
链路闭合，但仓库中所有模组来源仍是 BuiltIn fixture。本文冻结 DEC-039 方向的首个
实现阶段：MCP Server 的 tool 如何以 `ToolModule` 身份进入这条链路，运行期如何
只降级不扩面，进程外调用如何在 Core 侧获得与 BuiltIn 工具同源的门禁、取消与
shutdown 语义。回答四件事：

1. MCP `tools/list` 结果的哪个受控子集被消费，转换成什么，在哪里校验。
2. MCP server 会话事件映射为哪些模组状态机迁移，哪些事件被显式拒绝。
3. 注册后的 MCP tool 调用如何经过与 DEC-015 完全同源的执行门禁，取消、超时与
   shutdown 如何闭合。
4. server 提供的描述与结果作为不可信数据，在哪里被截断、脱敏，为什么不提升
   authority。

本文与 DEC-039 同时生效；冲突时以 DEC 为准。既有语义依据：DEC-009（模组边界与
fail closed）、DEC-015（执行边界：身份一致、至多一次、参数本地校验、模型可归因
失败）、DEC-002（版本化契约加法演进）、DEC-004（权限与确认，本阶段不新增豁免）。

## 2. 背景、目标与非目标

### 2.1 背景

[DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md) 解除了「MCP 作为
**部署与初始化时注册**的外部 Tool 来源」的否决：MCP tool 不是第二套注册体系，而是
origin 为 `out_of_process` 的 `ToolModule` 成员，与 BuiltIn/HostProvided 同规则
协商、暴露与执行。实现前置（DEC-009 模组体系落地）已由 TM0–TM2 满足；本设计把
该方向冻结为可实施的 Core 契约。

### 2.2 目标

- 一个确定性转换纯函数：MCP server tool listing 的受控子集 → 通过真实 manifest
  解析器的 `ToolModuleManifest`（origin `out_of_process`），任何 descriptor 缺陷
  整组拒绝（fail closed，无部分成员）。
- 会话生命周期映射：server 连接只在部署窗内准入；断开与能力列表变化在运行期只
  映射为降级（revoke），绝不扩面；变更后的 listing 需要新的注册周期。
- 执行适配：宿主注入的 transport 承载进程外 I/O；Core 侧 dispatcher 落地与
  `BuiltinToolRegistry`（DEC-015）同源的调用门禁，并把在途调用经 Executor 路由，
  取消/deadline/shutdown 全部转化为显式结果。
- 不可信数据纪律的落地证据：描述有界、事件与投影脱敏、结果不提升 authority。

### 2.3 非目标

- 不实现 MCP 协议本身：JSON-RPC 编码、stdio/HTTP 传输、连接管理、重连与
  server 进程监督全部归宿主/Adapter；Core 只消费结构化 descriptor 与结果
  （DEC-039 §6）。
- 不覆盖 MCP 的 resources/prompts/sampling 能力面（DEC-039 §8）。
- 不做运行中动态发现与热插拔；不让模型发起或管理 server 连接（DEC-039 维持
  否决）。
- 不冻结 Tool 稳定引用 URI 形态（归 [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)
  立项冻结）；本阶段的引用载体就是既有 wire 名 + ToolId + digest 三元组。
- 不承诺任何真实 MCP server 的兼容性或质量（`RULE-10`）；真实互操作验证随
  宿主侧证据另行立项。
- 不改变 Workflow `ToolCall` 步骤语义：注册后的 MCP tool 对 DEC-019/DEC-021 是
  普通 tool 引用，权限挂钩点不变。

## 3. 系统上下文与依赖方向

```text
MCP Server (进程外，不可信)
   │  tools/list 结果（宿主在部署/初始化阶段采集）
   ▼
宿主/Adapter（拥有 client I/O、传输选型、server 生命周期）
   │  McpServerListing（受控子集）        │  IMcpToolTransport（执行期调用）
   ▼                                      ▼
Mira Core：convert_mcp_listing_to_module   Mira Core：McpToolDispatcher
        │                                          ▲
        ▼                                          │ ExposedToolSpec（TM2 投影）
ModuleRegistry（TM1 状态机/信任/tombstone） ──协商──┘
```

依赖方向：本阶段新增代码只依赖既有 `tool_module*.hpp` 公开契约与
`model_tool.hpp`/`model_schema.hpp` 的校验原语；Core 不引入 MCP 协议类型、第三方
client 依赖或任何平台 API（`RULE-01`、DEC-039 §7 供应链纪律）。宿主消费
`mcp.*` 公开头的最小闭包与 TM0–TM2 相同。

## 4. 数据模型：listing 受控子集

Core 消费的 MCP listing 是宿主从 `tools/list` 结果整理出的受控子集，不是原始
协议报文。字段即 DEC-039 §1 要求转换的统一 descriptor 内容：

```cpp
// 草案：include/mira/tool_module_mcp.hpp
struct McpToolDescriptor final {
    std::string name;         // MCP tool 名；必须满足模组成员受治理字符集
    std::string description;  // server 提供的文本；不可信，进入 manifest 前有界
    JsonSchema input_schema;  // 参数 schema；必须过 gate_schema_subset 同源校验
    bool read_only_hint = false;    // MCP annotations.readOnlyHint
    bool destructive_hint = false;  // MCP annotations.destructiveHint
};

struct McpServerListing final {
    std::vector<McpToolDescriptor> tools;  // 允许为空（投影为空模组）
};
```

不消费的字段（title、outputSchema、idempotentHint、openWorldHint 等）显式留在
宿主侧；扩大子集是加法演进（DEC-002），须走设计修订。`outputSchema` 不进入 v1：
MCP v1 结果无类型约束，成员 `result_schema` 固定为 `{"type":"object"}` 占位，
结果校验纪律见 §8。

## 5. 转换纯函数与转换矩阵

```cpp
// 草案
struct McpAdmissionOptions final {
    std::string module_id;                    // 宿主分配；受治理字符集
    SemanticVersion module_version{1, 0, 0};  // MCP 无工具版本，宿主声明模组版本
    std::string signer;                       // OutOfProcess 信任材料（TM1 消费）
    std::string signature_algorithm;
    std::string signature;
    std::vector<CapabilityId> required_capabilities;  // 宿主映射，MCP 不自带能力声明
    std::vector<std::string> conflicts_with;
    ModuleResourceLimits resources;  // 必须显式有界（零值非法，沿用 manifest 规则）
    // 逐成员风险覆盖：只允许升风险，不允许降（见 §5.2）
    std::vector<std::pair<std::string, ActionRisk>> risk_overrides;
};

[[nodiscard]] Result<ToolModuleManifest> convert_mcp_listing_to_module(
    const McpServerListing &listing, const McpAdmissionOptions &options,
    const CapabilityCatalog &catalog,
    const ToolModuleLimits &limits = kDefaultToolModuleLimits,
    const McpAdmissionLimits &mcp_limits = kDefaultMcpAdmissionLimits);
```

实现方式与 `make_simulator_reference_module` 相同：组装 manifest JSON 后过**真实**
`parse_tool_module_manifest`，让 TM0 的全部校验矩阵（字符集、成员内唯一、schema
同源子集、资源上限、ABI、capability 目录）原样生效，不在转换层复刻第二套规则。

### 5.1 确定性映射

- `name` → 成员 `name` 原文；不前缀、不改名（TM2 wire 规则 v1）。字符集外的
  MCP 名（含大写、空格、路径文本）整组拒绝——这是 DEC-039 §3 的显式 fail
  closed，不是缺陷；豁免只能经宿主在**自己侧**改名后重新提交 listing，Core 不
  提供改写通道。
- `description` → 成员 `description`，超过 `max_description_bytes` 拒绝（不截断：
  manifest 是身份输入，静默截断会造成「看到的不是描述的」歧义；有界投影见 §9）。
- `input_schema` → `arguments_schema`，过与 `resolve_tool_calls` 同源的
  `gate_schema_subset`；子集外 schema（`$ref`、`oneOf`、复合类型等）整组拒绝。
- `result_schema` 固定 `{"type":"object"}`。
- 成员 `version` 取 `options.module_version`（MCP 工具无独立版本线）。
- 成员 `required_capabilities`/`data_access` 留空：MCP descriptor 不携带这些
  声明，能力要求由宿主在模组级 `required_capabilities` 映射；逐成员声明留待
  有真实消费者证据的加法演进。

### 5.2 副作用声明（hint → ActionRisk）

MCP annotations 是 server 自报的提示，不可信（`RULE-09`）。映射确定性固定：

| readOnlyHint | destructiveHint | ActionRisk |
| --- | --- | --- |
| true | 任意 | `read_only` |
| false/缺省 | true | `critical` |
| false/缺省 | false/缺省 | `user_visible` |

宿主 `risk_overrides` 只允许**升**风险（相对上表结果）；任何降风险条目使整组
转换失败。理由：server 低报风险不可信，宿主高报是保守方向的显式部署决策；
降风险等于用部署配置推翻安全默认值，必须走目录/设计评审而不是一个映射表。

### 5.3 转换 fail-closed 清单（负路径矩阵）

下列任一情况返回单一错误，不产生部分 manifest（对齐 TM0 门禁 `M7-TM0-G2` 的
错误 domain `mira.tool_module`）：

1. options 基础字段非法：空/字符集外 `module_id`、零值资源上限、非法
   `risk_overrides`（未知成员、降风险、重复条目）、越界 signer/signature。
2. listing 越界：descriptor 数超过 `mcp_limits.max_descriptors`，名称/描述超限。
3. 单个 descriptor 非法：空名、字符集外名、重名、空/非对象 `input_schema`、
   子集外 schema、空描述。
4. 信任面不完整：origin `out_of_process` 要求 signer/algorithm/signature 三者
   非空（结构校验；密码学验证归 TM1 `verify_module_trust` 与宿主注入的
   verifier）。
5. 空 listing：整组拒绝，拒绝理由来自真实解析器的既有 1..256 成员上界
   （`M7-TM0-G5` 冻结的有界性约束优先于本设计；零成员模组无暴露意义）。
   宿主对不暴露任何 tool 的 server 跳过准入，不注册空模组。

> 2026-09-20 更正：本节初稿曾写「空 listing 合法：产出零成员模组」。实现取证
> （Independent-Verification-Agent 首轮）发现该口径与 TM0 冻结契约
> `mira.tool_module.manifest.v1` 的 1..256 成员上界冲突；按上位契约优先原则
> 修订为整组拒绝，实现与测试随之对齐。

### 5.4 确定性与投影

转换是纯函数：无 I/O、无时钟、无随机；同 listing + 同 options 产出逐字节相同
的 manifest 与 digest，跨进程一致。版本化 JSON 投影
`mira.tool_module.mcp.admission.v1` 供事件、Replay 与跨进程确定性门禁使用，
内容为 module_id/version/manifest digest/逐成员（name、side_effect、
arguments_schema 的 canonical digest）——**不含 description 原文与任何签名材料**
（脱敏规则见 §9）。

## 6. 会话生命周期映射（运行期只降级）

DEC-039 §2：MCP 能力发现是部署与初始化阶段的宿主动作；运行中能力列表变化不
静默改写暴露面，只允许降级路径。映射冻结为纯策略函数 + 薄应用组件：

```cpp
// 草案
enum class McpSessionEvent : std::uint8_t { ServerConnected, ServerDisconnected,
                                            ToolListChanged };
enum class McpAdmissionAction : std::uint8_t { AdmitModule, RevokeModule,
                                               RejectEvent, NoAction };
[[nodiscard]] McpSessionPlan plan_mcp_session_action(McpSessionEvent event,
                                                     const ModuleRecord *record,
                                                     bool registry_sealed,
                                                     bool registry_closed);

class McpModuleAdmission final { /* 持 catalog + registry 引用与已准入 module_id */ };
```

策略矩阵（`record` 为 null 表示该 module_id 未注册）：

| 事件 | record | sealed/closed | 动作 | 冻结语义 |
| --- | --- | --- | --- | --- |
| ServerConnected | null | 未 seal | `AdmitModule` | 部署窗内：转换 → TM1 信任验证 → `register_module`；晋升 Staged/Active 仍由宿主显式驱动 |
| ServerConnected | null | 已 seal | `RejectEvent` | 注册窗口关闭：运行期注册被拒绝，状态不变 |
| ServerConnected | null/任意 | 已 close | `RejectEvent` | close 后一切变更拒绝（TM1 规则不变） |
| ServerConnected | 非 null | 任意 | `RejectEvent` | 同 module_id 已准入；运行中重复连接不得触发第二次注册或成员更新 |
| ServerDisconnected | 非 null、非终态 | 未 close | `RevokeModule` | 只降级：digest 进 tombstone；在途调用按旧代正常结算（设计 §9） |
| ServerDisconnected | 终态（Revoked/Quarantined） | 未 close | `NoAction` | 已终态，幂等 |
| ServerDisconnected | null | 任意 | `NoAction` | 未注册的 server 断开不产生状态变更 |
| ToolListChanged | 非 null | 未 close | `RevokeModule` | 变更后的 listing 是新 digest，本注册周期内不可再注册（module_id 一次性）；安全方向是撤销旧面，新面走下一部署周期 |
| ToolListChanged | null | 任意 | `NoAction` | 尚无准入面可降级 |
| 任意 | 任意 | 已 close | `RejectEvent` | close 后事件全部拒绝且计数可见 |

要点：

- 「重连接 + 变更了的工具列表」在任何运行期组合下都**不可能**扩大暴露面：新
  digest 准入必须发生在新的注册窗口（新 registry 生命周期或下一次部署），届时
  旧 digest 已在 tombstone 中（若被 revoke），`M7-TM1-G2` 的同 digest 重注册
  拒绝规则继续生效。
- `McpModuleAdmission` 是串行控制面组件（与 `ModuleRegistry` 同纪律）：不产生
  线程、不读时钟、不做 I/O；它只做「策略 → registry 调用」的绑定与结果折叠，
  审计事件复用 TM1 的 `mira.tool_module.lifecycle.v1`（revoke 理由有界脱敏）。

## 7. 执行适配与 Executor 路由

### 7.1 Transport 边界（宿主实现）

```cpp
// 草案
class IMcpToolTransport {
  public:
    virtual ~IMcpToolTransport() = default;
    [[nodiscard]] virtual Result<JsonValue> invoke(const std::string &wire_name,
                                                   const JsonValue &arguments,
                                                   const McpInvocationProbe &probe) = 0;
};
```

- transport 拥有真正的进程外调用（MCP client I/O、序列化、server 超时）；传输
  选型（stdio/HTTP）不进入 Core 公共契约。
- 实现必须在阻塞切片之间轮询 `probe`（`stop_requested()`/`deadline_expired()`），
  探针触发后尽快返回 `Cancelled`；这是「可解除阻塞路径」（DEC-039 §6）的宿主
  义务。Core 不强制终止——超时与取消都是协作式（根 AGENTS.md 纪律）。
- 实现不得抛异常；Core 侧把逃逸异常折叠为 Internal 错误结果，不吞掉。

### 7.2 Dispatcher：与 DEC-015 同源的门禁

```cpp
// 草案
class McpToolDispatcher final {
  public:
    McpToolDispatcher(ToolExposure exposure, McpDispatchLimits limits);
    [[nodiscard]] Result<ToolExecutionRecord> execute(const ToolProposal &proposal,
                                                      const OperationContext &context,
                                                      executor::Executor &executor,
                                                      IMcpToolTransport &transport);
    McpDispatcherCloseReport close(std::chrono::milliseconds drain_budget);
};
```

dispatcher 绑定一份 TM2 投影快照（`ToolExposure`，含 generation 与
snapshot_digest），`execute` 的门禁与 `BuiltinToolRegistry::execute`（DEC-015）
逐条同源：

1. close 后拒绝新派发（`InvalidState`）。
2. `tool_id` 必须在绑定曝光内；`wire_name`/`tool_version`/`has_side_effects` 与
   曝光一致，否则 `InvalidState`（注册面/曝光面漂移 fail closed）。
3. `OperationId` 至多一次派发；重复派发 `AlreadyExists`（W-02）。
4. 参数先过本地 schema 校验（曝光内 `parameters_schema`，与
   `resolve_tool_calls`/manifest 同一子集）；违例产出**失败记录**（模型可归因，
   可回填给模型自我修正）。
5. 模组聚合资源在调度层强制：并发上限达到时 `ResourceExhausted`（明确拒绝，
   不排队，设计 §9）；结果序列化后超过 `max_total_result_bytes` 或 bridge 上限
   产出失败记录。

结果语义对齐 DEC-015：模型可归因失败（参数违例、transport 报错、结果超限、
deadline 超时）→ failed `ToolExecutionRecord`；取消 → `Cancelled` 错误结果；
系统条件（close 后、身份失配、重复派发、并发超限、提交拒绝）→ 错误结果，不
进入模型回填。

### 7.3 Executor 路由、取消、deadline 与 shutdown

- transport 调用经 `submit_mcp_invocation()` 以 `submit_auto()` 承载（与 TM1
  `submit_module_verification` 同模式）：同步提交拒绝（stopping/capacity）返回
  错误结果；admission 拒绝经 future 逃逸的异常由 `consume_mcp_invocation()`
  折叠为显式错误。**future 必须被消费**：正常路径在 `execute` 内消费；被放弃
  的在途 future（取消/超时放弃等待后）进入 pending 清单，由 `close()` 有界排空。
- `execute` 在调用方 worker 上有界等待（调用方本身应在 Executor worker 上，绝
  不在串行控制面）：轮询调用方 `OperationContext::cancelled()` 与
  deadline；取消/超时先置探针 stop 标志，再等一个有界宽限期让 transport
  自行解除阻塞；宽限耗尽则放弃等待（返回显式结果），future 留待 close 排空。
- deadline 取 `min(context.deadline, now + limits.max_invocation_duration)`：
  即便调用方未给 deadline，单次 invocation 也有界（`RULE-08`）。
- `close(budget)`：拒绝新派发 → 对 pending 逐个置 stop → 在预算内排空并丢弃
  结果（计数入 report）→ 幂等；析构若未显式 close 则以默认预算执行。关闭顺序
  归宿主 shutdown 序列第 3–5 步（设计 §10）：先停 producer，再降级模组，再对
  在途 invocation 协作取消并等待有界结算，最后关 registry。

### 7.4 并发与所有权

- dispatcher 的簿记（dispatched 集、在途计数、pending 清单）由互斥保护；
  `execute` 等待期不持锁。dispatcher 不拥有 executor、transport 或曝光外的任何
  状态；生命周期由宿主（Runtime/Session 装配层）持有并按 §7.3 顺序关闭。
- 在途调用不受模组 revoke 影响：按其钉住的 generation 正常结算（TM2 §8.4 既有
  语义）；结算结果照常回填。

## 8. 不可信数据与结果纪律

- **描述**：server 文本只进入 manifest `description`（长度受控、无前缀注入
  防护要求之外的特殊处理——描述是给模型看的说明，不进入 System/Developer
  权威层）；admission JSON 投影与全部事件不含 description 原文（§5.4/§9）。
- **结果**：transport 返回的 JsonValue 是不可信外部数据（`RULE-09`）。回填经
  既有 `build_tool_result_input`，携带 user/tool 角色载荷，不携带 System/
  Developer authority（M3 既有行为，本阶段以负向测试钉住）。结果的 provenance
  标记沿用模型输入项既有字段，不新增第二套机制。
- **错误摘要**：transport/校验失败进入 `safe_error_summary` 前统一有界截断
  （512 字节，与 DEC-015 相同口径），不回传 server 原始报文全文。
- **签名材料**：signer/algorithm/signature 只进 manifest 结构与 TM1 信任验证，
  不进事件、投影与报告。

## 9. 可观测性

- 准入/降级审计复用 TM1 生命周期事件（`mira.tool_module.lifecycle.v1`）与协商
  事件（`mira.tool_module.negotiation.v1`），不新增第二套事件 schema；revoke
  理由经有界脱敏。
- 转换结果投影 `mcp_admission_to_json`（`mira.tool_module.mcp.admission.v1`，
  §5.4）作为跨进程确定性门禁与审计锚点。
- dispatcher 统计（派发数、失败记录、取消、超时、提交拒绝、并发超限、排空
  计数）以只读 stats 暴露；每个拒绝面都有计数，无静默丢弃。

## 10. 错误处理与降级

| 故障 | 默认处理 |
| --- | --- |
| descriptor 非法（字符集/重名/子集外 schema/越界） | 转换整组拒绝，无部分 manifest |
| 降风险 override | 整组拒绝（部署配置不得推翻安全默认值） |
| OutOfProcess 信任材料缺失/验证失败 | TM1 既有规则：Quarantined，不注册 |
| seal 后 ServerConnected | 事件拒绝，状态不变 |
| 运行期 ToolListChanged / 断开 | revoke（只降级）；新 listing 走下一注册周期 |
| transport 抛异常 | 折叠为 Internal 错误结果/失败记录，不吞掉 |
| 提交拒绝（stop/capacity） | 显式错误结果，dispatched 预约回滚 |
| 执行中取消 | 置探针 → 有界宽限 → `Cancelled`；future 由 close 排空 |
| deadline 超时 | 置探针 → 有界宽限 → 失败记录（DeadlineExceeded 语义） |
| 结果超限 | 失败记录，不截断回传 |
| close 后派发 | `InvalidState` 拒绝，计数可见 |

## 11. 兼容性

- 既有 TM0–TM2 契约零变更：本阶段只加法新增 `tool_module_mcp.hpp` 公开头与
  `src/tool/tool_module_mcp.cpp` 库源（入 `mira_core`）；manifest schema、
  事件 schema、`resolve_tool_calls` 语义不动。
- 空曝光、无 MCP 模组的运行路径与 M3 空 allowlist 行为一致；空 listing 本身
  按 §5.3 第 5 条整组拒绝，不产生模组。
- 宿主升级路径：已有 `BuiltinToolRegistry` 用户不受影响；MCP 消费是新装配
  （listing 采集 → admission → registry → TM2 投影 → dispatcher）。

## 12. 测试策略

对应 [M7](../plans/m7-tools-evaluation-platform-v1.md) MCP 阶段门禁
`M7-MCP-G1`–`G6`：转换矩阵正负路径与确定性（G1）、生命周期只降级矩阵（G2）、
与 DEC-015 的单一门禁一致性对照（G3）、取消/deadline/shutdown/资源上限闭合
（G4）、不可信数据脱敏负向（G5）、最小 consumer 闭包（G6）。测试的编写、运行
与 sanitizer 取证由 Independent-Verification-Agent 独立完成；`--report` 跨进程
字节一致作为确定性门禁（沿 TM0–TM2 口径）。

## 13. Executor 专项

- 承载方式：transport invocation 为普通有限任务 `submit_auto()`，future 必消费
  （§7.3 的双消费路径）；admission/registry 侧无新增异步路径（纯串行控制面）。
- 取消：协作式探针（stop 标志 + deadline），三层传播——调用方 context →
  dispatcher 探针 → transport 轮询；无抢占承诺。
- 关闭顺序：宿主按设计 §10 六步执行；dispatcher `close()` 是第 3 步的组成，
  registry `close()` 第 5 步，Executor `shutdown(true)` 永远最后由非 worker
  线程执行。
- 能力缺口：未发现需要登记 `EXE-*` 的场景；若宿主 transport 需要 wakeup 类
  原语超出探针表达力，按台账流程登记后再评估。

## 14. 关联文档

- [DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md)（决策载体）、
  [DEC-009](../decisions/DEC-009-tool-module-boundary.md)、
  [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)（后续引用
  语法阶段）
- [工具模组设计](tool_module_design.md)（§5/§6/§7/§8/§9/§10 为上位语义）
- [M7 计划](../plans/m7-tools-evaluation-platform-v1.md)（MCP 阶段工作项与门禁）
- [威胁模型与确认协议](../security/threat_model_and_confirmation.md)
