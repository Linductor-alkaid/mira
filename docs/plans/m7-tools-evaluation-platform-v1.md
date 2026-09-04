# M7：Tool 模组、评估、生产加固与跨平台 v1

> 状态：Blocked（2026-09-05，[DEC-011](../decisions/DEC-011-demo-first-external-validation.md)：前置 M5/M6 已终止，范围与前置待外部 demo 需求证据重定义）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M4、M5、M6（M5/M6 已取消，前置待重定义）
> 建议发布点：v1.0（待重定义）
> 更新日期：2026-09-05

## 1. 目标

把 M0–M6 的能力收敛为可发布 v1：交付受签名、能力协商和生命周期治理的 ToolModule；建立从
Contract 到真实平台/soak 的统一评估体系；完成至少 Android 首要真实 Host/Adapter 验证，并对 Linux、
Windows、Android 的支持等级作证据化声明；完成安全、供应链、恢复、性能和发布材料门禁。

M7 不以“能编译”“Simulator 通过”或单次模型成功代替平台与发布证据。任何目标环境缺失、skip、
未确认输入释放或真实互操作空白都会保持相应工作项和退出条件未完成。

## 2. 范围与非目标

### 2.1 范围

- CapabilityCatalog、ToolModule manifest、ModuleRegistry、协商和三类投影。
- BuiltIn、HostProvided 与受监督 OutOfProcess Tool 隔离、Policy/confirmation/Replay。
- Simulator 参考模组、Android HostProvided 模组和 policy model 绑定。
- EvalCase/RunManifest、fault injection、recorded/live Provider、regression/soak/safety harness。
- Android 真实 Host/JNI/权限/生命周期/输入；Linux/Windows Adapter 的 v1 支持等级收敛。
- 性能、实时性、Memory、本地模型、Provider、Tool、安全、供应链和发布门禁。

### 2.2 非目标

- Runtime 中动态发现/热插拔工具、远端模组仓库或 MCP 式分发。
- 分布式 Runtime、云控制面或跨设备一致性。
- 未经目标设备数据支持的硬实时、全模型/全 Provider/全平台兼容声明。
- 完整产品 UI；只交付 Host/Adapter、可信确认/Takeover 协议和参考集成证据。
- 自动接受性能、安全或兼容性回归；超预算必须修复或通过明确决策记录接受。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M4、M5、M6 全部完成，M3 的 Provider/传输/文件生命周期开放项亦已关闭。
- v1 release profile 明确：目标 OS/架构、Android API/设备、Provider profile、ModelPackage、
  ToolModule、数据保留和 benchmark 阈值均有 owner。
- 真实平台测试设备/模拟环境、受控账号、费用上限、签名 key/测试证书和隔离副作用环境可用。
- HostProvided attestation 暂定默认值、跨模组 wire name 规则和各平台支持等级在实现前冻结；改变
  DEC-009 或安全边界时先更新/新增决策记录。

### 3.2 设计与决策依据

- [工具模组设计](../design/tool_module_design.md)
- [Model Provider 与 Tool 扩展设计](../design/model_provider_and_tool_design.md)
- [评估与基准体系设计](../design/evaluation_and_benchmark_design.md)
- [Mira Runtime 设计](../design/mira_runtime_design.md)
- [实时控制层设计](../design/realtime_control_design.md)
- [本地感知与任务模型设计](../design/local_perception_and_task_models.md)
- [Context 与 Memory 设计](../design/context_and_memory_design.md)
- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
- [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md) 至
  [DEC-009](../decisions/DEC-009-tool-module-boundary.md)

## 4. 工作项

### 4.1 ToolModule 契约与 Registry

- [ ] `M7-01` 冻结 CapabilityCatalog 词汇、`EnvironmentCapabilities -> env.*` 纯函数映射、版本和
  变更流程；未知 ID、类别不匹配和不诚实 capability fail closed。
- [ ] `M7-02` 实现 ToolModule manifest/schema/digest/签名、SemanticVersion、origin、资源、冲突和
  ABI 校验；失败整组 Quarantined，不部分注册。
