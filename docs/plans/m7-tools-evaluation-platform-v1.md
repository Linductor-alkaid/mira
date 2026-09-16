# M7：Tool 模组体系（DEC-042 重定义）

> 状态：Planned（2026-09-16 依 [DEC-042](../decisions/DEC-042-m7-scope-redefinition.md)
> 重定义；TM0 首阶段进入实施）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M4（已完成）；[DEC-042](../decisions/DEC-042-m7-scope-redefinition.md)
> 建议发布点：Tool module alpha（分阶段锚点，非发布物）
> 更新日期：2026-09-16

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
- **后续阶段**：MCP 准入（DEC-039：descriptor 转换矩阵、Adapter 执行适配、
  取消/shutdown 闭合）、稳定引用与 Skill（DEC-040：引用语法、兼容状态投影、
  Skill 发布生命周期）随各自立项在 M7 内增补工作项与门禁，不预分配编号。

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

### 4.2 TM1：Registry 生命周期（后续轮，实施前冻结细项）

- [ ] `M7-TM1-01` ModuleRegistry 状态机、不可变 snapshot、初始化/部署期注册、
  运行期只降级、revoke tombstone、生命周期事件与 shutdown 顺序测试。
- [ ] `M7-TM1-02` 来源信任验证：BuiltIn 构建 digest、HostProvided 宿主显式注入
  + allowlist（DEC-009 暂定默认值升格为 v1 冻结决策）、OutOfProcess 包签名；
  校验失败 `Quarantined`。
- [ ] `M7-TM1-03` 协商触发挂接：session 建立、能力变化（含 epoch invalidation
  后重申报）、模组状态变化；结论作为事件提交，在途请求按旧 snapshot 结算。

### 4.3 TM2：LLM 暴露投影（后续轮，实施前冻结细项）

- [ ] `M7-TM2-01` 协商结果 → ToolRegistry view → per-request `ExposedToolSpec`
  投影；模组级/任务级排除理由记录；`tool_snapshot_digest` 绑定 module digest
  集合与 ToolSpec digest 集合（DEC-002 加法演进）。
- [ ] `M7-TM2-02` `wire_name` 跨模组命名规则冻结（设计 §17 开放项收口）。
- [ ] `M7-TM2-03` Simulator BuiltIn 参考模组、Replay module digest 绑定、与
  `resolve_tool_calls` 既有 fail-closed 语义的组合测试。

### 4.4 后续阶段（立项时增补工作项与门禁）

- MCP 工具模组准入（[DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md)
  §验证方式：descriptor 转换矩阵、模组生命周期降级、单一门禁一致性、在途取消
  与 shutdown 闭合、脱敏与不提升权限负向）。
- Tool 稳定引用与 Skill（[DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)
  §验证方式：引用解析矩阵、兼容状态投影、`Invalid` 准入拒绝、Skill 生命周期、
  Procedure 索引投影）。

## 5. TM0 门禁（2026-09-16 跑前冻结；同日交付取证）

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

## 6. Executor 路由与关闭

TM0 全部为串行控制面内的同步纯计算（协商有界、无 I/O），不新增异步路径；TM1
的验证/安装类工作按工具模组设计 §10 以 `submit_auto()` 承载，future 必须消费。
本里程碑不引入脱离 Executor 生命周期的线程或定时器；能力缺口先登记
`docs/executor_feedback/ledger.md`。

## 7. 测试矩阵

| 层级 | 必测内容 |
| --- | --- |
| Contract | catalog/digest、派生 golden、manifest 校验矩阵、协商 golden 与 fail-closed 负向 |
| Component（TM1 起） | 状态机、签名/allowlist 验证、tombstone、事件 |
| Integration（TM2 起） | 暴露投影、`resolve_tool_calls` 组合、Replay digest |
| 边界 | 输入规模上限、空集、全不可用、同输入跨进程 digest 一致 |

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
