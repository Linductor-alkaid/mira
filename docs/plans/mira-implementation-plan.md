# Mira 实施总计划

> 状态：In Progress
> 负责人：Mira Maintainers
> 更新日期：2026-09-24（DEC-037 Stage T1「条件策略契约与确定性 Runtime 最小闭环」
> 经 [M26](m26-temporal-policy-stage-t1.md) 立项并跑前冻结（`M26-01`）：正式契约
> ——`TemporalHistory` 有界观测环、`TrackedEntity`/`PolicyWorldView` 输入契约、
> `ReactiveRule` schema v1（闭集谓词 + provenance + `mira.policy.rule.v1`）、
> `IPolicyRuntime`/`ReactivePolicyRuntime` 评估与宿主显式归纳/测试/晋升生命周期、
> T1 子集九类 `mira.policy.*.v1` 版本化事件——随本文件 §4 冻结；门禁
> `T1-G1`–`G6`（`tests/m26/` 契约/生命周期矩阵 + 确定性闭环评估 harness，label
> `integration;m26`，数据集 digest 锚定 + 跨进程报告一致）与阶段冻结协议必答
> 八问 + 决策记录表随立项冻结；闭环证明口径 = 设计 §12「重复事件 → 候选规则 →
> 晋升后无需 Agent 介入正确执行」，指标全部为管线行为指标（RULE-10）；T1 零
> Executor 注册面（step/归纳/测试为有界同步函数，realtime/低延迟 lane 为 T2+/T6
> 集成形态）；范围注记交叉点——规则产出非授权来源（RULE-09 延伸）、策略激活的
> Workflow/工具化表达与 EventStore 桥接为显式非目标、Temporal Policy 与
> PolicyEngine/WorkflowPolicy 术语消歧随 glossary 词条落地；真机感知（T2/T3）与
> 连续控制（T6）仍受 DEC-011 门禁（`MNT-202609-27` 证据）；状态 `In Progress`
> （`M26-01` 立项冻结已交付，实现自 `M26-02` 起推进）；详见 §4.1 第 19 条与
> [M26 文件](m26-temporal-policy-stage-t1.md)。）
> 此前 2026-09-23（宿主集成轮 [M25](m25-host-integration-round.md)
> 交付关闭（**`Completed`**）：[PR #69](https://github.com/Linductor-alkaid/mira/pull/69)
> （head `4abf8ee`）CI 24/24 全部 SUCCESS——linux（gcc/clang × Debug/Release）、
> windows（Debug/Release）、android（arm64/x86_64 NDK 交叉）、sanitizers
> （ASAN/UBSAN/TSAN）与 quality 管线全绿；`HI-G1`–`G6` 与 `M25-01`–`06` 全部
> 勾选。交付面：`M25-02` 供给缝契约与实现——`agent_loop.hpp` 的
> `WorkingContextSeamOptions`/`WorkingContextSupplier`/
> `set_working_context_supplier` 与 `build_request` 注入（每步恰一次供给、
> 身份对齐门槛三腿、环境纪元归宿主回调、固定顺序截断 + 标注、失败降级 +
> 异常隔离，未注入零漂移）；`M25-05` 参考宿主
> `examples/working_context_host_consumer.cpp`（label `consumer`、离线
> `TIMEOUT 60`，W3/W4/W5 宿主显式编排示范）；`M25-06` 文档同步（API 手册
> 供给缝条目、context-memory 交叉引用、README 能力表、DEC-035/DEC-044 关联
> 回填、Curator 设计实现注记、术语表供给缝词条）。首轮 CI 失败（clang
> `-Wunused-lambda-capture` 对 IVA 测试文件 constexpr 捕获的编译错）经升级
> 裁决授权单行语义中性修复后复跑全绿。遗留：Loop 内自动化、平台 Adapter
> 宿主接入、真实模型与 Stage E 语义指标（`MNT-202609-27` 通道，RULE-10）。）
> 此前 2026-09-23（宿主集成轮经 [M25](m25-host-integration-round.md)
> 立项并跑前冻结（`M25-01`）：接线形态冻结为
> [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)——
> Agent Loop 薄缝：可选快照供给依赖（沿 setter 注入先例，未注入零漂移），
> `build_request` 经既有 `context_items_from_working_context`（Layer 0 唯
> 一准入转换）注入已提交快照条目（身份对齐门槛 + 有界渲染 + 失败降级）；
> W3 信号/W4 晋升/W5 fork-merge 保持宿主显式编排、Loop 零自动化（M23/M24
> 冻结语义零触碰）；自动 fork/merge、自动晋升触发与模型介导语义合并均不并
> 入（须各自上位决策）；验收三层（`tests/m25/` 契约矩阵 + `tests/integration/`
> 单系统闭环 + `examples/` 参考宿主），无模型无语义声明（RULE-10，issue #48
> 对照指标归真实模型轮与 Stage E）；范围钉死 Core 参考宿主，平台 Adapter
> 另行立项；实现未开始，状态 `Planned`；详见 §4.1 第 18 条与
> [M25 文件](m25-host-integration-round.md)。）
> 此前 2026-09-23（DEC-035 Stage W5「Subagent Fork / Merge」交付关闭
> （同日跑前冻结）：快照 fork——子会话基线 + schema 1.2 加法溯源（fork 仅非
> nil 进入 digest canonical 对象，v1.0/v1.1 载荷读回保留原戳记且 digest 逐位
> 一致）、局部 delta——独立 schema v1 机械三分类投影（inherited 剔除 / 血统
> supersede / addition）、parent merge policy——引用驱动机械合并经既有 §5.2
> 提交管线（同水位冲突 fail-closed 不豁免；候选 `generated_by` 继承父值，零
> 效果合并 `IdempotentNoOp` 对 nil/curated 两类父快照普遍可达）；既有 m21
> 套件六处戳记随升版同步；IVA 独立 22 用例 `W5-G1`–`G6` 全绿 + 冻结链 eval
> harness 跨进程字节一致；本地 clang-tidy 零违例 + 五检查全绿；PR
> [#68](https://github.com/Linductor-alkaid/mira/pull/68) CI 24/24 全绿
> （linux/windows/android/sanitizers/quality）；**Stage W5 同日关闭
> （`Completed`）**；详见 §4.1 第 17 条与
> [M24 文件](m24-context-curator-stage-w5.md)。）
> 此前 2026-09-23（DEC-035 下一阶段 Stage W5「Subagent Fork / Merge」经
> [M24](m24-context-curator-stage-w5.md) 立项并跑前冻结（`M24-01`）：多 Agent
> 工作流场景边界冻结为
> [DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)——
> subagent 为 Agent Harness 控制平面内父会话旁的子 Session（不设第三执行面，
> Workflow fork 保持 DEC-019 未承诺扩展位）、fork = 子会话基线 + 快照 schema
> 1.2 加法溯源（不复用 M20 epoch「作废」语义）、局部 delta 独立 schema v1、
> parent merge policy 为机械确定性合并并经既有 §5.2 提交管线（同水位冲突
> fail-closed 不豁免）；门禁 `W5-G1`–`G6`（`tests/m24/`，契约 + 冻结链确定性
> harness）与阶段冻结协议必答八问 + 决策记录表随立项冻结；宿主集成接线（归
> 宿主集成轮）与自动晋升触发（维持 M23 宿主显式现状）为显式非目标；实现未
> 开始，状态 `Planned`；详见 §4.1 第 17 条与
> [M24 文件](m24-context-curator-stage-w5.md)。）
> 此前 2026-09-22（DEC-035 Stage W4「Memory Promotion」交付关闭（同日
> 跑前冻结）：快照耐久语句（constraints / decisions / verified_facts /
> failed_attempts）经 `MemoryConsolidator` 既有纪律晋升长期记忆——共享管线
> 入口 `consolidate_candidates`（管线体只保留一份）、duplicate 判定收紧禁止
> 验证等级降级、晋升候选恒 `Unverified`+`model_assisted`+
> `source_namespace="working-context"`、确定性 record id（独立 seed 空间）、
> 宿主显式触发经泛型 Deferrable 路由；IVA 两轮取证 16 case W4-G1–G6 全绿
> （首轮抓到报告候选被管线搬空的缺陷，修复后复验），本地门禁 ctest 92/92 +
> 三 sanitizer 零报告 + 五检查 + clang-tidy 零违例 + NDK 两 ABI 预演；
> PR [#67](https://github.com/Linductor-alkaid/mira/pull/67) 双 pipeline CI
> 24/24 全绿；**Stage W4 同日关闭（`Completed`）**；详见 §4.1 第 16 条与
> [M23 文件](m23-memory-promotion-stage-w4.md)。）
> 此前 2026-09-22（DEC-035 下一阶段 Stage W4「Memory Promotion」经
> [M23](m23-memory-promotion-stage-w4.md) 立项并跑前冻结（`M23-01`）：快照
> 耐久语句经 `MemoryConsolidator` 既有纪律晋升长期记忆——共享管线入口
> `consolidate_candidates`（管线体只保留一份）、section→kind 冻结映射
> （constraints→Preference 审批门、decisions→ApplicationFact、
> verified_facts→EnvironmentFact、failed_attempts→RecoveryLesson；任务导向
> section 与 `important_refs` 不晋升）、晋升候选恒 `Unverified`+
> `model_assisted`+`source_namespace="working-context"`、duplicate 判定收紧
> 禁止验证等级降级、确定性 record id（TR2 同款 seed 派生）、宿主显式触发经
> 泛型 Deferrable 路由不新增 Supervisor 方法；门禁 W4-G1–G6 与阶段冻结协议
> 必答八问 + 决策记录表随立项冻结；详见 [M23 文件](m23-memory-promotion-stage-w4.md)。）
> 此前 2026-09-22（DEC-040 第三阶段 TR2「WorkflowRuntime 接线与执行」
> 交付关闭（2026-09-21 跑前冻结）：IR v1.1 引用表达加法演进、库挂载
> tool_refs 清单（宿主 attach + 发布门禁自动提取）、`create_run` 准入消费
> （Invalid 拒绝 / Degraded 放行 + 事件留痕）、Skill 经同一 Tool 通道的子
> Workflow 调用执行（DEC-015 同门禁、深度界 2）、Procedure 索引 IMemory
> 写入接线；实现期按上位 M4 契约修订 Procedure evidence 口径并留更正注记；
> IVA 三轮取证（缺陷抓取 → 修复复验 → 拆分保真）18 gate/290 MIRA_CHECK
> 全绿，本地门禁 ctest 91/91 + 三 sanitizer 零报告 + 五检查（含
> `architecture-check`）+ NDK 两 ABI；PR #66 三轮 CI 后 24/24 全绿，master
> 合并提交 run success；**M7 总里程碑同日关闭（`Completed`）**；详见 §4.1
> 第 15 条与 [M7 文件](m7-tools-evaluation-platform-v1.md)。）
> 此前 2026-09-21（维护轮 [DEC-043](../decisions/DEC-043-architecture-policy-and-baseline.md)
> 落地：机器可检查的架构策略 `tools/architecture-policy.json` 与 CI 门禁
> `architecture-check`、存量违规基线、[公共术语表](../project/glossary.md)、
> [阶段冻结与决策协议](../project/stage_freeze_protocol.md)、契约四件套标准；
> 详见[维护计划](maintenance-2026-09-architecture-governance.md)。）
> 此前 2026-09-21（DEC-040 第二阶段 TR1「Skill 发布生命周期与 Procedure
> 索引投影」交付关闭：Skill 描述符（钉住源 Workflow id + `ir_digest`）与暴露面
> 确定性派生、`SkillPublicationRegistry` 宿主显式发布/升级/撤销生命周期、
> Procedure 索引投影（以显式发布为界、可重建）；规范见
> [Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md)
> §17，IVA 两轮取证 23/23 gate 全绿，PR CI 24/24（PR #64，`2833bc4`），详见
> §4.1 第 14 条与 [M7 文件](m7-tools-evaluation-platform-v1.md)。）
> 此前 2026-09-21（DEC-040 首阶段 TR0「Tool 稳定引用与兼容投影」交付关闭：
> 引用语法 v1 冻结（钉住 spec digest / 跟随最新）、Workflow 引用清单提取、解析
> 矩阵与 `Runnable`/`Degraded`/`Invalid` 确定性兼容投影、`Invalid` 准入拒绝决策
> 与 `Degraded` 留痕产物；专项设计
> [Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md)
> 随立项交付，IVA 两轮取证 21/21 gate 全绿，PR CI 24/24（PR #63，`a180e88`），
> 详见 §4.1 第 13 条与 [M7 文件](m7-tools-evaluation-platform-v1.md)。）
> 此前 2026-09-20（M7 MCP 准入阶段（DEC-039 首个实现阶段）跑前冻结工作项
> `M7-MCP-01`–`04` 与门禁 `M7-MCP-G1`–`G6` 后交付：MCP `tools/list` 受控子集 →
> `out_of_process` 模组 manifest 的确定性转换（经真实 TM0 解析器）、会话生命周期
> 只降级映射、宿主 transport 执行适配与 DEC-015 同源门禁、在途调用取消/deadline/
> shutdown 闭合；专项设计 [MCP 准入设计](../design/mcp_tool_admission_design.md)
> 随立项交付，IVA 两轮取证 25/25 gate 全绿，PR CI 24/24（PR #62，`ae7410d`），
> 详见 §4.1 第 12 条与 [M7 文件](m7-tools-evaluation-platform-v1.md)。）
> 此前 2026-09-20（依赖维护 `MNT-202609-34`（Completed）：Executor pin
> `e2dc8ca` → `v0.5.0`（`2ae4fc8`）。上游把 Mira 所处开发线正式定稿发布，4 个功能
> 提交逐一收敛 `MNT-202609-33` 向上游登记的 TSAN/稳定性发现 executor#185–#188；
> 公开 `include/` 头文件零改动，Mira 编译面不变。本地门禁与 executor TSAN 4 项收敛
> 取证全绿，PR CI 24/24（PR #61，`ab88fc9`）后完成，详见 §4.1 第 5 条与
> [维护计划](maintenance-2026-09-post-stage-f.md)。）
> 此前 2026-09-19（M7 第二阶段 TM1「Registry 生命周期」跑前冻结工作项
> `M7-TM1-01`–`03` 与门禁 `M7-TM1-G1`–`G6` 后交付：状态机与不可变 snapshot、
> revoke tombstone、版本化生命周期事件、三 origin 来源信任（DEC-009
> HostProvided allowlist 暂定默认值升格 v1 冻结）、协商触发挂接与 Executor
> 路由部署验证，详见 §4.1 第 10 条与
> [M7 文件](m7-tools-evaluation-platform-v1.md)。）
> 此前 2026-09-16（`MNT-202609-30` 的 M7 重定义提案经
> [DEC-042](../decisions/DEC-042-m7-scope-redefinition.md) 批准：M7 收敛为
> [DEC-009](../decisions/DEC-009-tool-module-boundary.md) Tool 模组体系分阶段
> 落地（TM0 契约与协商 → TM1 Registry 生命周期 → TM2 LLM 暴露投影，后续衔接
> [DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md) MCP 准入与
> [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md) 稳定引用/
> Skill），状态 `Blocked` -> `Planned`，原 `M7-01`–`M7-28` 按 DEC-042 迁移映射
> 处置，TM0 工作项与门禁跑前冻结并进入实施，同日 TM0 交付关闭，见 §4.1 第 9 条。）
> 此前 2026-09-15（[Issue #55](https://github.com/Linductor-alkaid/mira/issues/55)/
> [#56](https://github.com/Linductor-alkaid/mira/issues/56) 的架构缺口评审冻结为四份
> 方向决策：[DEC-038](../decisions/DEC-038-unified-behavior-trace.md)（统一 Behavior
> Trace）、[DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md)（MCP 工具模组
> 准入，部分修订 DEC-009）、[DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)
> （Tool 稳定引用与 Skill 层级）、[DEC-041](../decisions/DEC-041-session-world-state-projection.md)
> （会话 World State 投影）；全部只冻结方向与边界，实现未开始，见 §4.1 方向登记。
> 此前 2026-09-14：DEC-035 Stage W1 由 [M20](m20-working-context-stage-w1.md)
> 承载并本地交付：`WorkingContextSnapshot` 确定性契约、checkpoint → snapshot
> 投影、五元组提交与 store、Layer 0 候选转换、supervisor Deferrable 路由；
> working-context 评估 W1-G1–G6 首轮全绿（投影保真、身份/水位绑定、提交纪律、
> epoch 链隔离、恢复重建、跨进程确定性），基准登记于
> [context-intelligence-working-context-v1](../benchmarks/context-intelligence-working-context-v1.md)；
> PR #49 两轮 CI（tidy 平凡拷贝移动一处修复后）head 双 pipeline 24/24 全绿，
> 里程碑已关闭。同日早前：Stage D 由 [M19](m19-context-intelligence-stage-d.md)
> 承载并交付关闭：Layer 3 `ISemanticConsolidator` 契约与 `IModelProvider` 供给
> 参考固化器、`ConversationCheckpoint` 五元组提交、固化管线评估 D1–D5 首轮
> 全绿（provenance 零违例、标记/跨会话零泄漏、提交纪律全通过），基准登记于
> [context-intelligence-consolidation-v1](../benchmarks/context-intelligence-consolidation-v1.md)，
> PR #47 三轮 CI（clang 捕获、chrono 平台转换、RNG 顺序两处修复后）
> head 双 pipeline 24/24 全绿。
> 同日早前（2026-09-13）：Stage C 由 [M18](m18-context-intelligence-stage-c.md)
> 承载并交付关闭：Layer 2 `IContextReranker` 契约与确定性参考重排器、B/C 列对照
> C1–C4 首轮全绿（混合轮 MRR uplift +0.0139），基准登记于
> [context-intelligence-rerank-v1](../benchmarks/context-intelligence-rerank-v1.md)，
> PR #46 三轮 CI head 双 pipeline 24/24 + master run 12/12。同日早前：Stage B 由
> [M17](m17-context-intelligence-stage-b.md)
> 承载并交付关闭：Layer 1 检索召回契约与参考索引、检索评估 R1–R4 首轮全绿，
> PR #45 三轮 CI（两轮 tidy 修复后）head 双 pipeline 24/24 + master run 12/12；
> Stage C 门槛以检索评估 v1 的 B 列基线为起点。同日早前：Stage A 由 [M16](m16-context-intelligence-stage-a.md)
> 交付关闭——long-session 基线首轮 G1–G6 全绿，PR #44 三轮 CI 36/36。此前：
> `MNT-202609-28` 冻结最小评估 Profile 与 DEC-034；`MNT-202609-33` 升级 Executor
> pin 至 `e2dc8ca`，PR CI 24/24 全绿后完成）
> 设计依据：[Mira Runtime 设计](../design/mira_runtime_design.md)、[Context 与 Memory 设计](../design/context_and_memory_design.md)、
> [LLM API 协议设计](../design/llm-api-protocol-design.md)、[Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)

## 1. 计划目的

本文是 Mira 从空仓库走向可发布 Runtime 的交付入口，只维护范围、里程碑依赖、发布点和通用
门禁。每个里程碑的工作项、测试、风险和逐轮验证记录放在独立文件中。

勾选规则遵循[项目管理与文档规范](../project/project_management_and_documentation.md)：只有实现、
测试、必要文档和验收证据同时完成，工作项才可标记为 `[x]`。环境不足导致的未运行验证保持
未勾选，并记录环境、负责人和补跑条件。

## 2. v1 交付边界

### 2.1 v1 包含

- C++20 平台无关 Agent Core 和稳定的宿主集成边界，首期构建组合覆盖 Linux、Windows、Android。
- `Observe -> Reason -> Plan -> Act -> Verify` 可中断闭环。
- OpenAI-compatible 外部 LLM/VLM Provider。
- Screenshot 与结构化 UI 组成的 Observation Pipeline。
- 离散输入。
- Task、Session、事件、Checkpoint、Memory 与 Replay。
- Simulator 参考环境及至少一个真实平台 Adapter 的契约验证；首个真实目标为 Android Host/NDK。

2026-09-05 起（[DEC-011](../decisions/DEC-011-demo-first-external-validation.md)），依赖
M5/M6 的交付项（本地 OCR/检测/任务 ONNX 感知、连续轨迹与摇杆控制、Human Takeover 实现、
任务模型数据治理与蒸馏链）按原范围终止；这些能力是否以及以何种范围重新进入交付边界，
由独立 demo 仓库的需求验证证据重新定义。"真实平台 Adapter 契约验证"的里程碑载体随 M7
重定义确定。

### 2.2 v1 不包含

- Core 内本地运行通用 LLM/VLM。
- 具体产品 UI 或完整 Android 应用；能力验证 demo 由独立仓库承载（DEC-011），不进入本仓库。
- 未经目标设备实测的硬实时保证。
- 自动将 Runtime 事件、截图或 Memory 转为训练数据。
- 任意代码执行、任意 shell 或模型绕过 Policy 直接调用平台。
- 分布式 Runtime、跨设备一致性和云端控制面。

## 3. 不可破坏的项目约束

- [ ] `RULE-01` Core 不依赖 Android、Windows、Linux 等平台 SDK；平台能力只经 Adapter 注入。
- [ ] `RULE-02` 所有 Mira 发起的异步、阻塞、定时、串行控制和实时任务由 Executor 管理。
- [ ] `RULE-03` Task 状态只有串行控制面可以提交，迟到 completion 不能复活旧 epoch 或终态。
- [ ] `RULE-04` 模型输出只形成结构化 Decision；Action 在本地经过 capability、freshness、权限和
  SafetyPolicy 校验。
- [ ] `RULE-05` 外部副作用采用至多一次派发语义；不确定结果必须 Observe/Verify，禁止盲目重发。
- [ ] `RULE-06` 连续控制具有最大时长、watchdog 和可验证的 `Up/Cancel` 安全收敛。
- [ ] `RULE-07` EventStore 是已提交事实的权威记录；Checkpoint、Memory 和索引是可重建投影。
- [ ] `RULE-08` 所有队列、缓存、上下文、动作、模型请求和并发 operation 都有容量或预算上限。
- [ ] `RULE-09` Observation、Memory、Tool 和模型响应中的外部内容均是不可信数据，不能提升为
  SystemPolicy 或授权。
- [ ] `RULE-10` 性能、实时性、兼容性和跨平台声明必须由目标环境证据支撑。
- [ ] `RULE-11` Executor 能力不足时登记 `docs/executor_feedback/ledger.md`，不得静默引入平行生命周期。
- [ ] `RULE-12` 训练数据导出默认关闭，必须经过授权、脱敏、来源追踪、删除传播和审计。

## 4. 里程碑与发布点

| 里程碑 | 目标 | 前置 | 建议发布点 | 状态 |
| --- | --- | --- | --- | --- |
| [M0](m0-engineering-baseline.md) | 仓库、跨平台构建组合、Executor 集成和质量基线 | 无 | 内部工程基线 | Completed |
| [M1](m1-core-contracts.md) | 公共契约、状态机、持久化和安全边界冻结 | M0 | Core contract alpha | Completed |
| [M2](m2-observation-simulator-android-host.md) | Observation、坐标、Simulator 与 Android Host ABI | M1 | Environment alpha | Completed |
| [M3](m3-model-provider-agent-loop.md) | OpenAI-compatible Provider 和视觉离散闭环 | M2 | Agent loop alpha | Completed |
| [M4](m4-context-memory-recovery.md) | Context/Memory、Replay 和恢复 | M3 | Stateful agent beta | Completed |
| [M5](m5-local-perception-task-models.md) | 本地视觉、任务模型注册与 ONNX 推理（原范围终止） | M3 | 无（见 DEC-011） | Cancelled |
| [M6](m6-realtime-control-takeover.md) | 连续控制、实时路径和 Human Takeover（原范围终止） | M2、M5 | 无（见 DEC-011） | Cancelled |
| [M7](m7-tools-evaluation-platform-v1.md) | Tool 模组体系分阶段落地（[DEC-009](../decisions/DEC-009-tool-module-boundary.md) 模组体系 TM0–TM2、[DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md) MCP 准入与 [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md) TR0–TR2 全部交付关闭；总里程碑退出条件于 2026-09-22 复核通过；范围重定义见 [DEC-042](../decisions/DEC-042-m7-scope-redefinition.md)） | M4；DEC-042 | Tool module alpha（分阶段锚点，非发布物） | Completed |
| [M8](m8-workflow-contracts.md) | Workflow 双路径契约冻结（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 A） | M4 | Workflow contract alpha | Completed |
| [M9](m9-workflow-runtime-minimal-loop.md) | Workflow Runtime 最小闭环（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 B：Strict/DryRun 执行、暂停/取消、操作工具闭环） | M8 | Workflow runtime alpha | Completed |
| [M10](m10-workflow-intervention-and-policy-set.md) | Workflow 介入与执行策略全集（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 C：策略全集、对话 patch 执行、决策点交互） | M9 | Workflow intervention alpha | Completed |
| [M11](m11-trajectory-compilation-and-task-induction.md) | 成功轨迹编译与任务归纳（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 D：轨迹采集、编译、归纳、DryRun 入库门禁） | M9（阶段 B；M10 生效态为输入） | Workflow compilation alpha | Completed |
| [M12](m12-app-model-and-navigation.md) | App Model 与导航（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 E：UI 状态图、Navigation Planner、GUI Mapping 数据面、置信度、`screen_state` 谓词） | M9（阶段 B；感知能力按 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)） | Workflow navigation alpha | Completed |
| [M13](m13-memory-and-learning-loop.md) | Memory 与学习闭环（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 F：四类记忆组织、Episode/Lesson 学习契约、失败检索、恢复复用） | M11、M12（阶段 D/E） | Workflow learning alpha | Completed |
| [M14](m14-recovery-orchestration.md) | Workflow 恢复编排（[DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)：`WaitingAgent` 到 resume 的有界恢复闭环、恢复决策模型请求、`WorkflowRecoveryAttempted` 审计） | M13（`MNT-202609-24` 立项） | Workflow recovery alpha | Completed |
| [M15](m15-eval-harness-and-baseline.md) | 最小评估 Harness 与基线轮（[DEC-034](../decisions/DEC-034-minimal-eval-profile.md)：四臂对照、17 case、G1–G6 门禁、recorded 基线与 soak） | M14；`MNT-202609-28` 冻结 | Workflow learning alpha 验收的评估基线 | Completed |
| [M16](m16-context-intelligence-stage-a.md) | Context Intelligence Stage A——long-session 基线（[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)：Layer 0 有界性与选择/丢弃审计基线，不引入模型） | M4；DEC-032 冻结；`MNT-202609-28` profile 纪律 | Stage B–F 对照基线（非发布物） | Completed |
| [M17](m17-context-intelligence-stage-b.md) | Context Intelligence Stage B——Layer 1 检索召回（[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)：`IContextEmbedder`/`IContextRetriever`，覆盖 Conversation/Episode/Lesson；无模型、无 ANN、无持久化） | M16（Stage A 基线可重复）；DEC-032 §5.2 | Stage C reranker 对照的 B 列基线（非发布物） | Completed |
| [M18](m18-context-intelligence-stage-c.md) | Context Intelligence Stage C——Layer 2 重排对照（[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)：`IContextReranker` 契约、确定性参考重排器、reranker 缺席/失败降级、B/C 列对照数据；无模型） | M17（检索评估 v1 B 列基线可重复）；DEC-032 §5.3 | Stage D 固化对照的方法学锚点（非发布物） | Completed |
| [M19](m19-context-intelligence-stage-d.md) | Context Intelligence Stage D——Layer 3 语义固化（[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)：`ISemanticConsolidator` + `ConversationCheckpoint`、`IModelProvider` 供给参考固化器、provenance fail-closed、五元组提交与终态幂等、冲突优先级落地；无真实模型） | M16/M17/M18；DEC-032 §5.4/§6/§12 | Stage E 真机评估的方法学锚点（非发布物） | Completed |
| [M20](m20-working-context-stage-w1.md) | Context Curator Stage W1——`WorkingContextSnapshot` 确定性契约（[DEC-035](../decisions/DEC-035-context-curator-working-context.md)/[Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)：checkpoint 确定性投影、水位/digest/epoch 生命周期、五元组提交与终态幂等、Layer 0 转换、恢复重建；无模型） | M19；[Context Curator 设计](../design/context_curator_design.md) §4/§5/§7/§8 冻结 | Stage W2–W5 提交管线与输入形态锚点（非发布物） | Completed |
| [M21](m21-context-curator-stage-w2.md) | Context Curator Stage W2——`IContextCurator` 契约与模型供给参考实现（[DEC-035](../decisions/DEC-035-context-curator-working-context.md)/[Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)：快照 schema 1.1 加法扩展（五 Curator section + `generated_by`）、previous+checkpoint+recent events 增量 curation、provenance 绑定与退化防护、`ProviderContextCurator` 经 `IModelProvider` 供给（DEC-036 口径）、Supervisor Deferrable 路由） | M20；[Context Curator 设计](../design/context_curator_design.md) §4.2/§6/§13 与 M21 §4 冻结 | Stage W3 自动触发的输入形态锚点（非发布物） | Completed |
| [M22](m22-working-context-stage-w3.md) | Context Curator Stage W3——Supervisor 自动触发（[DEC-035](../decisions/DEC-035-context-curator-working-context.md)/[Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)：`WorkingContextTriggerPolicy` 双轴触发策略（序列水位 + 执行事件增量）、`WorkingContextAutoCurator` 每会话链 coalescing、task boundary forced flush、失败回退；全部经既有 Deferrable 路由，无隐藏后台循环） | M21；快照链长会话基线可复现（[curation 评估 v1](../benchmarks/context-intelligence-working-context-curation-v1.md)）；[Context Curator 设计](../design/context_curator_design.md) §8/§13 与 M22 §4 冻结 | Stage W4 Memory promotion 与 Stage E 评估矩阵的输入形态锚点（非发布物） | Completed |
| [M23](m23-memory-promotion-stage-w4.md) | Context Curator Stage W4——Memory Promotion（[DEC-035](../decisions/DEC-035-context-curator-working-context.md)：快照耐久语句经 `MemoryConsolidator` 既有纪律晋升长期记忆——`consolidate_candidates` 共享管线入口、section→kind 冻结映射、`Unverified`+`model_assisted` 纪律、duplicate 判定收紧禁止验证等级降级；宿主显式触发、泛型 Deferrable 路由；无模型） | M21（Stage W2 关闭）；[Context Curator 设计](../design/context_curator_design.md) §13 与 M23 §4 冻结 | Stage W5 subagent fork/merge 与宿主集成轮的输入形态锚点（非发布物） | Completed |
| [M24](m24-context-curator-stage-w5.md) | Context Curator Stage W5——Subagent Fork / Merge（[DEC-035](../decisions/DEC-035-context-curator-working-context.md)/[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)：快照 fork——子会话基线 + schema 1.2 加法溯源、局部 delta——独立 schema v1 机械三分类投影、parent merge policy——机械确定性合并经既有 §5.2 提交管线（同水位冲突不豁免）；无模型） | M23（Stage W4 关闭，已满足）；多 Agent 工作流场景冻结（已满足，[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)）；[Context Curator 设计](../design/context_curator_design.md) §13 与 M24 §4 冻结 | 宿主集成轮与真实模型轮/Stage E 评估矩阵的输入形态锚点（非发布物） | Completed |
| [M25](m25-host-integration-round.md) | 宿主集成轮——Agent Loop 快照供给缝与宿主编排参考（[DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)：可选快照供给依赖（未注入零漂移）、`build_request` 经 `context_items_from_working_context` 注入已提交快照条目（身份对齐 + 有界渲染 + 失败降级）；W3/W4/W5 宿主显式编排、Loop 零自动化；三层验收——`tests/m25/` 契约矩阵 + `tests/integration/` 单系统闭环 + `examples/` 参考宿主；无模型） | M23/M24 关闭（已满足）；接线验收形态冻结（已满足，[DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)）；M25 §4 冻结 | 真实模型轮与 Stage E 评估矩阵的接线前提（非发布物） | Completed |
| [M26](m26-temporal-policy-stage-t1.md) | Temporal Policy Stage T1——条件策略契约与确定性 Runtime 最小闭环（[DEC-037](../decisions/DEC-037-temporal-policy.md)：`TemporalHistory`/`TrackedEntity`/T1 子集版本化事件族/`ReactiveRule` schema v1 契约 + `IPolicyRuntime`/`ReactivePolicyRuntime` 最小闭环——冻结确定性数据集上「重复事件 → 候选规则 → 晋升后无需 Agent 正确执行」；无平台、无感知、无模型依赖，指标全部为管线行为指标） | DEC-037 冻结（已满足）；T1 纯 Core 确定性阶段常规授权（总计划 §4.1 第 7 条与 DEC-037 决策第 7 条，已满足）；M26 §4 冻结 | Stage T2/T3 真机感知与 T6 连续控制的契约与方法学锚点（非发布物） | In Progress |

### 4.1 当前状态复核与后续入口（2026-09-09）

阶段 A–F（M8–M13）实现已合入，阶段 F 已具备记忆域、Episode/Lesson 与失败检索。
本轮发现 `BUG-20260909-001`：Android CI 未构建独立的 `mira_workflow` 目标，
六个里程碑的跨平台取证项与退出条件一度重新打开。同日 P0 `MNT-202609-22` 修复
合入（PR #35，`8a5bd53`）：Android CI 显式编译 `mira_workflow` 并新增安装包
consumer 交叉链接门禁，合并提交 CI 12/12 通过，两 ABI 均实际编译全部 Workflow
源文件并完成安装闭包链接；六个里程碑据此逐项复核后恢复 `Completed`，已交付功能和
历史验证记录保留。Android 设备运行证据仍缺，由 `MNT-202609-27` 跟踪。

[阶段 F 后续计划](maintenance-2026-09-post-stage-f.md) 为下一轮执行入口：

1. P0 `MNT-202609-22`（Completed）：Android 两 ABI Workflow 编译与安装消费链接
   证据已补齐并回填六个里程碑与平台矩阵。
2. P1 `MNT-202609-23`–`26`：冻结并实现 Agent 采纳 lesson 的恢复编排——23 已于
   2026-09-10 完成（[恢复编排设计](../design/workflow_recovery_orchestration_design.md)
   与 [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)），24 已于
   2026-09-10 立项 [M14](m14-recovery-orchestration.md) 并交付关闭（PR #38，
   `bb77a0a`，CI 全绿）——编排器、增量字段、审计事件、17 场景矩阵与
   installed-consumer 均已取证；
   `MNT-202609-25` 已完成——真实 SQLite 学习后端跨 owner 重建取证通过，慢/失败
   store 下取消与 shutdown 闭合，DEC-030 §5 事件重建配方缺口登记
   `BUG-20260909-002`（修订提案待立项）；26 依证据推进 Library/Run/App Model
   跨进程持久化。
3. P1 `MNT-202609-27`–`30`：回收 miracle 真机/Provider 证据，建立任务评估基线，形成
   M7 重定义提案；外部证据缺失项保持未完成。28 已于 2026-09-12 完成——
   [最小评估 Profile](../design/discrete_workflow_eval_profile.md) 与
   [DEC-034](../decisions/DEC-034-minimal-eval-profile.md) 冻结四臂对照、17 case、
   指标口径与跑前阈值。29 已于 2026-09-12 立项 [M15](m15-eval-harness-and-baseline.md)
   承载并本地交付首轮：harness、四臂 17 case、G1–G6 门禁、recorded 基线（170 run
   全绿）与 soak（RSS +2.0%）登记于
   [discrete-workflow-eval-v1](../benchmarks/discrete-workflow-eval-v1.md)；live
   canary 与真实平台组仍分别等受控凭据与 27 证据。
4. P2 `MNT-202609-31`–`32`：Procedure 检索消费者、语义召回与 retention 按实际需求立项。
5. 依赖维护 `MNT-202609-33`（2026-09-12，Completed）：Executor pin `4fd8e60` →
   `e2dc8ca`，吸收上游停机/提交交错 UAF 修复（P-001/P-002，Mira 集成测试的 realtime
   register/start/stop 路径在修复范围）与线程池热路径/lock-free 池重建；Mira 使用面
   公开契约未变，本地门禁（debug 69/69、TSAN 68/68 零报告、ASAN 69/69、四检查目标、
   Android arm64 交叉编译）、executor 自身套件、新旧 pin TSAN 对照与 PR CI 24/24
   （PR #43，`5a18df7`）全部通过；证据见[维护计划验证记录](maintenance-2026-09-post-stage-f.md)。
   2026-09-20：`MNT-202609-34` 升级 pin `e2dc8ca` → `v0.5.0`（`2ae4fc8`）：上游将该线
   正式定稿发布，4 个功能提交逐一收敛上述登记的 executor#185–#188（`push_batch_exact`
   批次保留误杀、`~TaskMonitor` 析构竞争、mpsc 测试同步域、benchmark 序列化），公开
   `include/` 头文件零改动；本地门禁（debug 86/86、TSAN 85/85 零报告、ASAN 86/86、
   四检查目标、Android arm64 交叉编译）、executor 自身套件与 TSAN 原失败 4 项收敛取证、
   PR CI 24/24（PR #61，`ab88fc9`）全部通过；证据见[维护计划验证记录](maintenance-2026-09-post-stage-f.md)。
6. 方向登记（2026-09-12，实现未开始）：[Issue #39](https://github.com/Linductor-alkaid/mira/issues/39)
   （长会话上下文管理）与 [Issue #25](https://github.com/Linductor-alkaid/mira/issues/25)
   （Android 混合视觉 grounding）的架构改动方案已结合现状评审并冻结为
   [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)（
   [Context Intelligence 设计](../design/context_intelligence_design.md)）与
   [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)（
   [视觉 Grounding 设计](../design/visual_grounding_design.md)）。两者均只冻结方向与
   目标契约：#39 的 Stage A 基线依赖 `MNT-202609-28` profile，其检索层为
   `MNT-202609-31/32` 提供架构框架；#25 的实现以 `MNT-202609-27` 真机证据为
   `MNT-202609-30` M7 重定义的直接输入。里程碑文件在进入 `Planned` 前创建，不预分配
   编号。2026-09-13：DEC-032 Stage A 依本条入口立项
   [M16](m16-context-intelligence-stage-a.md) 并本地交付首轮基线（详见 §4.2 当日
   记录）；Stage B 的门槛「Stage A 基线可重复」已具备，进入实现前仍须创建里程碑
   文件。DEC-033 状态不变（等 27 证据）。
7. 方向登记（2026-09-14，实现未开始）：[Issue #50](https://github.com/Linductor-alkaid/mira/issues/50)
   （Temporal Policy——从连续感知中学习并固化高频条件策略）冻结为
   [DEC-037](../decisions/DEC-037-temporal-policy.md)（
   [Temporal Policy 设计](../design/temporal_policy_design.md)）。只冻结方向与目标
   契约：Stage T1（`TemporalHistory`/`ReactiveRule` 契约与条件策略 Runtime 确定性
   闭环，纯 Core 无平台依赖）可按常规授权立项，进入实现前新建里程碑文件；
   Stage T2/T3（真机感知）与 T6（连续控制注入）受 DEC-011 门禁
   （`MNT-202609-27` 证据）；Mirador 为外部视觉供给候选，Core 不引入源依赖。
   同日 DEC-036（[修订记录](../decisions/DEC-036-consolidation-model-supply.md)）
   将固化/Curator 模型供给改为可用源模型口径，解除 Stage W2/Stage D 真实模型轮的
   "小模型供应链复核"等待项。两轮文档分别经
   [PR #51](https://github.com/Linductor-alkaid/mira/pull/51)（head `890f5e6`，merge
   `52cf75c`）与 [PR #52](https://github.com/Linductor-alkaid/mira/pull/52)（head
   `f84eb60`，merge `a76c509`）合并：PR CI 双 pipeline 各 12 项全绿（#51：push run
   `34863847977` / pull_request run `34863881245`；#52：push run `34863847436` /
   pull_request run `34863920581`），master 合并提交 run `34867064691` 与
   `34867146055` 均 success。
8. 方向登记（2026-09-15，实现未开始）：[Issue #55](https://github.com/Linductor-alkaid/mira/issues/55)
   （多模块扩张暴露的中间语义层缺口）与 [Issue #56](https://github.com/Linductor-alkaid/mira/issues/56)
   （Tool/MCP 到 Workflow 的统一可编辑行为模型）经现状评审冻结为四份方向决策：
   [DEC-038](../decisions/DEC-038-unified-behavior-trace.md)（统一 Behavior Trace：
   L0 事件/L1 语义行为/L2 narrative 三层投影，承接 DEC-026 §4 的轨迹抽取非目标，
   服务 #56 轨迹编译、Context 压缩与失败分析）；[DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md)
   （MCP 工具模组准入：MCP Tool 以 `ToolModule` 身份在部署/初始化时注册，部分修订
   DEC-009 备选方案第 5 条，运行中热插拔否决不变，实现前置 M7 重定义）；
   [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md)（Tool 稳定逻辑
   引用、Workflow 兼容状态投影 `Runnable/Degraded/Invalid`、Skill=暴露为 Tool 的
   Workflow）；[DEC-041](../decisions/DEC-041-session-world-state-projection.md)
   （会话 World State 投影：从事件确定性重建的当前环境认知，Context/Recovery/
   导航共享消费）。评审同时确认：Authority（DEC-004/DEC-021 §4）与 Capability
   协商（DEC-009）的既有设计已覆盖两 issue 的相应主张，不新建第二套机制；
   Execution Router 不作为独立控制平面（通道选择由双平面切换 + Tool 通道承担）。
   四份决策只冻结方向与边界：L1/L2 schema、引用语法、World State 更新算子等
   契约随首阶段里程碑冻结，里程碑文件在进入 `Planned` 前创建，不预分配编号；
   DEC-039/040 的实现前置是 M7 重定义（`MNT-202609-30`）。本轮文档经
   [PR #57](https://github.com/Linductor-alkaid/mira/pull/57)（head `0347c78`，
   merge `636097d`）合并：PR CI 双 pipeline 各 12 项全绿（push run
   `34976647137` / pull_request run `34976681350`），master 合并提交 run
   `34979069488` success；合并前独立核验 336 个仓库内链接 0 断链、
   `docs-check`（116 文件 / 1547 相对链接）/`format-check`/`platform-boundary-check`/
   `sbom-check` 四项目标全绿。

9. M7 重定义落地入口（2026-09-16）：`MNT-202609-30` 的 M7 重定义提案经专项决策
   [DEC-042](../decisions/DEC-042-m7-scope-redefinition.md) 批准：M7 收敛为
   [DEC-009](../decisions/DEC-009-tool-module-boundary.md) Tool 模组体系分阶段
   落地（TM0 契约与协商 → TM1 Registry 生命周期 → TM2 LLM 暴露投影，后续衔接
   [DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md) MCP 准入与
   [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md) 稳定引用/
   Skill 的实现阶段），前置改为 M4 + DEC-042，建议发布点改为 Tool module alpha
   （分阶段锚点，非发布物）；原 `M7-01`–`M7-28` 按 DEC-042 §4 迁移映射处置——
   真实平台（原 M7-18–22）与 v1.0 发布门禁（原 M7-23–28）保持推迟，
   `MNT-202609-27` 证据门禁不变，[DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)
   不被本重定义解锁。M7 状态 `Blocked` -> `Planned`；TM0 工作项
   （`M7-TM0-01`–`05`）与门禁（`M7-TM0-G1`–`G6`）在
   [M7 文件](m7-tools-evaluation-platform-v1.md) 跑前冻结并进入实施。
   同日 TM0 交付关闭：`include/mira/tool_module.hpp` + `src/tool/tool_module.cpp`
   （CapabilityCatalog 13 条受治理词表、`env.*` 派生纯函数、
   `mira.tool_module.manifest.v1` fail-closed 校验、`negotiate_modules` 确定性
   协商）；`tests/m7/` 契约测试与 minimal-consumer 闭包由 Independent-
   Verification-Agent 独立验证（本地 84/84 + 三 sanitizer 零报告 + golden 跨
   进程字节一致 + NDK 两 ABI 预演；实现期修复 schema 拒绝 domain 归一与两处
   clang-tidy move 违例）。PR
   [#58](https://github.com/Linductor-alkaid/mira/pull/58)（head `8e2dd62`，
   merge `f2d2077`）双 pipeline 各 12 项首轮全绿（push run
   [`35125359147`](https://github.com/Linductor-alkaid/mira/actions/runs/35125359147)
   / pull_request run
   [`35125364698`](https://github.com/Linductor-alkaid/mira/actions/runs/35125364698)），
   master 合并提交 run
   [`35127320798`](https://github.com/Linductor-alkaid/mira/actions/runs/35127320798)
   success；`M7-TM0-01`–`05` 与 `M7-TM0-G1`–`G6` 关闭，下一阶段为 TM1。

10. M7 TM1 落地入口（2026-09-19）：TM0 关闭后依 §4.2 冻结 TM1 细项
   （`M7-TM1-01`–`03`）与门禁（`M7-TM1-G1`–`G6`）并交付 Registry 生命周期：
   三 origin 来源信任（DEC-009 HostProvided allowlist 暂定默认值升格 v1 冻结，
   注记见该决策）、七状态受控转换与不可变 snapshot、revoke tombstone、跨模组
   wire 名冲突激活 fail-closed、协商触发挂接（session/epoch/模组状态三类，
   closed registry 拒绝）、两类版本化事件与 Executor 路由部署验证
   （`submit_auto()` + consume 折叠拒绝面）。测试由 Independent-Verification-
   Agent 两轮独立取证（首轮抓到提交拒绝路径两处缺陷，修复后 20/20 门复验通过，
   报告跨进程字节一致 md5 `aba09395c81b616f4adc0cd3d75829ab`）。本地门禁：
   全量 ctest 85/85、三 sanitizer m7 目标零报告、四检查、clang-tidy 预检与
   NDK r26.3 两 ABI 预演通过。PR
   [#59](https://github.com/Linductor-alkaid/mira/pull/59)（head `01de324`，
   合并提交 `76eec95`）双 pipeline 各 12 项首轮全绿（push run
   [`35425708347`](https://github.com/Linductor-alkaid/mira/actions/runs/35425708347)
   / pull_request run
   [`35425754169`](https://github.com/Linductor-alkaid/mira/actions/runs/35425754169)），
   master 合并提交 run
   [`35427226722`](https://github.com/Linductor-alkaid/mira/actions/runs/35427226722)
   success；`M7-TM1-01`–`03` 与 `M7-TM1-G1`–`G6` 关闭，下一阶段为 TM2（LLM
   暴露投影，实施前冻结细项）；证据与限制见
   [M7 文件](m7-tools-evaluation-platform-v1.md) 验证记录。

11. M7 TM2 落地入口（2026-09-19）：TM1 关闭后依 §4.3 冻结 TM2 细项
   （`M7-TM2-01`–`03`）与门禁（`M7-TM2-G1`–`G6`）并交付 LLM 暴露投影：
   `project_tool_exposure` 投影纯函数（active 集与协商结论错配整组 fail-closed、
   空集空视图兼容 M3）、ToolId 确定性派生（TM2 身份分配）、两级排除理由
   （模组级 Unavailable/Conflict/Revoked/ReservedWireName，任务级
   TaskPolicy/TaskBudget）、合成 `snapshot_digest`（绑定 generation + module
   digest 集 + ToolSpec digest 集，DEC-002 加法演进）、`wire_name` 规则 v1 冻结
   （设计 §17 收口，DEC-009 注记）、Simulator 参考模组
   `builtin.simulator.env` 与 Replay `verify_recorded_module_digests` 绑定、
   `resolve_tool_calls` 组合语义。附带修复：TM0 manifest 解析器成员重名检测
   悬垂 `string_view` 缺陷（等长短名误判重复，IVA 首轮发现，主循环修复后回归
   门落地）；**format 门禁空转缺陷**——`format-check` 目标 `VERBATIM` 下
   `-DROOT_DIR="..."` 内嵌引号被字面保留导致 GLOB 匹配零文件、检查静默通过
   （自 M0 基线起，既往各轮"format 检查通过"证据对 C++ 文件为空转），本轮修复
   脚本与传参并整树按 CI 口径（clang-format 18.1.8）机械重排。测试由
   Independent-Verification-Agent 三轮独立取证（86/86 ctest、三 sanitizer
   9 项零报告、真实 format 门禁 214 文件、`--report` 跨四构建树字节一致）。
   PR
   [#60](https://github.com/Linductor-alkaid/mira/pull/60)（head `21e749d`，
   合并提交 `0655d65`）pull_request run
   [`35452396490`](https://github.com/Linductor-alkaid/mira/actions/runs/35452396490)
   12 项全部通过；push run
   [`35452378534`](https://github.com/Linductor-alkaid/mira/actions/runs/35452378534)
   首轮 11/12（TSAN 作业被基础设施取消，无测试失败输出，重跑后 success）；
   master 合并提交 run
   [`35455012483`](https://github.com/Linductor-alkaid/mira/actions/runs/35455012483)
   success（12/12）。`M7-TM2-01`–`03` 与 `M7-TM2-G1`–`G6` 关闭，M7 已立项
   阶段（TM0–TM2）全部关闭，后续阶段（DEC-039/DEC-040）立项时增补；证据与
   限制见 [M7 文件](m7-tools-evaluation-platform-v1.md) 验证记录。

12. M7 MCP 准入落地入口（2026-09-20）：TM2 关闭后依 M7 §4.4 冻结 MCP 阶段
   细项（`M7-MCP-01`–`04`）与门禁（`M7-MCP-G1`–`G6`）并交付（DEC-039 首个实现
   阶段，前置「DEC-009 模组体系落地」已满足）：专项设计
   [MCP 准入设计](../design/mcp_tool_admission_design.md) 随立项交付；
   `convert_mcp_listing_to_module` 转换纯函数（受控 listing 子集经**真实** TM0
   解析器产出 `out_of_process` 模组；hint→ActionRisk 确定性映射、宿主
   `risk_overrides` 只升不降、fail-closed 前置检查；空 listing 整组拒绝——IVA
   首轮发现冻结稿与 TM0 1..256 成员上界冲突，按上位契约优先修订设计并留更正
   注记）；脱敏投影 `mira.tool_module.mcp.admission.v1`；
   `plan_mcp_session_action`+`McpModuleAdmission` 会话生命周期只降级（部署窗
   准入、断开/能力列表变化 revoke、变更 digest 需新注册周期）；
   `IMcpToolTransport`/`McpToolDispatcher` 执行适配（DEC-015 同源门禁、
   OperationId 至多一次预约-回滚、聚合资源上限；`submit_auto()` 路由 + future
   必消费 + 协作取消/deadline/close 有界排空）。测试由 Independent-
   Verification-Agent 两轮独立取证（25 gate/456 断言、三 sanitizer 零报告、
   `--report` 跨进程/跨树 md5 `19b8f358f9615d2caa7415eee6b12714`、与
   `BuiltinToolRegistry` 的 11 行 DEC-015 对照逐条一致）。本地门禁：全量 ctest
   87/87、四检查（format 217 文件）、clang-tidy 零违例、NDK 两 ABI 编译且符号
   在库。PR
   [#62](https://github.com/Linductor-alkaid/mira/pull/62)（head `39884de`，合并
   提交 `ae7410d`）双 pipeline run
   [`35512461239`](https://github.com/Linductor-alkaid/mira/actions/runs/35512461239)/
   [`35512472810`](https://github.com/Linductor-alkaid/mira/actions/runs/35512472810)
   各 12 项首轮全绿，master 合并提交 run
   [`35513574793`](https://github.com/Linductor-alkaid/mira/actions/runs/35513574793)
   success；`M7-MCP-01`–`04` 与 `M7-MCP-G1`–`G6` 关闭，下一阶段为 DEC-040
   稳定引用与 Skill（实施前冻结细项）；证据与限制见
   [M7 文件](m7-tools-evaluation-platform-v1.md) 验证记录。

13. DEC-040 首阶段 TR0 立项与交付入口（2026-09-21）：MCP 准入关闭后依 M7 §4.5
    冻结 TR0 细项（`M7-TR0-01`–`04`）与门禁（`M7-TR0-G1`–`G6`）并交付（DEC-040
    首个实现阶段，前置「模组体系落地」已满足）：专项设计
    [Tool 稳定引用与 Skill 设计](../design/tool_reference_and_skill_design.md)
    随立项交付；引用语法 v1 冻结（`toolref:<wire-name>` 跟随 / `@<spec digest>`
    钉住，受治理词表字符集跨源同命名空间；版本约束钉住不进 v1）；引用清单工件
    `mira.workflow.tool_refs.v1`（发布期观察、绑定 `definition_digest`、IR v1
    零改动）；解析矩阵与兼容状态投影（`Runnable`/`Degraded`/`Invalid` 确定性
    重算，骨架可绑定判定复用 M3 严格 schema 校验器零漂移）；`Invalid` 准入拒绝
    决策与 `Degraded` 留痕投影 `mira.workflow.tool_compat.v1`（脱敏）；投影不进
    执行路径，DEC-015 语义不变。引用层为 Workflow 资产面契约，实现入
    `mira_workflow`（`src/workflow/tool_reference.cpp`），consumer 闭包链接
    workflow 库。测试由 Independent-Verification-Agent 两轮独立取证（21 gate/
    约 239 断言，四树全绿、三 sanitizer 零报告、`--report` 跨树 md5
    `4c4d8c72654e3211d6f9b6e3f7901666`；首轮抓到 `toolref:<name>@` 误判跟随的
    解析缺陷，修复后零测试改动复验）。本地门禁：全量 ctest 88/88、四检查
    （format 220 文件）、clang-tidy 零违例、NDK 两 ABI 编译且符号在库。PR
    [#63](https://github.com/Linductor-alkaid/mira/pull/63)（head `4313cc1`，
    格式修复 `522bfb3`，合并提交 `a180e88`）：首轮 push run quality 因测试
    文件 clang-format 违例失败（唯一违例文件，空白重排），修复后双 pipeline run
    [`35522450999`](https://github.com/Linductor-alkaid/mira/actions/runs/35522450999)/
    [`35522453203`](https://github.com/Linductor-alkaid/mira/actions/runs/35522453203)
    各 12 项全部通过，master 合并提交 run
    [`35523751427`](https://github.com/Linductor-alkaid/mira/actions/runs/35523751427)
    success；`M7-TR0-01`–`04` 与 `M7-TR0-G1`–`G6` 关闭，下一阶段为 DEC-040
    TR1（Skill 生命周期、Procedure 索引投影、Runtime 接线与 IR 引用表达加法
    演进，实施前冻结细项）；证据与限制见
    [M7 文件](m7-tools-evaluation-platform-v1.md) 验证记录。

14. DEC-040 第二阶段 TR1 立项与交付入口（2026-09-21）：TR0 关闭后依 M7 §4.6
    冻结 TR1 细项（`M7-TR1-01`–`04`）与门禁（`M7-TR1-G1`–`G6`）并交付：
    Skill 描述符 `mira.skill.descriptor.v1`（name wire 身份、显式版本、源
    Workflow id + `ir_digest` 钉住、暴露面确定性派生——description 取
    summary、参数 schema 从 `WorkflowParameterSpec` 映射过
    `gate_schema_subset`、副作用由 TR0 引用清单 × 视图推导）；
    `SkillPublicationRegistry` 宿主显式发布/升级/撤销（publish_validated
    runnable 门禁、只降级、seal/close 部署窗、幂等 NoOp、superseded 轨迹、
    版本化事件 `mira.skill.publication.v1` 脱敏）；Procedure 索引投影
    `mira.skill.procedure_index.v1`（以宿主显式发布为界——未发布库资产不
    自动索引，DEC-029 否决的自动写入面不复活；statement 固定 canonical
    JSON、无时钟、可重建、不写 `IMemory`）。TR1 无执行面（Skill 不进
    registry/exposure，子 Workflow 调用执行与 `create_run` 准入消费、
    `Degraded` 事件发射、IR 引用表达加法演进归 TR2，实施前冻结细项）。
    测试由 Independent-Verification-Agent 两轮独立取证（23 gate/430 断言，
    四树全绿、三 sanitizer 零报告、`--report` 跨树 md5
    `e8a91ca5e030df1584250dcb964fa1a3`；首轮发现升级后原样重发布当前
    descriptor 被误拒的幂等缺陷，裁决为实现缺陷并修复，经独立探针与新增
    断言复验）。本地门禁：全量 ctest 89/89、四检查（format 223 文件）、
    clang-tidy 零违例、NDK 两 ABI 编译且符号在库。PR
    [#64](https://github.com/Linductor-alkaid/mira/pull/64)（head `8159e24`，
    合并提交 `2833bc4`）双 pipeline run
    [`35528677082`](https://github.com/Linductor-alkaid/mira/actions/runs/35528677082)/
    [`35528693696`](https://github.com/Linductor-alkaid/mira/actions/runs/35528693696)
    各 12 项首轮全部通过，master 合并提交 run
    [`35530911707`](https://github.com/Linductor-alkaid/mira/actions/runs/35530911707)
    success；`M7-TR1-01`–`04` 与 `M7-TR1-G1`–`G6` 关闭，下一阶段为 DEC-040
    TR2（WorkflowRuntime 接线与执行，实施前冻结细项）；证据与限制见
    [M7 文件](m7-tools-evaluation-platform-v1.md) 验证记录。

15. DEC-040 第三阶段 TR2 立项与交付入口（2026-09-22）：TR1 关闭后依 M7
    §4.7 冻结 TR2 细项（`M7-TR2-01`–`05`）与门禁（`M7-TR2-G1`–`G6`，
    跑前冻结遵循[阶段冻结与决策协议](../project/stage_freeze_protocol.md)
    必答八问 + 决策记录表）并交付（DEC-040 消费者接线阶段，前置 TR1 已
    满足）：专项设计 §18 随冻结交付（文件升 v1.3）；IR reader 升 `{1,1}`
    （v1.1 引用表达加法演进：ToolCall `arguments["tool"]` 接受 `toolref:`
    引用，准入期重写为裸 wire 名，执行路径与 DEC-015 零感知）；refs 挂载表
    （`attach_workflow_tool_refs` fail-closed/幂等/容量 + `publish_validated`
    发布门禁自动提取挂载，提取失败 `tool-refs-unresolvable` 拒绝且库零变更）；
    `create_run` 准入消费 TR0 投影（dispatching 策略 × 挂载或 v1.1 定义，
    `Invalid` 拒绝 / `Degraded` 放行 + `mira.workflow.tool-compat-degraded.v1`
    事件每 run 恰一次 / DryRun 与无挂载 v1.0 零漂移）；Skill 经同一
    `BuiltinToolRegistry` 的子 Workflow 调用执行适配（DEC-040 §3.2，DEC-015
    同门禁无豁免、嵌套深度界 `max_skill_call_depth=2`、发布缺失/Revoked/
    descriptor 漂移 fail-closed）；Procedure 索引 IMemory 写入接线（确定性
    mutation id 幂等、confidence 0.3、Revoked 不写）。实现期裁决一处（IVA
    首轮发现）：M4 mutation 契约要求 Add 必带事件 evidence，冻结稿「无事件
    provenance」口径按上位契约优先修订——sync 发射
    `mira.workflow.procedures-synced.v1` 审计事件作为 evidence 锚，更正
    注记落 M7 §4.7 与设计 §18.4。测试由 Independent-Verification-Agent
    两轮独立取证（18 gate/290 MIRA_CHECK + 119 fixture 断言，debug+三
    sanitizer 树全绿，`--report` 跨进程 md5
    `58cf0e9cf97260d40b073c50e7d90b86`；首轮抓到上述 evidence 缺陷，修复后
    复验）。本地门禁：全量 ctest 90/90、三 sanitizer 零报告、四检查
    （format 225 文件）、clang-tidy 零违例、NDK 两 ABI 编译且符号在库。
    `M7-TR2-01`–`05` 与 `M7-TR2-G1`–`G6` 关闭；M7 已立项阶段（TM0–TM2、
    MCP 准入、TR0–TR2）全部交付，DEC-040 的三阶段计划全部落地；总里程碑
    关闭随 PR CI 回填与退出条件复核评审；证据与限制见
    [M7 文件](m7-tools-evaluation-platform-v1.md) 验证记录。
    同日：PR [#66](https://github.com/Linductor-alkaid/mira/pull/66) CI 共
    三轮——首轮 22/24（quality：新单文件测试矩阵超 `architecture-check`
    1200 行预算，拆分为 wiring/skill 两 TU + 共享 fixture，断言逐条保真）、
    二轮 23/24（windows-debug：MSVC C4702 拆分遗留不可达代码 + C4515 命名
    空间自引用，两处测试面机械修复）、三轮 head `ff654a8` 双 pipeline 各
    12/12 全绿（push
    [`35642649408`](https://github.com/Linductor-alkaid/mira/actions/runs/35642649408)
    / pull_request
    [`35642651479`](https://github.com/Linductor-alkaid/mira/actions/runs/35642651479)），
    master 合并提交 run
    [`35645301901`](https://github.com/Linductor-alkaid/mira/actions/runs/35645301901)
    success（12/12）。**M7 总里程碑退出条件同日复核通过并关闭（`Completed`）**：
    全部已立项阶段（TM0–TM2、MCP、TR0–TR2）按「冻结 → 实现 → IVA 取证 →
    本地门禁 → PR CI」节奏关闭；`M7-01`–`M7-06` 重定义映射项全部有验证记录
    （详见 [M7 文件](m7-tools-evaluation-platform-v1.md) §9 末条）；推迟项
    按 DEC-042 保持推迟。

16. DEC-035 Stage W4「Memory Promotion」立项与交付入口（2026-09-22）：M7
    总里程碑关闭后，依总计划前注与 [M22](m22-working-context-stage-w3.md)
    关闭记录点名的 DEC-035 下一阶段，经 [M23](m23-memory-promotion-stage-w4.md)
    立项（`M23-01`）并跑前冻结契约语义、门禁 `W4-G1`–`G6` 与阶段冻结协议
    必答八问 + 决策记录表：快照耐久语句（constraints / decisions /
    verified_facts / failed_attempts 四 section）经 `MemoryConsolidator`
    既有纪律晋升长期记忆——`MemoryConsolidator` 新增 `consolidate_candidates`
    共享管线入口（管线体只保留一份，`consolidate()` 重构为其调用方）、
    duplicate 判定收紧为"已存副本验证等级 ≥ 提案等级"以杜绝 `HumanConfirmed`
    被 `Unverified` 晋升副本降级、晋升候选恒 `Unverified`+`model_assisted`+
    `source_namespace="working-context"`、确定性 record id 从独立 seed 空间
    派生（TR2 同款模式）、任务导向 section 与 `important_refs` 不晋升、宿主
    显式触发经 `ContextMemorySupervisor::submit` 泛型 Deferrable 路由（不新增
    Supervisor 方法）、无模型无 benchmark（RULE-10，门禁全部为契约测试）。
    同日交付：测试由 Independent-Verification-Agent 两轮独立取证（16 case
    W4-G1–G6；首轮抓到 `promote_working_context_to_memory` 将投影候选随管线
    move 导致报告不可审计的缺陷——报告先接管投影、管线按拷贝消费，修复后
    复验全绿；另冻结 §4.1 实现注记：空 evidence 丢弃分支经公共 API 不可达，
    快照 `validate()` 先行整体拒绝）。本地门禁：debug 全量 ctest 92/92、
    三 sanitizer 零报告、五检查（format 229 文件）、clang-tidy 零违例、
    NDK 两 ABI 预演通过。同日：PR
    [#67](https://github.com/Linductor-alkaid/mira/pull/67)（head `1f63632`）
    双 pipeline 各 12/12 全绿（push
    [`35679225851`](https://github.com/Linductor-alkaid/mira/actions/runs/35679225851)
    / pull_request
    [`35679247197`](https://github.com/Linductor-alkaid/mira/actions/runs/35679247197)）。
    `M23-01`–`05` 与 `W4-G1`–`G6` 关闭，**Stage W4 交付完成（`Completed`）**；
    证据与限制见 [M23 文件](m23-memory-promotion-stage-w4.md) 验证记录；
    遗留：Stage W5（subagent fork/merge）、自动晋升触发与宿主集成接线、
    真实模型接入与语义质量声明（归真实模型轮与 Stage E 证据通道）。

17. DEC-035 Stage W5「Subagent Fork / Merge」立项与交付入口（2026-09-23）：W4 关
    闭后，依[Context Curator 设计](../design/context_curator_design.md) §13
    W5 行门禁先冻结缺失的前置——「多 Agent 工作流场景」原无任何文档承载
    （全语料仅 §13 W5 行一处占位；issue #48「Subagent 与上下文隔离」节是需
    求记录非决策），经专项决策
    [DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)
    冻结：subagent 为 Agent Harness 控制平面内父会话旁的子 Session（不设
    `SubagentRuntime` 或第三执行面；Workflow 分支合并/并行/子 Workflow 保持
    [DEC-019](../decisions/DEC-019-workflow-ir-contract.md) §2 未承诺扩展
    位，issue #48 强制 flush 清单中的 `workflow fork` 归该扩展链）；fork =
    子会话基线 + 快照 schema 1.2 加法溯源（不复用 M20 task_epoch「作废」语
    义——live 单五元组、会话水位单调与 W-07 系统性阻断同会话并行分支）；
    curated result = 子侧既有 `IContextCurator` 管线产物 + 机械 delta 投影
    （独立 schema v1：inherited 剔除 / 同 section 血统交集 supersede / 其余
    addition 三分类）；parent merge policy = 机械确定性合并（引用驱动、
    stale supersede 重分类、固定顺序截断），产物经既有 §5.2 提交管线落库
    （同水位冲突 fail-closed 不豁免，无第二写路径）；子代理无动作路径，如
    需环境动作在其自身会话按既有 ActionLease 纪律执行。经
    [M24](m24-context-curator-stage-w5.md) 立项（`M24-01`）并跑前冻结契约
    语义、门禁 `W5-G1`–`G6`（`tests/m24/` 契约/集成 + 冻结 fork/merge 链
    确定性 harness，label `integration;m24`）与阶段冻结协议必答八问 + 决策
    记录表；无模型无语义质量声明（RULE-10，issue #48 对照指标归真实模型轮
    与 Stage E 证据通道）；宿主集成接线（归宿主集成轮另行立项）与自动晋升
    触发（维持 M23 冻结的宿主显式现状，如需自动化须上位决策）为显式非目
    标。实现与验证同日完成（提交 `4e10449`，PR
    [#68](https://github.com/Linductor-alkaid/mira/pull/68) CI 24/24 全绿）：
    schema 1.2 加法溯源与新契约 `context_working_context_fork.hpp/.cpp` 落
    地、既有 m21 套件六处戳记同步；IVA 独立 22 用例 `W5-G1`–`G6` 全绿 +
    冻结链 eval harness 跨进程字节一致；本地 clang-tidy 零违例、五检查全绿
    （测试套件按 max-file-lines 裁决拆分为契约 TU + 生命周期 TU + 共享助手
    头）。`M24-01`–`05` 与 `W5-G1`–`G6` 关闭，**Stage W5 交付完成
    （`Completed`）**；证据与限制见 [M24 文件](m24-context-curator-stage-w5.md)
    验证记录；遗留：宿主集成接线、自动 fork/merge 与自动晋升触发、模型介导
    语义合并、真实模型接入与语义质量声明（归真实模型轮与 Stage E 证据通
    道）。

18. 宿主集成轮立项入口（2026-09-23）：W5 关闭后，遗留四项中唯一无外部依赖
    的「宿主集成接线」依 M24 §2/§5 留痕（「接线验收形态无任何已冻结文档，
    八问不可答」）先补冻结面，经专项决策
    [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md) 冻结
    接线形态：Agent Loop 薄缝——可选快照供给依赖（沿 `set_event_store`/
    `set_tool_registry` setter 注入先例，未注入时请求装配与现状逐字节一
    致），`build_request` 经既有 `context_items_from_working_context`
    （Layer 0 唯一准入转换）注入当前会话**已提交**快照条目（身份对齐门槛
    = 快照 `session_id`/`task_id`/`task_epoch` 与当前 `AgentLoopSpec` 任务
    帧一致，不一致跳过并计诊断；环境纪元不比较——Loop 不持有该值、M3 契约
    不扩面，门控归宿主供给回调闭包；有界渲染固定顺序截断、供给失败降级为
    无快照条目 + 诊断事件）；**零自动化**——W3 信号上
    报、W4 终态晋升（`submit<WorkingContextPromotionReport>` 泛型
    Deferrable）、W5 fork/merge（父水位严格前进才可提交，同水位
    fail-closed 不豁免）全部宿主显式编排，Loop 内零 Supervisor/AutoCurator
    调用（M23 §5/M24 §5 决策表与 [DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)
    第 7 条冻结语义零触碰）；自动 fork/merge、自动晋升触发与模型介导语义
    合并不并入本轮（各按既有决策留痕归口，须上位决策方可立项）。经
    [M25](m25-host-integration-round.md) 立项（`M25-01`）并跑前冻结契约语
    义、门禁 `HI-G1`–`G6`（三层：`tests/m25/` 契约/生命周期矩阵 label
    `integration;m25` + `tests/integration/working_context_host_test.cpp`
    单系统闭环 label `integration` + `examples/working_context_host_consumer.cpp`
    参考宿主 label `consumer`）与阶段冻结协议必答八问 + 决策记录表；范围钉
    死 Core 参考宿主（simulator/offline、scripted provider），平台 Adapter
    宿主接入另行立项；无模型无语义质量声明（RULE-10，issue #48 对照指标归
    真实模型轮与 Stage E 证据通道）。实现与验证同日完成（2026-09-23，提交
    `4abf8ee`，[PR #69](https://github.com/Linductor-alkaid/mira/pull/69)
    CI 24/24 全绿）：`M25-02` 供给缝契约与实现、`M25-03`/`M25-04` 测试矩阵由
    IVA 独立交付并复验 `HI-G1`–`G5` 全绿、`M25-05` 参考宿主与 `M25-06` 文档
    同步交付，IVA 复验 `HI-G6` 后 `HI-G1`–`G6` 与 `M25-01`–`06` 关闭，
    **Stage W5 链宿主集成轮交付完成（`Completed`）**；证据与遗留见
    [M25 文件](m25-host-integration-round.md) 验证记录（Loop 内自动化、平台
    Adapter 宿主接入、真实模型/Stage E 语义指标维持非目标留痕）。

19. DEC-037 Stage T1 立项入口（2026-09-24）：M25 关闭后，遗留三项中唯一具备
    完整立项条件的是 DEC-037 Stage T1——总计划 §4.1 第 7 条与
    [DEC-037](../decisions/DEC-037-temporal-policy.md) 决策第 7 条授权「纯 Core
    确定性阶段按常规授权立项」且 M21/M22 关闭记录两度点名其为下一可用入口；
    平台 Adapter 宿主接入须先补专项决策冻结接线验收形态（M24 §2/§5 同型留痕
    「八问不可答」），真实模型轮与 DEC-032 Stage E 受 `MNT-202609-27` 外部证据
    阻塞，DEC-038/DEC-041 首阶段须先做实体词表对齐并补专项设计。经
    [M26](m26-temporal-policy-stage-t1.md) 立项（`M26-01`）并跑前冻结正式契约
    （`TemporalHistory` 有界观测环与重放重建、`TrackedEntity`/`PolicyWorldView`
    输入契约、`ReactiveRule` schema v1 闭集谓词 + provenance + 幂等 rule_id、
    `IPolicyRuntime`/`ReactivePolicyRuntime` step 评估语义（激活门、固定匹配序、
    冲突 fail-closed）、宿主显式归纳/采纳/测试/晋升/降级生命周期（`min_support`
    + 测试分部零误触发 + `min_test_support` 证据晋升）、T1 子集九类
    `mira.policy.*.v1` 版本化事件（schema 名与逐类载荷键集逐一冻结）、
    `mira.temporal_policy` 错误域（12 码显式 int32 枚举 + 稳定名映射，沿
    `mira.*` 域惯例））、门禁 `T1-G1`–`G6`（`tests/m26/` 契约/生命周期矩阵 +
    `tests/m26/m26_temporal_policy_eval.cpp` 确定性闭环评估 harness，label
    `integration;m26`，数据集 digest 锚定 + `--report` 跨进程一致）与阶段冻结
    协议必答八问 + 决策记录表；闭环证明口径 = 设计 §12「重复事件 → 候选规则 →
    晋升后无需 Agent 介入正确执行」，指标全部为管线行为指标（RULE-10）；T1 零
    Executor 注册面——step/归纳/测试为有界同步函数、harness 编排经
    `submit_auto()` 且 future 必消费、realtime/低延迟 lane 为 T2+/T6 集成形态
    （生产 realtime 部署验证随其立项冻结）；范围注记交叉点——规则产出非授权
    来源（RULE-09 延伸，DEC-004 PolicyEngine 边界不变）、策略激活的 Workflow/
    工具化表达（设计 §8）与 EventStore 事件桥接为显式非目标（随其立项独立冻结）、
    Temporal Policy 与 PolicyEngine/WorkflowPolicy 术语消歧随 glossary 词条
    落地；真机感知（T2/T3）与连续控制注入（T6）仍受 DEC-011 门禁
    （`MNT-202609-27` 证据），不被本立项解锁。状态 `In Progress`（2026-09-24
    `M26-01` 立项冻结已交付；实现自 `M26-02` 起推进，门禁验证完成前工作项保持
    未勾选）。

M5/M6 保持 Cancelled；M7 经 DEC-042 重定义为 `Planned`（TM0–TM2 常规交付节奏，
后续阶段随立项增补）。#8 的最小 BuiltIn 工具闭环已由 DEC-015 和
维护轮交付；M7 剩余的是模组治理、隔离、评估及发布范围重定义，不再把 #8 列作未实现。
本轮不新增产品范围决策；具体依赖、负责人、验收与补跑条件见上述计划。

### 4.2 历史实施与验收记录

`M0 -> M1 -> M2 -> M3 -> M4` 已完成。2026-09-05 起（
[DEC-011](../decisions/DEC-011-demo-first-external-validation.md)），`M3 -> M5 -> M6`
交付链终止、M7 挂起：后续能力需求由独立仓库 demo 产品的验证证据重新定义，产出新的或
重定义的里程碑后恢复交付；在此之前不设关键路径。任何里程碑都不得以“后续再补取消、安全
或验证”关闭。2026-09-06 起，miracle 第一轮真机反馈经
[维护计划 maintenance-2026-09-host-abi-feedback.md](maintenance-2026-09-host-abi-feedback.md)
与 [DEC-012](../decisions/DEC-012-host-adapter-feedback-round1.md) 落地（GitHub #7–#13）；
其中 #8（AgentLoop ToolProposals）当时为方向登记；其最小闭环已于 2026-09-07
由 DEC-015 交付，M7 模组体系仍为 Blocked，待 POST-01 证据重定义。
同日第二轮反馈（GitHub #14、#15）经
[维护计划 maintenance-2026-09-transport-and-image-media.md](maintenance-2026-09-transport-and-image-media.md)
与 [DEC-013](../decisions/DEC-013-transport-export-and-image-media.md) 落地：官方网络
传输头随安装包导出，模型图像 wire 媒体类型改为工件记录驱动并由宿主负责编码。

2026-09-07 起（[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)），
Mira 的长期架构方向确立为 Agent Harness 控制平面 + Workflow 数据平面双路径
（[Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)）。
该决策不改变既有里程碑状态、不解冻 M7、不自动恢复 M5/M6，也不改变 v1 交付边界现状；
其落地以 M7 重定义或新增里程碑承载，工作项、Executor 路由、测试矩阵与退出条件在对应
阶段文档定义，阶段划分以设计文档第 16 节为输入。

2026-09-07 起，Workflow 方向的阶段 A 由新增里程碑
[M8](m8-workflow-contracts.md)（`Proposed`）承载；选择新增里程碑而非重定义 M7 的理由见
该文件第 1 节。后续阶段 B–F（Runtime 最小闭环、介入与策略、编译与抽象、App Model 与
导航、Memory 与学习闭环）依次由新增里程碑承载：每个阶段进入 `Planned` 前，先依据
前一阶段冻结的契约完成对应专项设计与决策，再创建里程碑文件；不对未创建的里程碑预分配
编号。M7 状态不因该方向改变，GitHub
[#8](https://github.com/Linductor-alkaid/mira/issues/8) 最小闭环随后由 DEC-015
提前交付；模组化范围仍随 M7 重定义。
2026-09-08，M8 经维护者评审转 `Planned` 并进入实施（`In Progress`）；其契约决策
（[DEC-019](../decisions/DEC-019-workflow-ir-contract.md) 至
[DEC-022](../decisions/DEC-022-conversation-patch-semantics.md)）与专项设计
（[Workflow Runtime 设计](../design/workflow_runtime_design.md)）已冻结。
2026-09-08，`Mira::workflow` 契约模块与五组契约测试随同交付，PR
[#29](https://github.com/Linductor-alkaid/mira/pull/29) CI 全绿（三平台 + sanitizer +
quality 矩阵），M8 关闭（`Completed`）。同日，阶段 B（Runtime 最小闭环）里程碑
[M9](m9-workflow-runtime-minimal-loop.md) 依据 `M8-05` 设计创建：经维护者评审（用户指示
依设计与计划推进下一步开发，与 M8 同一授权模式）转 `Planned` 并进入实施（`In Progress`）；
其范围、契约补全（`M9-01` 冻结 M8 暂定默认值与 ToolCall 工具绑定约定）、Executor 路由
定稿与测试矩阵见该文件。同日交付 `WorkflowRuntime`（Strict/DryRun 执行闭环、暂停/恢复/
取消、恢复钩子、shutdown、run/pause/resume/cancel 四操作 BuiltIn 闭环与模型发起
`run_workflow` 端到端），PR [#30](https://github.com/Linductor-alkaid/mira/pull/30) CI
全绿（三平台 + sanitizer + quality 矩阵 24 项），M9 关闭（`Completed`）；阶段 C（对话
驱动 patch 与执行策略全集）里程碑可依据该设计进入 `Planned`。同日，阶段 C 专项决策
[DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)（策略全集运行时
语义与检查点）与 [DEC-024](../decisions/DEC-024-conversation-patch-execution.md)（对话
patch 执行语义与决策点交互）冻结，`workflow_runtime_design` 升至 v0.3（§11 阶段 C 实施
规范）；里程碑 [M10](m10-workflow-intervention-and-policy-set.md) 依据其创建，经维护者
评审（用户指示依设计与计划推进下一步开发，与 M8/M9 同一授权模式）转 `Planned` 并进入
实施（`In Progress`）。同日交付策略全集运行时（失败升级、检查点、升级预算）、对话
patch 执行闭环（准入矩阵、幂等双检、步边界生效、审计、回退、策略切换）与决策点交互
（`WaitingUser` 两类来源、`resolve_decision` 唯一出口、`request_user_input` 工具、
模型发起端到端），五操作与决策工具 BuiltIn 闭环齐活；PR
[#31](https://github.com/Linductor-alkaid/mira/pull/31) CI 全绿（三平台 + sanitizer +
quality 矩阵 24 项；首轮 quality 1 处 clang-tidy `performance-move-const-arg` 修复后
复验），M10 关闭（`Completed`）；阶段 D（成功轨迹编译与任务归纳）里程碑可依据该设计
创建。同日，阶段 D 专项决策
[DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md)
（成功轨迹契约、编译与入库门禁）与 [DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md)
（任务归纳与参数化提议）冻结，`workflow_runtime_design` 升至 v0.4（§12 阶段 D 实施
规范）；里程碑 [M11](m11-trajectory-compilation-and-task-induction.md) 依据其创建，经
维护者评审（用户指示依设计与计划推进下一步开发，与 M8/M9/M10 同一授权模式）转
`Planned` 并进入实施（`In Progress`）。同日交付轨迹契约与采集（`capture_trajectory`）、
字面量编译与默认值固化、结构 diff 任务归纳与参数化重写（`workflow_compiler` 模块）、
DryRun 入库门禁（`publish_validated`：内容派生证据、幂等 NoOp、失败库零变更）与
publish 三员审计事件；本机全矩阵（Debug/Release/ASAN/UBSAN 60/60、TSAN 59/59、
quality）通过，PR
[#32](https://github.com/Linductor-alkaid/mira/pull/32) CI 全绿（三平台 + sanitizer +
quality 24 项；首轮 quality 1 处 clang-tidy `performance-move-const-arg` 修复后复验），
M11 关闭（`Completed`）；阶段 E（App Model 与导航）里程碑可依据该设计创建（前置为
阶段 B；感知能力按 DEC-011 重定义）。同日，阶段 E 专项决策
[DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)（App Model 契约
与置信度）与 [DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md)
（Navigation Planner 与 Navigate 步骤解析）冻结，`workflow_runtime_design` 升至 v0.5
（§13 阶段 E 实施规范）；里程碑 [M12](m12-app-model-and-navigation.md) 依据其创建，经
维护者评审（用户指示依设计与计划推进下一步开发，与 M8–M11 同一授权模式）转 `Planned`
并进入实施（`In Progress`）。同日交付 `workflow_navigation` 模块（`AppModel` 契约与
置信度纯函数、确定性 Dijkstra 规划器）、导航两员事件与 `WorkflowRuntime` 集成
（导航上下文安装、Navigate 准入条件化与逐边执行、`screen_state` 谓词绑定、置信度
回写；DryRun 真实规划使 `publish_validated` 门禁对导航可达性有约束力）；本机全矩阵
（Debug/Release/ASAN/UBSAN 63/63、TSAN 62/62、本地门禁）通过，PR
[#33](https://github.com/Linductor-alkaid/mira/pull/33) CI 全绿（三平台 + sanitizer +
quality 24 项；两轮 quality 各 1 处 clang-tidy 违例——`performance-inefficient-
string-concatenation` 与 `bugprone-branch-clone`——修复后复验），M12 关闭
（`Completed`）；阶段 F（Memory 与学习闭环，前置 D/E）里程碑可依据架构设计 §10/§16
创建（先专项设计与决策，不预分配编号）。同日，阶段 F 专项决策
[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)
（Memory 四类组织与 Workflow 学习契约）与
[DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)
（学习闭环运行时语义）冻结，`workflow_runtime_design` 升至 v0.6（§14 阶段 F 实施
规范）；里程碑 [M13](m13-memory-and-learning-loop.md) 依据其创建，经维护者评审
（用户指示依设计与计划推进下一步开发，与 M8–M12 同一授权模式）转 `Planned` 并进入
实施（`In Progress`）。同日交付 `workflow_learning` 模块（`MemoryDomain` 四类组织、
`WorkflowEpisodeRecord`/`WorkflowRecoveryLesson`/`WorkflowFailureSignature` 契约、
纯转换与失败检索查询构建）与 `WorkflowRuntime` 学习集成（`set_learning_context`、
结算期 Episode 记录、失败驱动升级的检索进入 `agent_continuation().relevant_lessons`、
`record_recovery_lesson` 恢复复用闭环、两员学习审计事件）；本机全矩阵
（Debug/Release/ASAN/UBSAN 66/66、TSAN 65/65、本地门禁）通过；PR
[#34](https://github.com/Linductor-alkaid/mira/pull/34) CI 全绿（head `bff2fe3`，push
pipeline run [`34310027223`](https://github.com/Linductor-alkaid/mira/actions/runs/34310027223)、
pull_request pipeline run
[`34310029920`](https://github.com/Linductor-alkaid/mira/actions/runs/34310029920)）：
Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android arm64-v8a 与
x86_64（NDK）、ASAN/UBSAN/TSAN 与 quality（clang-tidy 18 + clang-format + docs/sbom/
platform-boundary 检查）全部 24 项通过（两轮修复后复验：MSVC 需要 `<numeric>` 提供
`std::accumulate`；clang-tidy `optin.performance.Padding` 要求 RunRecord 按对齐分组
重排字段），M13 关闭（`Completed`）。DEC-014 阶段 A–F 至此全部交付；后续方向（真实
平台 Adapter 契约验证与 M7 重定义、阶段 F 显式非目标中的 Procedure 索引/失败检索
向量腿/Agent 采纳 lesson 的编排）按证据另行立项，不设隐式关键路径。

2026-09-12，维护者依据开放 issue 复核两条长期能力需求并冻结架构方向（维护者指示：
阅读 issue、结合项目实际更新架构）：[Issue #39](https://github.com/Linductor-alkaid/mira/issues/39)
（Context Intelligence / 长会话上下文管理）经现状核对（Layer 0 确定性压缩已由 M4 的
`StandardContextManager` 交付；检索三腿齐备但 embedding 供给方缺失；无会话级语义固化）
冻结为 [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 与
[Context Intelligence 设计](../design/context_intelligence_design.md)；
[Issue #25](https://github.com/Linductor-alkaid/mira/issues/25)（Android 混合视觉
grounding）经现状核对（Observation 契约已预留 `PerceptionEvidence`/`ElementRef` 占位、
坐标变换链与 accessibility 全链已交付、pipeline 无 perception 挂钩且无推理依赖）冻结为
[DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md) 与
[视觉 Grounding 设计](../design/visual_grounding_design.md)，并显式遵守 DEC-011 的
demo 证据重入门禁（`MNT-202609-27/30`）。同日同步的文档：
[Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
（v0.3）、[Context 与 Memory 架构设计](../design/context_and_memory_design.md)（v0.5）、
[Observation、坐标与 Android Host ABI 设计](../design/observation_coordinate_android_host.md)
（v1.1）、[本地感知与任务模型设计](../design/local_perception_and_task_models.md)
（假设记录注记，v1.2）与[阶段 F 后续维护计划](maintenance-2026-09-post-stage-f.md)。
本变更仅涉及文档；未新增实现、测试或能力声明，全部里程碑状态不变。交付经 PR
[#40](https://github.com/Linductor-alkaid/mira/pull/40)（合并提交 `c8b41bf`，两
pipeline 24/24 全绿）合入，CI 证据见[阶段 F 后续维护计划](maintenance-2026-09-post-stage-f.md)
验证记录。

2026-09-13，维护者指示「依设计与计划推进下一阶段开发」（与 M8–M15 同一授权模式）。
经计划核对：维护计划内可执行项（26/29 剩余件/30/31/32）均被 `MNT-202609-27` 外部
证据或受控凭据阻塞，唯一解锁的新方向入口是 DEC-032 Stage A（前置
`MNT-202609-28` 已完成）。据此新增里程碑 [M16](m16-context-intelligence-stage-a.md)
承载：long-session 基线 harness（`tests/m16/m16_long_session_baseline.cpp`，独立
ctest 目标）以 100/500/1000 轮合成负载（设计 §11 七类内容混合）与全量历史重呈现
的最坏情形驱动现有 `StandardContextManager`（无模型、无新契约、无新异步路径），
跑前冻结 G1–G6 门禁与 workload profile。首轮 recorded 基线全绿：原始 presented
tokens 线性增长（286,579 → 2,917,016，10.2×）而选择 token 三个 N 共用平台上界
27,147（utilization 0.629–0.699，`input_budget` 38,784 之下）；用户纠正 66/66 全
保留（P1 最小执行集，最小集 token 线性 3,135 → 5,797）；N=1000 终末请求 3,187 项
对话/历史与 600 对工具记录被逐出——长会话连续性损失的量化基线，Stage B 检索的
直接动机。基线与发现登记于
[context-intelligence-long-session-v1](../benchmarks/context-intelligence-long-session-v1.md)
（`dataset_digest`
`b8b082e7e26d2206505a6e54fafe495a551b4f49145d1ca2c2948f33a77c64a5`）。本地门禁：
debug ctest 70/70、ASAN/UBSAN/TSAN m16 通过零报告、format/docs/platform-boundary/
sbom 四检查通过；同进程与跨进程确定性复验一致。限制：miracle 派生负载回放归 27
语料补跑；token 为保守上界非 Provider 实计数；Windows/Android/Release/quality 由
PR CI 回填后 M16 关闭。本轮无公开契约变更。

2026-09-13（第二次），维护者指示「依设计与计划推进下一步开发」（与 M8–M16 同一授权
模式）。经计划核对：Stage A 已关闭且其 G5 确定性证据满足 Stage B 前置门槛，据此新增
里程碑 [M17](m17-context-intelligence-stage-b.md) 承载 DEC-032 Stage B——Layer 1 检索
召回。交付：`include/mira/context_retrieval.hpp`（`IContextEmbedder` 外部供给契约、
`IContextRetriever`、`ContextIndexAsset` 三类资产模型、`ContextQuery`/`RetrievalBudget`/
`ContextCandidate`、候选 JSON `mira.context.candidate.v1`）；`InMemoryContextIndex`
参考索引（exact 逐字子串强制过滤 + 词法 token 覆盖率 + 有界线性 cosine 三腿混合，
`MemoryQueryQuality` 逐腿降级、ACL 默认拒绝、deadline 部分结果、水位前进使旧
embedding 失效、禁止标记 fail-closed、token packing）；`segment_conversation()`
确定性窗口切分（`ConversationEntry` 增补 `session_sequence` 回填，加法式契约变更）；
`context_item_from_candidate()` 使检索候选以 P4 `RetrievedMemory` 进入
`StandardContextManager` 准入；`ContextMemorySupervisor::schedule_context_retrieval`
（Interactive）承载 Executor 路由。无模型、无 ANN、无向量持久化、无 AgentLoop 集成
（显式非目标）。检索评估 harness（48 锚定查询 × 528 资产确定性数据集，token-hash
确定性供给方）首轮 R1–R4 全绿：Recall@10 = 1.0（48/48）、MRR 0.986（降级路径 1.000）、
ACL 零泄漏、跨进程报告字节级一致；登记于
[context-intelligence-retrieval-v1](../benchmarks/context-intelligence-retrieval-v1.md)
（`dataset_digest`
`ffc59f6d0a0a375a6e8730906219339900b616ed1f861b274c437515a367649e`）。本地门禁：
debug ctest 72/72、ASAN/UBSAN/TSAN m17 通过零报告、format/docs/platform-boundary/
sbom 四检查通过。限制：token-hash 指标为管线行为基线非语义质量声明；真实 embedder
供给方与 Stage C 对照待后续。Windows/Android/Release/quality 由 PR CI 回填后 M17 关闭。

2026-09-13（第三次），同一授权模式推进 DEC-032 Stage C。前置核对：M17 已关闭且其
检索评估 v1 B 列基线满足 Stage C 门槛，据此新增里程碑
[M18](m18-context-intelligence-stage-c.md) 承载 Layer 2 重排对照（profile 与门禁
C1–C4 跑前冻结）。交付：`include/mira/context_rerank.hpp`（`IContextReranker`、
`RankedContextItem`、`ContextRerankConfig`/`ContextRerankWeights`、确定性参考重排器
`TokenOverlapContextReranker`——查询 token 覆盖 F1 + `exact_terms` 加成，
min-max 融合默认 0.60/0.40，全等集合归一 0.5）；`src/context/context_rerank.cpp`
（入 `mira_core`）；`ContextMemorySupervisor::schedule_context_rerank`（Interactive）；
重排失败返回错误、调用方降级检索序 Top-K（设计 §8）。对照评估 harness 复用 M17
冻结数据集（digest 断言锚定）与 token-hash 供给方，首轮 C1–C4 全绿：B/C 两列
Recall@10 = 1.0，混合轮 MRR uplift +0.0139（0.9861 → 1.0000，F1 重排压回 hash
噪声干扰），ACL 零泄漏，跨进程报告字节级一致；登记于
[context-intelligence-rerank-v1](../benchmarks/context-intelligence-rerank-v1.md)。
本地门禁：debug ctest 74/74、ASAN/UBSAN/TSAN m18 通过零报告、format/docs/
platform-boundary/sbom 四检查通过。限制：确定性供给方下 uplift 为管线行为方向
非语义声明；真实 reranker 模型与 AgentLoop 集成待后续。Windows/Android/Release/
quality 由 PR CI 回填后 M18 关闭。

2026-09-13：PR CI 证据回填并关闭。PR
[#46](https://github.com/Linductor-alkaid/mira/pull/46)（head `8b122c2`，合并提交
`7aaae31`）push 与 pull_request pipeline run
[`34763228402`](https://github.com/Linductor-alkaid/mira/actions/runs/34763228402)/
[`34763231930`](https://github.com/Linductor-alkaid/mira/actions/runs/34763231930)
各 12 项、合并提交 master pipeline run
[`34764028390`](https://github.com/Linductor-alkaid/mira/actions/runs/34764028390)
12 项全部通过，首轮 CI 一次通过。Stage C 关闭后，DEC-032 下一阶段为 Stage D
（`ISemanticConsolidator` + `ConversationCheckpoint`，经 `IModelProvider` 配置
小模型），进入实现前依设计 §12 新建里程碑文件。

2026-09-14，维护者指示「依设计与计划推进下一步开发」（与 M8–M18 同一授权模式）。
经计划核对：DEC-032 下一阶段为 Stage D，前置（Stage C 关闭、设计 §5.4/§6/§7/§8
冻结）已满足，据此新增里程碑 [M19](m19-context-intelligence-stage-d.md) 承载
Layer 3 语义固化（profile 与门禁 D1–D5 跑前冻结）。交付：
`include/mira/context_consolidation.hpp`（`ConversationStatement` 四类语句别名、
`ConsolidationOptions` 有界与标记配置、`ConversationCheckpoint`（五元组、
`validate()`、排除叙事的 `projection_digest()`、JSON `mira.context.checkpoint.v1`）、
`ISemanticConsolidator`、`ProviderSemanticConsolidator`（编号转录 +
`StrictJsonSchema` + 严格解析 fail-closed + provenance 越界丢弃 + 标记过滤 +
边界裁剪）、`IConversationCheckpointStore`/`InMemoryConversationCheckpointStore`
（水位单调、有界保留）、`commit_conversation_checkpoint`（五元组校验、终态
幂等、幂等 NoOp、同水位冲突 fail-closed）、`context_items_from_checkpoint`
（约束 → P1 `UserConstraint`、摘要/决策/线索 → P3 `CheckpointSummary`、全部
`UntrustedExternalData` authority、偏好不转换））；`src/context/
context_consolidation.cpp`（入 `mira_core`）；`ConversationSegment` 增补逐条目
`entries`（加法式契约变更，转录引用与 provenance 绑定所需）；
`ContextMemorySupervisor::schedule_context_consolidation`（Deferrable、取消
探针透传、shutdown 拒绝与在途取消）。无真实模型、无 AgentLoop 集成、无
`ContextIntelligenceService`（显式非目标）。固化评估 harness（12 会话 × 40
条确定性数据集、digest `31758a94…` 锚定、脚本化 `IModelProvider` 供给方）首轮
D1–D5 全绿：约束/决策/线索召回与语句 precision 均 1.0（204/204）、provenance
零违例、标记与跨会话零泄漏、降级轮 0 提交且种子 checkpoint 保留、陈旧/终态
候选 100% 丢弃、跨进程报告字节级一致；登记于
[context-intelligence-consolidation-v1](../benchmarks/context-intelligence-consolidation-v1.md)。
本地门禁：debug ctest 76/76、ASAN/UBSAN/TSAN m19 通过零报告、format/docs/
platform-boundary/sbom 四检查通过、miniconda clang-tidy 18.1.8 预检库源
（一处 `performance-move-const-arg` 已修复）。

2026-09-14：PR CI 证据回填并关闭。PR
[#47](https://github.com/Linductor-alkaid/mira/pull/47)（head `a2c4c0d`，合并提交
`ba46767`）push 与 pull_request pipeline run
[`34772280749`](https://github.com/Linductor-alkaid/mira/actions/runs/34772280749)/
[`34772282466`](https://github.com/Linductor-alkaid/mira/actions/runs/34772282466)
各 12 项全部通过（Linux GCC/Clang Debug/Release、Windows MSVC、Android 两 ABI、
ASAN/UBSAN/TSAN、quality），第三轮全绿——前两轮分别修复 clang
`-Wunused-lambda-capture`、NDK/MSVC `chrono` duration 显式转换、噪声条目
RNG 抽取顺序歧义（数据集 digest 重新钉定，实现代码零变更）。Stage D 关闭后，
DEC-032 下一阶段为 Stage E（miracle 真机评估，前置 `MNT-202609-27` 证据通道，
当前 Blocked）；Stage F（提示压缩实验）归后续立项。

2026-09-14，维护者指示「依设计与计划推进下一步开发」（与 M8–M19 同一授权模式）。
经计划核对：Stage E 仍被 `MNT-202609-27` 外部证据通道阻塞，Stage D 之后无内部
阻塞的入口是当日新登记的方向 issue
[#48](https://github.com/Linductor-alkaid/mira/issues/48)（Context Curator /
Working Context）。据此冻结
[Context Curator 设计](../design/context_curator_design.md)与
[DEC-035](../decisions/DEC-035-context-curator-working-context.md)（阶段名
W1–W5，不占用 DEC-032 Stage E/F 编号），并新增里程碑
[M20](m20-working-context-stage-w1.md) 承载 Stage W1——`WorkingContextSnapshot`
确定性契约（无模型；profile 与门禁 W1-G1–G6 跑前冻结）。交付：
`include/mira/context_working_context.hpp`（`WorkingContextItem` 三 section
快照契约、确定性 id 与 `state_digest`、`WorkingContextMergeOptions` 有界配置、
`working_context_from_checkpoint` 全有或全无投影、`IWorkingContextStore`/
`InMemoryWorkingContextStore`（水位单调、有界保留环、epoch 新链）、
`commit_working_context`（终态幂等、五元组校验、幂等 NoOp、同水位冲突
fail-closed）、`context_items_from_working_context`（约束 → P1 `UserConstraint`、
决策/未决 → P3 `CheckpointSummary`、`UntrustedExternalData` authority、id 空间
与 checkpoint 条目分离））；`src/context/context_working_context.cpp`（入
`mira_core`）；`ContextMemorySupervisor::schedule_working_context_commit`
（Deferrable、shutdown 拒绝）。无 Curator 模型、无增量 merge、无自动触发
（W2–W3 显式非目标）。working-context 评估 harness（12 会话 × 5 checkpoint
链确定性数据集、digest `ed81befb…` 锚定）首轮 W1-G1–G6 全绿：60/60 链提交、
840 条目投影保真零违例、重放 12/12 NoOp、同水位冲突/陈旧/身份不匹配/终态迟到
100% 丢弃且存储不变、epoch 新链 12/12 提交且旧链可回查、恢复 12/12 幂等重建、
跨进程报告字节级一致；登记于
[context-intelligence-working-context-v1](../benchmarks/context-intelligence-working-context-v1.md)。
本地门禁：debug ctest 78/78（原 76 + 本里程碑 2 目标）、ASAN/UBSAN/TSAN
（`setarch -R`）m20 两目标零报告、format/docs/platform-boundary/sbom 四检查
通过、miniconda clang-tidy 18.1.8 预检库源零违例、本机同版本 NDK 两 ABI
交叉编译预演通过。限制与未执行项：确定性投影口径非语义质量声明（`RULE-10`）；
W2–W5、真实模型与真机评估为显式非目标；Windows/Release/quality 由 PR CI
回填后 M20 方可关闭（`M20-07`）。

2026-09-14：PR CI 证据回填并关闭。PR
[#49](https://github.com/Linductor-alkaid/mira/pull/49)（head `bf4e100`，合并提交
`48f4781`）push 与 pull_request pipeline run
[`34848824021`](https://github.com/Linductor-alkaid/mira/actions/runs/34848824021)/
[`34848828495`](https://github.com/Linductor-alkaid/mira/actions/runs/34848828495)
各 12 项全部通过（Linux GCC/Clang Debug/Release、Windows MSVC、Android 两 ABI、
ASAN/UBSAN/TSAN、quality），第二轮全绿——第一轮修复 CI quality 报
`performance-move-const-arg`（lambda init-capture 对平凡可拷贝的
`WorkingContextMergeOptions` 使用 `std::move`；M19 同型违例，本机 tidy 预检
此后纳入全部被修改库源编译单元）。Stage W1 关闭后，DEC-035 下一阶段为
Stage W2（`IContextCurator` 契约与模型供给参考实现，进入实现前须依设计 §13
新建里程碑文件并冻结增量 merge 语义；模型候选经供应链复核通道）；DEC-032
Stage E（miracle 真机评估）保持 Blocked 等 `MNT-202609-27` 证据通道。

2026-09-15，维护者指示「依设计与计划推进下一步开发」（与 M8–M20 同一授权模式）。
经计划核对：DEC-035 Stage W2 的两项门禁均已满足（W1 由 M20 关闭；模型供给经
[DEC-036](../decisions/DEC-036-consolidation-model-supply.md) 改为可用源模型
口径，"小模型供应链复核"等待项已解除），据此创建
[M21](m21-context-curator-stage-w2.md) 承载 Stage W2——`IContextCurator` 契约
与模型供给参考实现（增量 merge 语义与退化防护随该文件 §4 跑前冻结）。交付：
快照 schema 加法式升级 1.1（`active_tasks` / `verified_facts` /
`failed_attempts` / `important_refs` / `next_actions` 五 Curator section +
`generated_by`，v1.0 兼容读取，`working_context_from_checkpoint` 映射不变，
Layer 0 转换扩至八 section 且不引入新 kind）；新公开契约
`include/mira/context_curator.hpp`（`ContextCurationOptions`、输出 schema
`mira.working_context.curator.output.v1`、`IContextCurator` 与
`ProviderContextCurator`——三段编号转录 `prev:`/`ckpt:`/`event:`、provenance
并集绑定、逐条 fail-closed、`degenerate-merge` 退化防护、候选身份五元组 +
累积 `source_checkpoints` 链 + `generated_by` 记录实际模型 profile）；
`ContextMemorySupervisor::schedule_working_context_curate`（Deferrable，
Curator 失败不提交、调用方保留已存快照）。测试 `tests/m21/`（契约 10 组 +
curation eval harness，label `integration;m21`；测试的编写、运行与 sanitizer
取证由 Independent-Verification-Agent 独立完成，实现侧修复一处 transcript
块前缀偏差后复验）。curation 评估 harness（12 会话 × 5 轮链 + 脚本化确定性
Curator，digest `4e3221cb…b6b6` 锚定）首轮 W2-G1–G6 全绿：递进链 60/60 提交、
绑定/身份/merge/转换零违例、对抗输出 84/84 丢弃、同水位冲突/陈旧/身份四联/
终态迟到/退化输出 100% 拒绝且 store 不变、五类失败 60/60 正确降级、恢复
60/60 幂等、跨进程报告字节级一致；登记于
[curation 评估 v1](../benchmarks/context-intelligence-working-context-curation-v1.md)。
本地门禁：debug 全量 ctest **80/80**（原 78 + 本里程碑 2 目标）、
ASAN/UBSAN/TSAN（`setarch -R`）m21 两目标零报告、format/docs/
platform-boundary/sbom 四检查通过、clang-tidy 18.1.8 预检全部三个被改库源
零违例、本机 NDK r26.3 两 ABI 交叉编译与 arm64 installed-consumer 闭包预演
通过。限制与未执行项：脚本化确定性供给方口径，非语义质量声明（`RULE-10`）；
真实模型接入与对照指标归真实模型轮/Stage E；W3–W5 为显式非目标；
Windows/Release/quality 由 PR CI 回填后 M21 方可关闭（`M21-06`）。

2026-09-15：M21 PR CI 证据回填并关闭。PR
[#53](https://github.com/Linductor-alkaid/mira/pull/53)（head `0ad7218`，合并
提交 `6b7e377`）push 与 pull_request pipeline run
[`34884298162`](https://github.com/Linductor-alkaid/mira/actions/runs/34884298162)/
[`34884341364`](https://github.com/Linductor-alkaid/mira/actions/runs/34884341364)
各 12 项首轮全部通过（Linux GCC/Clang Debug/Release、Windows MSVC
Debug/Release、Android 两 ABI 编译级、ASAN/UBSAN/TSAN、quality），无需修复轮。
Stage W2 关闭后，DEC-035 下一阶段为 Stage W3（Supervisor 自动触发：
watermark / event count / task boundary、coalescing、forced flush，进入实现前
新建里程碑文件并冻结触发策略；门禁为 W2 关闭与快照链在长会话基线上可复现）；
DEC-032 Stage E（miracle 真机评估）保持 Blocked 等 `MNT-202609-27` 证据通道，
DEC-037 Stage T1 可按常规授权另行立项。

2026-09-15，同一授权模式下创建 [M22](m22-working-context-stage-w3.md) 承载
DEC-035 Stage W3——Supervisor 自动触发（两项进入门禁复核通过：W2 由 M21
关闭；快照链长会话基线可复现由 curation 评估 v1 恢复 60/60 字节一致取证）。
交付：`include/mira/context_working_context_auto.hpp` +
`src/context/context_working_context_auto.cpp`——`WorkingContextTriggerPolicy` /
`evaluate_working_context_trigger` 双轴纯策略（序列水位距离优先、宿主上报
执行事件增量其次；设计 §8 "token watermark" 细化为序列水位，体量预算留在
M21 options）、`WorkingContextAutoCurator`（每会话链 coalescing、unsettled
闸门杜绝同水位重 curate、task boundary forced flush 有界排干 +
`auto-refresh-current` 短路、失败回退燃点重臂无紧重试、previous 取 store
latest 且 epoch 新链隔离、共享 future 双副本、析构 2×deadline 有界 drain）；
全部 curation 经既有 `schedule_working_context_curate` Deferrable 路由，无
隐藏后台循环。测试 `tests/m22/`（契约 16 用例 + auto-trigger eval harness，
label `integration;m22`；测试的编写、运行与 sanitizer 取证由
Independent-Verification-Agent 独立完成并复验，期间发现并修复燃点
`shared_future` 二次 `share()` 空句柄缺陷）。auto-trigger 评估 harness
（8 会话 × 40 信号冻结数据集，digest `6f2ab2e5…1535` 锚定）首轮 W3-G1–G6
全绿：640 次信号判定、206 次燃点与纯函数预测零失配（watermark 72 /
event count 31）、coalescing 吸收 24/24 零重复调度、forced 16/16 提交、
五类失败 40/40 正确回退、重放 103/103 快照字节一致；登记于
[auto-trigger 评估 v1](../benchmarks/context-intelligence-working-context-auto-trigger-v1.md)。
本地门禁：debug 全量 ctest **82/82**（原 80 + 本里程碑 2 目标）、
ASAN/UBSAN/TSAN m22 两目标零报告、format/docs/platform-boundary/sbom 四
检查通过、clang-tidy 18.1.8 预检被改库源零违例、本机 NDK r26.3 两 ABI
交叉编译预演通过。PR [#54](https://github.com/Linductor-alkaid/mira/pull/54)
首轮 CI 中 android 四任务因 `android-actions/setup-android@v3` 在 runner
镜像上持续损坏（"Failed to find package 'tools'"）失败，`62b7a04` 移除该
action 改用 runner 预装 sdkmanager 后，两 pipeline 各 12 项全绿
（push [`34932243071`](https://github.com/Linductor-alkaid/mira/actions/runs/34932243071)
/ pull_request [`34932244666`](https://github.com/Linductor-alkaid/mira/actions/runs/34932244666)），
合入 `8630f19` 并关闭 M22。DEC-035 下一阶段为 Stage W4（Memory Promotion，
经 `MemoryConsolidator` 既有纪律，进入实现前新建里程碑文件并冻结边界
测试）；DEC-032 Stage E 保持 Blocked 等 `MNT-202609-27` 证据通道，
DEC-037 Stage T1 可按常规授权另行立项。

M4–M7 的范围、稳定工作项、Executor 路由、测试矩阵、风险、退出条件和验证记录已拆入各自阶段
文档。`Planned` 仅表示范围和验收方式已明确，不表示前置已满足或实现已开始。M3 已于 2026-09-02
完成跨平台 TLS、upload fixture 与 MiniMax-M3 Responses 分能力互操作验收；支持声明严格限于兼容性
矩阵中标记 `InteropVerified` 的字段，image=`Failed` 和其他 `Unknown` 能力不被外推。

## 5. 首批架构决策

| 决策 | 主题 | 状态 | 冻结点 |
| --- | --- | --- | --- |
| [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md) | Runtime 的 Executor 所有权与串行控制面 | Accepted | M0 |
| [DEC-002](../decisions/DEC-002-public-contract-versioning.md) | 公共契约、结果和版本化边界 | Accepted | M1 |
| [DEC-003](../decisions/DEC-003-event-sourced-persistence.md) | EventStore 事实源与副作用日志协议 | Accepted | M1 |
| [DEC-004](../decisions/DEC-004-security-authority-confirmation.md) | 权限、能力授权和 Human Confirmation | Accepted | M1 |
| [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md) | Observation 坐标与 Android Host 边界 | Accepted | M2 |
| [DEC-006](../decisions/DEC-006-local-perception-task-models.md) | 本地感知和任务 ONNX 模型边界 | Accepted | M5 |
| [DEC-007](../decisions/DEC-007-llm-api-protocol-strategy.md) | LLM API 规范契约与协议方言策略 | Accepted | M3 |
| [DEC-008](../decisions/DEC-008-transport-dependency-strategy.md) | 历史 M3 传输基线 | Superseded by DEC-010 | M3 |
| [DEC-009](../decisions/DEC-009-tool-module-boundary.md) | 工具模组边界与能力协商 | Accepted | M7 |
| [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md) | 锁定 Mbed TLS、受管代理与远端文件生命周期 | Accepted | M3 |
| [DEC-011](../decisions/DEC-011-demo-first-external-validation.md) | Demo 优先验证、M5/M6 终止与外部消费边界 | Accepted | M4 后 |
| [DEC-012](../decisions/DEC-012-host-adapter-feedback-round1.md) | Host Adapter 第一轮反馈契约修订（artifact 注入、输入时长、UI 树线格式、lease 统计） | Accepted | M4 后维护轮 |
| [DEC-013](../decisions/DEC-013-transport-export-and-image-media.md) | 传输头文件导出与模型图像 wire 媒体类型（宿主负责编码） | Accepted | M4 后维护轮 |
| [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) | Agent Harness 控制平面与 Workflow 数据平面双路径架构 | Accepted | M7 重定义（暂定） |
| [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md) | BuiltIn 工具执行边界与 AgentLoop 工具闭环（GitHub #8） | Accepted | M4 后维护轮 |
| [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md) | 对话事件、会话投影与步边界用户消息 | Accepted | M4 后维护轮 |
| [DEC-017](../decisions/DEC-017-complete-task-command.md) | 任务终态完成命令 `complete_task` | Accepted | M4 后维护轮 |
| [DEC-018](../decisions/DEC-018-takeover-input-release-and-operation-admission.md) | Takeover 平台输入释放与暂停态操作准入 | Accepted | M4 后维护轮 |
| [DEC-019](../decisions/DEC-019-workflow-ir-contract.md) | Workflow IR 公共契约与版本化 | Accepted | M8 |
| [DEC-020](../decisions/DEC-020-workflow-run-lifecycle.md) | WorkflowRun 生命周期、Task 状态映射与执行策略 | Accepted | M8 |
| [DEC-021](../decisions/DEC-021-workflow-tool-channel.md) | Workflow 操作的 Tool 通道表达 | Accepted | M8 |
| [DEC-022](../decisions/DEC-022-conversation-patch-semantics.md) | 对话 patch 语义与 Conversation 工件 | Accepted | M8 |
| [DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md) | 执行策略全集运行时语义与检查点（阶段 C） | Accepted | M10 |
| [DEC-024](../decisions/DEC-024-conversation-patch-execution.md) | 对话 patch 执行语义与决策点交互（阶段 C） | Accepted | M10 |
| [DEC-025](../decisions/DEC-025-success-trajectory-compilation-and-publish-gate.md) | 成功轨迹契约、Workflow 编译与入库门禁（阶段 D） | Accepted | M11 |
| [DEC-026](../decisions/DEC-026-task-induction-and-parameterization.md) | 任务归纳与参数化提议（阶段 D） | Accepted | M11 |
| [DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md) | App Model 契约与置信度（阶段 E） | Accepted | M12 |
| [DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md) | Navigation Planner 与 Navigate 步骤解析（阶段 E） | Accepted | M12 |
| [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md) | Memory 四类组织与 Workflow 学习契约（阶段 F） | Accepted | M13 |
| [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md) | 学习闭环运行时语义（阶段 F） | Accepted | M13 |
| [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md) | Agent Harness 恢复编排运行时语义（阶段 F 后续） | Accepted | M14 |
| [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) | Context Intelligence 分层上下文管理（Issue #39；Reduce/Retrieve/Rerank/Consolidate/Compress，Hot/Warm/Cold） | Accepted（方向；Stage A 基线由 [M16](m16-context-intelligence-stage-a.md) 交付，Stage B Layer 1 由 [M17](m17-context-intelligence-stage-b.md) 交付，Stage C Layer 2 重排由 [M18](m18-context-intelligence-stage-c.md) 交付，Stage D Layer 3 固化由 [M19](m19-context-intelligence-stage-d.md) 承载，Layer 4 未开始） | M16（Stage A）；M17（Stage B）；M18（Stage C）；M19（Stage D）；Stage E–F 逐阶段另行立项 |
| [DEC-035](../decisions/DEC-035-context-curator-working-context.md) | Context Curator 与 Working Context（Issue #48；`WorkingContextSnapshot` 状态投影 + `IContextCurator` 方向，Stage W1–W5） | Accepted（方向；Stage W1 由 [M20](m20-working-context-stage-w1.md) 交付，W2–W5 逐阶段另行立项） | M20（Stage W1，已完成）；W2–W5 逐阶段另行立项 |
| [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md) | Android 混合视觉 Grounding 管线（Issue #25；统一区域契约、事件驱动调度、许可约束） | Accepted（方向；实现受 DEC-011 证据门禁约束） | 保持 `MNT-202609-27` 证据门禁（DEC-042 不解锁；随证据另行立项） |
| [DEC-034](../decisions/DEC-034-minimal-eval-profile.md) | 离散动作与 Workflow 最小评估 Profile v1（四臂对照、17 case、跑前冻结口径与阈值） | Accepted（规范冻结；harness 实现归 `MNT-202609-29`） | 阶段 F 后续（`MNT-202609-28` 产出） |
| [DEC-036](../decisions/DEC-036-consolidation-model-supply.md) | 语义固化与 Working Context 的模型供给口径（可用源模型，不要求专用小模型；部分修订 DEC-032/DEC-035） | Accepted | M17–M20（历史证据保留，不追溯改写） |
| [DEC-037](../decisions/DEC-037-temporal-policy.md) | Temporal Policy——高频条件策略的经验固化方向（Issue #50；统一 Policy 抽象，Stage T1–T6） | Accepted（方向；Stage T1 契约与门禁由 [M26](m26-temporal-policy-stage-t1.md) 立项冻结（`In Progress`）；T2/T3/T6 受 DEC-011 门禁） | M26（Stage T1）；T2–T6 逐阶段另行立项 |
| [DEC-038](../decisions/DEC-038-unified-behavior-trace.md) | 统一 Behavior Trace——执行轨迹的三层语义投影（Issue #55/#56；L0 事件/L1 语义行为/L2 narrative，承接 DEC-026 §4 轨迹抽取非目标） | Accepted（方向；实现未开始） | 首阶段另行立项 |
| [DEC-039](../decisions/DEC-039-mcp-tool-module-admission.md) | MCP 工具模组准入——部署时注册的外部 Tool 来源（Issue #56；部分修订 DEC-009 备选方案第 5 条） | Accepted（方向；实现未开始） | M7 重定义已由 [DEC-042](../decisions/DEC-042-m7-scope-redefinition.md) 完成；实现随 M7 的 MCP 阶段（TM0–TM2 之后）立项 |
| [DEC-040](../decisions/DEC-040-tool-reference-and-skill-layer.md) | Tool 稳定引用、兼容状态与 Skill 层级（Issue #55/#56；引用钉住/跟随、`Runnable/Degraded/Invalid` 投影、Skill=暴露为 Tool 的 Workflow） | Accepted（方向；首阶段 TR0 稳定引用与兼容投影已由 [M7](m7-tools-evaluation-platform-v1.md) TR0 交付关闭（引用语法 v1 随该阶段冻结）；TR1 Skill 生命周期随后续阶段立项） | M7 模组体系落地后随其后阶段另行立项（[DEC-042](../decisions/DEC-042-m7-scope-redefinition.md)） |
| [DEC-041](../decisions/DEC-041-session-world-state-projection.md) | 会话 World State 投影——Runtime 当前环境认知的共享表示（Issue #55；纯函数更新、事件确定性重建、消费者只读） | Accepted（方向；实现未开始） | 首阶段另行立项 |
| [DEC-042](../decisions/DEC-042-m7-scope-redefinition.md) | M7 范围重定义——Tool 模组体系分阶段落地（`MNT-202609-30` 交付物；原 `M7-01`–`M7-28` 迁移映射，推迟项保持 DEC-011 证据门禁） | Accepted | M7（TM0 起） |
| [DEC-043](../decisions/DEC-043-architecture-policy-and-baseline.md) | 机器可检查的架构策略、基线与契约四件套交付标准（`MNT-202609-35`–`38`；policy 单一来源 + CI 门禁 + 基线渐进治理 + 术语表 + 阶段冻结协议） | Accepted | 维护轮（2026-09 第五轮起持续生效） |
| [DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md) | 多 Agent 工作流场景与 Subagent 上下文隔离边界（Issue #48；Stage W5：subagent = Agent Harness 控制平面内父会话旁的子 Session、fork = 子会话基线 + schema 1.2 溯源、机械确定性 merge policy、W3/W4 共存边界；Workflow fork 保持 DEC-019 扩展位） | Accepted（方向；实现由 [M24](m24-context-curator-stage-w5.md) 承载） | M24（Stage W5） |
| [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md) | Agent Loop 的 Working Context 快照供给缝与宿主编排边界（宿主集成轮：可选供给依赖 + `build_request` 经 Layer 0 转换注入已提交快照（身份对齐/有界/降级）；Loop 零自动化，W3/W4/W5 宿主显式编排；自动 fork-merge/自动晋升/模型介导合并须各自上位决策） | Accepted（实现已由 [M25](m25-host-integration-round.md) 交付关闭，PR #69） | M25（宿主集成轮） |

“Accepted”表示架构方向已生效，不表示对应实现工作项已经完成。具体实现仍由里程碑复选框和
验证记录证明。DEC-006 与 DEC-009 的架构方向保留；其落地里程碑（M5、M7）分别被 DEC-011
终止与挂起，落地范围待 demo 证据重定义。

## 6. 通用质量门禁

- [ ] 所有公开头文件可由最小外部 consumer 独立包含和链接。
- [ ] 状态转换、取消、shutdown、背压、提交拒绝、异常和不确定副作用有自动化测试。
- [ ] 每个 Provider/Adapter 通过共同 contract test，capability 与实际行为一致。
- [ ] OfflineReplay 不执行网络、Tool 或输入副作用。
- [ ] ASAN/UBSAN 常规运行；TSAN 在支持环境运行，不能运行时保留未完成门禁和补跑条件。
- [ ] 敏感字段、截图、UI Tree、Memory、模型请求和训练导出通过脱敏与权限负向测试。
- [ ] Android 真机或受支持模拟环境覆盖旋转、前后台、权限撤销、宿主销毁和输入释放。
- [ ] benchmark 记录硬件、OS、构建、Executor/模型配置、样本量和百分位。
- [ ] 依赖锁定、许可证和 SBOM 可重复生成。
- [ ] 文档、决策、计划状态和验证记录与实现同步。
- [ ] 架构策略检查（`tools/check_architecture.py`）与平台边界检查通过；跨模块
  依赖差异已按 [DEC-043](../decisions/DEC-043-architecture-policy-and-baseline.md)
  登记或走决策记录。

## 7. 计划维护规则

- 总计划只更新里程碑状态、依赖、范围和通用门禁；实施细节写入阶段文件。
- 新增里程碑或改变关键路径时，更新受影响的决策、设计和阶段前置条件。
- 里程碑只能在全部工作项和退出条件完成、且验证记录可复现后改为 `Completed`。
- 若某项被拆到后续阶段，原项保持未完成，除非范围变更经决策记录批准并明确迁移编号。