- [ ] `M7-03` 实现 ModuleRegistry 状态机、不可变 snapshot、初始化期注册、运行期只降级、revoke
  tombstone 与事件；跨模组 ToolId 冲突和过高 ABI 明确拒绝。
- [ ] `M7-04` 实现确定性能力协商：Active snapshot × environment × catalog，缺能力/未知能力/冲突
  整组不可用，同输入同输出 digest，epoch/capability 变化后重协商。
- [ ] `M7-05` 冻结 HostProvided attestation 与 `wire_name` 规则，更新 DEC-009 或新增决策；BuiltIn、
  HostProvided、OutOfProcess 的来源信任和包签名链均有可执行验证。

### 4.2 Tool 投影、执行与隔离

- [ ] `M7-06` 把协商结果投影为 ToolRegistry view 与 per-request `ExposedToolSpec`；记录模组级/任务级
  排除理由，`tool_snapshot_digest` 同时绑定 module 与 ToolSpec digest。
- [ ] `M7-07` 实现 ModelPackage `bindings.tool_modules` 的运行时门禁：policy 绑定集 ⊆ 协商通过集 ⊆
  Active 成员；越权 ToolIntent、版本不匹配和跨 snapshot 迟到结果拒绝。
- [ ] `M7-08` 交付 Simulator BuiltIn 参考模组和 Android HostProvided 参考模组，覆盖 capability 变化、
  revoke、资源上限、Policy/Grant/confirmation 和 Verify。
- [ ] `M7-09` 实现 OutOfProcess Tool supervisor、IPC schema、进程身份/沙箱、大小/时间/并发上限、
  crash/取消/shutdown 和 side-effect receipt；进程生命周期由 Executor blocking worker 管理。
- [ ] `M7-10` 闭合 Tool invocation 的至多一次派发、幂等 key、聚合配额、结果校验、ArtifactRef、
  revocation 与 `ExecutionUncertain`；模组可用不等于获得授权。
- [ ] `M7-11` 扩展 OfflineReplay：只使用 recorded module/spec digest 与 result，不加载真实模组、启动
  OOP 进程或执行 Tool；删除/撤销后以 tombstone/quality 降级表达。

### 4.3 评估 Harness 与生产可观测性

- [ ] `M7-12` 实现版本化 EvalCase、EvalRunManifest、oracle、budget、result 与报告 schema；case、fixture、
  dataset 和 baseline 使用不可变 digest。
- [ ] `M7-13` 实现通过公共 API 驱动的 benchmark harness：Simulator/真实 Adapter、recorded/live
  Provider、fault injection、fake/monotonic clock、每 case 独立 Session/scope/quota，所有并发受
  Executor 管理。
- [ ] `M7-14` 建立 L0–L5 suite：contract、component、loop、task、platform、soak/safety；覆盖 long
  context/Memory、本地模型、Tool、连续控制、恢复和供应商漂移。
- [ ] `M7-15` 实现统一 metrics/trace/report：分段延迟、queue/rejection、token/cost、Memory/model/
  Controller、side effect、安全和资源；默认事件与日志脱敏，大型产物以受控引用保存。
- [ ] `M7-16` 建立 active regression corpus 与 sealed holdout、污染检查、基线比较、置信区间和归因；
  失败按 Runtime/Provider/Model/Adapter/Tool/Environment/Test/Infrastructure 分类。
- [ ] `M7-17` 建立 stress/soak/crash matrix：Executor rejection/exception、store/artifact fault、network、
  Tool crash、ONNX deadline、controller watchdog、Host lifecycle 和 shutdown；验证终态、handle、
  副作用与输入释放 invariant。

### 4.4 真实平台与兼容性

- [ ] `M7-18` 实现 Android Host/JNI 参考集成：MediaProjection、Accessibility/UI tree、foreground/device
  state、主线程 dispatcher、离散/连续输入和可信 confirmation/Takeover 边界；Core 不依赖 JNI/SDK。
