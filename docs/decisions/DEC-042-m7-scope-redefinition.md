# DEC-042：M7 范围重定义——Tool 模组体系分阶段落地

> 状态：Accepted
> 日期：2026-09-16
> 决策人：Mira Maintainers
> 需求来源：[MNT-202609-30](../plans/maintenance-2026-09-post-stage-f.md)
> （产出 M7 重定义提案与任务迁移映射）、
> [Issue #56](https://github.com/Linductor-alkaid/mira/issues/56)
> （Tool/MCP 统一行为模型的实现入口）
> 上位决策：[DEC-009](DEC-009-tool-module-boundary.md)、
> [DEC-011](DEC-011-demo-first-external-validation.md)、
> [DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)
> 修订对象：[M7 里程碑](../plans/m7-tools-evaluation-platform-v1.md)
> （范围、前置与发布点重定义；原 `M7-01`–`M7-28` 按本决策第 4 节迁移映射处置）
> 关联决策：[DEC-015](DEC-015-builtin-tool-execution-boundary.md)、
> [DEC-033](DEC-033-hybrid-visual-grounding.md)、
> [DEC-034](DEC-034-minimal-eval-profile.md)、
> [DEC-039](DEC-039-mcp-tool-module-admission.md)、
> [DEC-040](DEC-040-tool-reference-and-skill-layer.md)

## 背景与问题

[M7](../plans/m7-tools-evaluation-platform-v1.md) 原范围把五类互不同源的交付汇在
一个里程碑里：DEC-009 Tool 模组体系、Tool 隔离、统一评估体系、真实平台验证与
v1.0 发布加固。2026-09-05 [DEC-011](DEC-011-demo-first-external-validation.md)
终止 M5/M6 后 M7 整体 `Blocked`，等待 miracle demo 证据（`MNT-202609-27`）驱动的
重定义提案。此后两类事实改变了前提：

1. **模组体系本身不依赖被门禁的证据**。[DEC-009](DEC-009-tool-module-boundary.md)
   模组体系（manifest、CapabilityCatalog、协商、Registry 生命周期、暴露投影）是
   纯 Core 工作；其设计已冻结
   （[工具模组设计](../design/tool_module_design.md) §15 的 TM0–TM4 阶段划分），
   与真实平台、评估体系、发布加固的证据需求无关。把它与被门禁项捆绑在同一个
   `Blocked` 里程碑里，使不依赖证据的地基无法开工——`RISK-2026-029`（集成面
   过大）预言的问题以另一种形式成立。
2. **新方向以模组体系为实现前置**。[DEC-039](DEC-039-mcp-tool-module-admission.md)
   （MCP 准入）与 [DEC-040](DEC-040-tool-reference-and-skill-layer.md)（Tool 稳定
   引用与 Skill）均已冻结方向，并显式声明实现前置是「DEC-009 模组体系落地
   （M7 重定义）」。在此之前任何 MCP/引用实现都属于绕过治理的临时方案，不予立项。

[MNT-202609-30](../plans/maintenance-2026-09-post-stage-f.md) 要求产出 M7 重定义
提案与任务迁移映射（逐项映射 `M7-01`–`M7-28` 到保留/缩减/推迟），经专项决策批准
后修改 M7 范围。本决策即该专项决策。

## 决策

1. **M7 重定义为「Tool 模组体系」里程碑**：承载 DEC-009 模组体系的分阶段落地
   （TM0 契约与协商 → TM1 Registry 生命周期 → TM2 LLM 暴露投影），其后依次衔接
   DEC-039 MCP 准入与 DEC-040 稳定引用/Skill 的实现阶段；每阶段内部 gate 独立
   验收（`RISK-2026-029` 缓解），不以并行未结项关闭总里程碑。范围只含纯 Core：
   不引入平台 SDK 依赖，不依赖任何真实设备证据。
2. **前置重定义**：前置由「M4、M5、M6」改为「M4（已完成）+ 本决策」。M5/M6 已被
   DEC-011 终止，不再作为前置出现。
3. **发布点重定义**：M7 建议发布点由「v1.0（待重定义）」改为「Tool module
   alpha（分阶段锚点，非发布物）」。v1.0 发布（真实平台收敛、L0–L5 评估全家桶、
   安全/供应链/发布门禁）从 M7 剥离，待 `MNT-202609-27` 证据与发布规划恢复后
   另行立项，本决策不为其预分配里程碑编号。
4. **原 `M7-01`–`M7-28` 迁移映射**（保留 = 进入重定义后 M7 的分阶段范围；
   缩减 = 部分保留并注明承担者；推迟 = 移出 M7，保持证据门禁或待另行立项）：

   | 原项 | 内容摘要 | 处置 | 承担者 / 条件 |
   | --- | --- | --- | --- |
   | M7-01 | CapabilityCatalog 词汇、`env.*` 派生映射、fail closed | 保留 | TM0（重定义 M7 首阶段） |
   | M7-02 | manifest/schema/digest/签名、版本、origin、资源、冲突、ABI 校验 | 缩减 | TM0 承载结构与内容校验（schema 子集、digest、资源上限、ABI）；签名验证随 TM1 状态机准入 |
   | M7-03 | Registry 状态机、不可变 snapshot、运行期只降级、tombstone、事件 | 保留 | TM1 |
   | M7-04 | 确定性能力协商（Active × env × catalog，同输入同 digest） | 保留 | TM0 交付纯函数；会话挂接与重协商触发随 TM1 |
   | M7-05 | HostProvided attestation 与 `wire_name` 规则冻结 | 缩减 | attestation 以 DEC-009 暂定默认值（宿主显式注入 + allowlist）作为 v1 决策随 TM1 冻结；`wire_name` 跨模组规则按设计 §17 随 TM2 冻结 |
   | M7-06 | 协商结果 → ToolRegistry view / `ExposedToolSpec`、`tool_snapshot_digest` 绑定 | 保留 | TM2 |
   | M7-07 | ModelPackage `bindings.tool_modules` policy 绑定门禁 | 推迟 | 无消费者（M5 已终止）；待 policy model 方向依 DEC-011 证据重入门禁（设计 TM4） |
   | M7-08 | Simulator BuiltIn 参考模组 + Android HostProvided 参考模组 | 缩减 | TM0 fake 模组与 TM2 Simulator 参考模组保留；Android HostProvided 参考模组推迟（`MNT-202609-27`） |
   | M7-09 | OutOfProcess supervisor、IPC、进程身份/沙箱、receipt | 推迟 | 完整 OOP 隔离归后续；DEC-039 MCP Adapter 阶段仅承载其必需的进程外 I/O 子集，并按 DEC-039 §6 冻结取消/shutdown 语义 |
   | M7-10 | 至多一次派发、幂等、聚合配额、revocation、`ExecutionUncertain` | 缩减 | 单工具语义由 DEC-015 既有执行边界承载（已交付）；模组聚合资源上限的判定随 TM0/TM1 以纯函数与状态机承载，执行层强制随 TM2 |
   | M7-11 | OfflineReplay 只用 recorded module/spec digest | 缩减 | Replay 不重放副作用的既有规则已交付（M4）；module digest 进入 replay 记录随 TM2 |
   | M7-12–M7-17 | 评估体系（EvalCase/harness/L0–L5/metrics/regression/soak） | 缩减 | 最小评估 profile 与 harness 已由 [M15](../plans/m15-eval-harness-and-baseline.md)/[DEC-034](DEC-034-minimal-eval-profile.md) 交付；其余 suite、soak/safety 矩阵与统一 metrics 推迟归发布轮立项 |
   | M7-18–M7-22 | Android/Linux/Windows 真实 Adapter 与兼容性矩阵 | 推迟 | `MNT-202609-27` 证据门禁不变；平台支持等级声明随未来发布立项 |
   | M7-23–M7-28 | v1 安全 suite、供应链、发布候选与 v1.0 发布材料 | 推迟 | 归未来 v1.0 发布里程碑；其中与模组直接相关的负向安全测试（未知能力、冲突、越权）已包含在各阶段测试矩阵内 |

5. **DEC-033 不被本决策解锁**：视觉 grounding 的实现入口保持
   `MNT-202609-27` 证据门禁；本决策只处置 M7 中与模组体系同源的项，DEC-033 的
   里程碑仍随其证据另行立项。`MNT-202609-30` 对「27 需求报告」的依赖就此解除
   （提案改为以 DEC-039/040 的实现前置为驱动），27 对真实平台项的证据供给角色
   不变。
6. **阶段门禁与编号纪律不变**：每阶段进入实施前在 M7 文件内冻结门禁与工作项；
   不预分配新的里程碑编号；对 DEC-009/DEC-039/DEC-040 契约的任何偏离先修决策
   再动实现。

## 非目标

- 不解锁 DEC-011 证据门禁，不为任何真实平台能力开口子。
- 不改变「运行中动态发现与热插拔」的否决（DEC-009/DEC-039 既有立场）。
- 不在本决策内冻结 MCP descriptor 转换矩阵、引用 URI 语法或 L1/L2 Trace schema
  （分别归 DEC-039/DEC-040/DEC-038 的实现阶段）。
- 不为 v1.0 发布重建排期；不宣称任何未验证的平台或性能保证（`RULE-10`）。

## 备选方案

- **维持 M7 整体 `Blocked` 等 27 证据**：模组体系这一无证据依赖的地基继续停摆，
  DEC-039/040 无法立项，Issue #56 的实现入口无限期后移。不采用。
- **不动 M7，新建里程碑承载模组体系**（M8 先例）：M8 当时回避重定义是因为
  DEC-014 明确不解冻 M7 且两方向验收不同源；本轮 MNT-202609-30 的验收路径恰恰
  是「专项 DEC 批准后修改 M7 范围」，且模组体系本就是 M7 的核心主体，另立编号
  反而留下两个重叠的 Tool 里程碑。不采用。
- **重定义同时立即纳入 MCP 实现**：违反 DEC-039 的「模组体系落地先行」前置，
  属绕过治理。不采用。
- **把推迟项整体删除**：真实平台、评估与发布范围仍是 v1 交付边界的组成部分，
  删除会改变 v1 边界本身；推迟（保持门禁与另行立项路径）而非删除。不采用。

## 影响与风险

- M7 状态由 `Blocked` 变为 `Planned`，模组体系进入常规交付节奏；被推迟项的
  追踪从 M7 工作项转为本决策映射表 + 未来立项，审查时需同时核对两处。
- v1.0 发布里程碑缺席期间，发布类承诺（平台矩阵、soak、安全 suite）没有承载
  里程碑；风险由「推迟而非删除 + 未来立项」承担，不构成能力声明。
- 模组体系与 Workflow/AgentLoop 的集成点（TM2 暴露投影）触及
  `ContextManager`/`tool_snapshot_digest` 既有语义，实现前需按 DEC-002 加法
  演进评审。
- 若 `MNT-202609-27` 证据长期缺位，被推迟项持续挂起；这不阻塞模组体系与
  DEC-039/040 的交付，但 v1 边界内其余能力维持现状。

## 验证方式

- 本决策生效后：M7 里程碑文件按映射表完成重写（范围、前置、发布点、分阶段
  工作项与门禁），总计划里程碑表、`MNT-202609-30` 状态与工具模组设计 §15 同步；
  docs/format/boundary 门禁通过。
- 各实现阶段以 M7 文件内跑前冻结的阶段门禁为准（TM0 门禁随本轮交付冻结）。

## 关联文档和工作项

- [MNT-202609-30](../plans/maintenance-2026-09-post-stage-f.md)（本决策为其交付物）
- [M7 里程碑](../plans/m7-tools-evaluation-platform-v1.md)（修订对象）
- [工具模组设计](../design/tool_module_design.md)（TM0–TM4 阶段划分）
- [DEC-009](DEC-009-tool-module-boundary.md)、
  [DEC-011](DEC-011-demo-first-external-validation.md)、
  [DEC-015](DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-033](DEC-033-hybrid-visual-grounding.md)、
  [DEC-034](DEC-034-minimal-eval-profile.md)、
  [DEC-039](DEC-039-mcp-tool-module-admission.md)、
  [DEC-040](DEC-040-tool-reference-and-skill-layer.md)
- [Mira 实施总计划](../plans/mira-implementation-plan.md)
