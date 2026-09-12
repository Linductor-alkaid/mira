# 离散动作与 Workflow 最小评估 Profile

> 状态：Active（profile 内容已由 [DEC-034](../decisions/DEC-034-minimal-eval-profile.md) 冻结）
> 版本：1.0
> 更新日期：2026-09-12
> 负责人：Mira Maintainers
> 适用范围：当前离散动作 Agent 闭环、Workflow 双平面与恢复编排的任务级评估
> 实现工作项：`MNT-202609-29`（实现、运行与 soak；本 profile 是其实现规范）

## 1. 文档目的与效力

本 profile 是[评估与基准体系设计](evaluation_and_benchmark_design.md)在当前已交付能力上的
第一个具体化实例：固定 case 集、fixture 与 digest 规则、失败分类、重复样本方法与预算、
四组对照臂、指标来源映射和跑前冻结的阈值。`MNT-202609-29` 依此实现 harness 并产出首轮
基线；任何指标口径、case 成员或阈值的变更须修订本文件并同步 DEC-034，已登记基线不回溯
改写。

本文中的"必须""不得"对评估 harness 与报告构成强制约束。代码事实以合入源码为准；本文
引用的公开 API 均已在 M3/M8–M14 交付，缺口在第 13 节如实列举并声明承载工作项。

## 2. 背景、目标与非目标

### 2.1 背景