- [ ] `M7-19` 在受支持 Android 设备/模拟环境运行 ABI、旋转、前后台、权限撤销、Host destroy、进程
  恢复、输入 release、截图/UI tree 和连续控制矩阵，更新 Android ABI 与平台兼容性证据。
- [ ] `M7-20` 实现并验证 Linux v1 Adapter 选定能力；若 release profile 只承诺构建支持，则明确
  capability fail closed 和非目标，不用 Simulator 冒充真实输入/截图。
- [ ] `M7-21` 实现并验证 Windows v1 Adapter 选定能力与 TLS/Provider 路径；按与 Linux/Android 相同
  contract、取消、权限、生命周期和 shutdown 语义取证。
- [ ] `M7-22` 完成三平台编译器/架构/依赖/Provider/ModelPackage/ToolModule 兼容性矩阵；区分 Configured、
  BuildVerified、ContractVerified、DeviceVerified 和 InteropVerified。

### 4.5 v1 安全、供应链与发布

- [ ] `M7-23` 执行 v1 安全 suite：prompt/Memory/Tool injection、confirmation replay/substitution、跨
  tenant、SSRF/redirect/proxy、路径穿越、包签名、Secret/log leak、Replay live effect 和 Takeover 竞态。
- [ ] `M7-24` 完成直接/传递依赖、许可证、SBOM、模型/模组签名、数据 provenance、升级/撤销/回滚和
  可重复构建审计；任何未批准依赖或模型包阻止发布。
- [ ] `M7-25` 在候选发布配置运行 Debug/Release、ASAN/UBSAN、适用 TSAN、静态检查、目标平台、live
  Provider canary、完整 benchmark 和 soak；skip 不算发布证据。
- [ ] `M7-26` 冻结 v1 各 profile 的 task success、安全零容忍项、成本、资源、尾延迟、realtime 和
  shutdown 阈值；超 regression budget 必须修复或以决策记录明确接受。
- [ ] `M7-27` 执行安装包/最小 consumer/API/schema/current-previous migration/配置兼容和升级回滚测试，
  生成 release candidate 与已知限制清单。
- [ ] `M7-28` 完成 v1.0 发布说明、用户/Host 集成指南、API 示例、安全与隐私说明、兼容性矩阵、
  benchmark 摘要、SBOM 和验收记录；总计划规则与适用通用门禁全部有证据后关闭 M7。

## 5. Executor 路由与关闭

| 工作 | Executor 能力 | Owner | 结算要求 |
| --- | --- | --- | --- |
| manifest/package 验证 | `submit_auto()` | ModuleRegistry | future 消费，失败分类 |
| 协商/snapshot commit | Runtime 串行控制面内纯函数 | Runtime owner | 有界、无 I/O、确定 digest |
| Tool invocation | 普通任务或 blocking worker | ExecutionSupervisor | 配额、取消、receipt、至多一次 |
| OOP 进程监督/IPC | blocking worker lifecycle | Tool supervisor | crash/stop/join 可诊断 |
| eval cases | Executor 受管普通任务 | Eval harness | 每 case 独立 scope/quota/settlement |
| platform dispatcher/realtime | Adapter 外部循环/realtime | Platform owner | capability、handle、release/status |

发布配置的关闭顺序至少为：停止 Task/Model/Tool producer，撤销动作租约并安全收敛 Controller，停止
新 Tool invocation 并取消/回收 OOP 模组，结算 Provider/Store/Memory/Adapter operation，关闭 Registry
与 store，隔离迟到 Host callback，最后由非 worker owner `shutdown(true)`。任何新能力缺口先登记
`EXE-*`，不能为赶发布引入平行生命周期。

## 6. 风险与阻塞

- `RISK-2026-029`：M7 同时汇合 Tool、平台、评估和发布，集成面过大。Owner：M7 release owner。
  缓解：按 4.1–4.5 顺序设内部 gate，每段保持可独立验收；不以并行未结项关闭总里程碑。
