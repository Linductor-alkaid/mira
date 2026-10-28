# M33：主动行为通道与打断预算

> 状态：Planned（2026-10-28 立项并跑前冻结；实现未开始）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-072](../decisions/DEC-072-autonomy-and-interruption-budget.md)；
> 来源为 Mirage [持续性数字个体路线图](https://github.com/Linductor-alkaid/mirage)
> §4.6/§6.1 R2/§8.3，**未**进入
> [Issue #111](https://github.com/Linductor-alkaid/mira/issues/111)）
> 前置：DEC-072 方向冻结（已满足——随本立项以 DEC-072 冻结）；
> [M32](m32-attention-focus-and-safe-switch.md) 关闭（**未满足**——
> 自主目标必须经同一注意力竞争队列）；
> [M30](m30-long-term-memory-integration.md) 关闭（**未满足**——
> 承诺实体由 M30 交付，DEC-070 §4 是自主提醒的唯一触发源）；
> DEC-069 容量预算的数值档位（**未满足**——自主行为的状态实体计入其预算）
> 建议发布点：非发布物；部署档位与主动交互契约的冻结锚点
> 更新日期：2026-10-28（立项并跑前冻结）

## 1. 目标

交付**自主行为的机制层**：让宿主能声明「Mira 想做这件事 / Mira 想说这句话」，
而**不必**自己实现预算记账、节流、权限判定、优先级仲裁、审计与恢复纪律。

具体交付一个可独立验收的能力增量：

1. **独立的第二输入源**：自主意图走与用户输入**分离**的接纳通道，
   但共用同一套协调层纪律（预算、租约、注意力竞争、审计、恢复、失败表达）。
2. **行为意图契约**：`AutonomousIntent` 的闭集字段与语义；**目标生成仍是宿主策略**。
3. **权限三分类**：允许 / 需确认 / 禁止，在 Core 侧**同源**判定；
   「需确认」可挂起等待授权；「禁止」无论用户是否在线都拒绝。
4. **打断预算（回复意愿）**：九道确定性闸门——类别开关、全局开关、权限判定、
   最小间隔、合并窗口、每类别频率上限、每日/每小时配额、静默期、冷却。
5. **用户否决是强制不变量**：「不要再…」后该类行为被 Core 拒绝。
6. **预算权威计数**：跨重启可靠、日界与时区显式、预留—结算、拒绝与豁免可审计。
7. **主动输出面**：不依赖「当前模型请求」的、带归属的输出通道。
8. **恢复语义**：崩溃后未完成的自主任务**不自动恢复**。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/autonomy_contracts.hpp`（新文件）与 `src/autonomy/`（新目录），
  落在 `core` 模块 roots 内，**policy 零差异**。
- 自主意图的接纳入口与权限三分类判定。
- 打断预算闸门与预算账本（含按类别禁用状态与反馈计数）。
- 自主行为的注意力接入（复用 M32 的评估队列与抢占协议）。
- 主动输出面（带归属的文本/结构化输出）。
- 自主事件族（动机产生、目标选择、预算消耗、拒绝、豁免、合并、冷却、否决、禁用）。
- 自主行为的恢复语义与周期检查的 Executor 承载。

### 2.2 非目标

- **不做**动机系统、价值评估、目标选择、「该不该说话」的策略：全部属宿主
  （DEC-072 §9/MOD2）。
- **不做** Core 侧的语音、动画、气泡：表现层属宿主且**单向**
  （DEC-072 §8/Y3）。
- **不做** `autonomy.*` 的 UI、持久化与默认值呈现：属宿主。
- **不做**「用户是否忽略了通知」的信号采集：属宿主可见性
  （DEC-072 §4/W4）；Core 只提供计数与判定。
- **不做**执行级续跑与自主任务的副作用续跑：沿用 DEC-071 §5/CP3 的排除。
- **不做**存储容量准入与归档清理：归 [M31](m31-capacity-admission-and-archival-recovery.md)。
- **不新建**调度器、重试线程、任务状态机或第二套预算体系。

## 3. 设计与决策依据

- [DEC-072](../decisions/DEC-072-autonomy-and-interruption-budget.md)——本里程碑的
  **主决策**：通道分离、`AutonomousIntent`、权限三分类、九道打断预算闸门、
  预算计数、审计、恢复、主动输出面、模块归属。
- [DEC-071](../decisions/DEC-071-attention-focus-and-safe-switch.md)——
  自主目标进入**同一条**注意力竞争队列；`Background` 只在焦点为空时执行；
  `High` 抢占仍受可中断性与授权约束。
- [DEC-070](../decisions/DEC-070-long-term-memory-behavior-contract.md)——
  承诺实体是主动提醒的唯一触发源；承诺不可靠检索维持。
- [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)——
  自主行为的状态实体计入其总量预算并受保护集合约束；
  周期清扫的 Executor 承载方式（`submit_delayed` 重装链）。
- [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)——
  输出阶段与归属语义；协调层是 owner。
- [安全与权限契约](../design/mira_runtime_design.md) 与
  [`include/mira/security.hpp`](../../include/mira/security.hpp)——
  `PolicyEngine`、`ConfirmationAuthority`、`PrincipalContext`。

## 4. 冻结的契约语义（跑前冻结，2026-10-28）

### 4.1 公开类型

```cpp
// include/mira/autonomy_contracts.hpp
enum class IntentSource : std::uint8_t {
    UserExplicit, SelfGenerated, Scheduled, EnvironmentTrigger
};

enum class AutonomyClass : std::uint8_t {
    Reminder,          // 主动提醒
    BackgroundLearning,// 后台学习
    PredictivePrep,    // 预测性准备
    IdleConsolidation, // 空闲记忆整理
};

enum class AutonomyPermission : std::uint8_t {
    Allowed,      // 只读无副作用
    NeedsConsent, // 有副作用，需用户确认
    Forbidden,    // 高风险，无论用户是否在线
};

struct AutonomousIntent final {
    std::string intent_id;              // 稳定标识 = 幂等键
    IntentSource source = IntentSource::SelfGenerated;
    AutonomyClass autonomy_class = AutonomyClass::Reminder;
    TaskOrigin origin = TaskOrigin::Autonomous;
    TaskPriority priority = TaskPriority::Background;
    std::string goal;
    std::vector<std::string> required_capabilities;
    std::optional<Timestamp> earliest_start;
    std::optional<Timestamp> deadline;
    std::optional<std::string> promise_id;   // 关联 DEC-070 §4 的承诺实体
    double expected_value = 0.0;             // 宿主估计，Core 不解释
    double risk_level = 0.0;                 // 宿主估计，Core 不解释
};

struct InterruptionGate final {
    bool class_enabled = false;             // 类别开关
    bool autonomy_enabled = false;          // 全局开关（默认关闭）
    std::chrono::seconds min_interval{0};   // 两次自主发言最小间隔
    std::chrono::seconds coalesce_window{0};// 合并窗口
    std::size_t max_per_class_per_day = 0;  // 每类别频率上限
    std::size_t max_tasks_per_day = 0;      // 每日任务配额
    std::size_t max_model_calls_per_hour = 0;// 每小时模型调用配额
    std::chrono::seconds cooldown{0};       // 被忽略/否决后的冷却
};
```

- `expected_value` / `risk_level` 由**宿主**估计；Core **不解释**这两项数值。
- 门禁配置由宿主提供，Core **强制执行**。

### 4.2 九道闸门（顺序固定）

| # | 闸门 | 拒绝/处理语义 |
| --- | --- | --- |
| 1 | 全局开关 | 关闭 → 拒绝（`autonomy_enabled` 默认 `false`） |
| 2 | 类别开关 | 关闭 → 拒绝 |
| 3 | 权限判定 | `Forbidden` → 拒绝；`NeedsConsent` → **挂起等待授权**（不消耗预算、不持 lease） |
| 4 | 静默期 | 命中 → 拒绝；**豁免须经授权路径且记录**；屏障类事件不可静默 |
| 5 | 冷却 | 命中 → 拒绝（附剩余冷却时间） |
| 6 | 最小间隔 | 距上次自主发言不足 → 拒绝（可配置为并入合并窗口） |
| 7 | 合并窗口 | 窗口内同类 → **合并**为一条，不新增计数 |
| 8 | 每类别频率上限 | 超限 → 拒绝 |
| 9 | 每日/每小时配额 | 超限 → 拒绝（可经豁免，豁免带理由并审计） |

- 判定是**纯函数**：给定（配置、账本、行为历史、当前时间）→ 唯一结论 + 理由。
- **不得**由模型自由裁量是否打扰用户。
- 反馈（称赞/忽略/否决）**只影响频率与优先级**，**不得**提升权限分类。

### 4.3 预算账本

- 单一 owner（Core），**跨进程重启可靠**。
- 「每日」的**日界与时区**必须显式声明，不得依赖未声明的本地时区。
- 预留—结算模式（沿 `BudgetLedger`）：执行前预留、执行后结算；
  未结算预留**保留**，不当零消耗清掉。
- 拒绝与豁免产生可审计事件；豁免**必须**带理由。
- 预算冻结稿带 `budget_version`。

### 4.4 主动输出面

```cpp
struct ProactiveOutput final {
    std::string intent_id;          // 归属到自主意图
    std::optional<std::string> promise_id;
    InputStage stage = InputStage::Accepted;  // 复用 DEC-068 的阶段语义
    std::string body;
};
```

- 输出面**不依赖**「当前模型请求」；与模型回复通道**分离**。
- 带归属（`intent_id` / `promise_id`），**不**冒认任何用户输入。
- 与用户任务的事件与失败**可区分**（宿主 UI 需要据此呈现不同状态）。

### 4.5 事件面

| 事件类型 | schema 名 | 载荷要点 |
| --- | --- | --- |
| `AutonomyIntentAccepted` | `mira.autonomy.intent-accepted.v1` | `intent_id`、`autonomy_class`、`source`、`priority` |
| `AutonomyIntentRejected` | `mira.autonomy.intent-rejected.v1` | `intent_id`、`gate`、`reason` |
| `AutonomyBudgetConsumed` | `mira.autonomy.budget-consumed.v1` | `intent_id`、消耗项、剩余量 |
| `AutonomyBudgetOverride` | `mira.autonomy.budget-override.v1` | `intent_id`、`rationale`（**必须**有） |
| `AutonomyOutputEmitted` | `mira.autonomy.output-emitted.v1` | `intent_id`、`promise_id`、`stage` |
| `AutonomyClassDisabled` | `mira.autonomy.class-disabled.v1` | `autonomy_class`、原因、来源（用户否决） |

- 版本标记写在 schema 名内；**不得**依赖 `EventEnvelope::schema_version`。
- 载荷**不得**含凭据或敏感正文。

### 4.6 Executor 路由与生命周期（Q7）

| 类别工作 | Executor 路径 | 完成语义 |
| --- | --- | --- |
| 意图接纳、闸门判定 | 同步确定性判定 | 直接返回 |
| 预算预留—结算 | 协调层同步账本（持久化经既有存储通道） | durable ack |
| 周期到期检查 | `submit_delayed` **重装链**（保证不重叠与可取消） | 下一趟重装 |
| 自主任务执行 | **既有** `submit_task` 与同一 Task 状态机 | `CommandOutcome` |
| 事件广播 | 既有事件分发通道 | 事件落盘 |

- **不新建 Executor**、**不新建定时器**、**不新建调度器**；
  自主调度**只有一个** owner（DEC-072 §7/V3-V4）。
- 周期 tick 派发被拒时**丢当前 tick 且不重试**（kairo 既有语义），
  收敛由「下一趟再来」承担。

### 4.7 决策记录表

| 决策点 | 选择 | 被否备选 | 依据 |
| --- | --- | --- | --- |
| 通道 | 独立的 `AutonomousIntent` 接纳通道 | 复用 `InputEnvelope` | DEC-072 §1/A1 |
| 下游纪律 | 与用户任务共用预算/租约/注意力/审计/恢复 | 自主走后门路径 | DEC-072 §1/A2 |
| 优先级 | 复用 DEC-071 的 `origin` × `priority` | 自建 `GoalPriority` 枚举 | DEC-071 §2 |
| 权限 | Core 同源三分类；禁止类一律拒绝 | 宿主自行分类 | DEC-072 §3 |
| 意愿 | Core 提供九道确定性闸门；宿主提供策略 | 模型裁量 / 宿主自律 | DEC-072 §4 |
| 否决 | Core 强制禁用的持久状态 | 宿主自律约定 | DEC-072 §4/W3 |
| 反馈 | 只影响频率与优先级 | 影响权限分类 | DEC-072 §4/W5 |
| 预算 | Core 单一 owner、跨重启、日界时区显式、预留—结算 | 宿主内存计数 | DEC-072 §5 |
| 恢复 | 未完成自主任务不自动恢复 | 与用户任务同恢复策略 | DEC-072 §7/V1 |
| 输出 | 独立于模型请求的带归属输出面 | 复用气泡通道 | DEC-072 §8 |
| 归属 | Core 机制层（headless），策略归宿主 | Core 实现动机系统 | DEC-072 §9 |

## 5. 阶段冻结协议必答八问（2026-10-28）

| # | 问题 | 回答 |
| --- | --- | --- |
| 1 | **行为**：本阶段改变或新增哪些行为？对应设计文档哪些冻结小节？ | 新增自主意图接纳、权限三分类、九道打断预算闸门、预算账本、强制否决状态、主动输出面、自主事件族、恢复语义。锚点：DEC-072 §1–§9；[核心契约设计 §5、§9](../design/core_contracts_and_state_machine.md) |
| 2 | **所有者**：新增可变状态的唯一所有者是谁？谁只读投影？ | 唯一 writer：协调层的自主子系统（预算账本、按类别禁用状态、反馈计数、承诺关联）。只读投影：`InterruptionGate` 配置、事件面。承诺实体由 M30 的记忆子系统持有；自主子系统**只引用** `promise_id` |
| 3 | **契约**：最小公共契约面是什么？哪些调用方允许使用？ | `include/mira/autonomy_contracts.hpp`：`IntentSource`/`AutonomyClass`/`AutonomyPermission`/`AutonomousIntent`/`InterruptionGate`/`ProactiveOutput` + 接纳入口 + 闸门查询 + 禁用/否决入口。允许使用者：宿主（策略实现方） |
| 4 | **层与依赖**：新文件属于哪个模块？include 方向是否已登记？ | `include/mira/autonomy_contracts.hpp` 与 `src/autonomy/` 落在 `core` roots 内 → **policy 零差异**；只依赖 `core` + `kairo` 公开头 |
| 5 | **复用**：哪条现有路径已承担部分职责？为何不新增平行路径？ | 复用：`MiraRuntime` 命令面与 Task 状态机、`PolicyEngine`/`ConfirmationAuthority`、`BudgetLedger` 的预留—结算形态、DEC-071 的注意力队列与抢占协议、DEC-068 的阶段与归属语义、M30 的承诺实体、EventStore 事实源。**不新增**：第二套任务状态机、第二个预算体系、后台调度器、重试线程 |
| 6 | **时序**：事件顺序、幂等键、过期结果规则、取消与 deadline 边界是什么？ | 事件顺序：`AutonomyIntentAccepted` → （闸门拒绝）`AutonomyIntentRejected` / （执行）`AutonomyBudgetConsumed` → （发言）`AutonomyOutputEmitted`；豁免 → `AutonomyBudgetOverride`。幂等键：`intent_id`。过期规则：同一 `intent_id` 在窗口内重复 → 返回既有记录，不重复执行。取消边界：自主任务取消沿既有 Task 取消语义；**需确认**类的挂起等待**不**持 lease、不消耗预算 |
| 7 | **Executor 路由**：每类工作由 Executor 哪种能力承载？句柄谁持有？关闭顺序如何？ | 见 §4.6。句柄由协调层持有并消费；`shutdown(true)` 由非 worker 线程执行 |
| 8 | **验证上下文**：评审者最小需要读哪些契约、spec 与测试才能验收？ | 契约：DEC-072 §1–§9、本文件 §4；设计：核心契约 §5/§9、Runtime §11；测试：`tests/m33/`（`M33-G1`–`M33-G9`），label `integration;m33` |

## 6. 测试与退出条件

### 6.1 门禁矩阵

| 门禁 | 内容 | 测试文件（`tests/m33/`） |
| --- | --- | --- |
| `M33-G1` | 通道分离：自主意图不走用户输入路径；自主任务经**同一条** `submit_task` 命令面 | `m33_channel_separation_test.cpp` |
| `M33-G2` | 权限三分类：三类各有用例；禁止类在用户在线与离线时**均**被拒；需确认类可挂起且不消耗预算 | `m33_permission_test.cpp` |
| `M33-G3` | 九道闸门逐个断言，每道拒绝有**确定理由** | `m33_interruption_budget_test.cpp` |
| `M33-G4` | 强制否决：用户否决后该类被 Core 拒绝；计数跨重启可靠；反馈不提升权限分类 | `m33_interruption_budget_test.cpp` |
| `M33-G5` | 预算计数：跨重启不丢；日界与时区显式；预留—结算；拒绝与豁免可审计 | `m33_budget_ledger_test.cpp` |
| `M33-G6` | 不绕过注意力：同一评估队列；Background 只在焦点为空时执行；High 抢占仍受可中断性与授权约束 | `m33_attention_integration_test.cpp` |
| `M33-G7` | 恢复语义：重启后未完成自主任务不自动恢复；承诺可恢复并重新评估 | `m33_recovery_test.cpp` |
| `M33-G8` | 主动输出面：不依赖模型请求；带归属；与用户任务事件可区分 | `m33_proactive_output_test.cpp` |
| `M33-G9` | 架构门禁与契约四件套同步；无自建线程/定时器/调度器 | CI quality 管线 |

### 6.2 退出条件

- [ ] `M33-01`–`M33-09` 全部工作项完成并勾选。
- [ ] `M33-G1`–`M33-G9` 全部通过，测试文件与 label 就位。
- [ ] CI 24/24 全绿（linux gcc/clang × Debug/Release、windows Debug/Release、
      android arm64/x86_64 NDK 交叉、sanitizers ASAN/TSAN/UBSAN、quality）。
- [ ] 公开 API、事件 schema、错误语义与示例已同步更新。
- [ ] 受影响的设计、决策、计划、术语表、API 手册已同步更新。
- [ ] 验证记录（§9）含 commit、环境、命令、结果与限制。

### 6.3 未声明的量

**打扰接受率、提醒关闭率、负反馈到禁用的收敛速度**等体验指标**不在**本阶段门禁内：
它们依赖真实用户与真实模型，按 issue #111 的要求留待标注场景与真实模型评估轮。
本阶段只声明**闸门与预算的管线行为**（RULE-10）。

## 7. 工作项

- [ ] `M33-01` 立项与跑前冻结：DEC-072 冻结面、本文件 §4 契约语义、
      §5 必答八问、§4.7 决策记录表、§6 门禁矩阵。
- [ ] `M33-02` 公开契约面：`include/mira/autonomy_contracts.hpp`
      （`IntentSource`/`AutonomyClass`/`AutonomyPermission`/`AutonomousIntent`/
      `InterruptionGate`/`ProactiveOutput`）、`mira.autonomy` 错误域与 wire schema 键集冻结。
- [ ] `M33-03` 自主意图接纳与权限三分类：接纳入口、三分类判定、
      「需确认」挂起等待授权的中断点、「禁止」一律拒绝。
- [ ] `M33-04` 打断预算闸门：九道闸门的确定性判定与理由；
      合并窗口的合并语义；静默期的豁免路径与记录。
- [ ] `M33-05` 预算账本与强制否决：跨重启可靠计数、日界与时区、预留—结算、
      拒绝与豁免审计、按类别禁用状态、反馈计数（不影响权限分类）。
- [ ] `M33-06` 注意力接入与执行路径：自主目标进入 M32 的评估队列；
      自主任务经既有 `submit_task`；执行级续跑**不**实现。
- [ ] `M33-07` 主动输出面：独立输出通道、归属、阶段语义、与用户任务事件可区分。
- [ ] `M33-08` 恢复与周期承载：未完成自主任务不自动恢复；
      承诺可恢复；周期检查走 `submit_delayed` 重装链。
- [ ] `M33-09` IVA 测试矩阵：`tests/m33/` 七组用例（§6.1），label `integration;m33`。
- [ ] `M33-10` 文档同步：API 手册新页、术语表、总计划索引、事件面说明、
      DEC-072 状态回填。

## 8. 风险与阻塞

| 风险 | 说明 | 缓解 / 归属 |
| --- | --- | --- |
| `M33-R1` 自主行为成为后门 | 绕过协调层纪律的执行路径 | A2 共用全部下游纪律；I3 最终仍是普通 Task；`M33-G1` 断言同一条命令面。M33-06 |
| `M33-R2` 打扰预算被绕过 | 某一闸门被跳过 | W1 要求全闸门确定性判定；`M33-G3` 逐闸门断言。M33-04 |
| `M33-R3` 计数与容量预算混淆 | 两套预算语义重叠 | B6 明确执行预算 vs 容量预算；自主状态实体计入 DEC-069 预算。M33-05 |
| `M33-R4` 静默期压制安全事件 | 用户静默导致屏障类事件被吞 | W2 明确屏障不可静默、豁免须授权并记录；`M33-G3` 断言。M33-04 |
| `M33-R5` 无用户在场时产生副作用 | 自主行为在用户不在时执行高风险动作 | AU2/AU3 三分类；V1 不自动恢复；需确认类在无可确认者时不得自动放行。M33-03 |
| `M33-R6` 依赖未兑现 | M30/M32 未关闭时本里程碑不可实施 | 前置已显式登记；不得抢跑 |

**外部阻塞**：M30（承诺实体）与 M32（注意力队列）**未关闭**是硬前置。
真实模型语义与用户体验指标仍需真实模型轮与真实用户取证，**不被本立项解锁**。

## 9. 验证记录

> 尚无记录。实现开始后按时间追加：日期、commit、环境、命令、结果、限制与剩余项。
> 未执行项必须保持未勾选并记录原因、负责人与补跑条件。