阶段 F 后续审计（`MNT-202609-21`）确认：单元/集成/consumer 测试与 M4 benchmark 存在，
但缺 Workflow 任务级统一评估与学习增益对照。[DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)
§5 的恢复预算默认值、`max_lessons_in_context` 等全部标注暂定（`RULE-10`），等待评估基线
校准；[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 的 Stage A
long-session 基线也依赖本 profile 先行冻结口径。`MNT-202609-28` 据此立项（验收即本文件
第 3–10 节内容）。

### 2.2 目标

- 在 Simulator + recorded Provider 的确定性环境中，用公共 API 驱动完整闭环，建立
  「直接 Agent / Strict Workflow / 恢复编排」的任务成功率、恢复率、成本与延迟基线。
- 以启用/禁用 learning context 的配对对照，产出学习闭环收益的首份有据对照（报告 paired
  delta，不预设结论）。
- 所有硬门禁与预算护栏在首轮运行前冻结（第 9 节），首轮正式运行固化基线 digest。

### 2.3 非目标（显式排除）

- **不沿用已取消里程碑的准入要求**：M5（本地感知与 ONNX）已 Cancelled，本 profile 不含
  OCR CER/WER、detector mAP/recall、state F1/ECE/OOD 等模型级指标；M6（实时控制）已
  Cancelled，不含控制回调 jitter、deadline miss、bounded-latency 门禁。连续动作
  （swipe/drag/joystick 轨迹）与视觉感知管线不在 case 集内。
- 不做真实平台/设备组（归 `MNT-202609-27`）、不做 live Provider 正式结论（live canary
  由 29 单独 manifest 单独报告，不进回归门禁）。
- 不实现 harness 本身、不校准 DEC-031 默认值（两者归 `MNT-202609-29`；本文件只冻结口径
  与阈值规则）。
- 不重开 M7 范围（归 `MNT-202609-30`），不新增 ToolModule/OOP 评估维度。

## 3. 四组对照（评估臂）

四臂共享同一 Simulator 环境装置、同一 recorded Provider 脚本集与同一 oracle 实现；臂间
只改变驱动面与 learning 配置，其余 manifest 字段尽量固定（评估设计 §8 的 A/B 纪律）。

| 臂 | 标识 | 驱动面（公开 API） | learning 配置 | 语义 |
| --- | --- | --- | --- | --- |
| A | `agent-direct` | `AgentLoop::run` + `MiraRuntime` 会话/任务 | 无（不装 `IMemory`） | 离散动作直接 Agent 闭环基线 |
| B | `workflow-strict` | `WorkflowRuntime`，policy=`Strict` | 无 | 无编排的 Workflow 失败语义基线 |
| C | `recovery-no-memory` | policy=`Recoverable` + `WorkflowRecoveryOrchestrator` | 不调用 `set_learning_context` | 恢复编排对照臂（no-memory） |
| D | `recovery-with-memory` | 同 C | 安装 `SqliteMemoryStore` learning context | 恢复编排 + 学习闭环 |

代码事实的如实声明（伪对照防范）：

- **A 臂不消费 lesson**：`AgentLoop` 实现不读取 `relevant_lessons`，lesson 检索与采纳只
  存在于恢复编排器（DEC-031 §1 明确不并入 `AgentLoop`）。因此本 profile 不设
  「直接 Agent + lesson」组合；A 臂的职责是给离散动作闭环建立无学习基线。
- **C 臂的「禁用 lesson」= 不安装 learning context**：这是当前唯一公开途径，语义是
  Episode/Lesson 记录与检索同时关闭（`WorkflowRuntime` 无 learning context 时全部学习
  路径为 NoOp）。细粒度开关（检索开、注入关）不存在于公开 API；若 29 的数据表明需要，
  按新增公开契约流程另行立项，本 profile 不预承诺。
- **C 与 D 的唯一差异**必须是 learning context 的有无；provider 脚本、workflow 定义
  digest、预算配置逐字段一致，manifest 记录两组配置 digest 以供核验。

## 4. EvalCase 集

### 4.1 case 规范与 digest 规则

每个 case 的规范内容为：目标/任务描述、环境 fixture（`SimulatorSetup` 选择与初始组件
状态）、注入脚本（工具失败序号、provider 决策脚本、store 故障旋钮）、oracle 断言与预算。
harness 实现时（29）将 case 集序列化为 canonical JSON 并计算 SHA-256 作为
`dataset_digest` 写入 RunManifest（对齐评估设计 §3/§7；canonical JSON 规则沿用
`workflow_definition_digest` 惯例）。首轮正式运行的 `dataset_digest` 连同本文件内容
digest 一并登记进基线记录；此后 case 成员或断言的任何变更产生新 digest 并保留变更说明。

provider 脚本按臂分 schema：A 臂决策用 `agent_decision_schema()`（M3 离散动作契约），
C/D 臂恢复决策用 `mira.workflow.recovery-decision.v1`（DEC-031 §3）。脚本 fixture 与
case 一同入 `dataset_digest`。

### 4.2 家族 R：离散动作基础闭环（臂 A）

| case | 任务与注入 | oracle 断言要点 |
| --- | --- | --- |
| `EVP1-R1` | 单步 tap 达成目标 | 终态 `Completed`；`executed_inputs()` 恰一条预期输入 |
| `EVP1-R2` | 多步导航 + 文本输入 + 滚动后验证（dual display fixture） | 终态完成；输入序列与预期序列逐一匹配；步数不超预算 |
| `EVP1-R3` | 目标在初始观察中已达成 | 零新输入派发即完成；不产生多余模型调用之外的副作用 |
| `EVP1-R4` | 目标不可达（环境中无目标元素） | 诚实 `Failed`；不无限重试；无重复副作用 |
| `EVP1-R5` | 目标需超预算步数 | `MaxSteps` 终态；预算耗尽路径闭合 |
| `EVP1-R6` | 中途注入用户消息修正目标（DEC-016 队列） | 消息进入后续请求上下文；按修正目标完成 |

A 臂 oracle 必须由宿主提供独立 `ILoopVerifier` 实现（对环境终态与输入序列的确定性
predicate）；不得使用 `ModelDoneVerifier`（它只信模型自报，违反评估设计 §6）。

### 4.3 家族 W：Workflow 失败与恢复语义（臂 B/C/D）

| case | 任务与注入 | oracle 断言要点 |
| --- | --- | --- |
| `EVP1-W1` | 纯 ToolCall 步链成功（`CountingTool` 无失败） | 终态 `Completed`；步 Disposition 全 `Completed` |
| `EVP1-W2` | 第 k 次 ToolCall 确定性失败（`ScriptedTool` 按序号） | B 臂：`Failed` 且无重复副作用；C/D 臂：升级 `WaitingAgent` 后被编排处置 |
| `EVP1-W3` | 同 W2，Recoverable + 恢复决策 `patch_and_resume`（skip patch） | 终态 `Completed`；`WorkflowRecoveryAttempted` 审计链完整（request→decision→patch→resume） |
| `EVP1-W4` | 修复无效：第一次 patch 后 Verify 步仍否决，第二次决策 `need_user` 或预算上交 | `DeferredToHost` 后 Run 保持 `WaitingAgent`；预算计数正确；无终态复活 |

### 4.4 家族 L：学习传递对照（臂 C/D，对照核心）

| case | 任务与注入 | oracle 断言要点 |
| --- | --- | --- |
| `EVP1-L1` | 学习阶段 run A：同 W3 签名失败→恢复→完成→宿主 `record_recovery_lesson`；评估阶段 run B：同签名再次失败升级 | D 臂：run B 检索命中 A 的 Episode+Lesson（`lessons_offered>0`、`kept>0`）且完成；C 臂：检索为空（`offered=0`）走无 lesson 修复路径。两臂其余配置逐字段一致 |
| `EVP1-L2` | lesson 版本漂移：库中 lesson 的 `ir_digest` 与当前 Run 不一致 | `lessons_stale>0`、`kept=0`；不误用失效 lesson；终态语义不因 stale 改变 |

L1 是双 run case（学习阶段 + 评估阶段共享同一 store），计为 2 个 run 单元。学习阶段只在
D 臂执行；C 臂的 L1 直接以空库进入评估阶段。

### 4.5 家族 F：故障与生命周期（跨臂注入）

| case | 注入 | oracle 断言要点 | 适用臂 |
| --- | --- | --- | --- |
| `EVP1-F1` | 模型请求在途取消（recorded provider 阻塞门 + 取消） | 协作取消收敛；终态幂等；无迟到响应副作用 | A、C、D |
| `EVP1-F2` | shutdown：在途请求/异步驱动时按序关闭全栈 | 关闭序列按第 11 节顺序；drain 有界；报告 clean；无新增输入派发 | A、B、C、D |
| `EVP1-F3` | 模型故障脚本：超时、拒答、决策格式错各一轮 | 预算内 `decision-invalid`/修复回合路径正确；预算耗尽出口闭合 | A、C、D |
| `EVP1-F4` | 慢（200 ms）/失败 memory store 旋钮 | 检索失败降级空结果不阻塞升级；审计 outcome=failed；取消/shutdown 闭合 | C、D |
| `EVP1-F5` | Takeover：恢复请求在途时 `request_human_takeover` | 恢复尝试 `Aborted`（`takeover`）；无新增自主动作；恢复前重新观察 | C、D |

### 4.6 臂适用矩阵与运行规模

| 家族 | A | B | C | D |
| --- | --- | --- | --- | --- |
| R（6） | ✓ | — | — | — |
| W（4） | — | ✓ | ✓ | ✓ |
| L（2） | — | — | ✓ | ✓ |
| F（5） | F1–F3 | F2 | 全部 | 全部 |

适用即纳入：A 臂 9 case、B 臂 5、C/D 臂各 11。重复方法见第 7 节（每 case 固定 seed 集
{0,1,2} 各 1 run + seed 0 追加 2 次重复），单轮总 run 数 ≈ 190（L1 双 run 已计入），
recorded 模式单机串行可在小时级完成。臂与家族的乘积组合不全部展开：不适用的组合（如
A 臂跑 W 家族）不产生 run，避免无意义样本稀释分层报告。

## 5. Oracle 与判定

判定优先级遵循评估设计 §6：Simulator ground truth（`WorkflowRunView::state` 终态、
`LoopOutcome`、`executed_inputs()` 回读、工具 dispatch 计数）→ 独立确定性 predicate
（本 profile 各 case 的断言表）→ 事件链审计（关联键重建）。被评系统的模型输出不得作为
成功判据；recorded 模式下没有人工评审层。

任务成功的必要条件（全臂一致）：终态为完成语义（`Completed` / `LoopOutcome::Completed`）
**且** oracle 对环境终态与输入序列的断言全部通过。仅终态完成而断言失败记
`oracle-failed`，不算成功。

## 6. 失败分类（闭集）

每个失败 run 归入唯一主类，写入结果记录；类成员闭集，新增须修订本文件：

| 分类 | 判定规则 |
| --- | --- |
| `oracle-failed` | 系统收敛但目标未达成或断言失败；报告层再依事件链归因到 Runtime/决策质量 |
| `runtime-defect` | Mira 公共契约违例：不变量破坏、终态复活、取消/shutdown 后新增动作、异常被吞 |
| `provider-script-mismatch` | recorded 脚本与实际请求序列不匹配（预期序列偏差，harness 装置问题） |
| `decision-invalid` | 模型决策不可解析或校验拒绝（预算内的预期失败路径） |
| `budget-exhausted` | 第 9.2 节任一预算护栏触发 |
| `fixture-defect` | 环境装置/装配错误（fixture 自身故障，非被评系统行为） |
| `infrastructure` | 运行环境故障（构建、进程、磁盘、计时器异常） |

`decision-invalid` 与 `budget-exhausted` 可能同时出现，取触发护栏的直接原因为主类。
`runtime-defect` 一律升级为阻断项（见 9.1 G2）。

## 7. 重复样本方法与预算

- **seed 与重复**：每 case 固定 seed 集 {0,1,2}（seed 驱动环境初始扰动，如 density 与
  组件顺序）；每 seed 1 run。seed 0 追加 2 次重复运行，用于 9.1 G4 的确定性断言。
- **执行顺序**：v1 全串行。延迟测量对并发敏感，串行排除干扰；学习传递（L1）有阶段
  顺序依赖。29 若引入并发以缩短 soak，须另立 manifest 并只用于吞吐类报告，不混入本
  profile 的延迟基线。
- **预算归属**：预算护栏按 run 计（见 9.2），总预算按轮计并在 manifest 冻结实际值；
  live canary 轮单独预算，不与 recorded 轮混算。
- **统计口径**：比例报告 Wilson 95% 区间；延迟报告经验分位数（P50/P95/P99/max）。首轮
  每臂家族格子样本量 3–15，**不足以支撑总体成功率宣称**；报告必须按臂×家族分层给出
  计数与区间，不聚合成单一数字做能力声明。n<40 不做 bootstrap 区间，声明经验分位数
  的局限。

## 8. 指标定义与来源映射

| 指标 | 定义 | 来源（已核实） | 状态 |
| --- | --- | --- | --- |
| 任务成功率 | oracle 判定成功的 run 比例（分臂×家族） | harness oracle | 原料齐备 |
| 恢复率 | C/D 臂：发生升级的 run 中最终 `Completed` 的比例 | `WorkflowRecoveryAttempted` 序列 + `WorkflowRunSettled` | 原料齐备 |
| 模型调用数 | 每 run 模型请求计数 | 模型事件计数 / `ModelGateway` ledger | 原料齐备 |
| token | input/output/cached/reasoning，按 `UsageQuality` 标注 | `ModelUsage`（`ModelResponse::usage`） | 原料齐备 |
| 成本 | micro-USD 计价 | `BudgetLedger` + `PriceTable`（recorded 轮记虚拟价，live canary 记实际价表） | 原料齐备 |
| 尾延迟 | 步级（`WorkflowStepStarted`→`StepSettled`）、恢复级（`RecoveryAttempted` 序列）、run 级（`RunStarted`→`RunSettled`）、A 臂步级（`LoopSettled` 时间戳） | 事件 envelope 的 monotonic 时钟配对 | 原料齐备 |
| 人工介入 | `WorkflowRecoveryOutcome::DeferredToHost` 计数（`need_user`、决策无效、预算上交） | `WorkflowRecoveryAttempted.outcome` | 原料齐备；A/B 臂记 N/A |
| 重复副作用 | 重复外部输入派发计数（目标零） | `SimulatorEnvironment::executed_inputs()` 回读 + oracle 序列比较 | 原料齐备（模拟器边界） |
| 内存/句柄 | 稳态 RSS 与 fd 数、run 间增长 | harness 读 `/proc/self`（Linux） | **缺口**：运行时无计量；仅 Linux 报告，Windows/Android 不报告不宣称（第 13 节） |
| shutdown 时长 | 第 11 节关闭序列的墙钟耗时 | harness 以 `steady_clock` 包裹关闭序列 | **缺口**：运行时报告无耗时字段，由 harness 侧计量 |

token 与成本的计量口径以 `UsageQuality` 如实分层：recorded 脚本通常为 `Estimated` 或
`Missing`，报告不得把估算值标为精确值。

## 9. 阈值（跑前冻结）

本节全部规则在 `MNT-202609-29` 首轮正式运行之前冻结；首轮运行只固化基线，不回填修改
阈值。违反硬门禁即该轮失败；护栏超出即该 run 记 `budget-exhausted` 并中止该 run。

### 9.1 硬门禁（零容忍，每 case 每臂每次运行）

- **G1** 重复外部副作用 = 0（`executed_inputs()` 与预期序列比较）。
- **G2** 无 `runtime-defect`：终态不复活；取消/Takeover/shutdown 之后无新增输入派发；
  异常不被吞（结果与事件可见）。
- **G3** 被拒 patch（越权或校验失败）之后无任何副作用执行。
- **G4** recorded 确定性：同 manifest 同 seed 的重复运行，经规范化的指标与事件序列
  逐位一致。规范化规则：剥离时间戳；随机生成的 `Id128` 标识（`EventId`/`TaskId`/
  `WorkflowRunId`/`ModelRequestId`/`WorkflowPatchId` 等，`Id128::generate` 使用
  `std::random_device`）按首次出现顺序映射为位置序号；会话/任务/事件顺序号、
  `run_epoch`/`run_patch_epoch`、内容 digest 字段与全部载荷枚举/计数保留原值参与比较
  （Memory 侧 `MemoryId`/`MutationId` 为确定性派生，不参与映射）。任何差异为 harness
  或被评系统缺陷，不是噪声。
- **G5** 事件链完整：每 run 可按关联键（`run_id`/`task_id`/`model_request_id`/
  `patch_id`/`decision_digest`/recovery `ordinal`）重建全链路。
- **G6** 事件与日志无凭据、无 `rationale`、无 API key 类载荷（既有脱敏纪律的回归断言）。

### 9.2 绝对预算护栏

| 护栏 | 上限 | 依据 |
| --- | --- | --- |
| 每 run 模型调用 | ≤ 64 | `max_steps=16`×(1+恢复) 与 `max_attempts_per_run=8`×(1+修复 1) 之和再留余量 |
| 每 run token（input+output） | ≤ 2,000,000 | 防 `RISK-2026-052` 无效修复循环 |
| 每 run 墙钟（recorded） | ≤ 300 s | 含全部恢复 attempt 与 drain |
| 每 case×臂 组合成本（live canary） | ≤ 1,000,000 micro-USD | live 轮护栏；recorded 轮记虚拟价 |
| shutdown 序列墙钟 | ≤ 120 s | 有界 drain 的绝对上限 |

### 9.3 基线锚定与回归规则

- 首轮正式运行 = 基线轮：登记 manifest digest、`dataset_digest` 与全部分层指标；基线
  记录按[现有 benchmark 文档格式](../benchmarks/long-task-memory.md)落 `docs/benchmarks/`。
- recorded 回归轮：相对基线的任何确定性差异（指标值、事件序列）须归因登记；未归因差异
  即回归失败。时序类指标（P99 步级/恢复级/run 级、shutdown 时长）相对基线 >1.5× 须归因
  登记，>2× 判回归失败；RSS 稳态（同 seed 重复间）增长 >10% 须归因登记。
- 基线轮本身不设成功率/恢复率通过线：v1 的任务是建立可复现基线，避免无数据拍阈值
  （`RULE-10` 纪律）。

### 9.4 学习增益判定（D vs C）

- 报告配对 case 的 paired delta：成功率、恢复率、模型调用、token、尾延迟、人工介入，
  附分层计数与区间；L1 的检索命中计数（`lessons_offered/kept`）作为机制证据一并报告。
- **v1 无通过线、不宣称收益**：对齐 DEC-031「无对照证据前不宣称恢复率改善」与 29 验收
  「不能凭命中一条 lesson 宣称成功率或成本改善」。是否为后续轮设增益阈值，由 29 的
  分布数据与 DEC 流程决定。

## 10. RunManifest 与基线登记

- manifest 字段对齐评估设计 §3（revision、build、platform、executor/runtime 配置
  digest、provider profile digest、`dataset_digest`、seed 集、时间范围）；本 profile 追加
  臂标识与 learning 配置 digest。
- 基线与回归报告的机器可读原始数据存 CI storage/artifact，仓库内报告只含方法、环境、
  命令、摘要、区间与限制（评估设计 §10 纪律）。
- 每轮报告附失败样本的事件引用（受 retention 约束），失败按第 6 节闭集归类。

## 11. Executor 路由与关闭

- harness 全部经公共 API 驱动（`AgentLoop::run` / `WorkflowRuntime::execute_run`+
  `wait_run` / `attempt_recovery`），不自建线程池、不直接调内部函数（评估设计 §10）。
- v1 串行执行：评估 owner 在调用线程依次驱动 run；无并发 case 任务。29 的 soak 若引入
  并发，case 级任务用 `submit_auto` 且 future 由评估 owner 持有并消费，容量与拒绝语义
  转明确结果。
- 关闭顺序（每 case 后与整轮结束均适用）：停止 harness 生产者 →
  `WorkflowRecoveryOrchestrator::shutdown()` → `WorkflowRuntime::shutdown()` → store
  close → `MiraRuntime` stop → 非Worker 线程 `executor.shutdown(true)`。shutdown 时长
  指标（第 8 节）覆盖该完整序列。
- 本 profile 不新增 Executor 能力需求；实现中若确认缺口，按台账流程登记后才可有临时
  边界。

## 12. 安全与脱敏

- recorded 轮零凭据零网络：provider 脚本为本地 fixture；live canary 单独 manifest，
  凭据只经宿主环境注入，不进事件、日志与仓库。
- lesson 与恢复决策的脱敏纪律回归断言并入 G6：参数投影缺省无值、`rationale` 不入事件
  （DEC-031 §9）。
- 基线报告中的事件引用遵守既有 retention 与脱敏规则；本 profile 不引入新的用户数据
  路径。

## 13. 已知缺口（如实声明）

| 缺口 | 影响 | 承载 |
| --- | --- | --- |
| lesson 细粒度禁用开关不存在 | C 臂只能以不装 learning context 实现 no-memory 语义（记录与检索同关） | 如需细粒度对照，由 29 数据支持后按新增公开契约立项 |
| `AgentLoop` 不消费 `relevant_lessons` | 无「直接 Agent + lesson」组合；A 臂基线不含学习面 | DEC-031 既定分层，不因评估改变 |
| 运行时无内存/句柄计量 | RSS/fd 由 harness 读 `/proc`（仅 Linux）；Windows/Android 不报告 | `MNT-202609-29` harness 侧 |
| 运行时 shutdown 报告无耗时字段 | shutdown 时长由 harness 墙钟计量 | `MNT-202609-29` harness 侧 |
| 无统一 fault injection 公共设施 | F 家族注入复用 m13/m14 fixture 旋钮模式（`ScriptedTool`、provider 阻塞门、store 故障开关） | harness 复用现有测试旋钮，不新建框架 |
| DEC-031 §5 默认值未校准 | 预算护栏以现行默认值为界，不代表最优 | 29 产出分布数据后另行决策 |

## 14. 分阶段落地

| 阶段 | 工作项 | 交付 |
| --- | --- | --- |
| 冻结（本轮） | `MNT-202609-28` | 本文件 + DEC-034；阈值与口径跑前冻结 |
| 实现与基线 | `MNT-202609-29` | harness（公共 API 驱动）、recorded 基线轮、live canary 单独报告、soak、fault/cancel/Takeover/rejection/shutdown 全场景结果 |
| 消费 | `MNT-202609-30`、DEC-032 Stage A | M7 重定义提案引用基线；long-session token 趋势基线复用本 profile 的计量与 manifest 纪律 |

## 15. 关联文档

- [评估与基准体系设计](evaluation_and_benchmark_design.md)（本 profile 的上位框架）
- [DEC-034](../decisions/DEC-034-minimal-eval-profile.md)（本 profile 的冻结决策）
- [DEC-031](../decisions/DEC-031-agent-recovery-orchestration.md)（恢复编排语义与暂定
  默认值的校准依赖）
- [Workflow 恢复编排设计](workflow_recovery_orchestration_design.md)
- [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)（Episode/Lesson
  契约，L 家族与 C/D 臂对照的语义基础）
- [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)（Stage A 基线
  依赖本 profile）
- [阶段 F 后续计划](../plans/maintenance-2026-09-post-stage-f.md)（`MNT-202609-28/29/30`
  工作项定义）
- [长期记忆基准报告](../benchmarks/long-task-memory.md)（基线文档格式先例）