- `RISK-2026-030`：HostProvided attestation 或 OOP sandbox 在各平台能力不同。Owner：M7 security owner。
  解除条件：决策冻结、每平台负向测试和明确降级；无法等价时分别声明 profile。
- `RISK-2026-031`：真实设备、账号、网络或费用不可用会阻塞发布证据。Owner：M7 release owner。
  补跑条件：受控设备/账号/网络/费用/凭据就绪；缺失时保持对应工作项未完成。
- `RISK-2026-032`：远端模型漂移和目标平台噪声影响结果可重复性。Owner：M7 evaluation owner。
  缓解：recorded deterministic 回归 + live canary 分离，manifest、重复运行和置信区间完整。
- `RISK-2026-033`：Linux/Windows 真实 Adapter 能力可能无法在 v1 同时达到 Android 深度。
  Owner：M7 platform owner。实现前冻结各 profile 支持等级；任何范围调整必须同步总计划和决策。

## 7. 测试与发布矩阵

| 层级 | 必测内容 |
| --- | --- |
| L0 Contract | Tool/Module/Provider/Model/Adapter schema、capability、取消、错误、版本 |
| L1 Component | Registry、Memory、OCR/detector/state、Controller、OOP Tool、Store |
| L2 Loop | Simulator Observe→Verify、恢复、Tool、local/VLM、continuous action |
| L3 Task | 多步任务、成本、Human、失败/恢复、应用/模型版本变化 |
| L4 Platform | Android/Linux/Windows 选定 capability、权限、生命周期、输入、TLS |
| L5 Soak/Safety | crash、长期、过载、注入、泄漏、重复副作用、shutdown、漂移 |

## 8. 退出条件

- [ ] `M7-01` 至 `M7-28` 全部完成并有可复现验证记录。
- [ ] ToolModule 协商、双消费者投影、Policy/confirmation、revoke、OOP 隔离和 Replay fail closed 通过。
- [ ] duplicate external side effect、confirmation bypass/replay、cross-tenant read、Replay live effect 和
  Secret leak 均为零；所有未确认输入释放阻止 clean 结果。
- [ ] Android 首要真实 Host/Adapter 完整运行；Linux/Windows 达到冻结的 v1 profile，未验证能力不宣称支持。
- [ ] M3 Provider 互操作、M4 Memory/恢复、M5 本地模型、M6 控制和 M7 Tool/平台均进入统一 eval 报告。
- [ ] 所有终态、取消、Takeover、rejection、异常、过载和 shutdown invariant 通过，无未消费 handle/future。
- [ ] 候选发布平台、sanitizer、静态检查、live canary、benchmark、stress/soak 和安全 suite 实际运行；skip
  保持未完成。
- [ ] 依赖/模型/模组/数据的 digest、许可、SBOM、签名、评估、回滚、撤销与删除传播信息完整。
- [ ] v1 安装、升级、current/previous migration、最小 consumer 和公开文档通过验收。
- [ ] 总计划全部适用通用门禁有证据，M0–M7 状态和 release notes 同步后方可发布 v1.0。

## 9. 验证记录

2026-09-01：依据总计划、DEC-009、ToolModule/评估/平台设计创建详细计划；状态为 `Planned`，尚未
实现 ToolModule、真实平台 Host 或 v1 eval harness，未执行发布门禁。负责人为 Mira Maintainers；
准入条件为 M4、M5、M6 完成以及 v1 profile、设备、账号、签名和测试环境就绪。

2026-09-05：按 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md) 状态改为
`Blocked`。阻塞原因：前置 M5/M6 已终止，本文件声明的范围（含"M5 本地模型、M6 控制进入
统一 eval 报告"等退出条件）失去前提，不能按原文交付。负责人为 Mira Maintainers；解除
条件为外部 demo 仓库产出需求验证报告，并据此完成 M7 范围、前置与发布点的重定义提案且
经决策记录批准。重定义前本文件保持冻结，`M7-01` 至 `M7-28` 与退出条件不适用；DEC-009
的 Tool 模组架构方向保留。
