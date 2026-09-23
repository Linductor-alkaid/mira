# M26：Temporal Policy Stage T1——条件策略契约与确定性 Runtime 最小闭环

> 状态：In Progress（2026-09-24 立项并跑前冻结（`M26-01` 交付）；实现按 §7 工作项
> 自 `M26-02` 起推进，门禁验证完成前工作项保持未勾选）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（[DEC-037](../decisions/DEC-037-temporal-policy.md)
> Stage T1；Temporal Policy 链的首个交付轮）
> 前置：DEC-037 方向冻结（已满足，2026-09-14）；[Temporal Policy 设计](../design/temporal_policy_design.md)
> §4/§6/§7/§11/§12/§13 存在（已满足，v0.1 方向级——正式契约以本文件 §4 为准，
> 草案偏差逐条注记）；常规授权（已满足——总计划 §4.1 第 7 条与 DEC-037 决策第
> 7 条「纯 Core 确定性阶段（T1/T4/T5）按常规授权立项」，与 M16–M25 各轮
> 「依设计与计划推进」同一授权模式；Stage E 被 `MNT-202609-27` 外部证据阻塞、
> 平台 Adapter 宿主接入须先补专项决策、DEC-038/DEC-041 首阶段须先做词表对齐，
> 经计划核对 T1 为唯一无外部依赖且准备面完整的入口）
> 建议发布点：非发布物；Stage T2/T3 真机感知与 T6 连续控制的契约与方法学锚点
> 更新日期：2026-09-24（立项并跑前冻结）

## 1. 目标

按 [DEC-037](../decisions/DEC-037-temporal-policy.md) 冻结的方向与
[Temporal Policy 设计](../design/temporal_policy_design.md) §13 T1 行，交付
条件策略的确定性最小闭环（纯 Core，无平台、无感知、无模型依赖）：

- **数据契约**：`TemporalHistory` 有界观测环（实体级、容量有界、可重建投影）、
  `TrackedEntity` 当前 tick 时序状态快照、`PolicyWorldView` 闭集标量事实视图。
- **规则契约**：`ReactiveRule` schema v1——闭集谓词条件语言、固定序 transitions、
  provenance source 引用（无裸规则，RULE-07）、版本化 wire schema
  `mira.policy.rule.v1`（DEC-002）。
- **Runtime 最小闭环**：`IPolicyRuntime` 接口 + `ReactivePolicyRuntime` 参考后端——
  宿主显式激活/停用、每 tick 有界确定性 `step()` 评估（激活门、固定匹配序、冲突
  fail-closed）、宿主显式归纳/采纳/测试/晋升/降级生命周期（证据晋升，RULE-10）。
- **事件面**：T1 子集九类版本化事件（`mira.policy.*.v1`）经宿主供给 sink 回调发射，
  失败与冲突对调用方可见。
- **闭环证明**（设计 §12 T1 口径）：冻结确定性模拟数据集上证明
  「重复事件 → 候选规则 → 测试晋升 → 后续出现无需 Agent 介入即可正确执行」；
  指标全部为管线行为指标（归纳/误触发/升级/恢复/Agent 调用数下降），确定性部分
  要求跨进程 digest 一致（沿用 M16–M20 口径）。

## 2. 范围与非目标

范围：`include/mira/temporal_policy.hpp`（单一新公共头）与
`src/temporal/temporal_policy.cpp`（入 `mira_core`；core 模块内，
`tools/architecture-policy.json` 零差异）；`tests/m26/` 契约/生命周期测试矩阵与
确定性闭环评估 harness（CMake label `integration;m26`）；文档同步——API 手册新页、
公共术语表词条、设计文档实现注记、README 能力表、总计划与 DEC-037 关联回填。

非目标（交叉点注记含在内，均不因本立项解锁）：

- **不做真机感知**：Temporal Cache、Motion Descriptor/DTW/phase 估计、Mirador 或
  任何感知侧供给接入归 Stage T2/T3，受 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)
  门禁（`MNT-202609-27` 证据通道）约束；T1 的实体/世界视图全部来自确定性模拟数据集。
- **不做连续控制注入**：`IInputProvider` 映射、轨迹生成、延迟/抖动/功耗/热与
  Takeover 实测归 Stage T6（同 DEC-011 门禁）；T1 产出的 `PolicyAction` 止于
  结构化标签，零平台副作用。
- **不做 HSM/Behavior Tree 后端与策略状态机**：`PolicyTransitioned` 事件与多后端
  选择归 Stage T5；T1 后端仅 Reactive Rule。
- **不做 Agent Rule Induction 全管线**（Stage T4）：消费 DEC-029 Episode 资产、
  EventStore 事实源桥接与 learning loop 纪律测试归 T4；T1 归纳输入为 harness 持有
  的确定性 episode 日志，provenance 以 `source_refs` + 重放重建证明（RULE-07）。
- **不建 Workflow 激活/停用通道**：设计 §8「Workflow 步骤可激活/停用策略」的
  Workflow/工具化表达**随其立项独立冻结**（沿 [M20](m20-working-context-stage-w1.md)
  §6「另行冻结」句式）；T1 激活面为宿主显式 API。T1 全程不触达
  [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md) 工具通道
  （无 `BuiltinToolRegistry`、无工具派发）；如未来经工具面表达，沿
  [DEC-021](../decisions/DEC-021-workflow-tool-channel.md) 纪律——工具标记不构成
  对下游动作的授权豁免。
- **规则产出不是授权来源**（RULE-09 延伸，DEC-037 决策 5）：`ReactiveRule` 触发
  不构成 [PolicyEngine](../../include/mira/security.hpp)（DEC-004）豁免——任何
  真实副作用仍经既有 capability/freshness/权限校验。T1 无真实副作用，本条为
  T2+/T6 集成的绑定约束，随集成阶段进入测试面。
- **术语边界（消歧注记）**：本阶段的 Temporal Policy 与既有
  `PolicyEngine`（安全授权决策，`include/mira/security.hpp:99`、
  [术语表](../project/glossary.md) L168）和 `WorkflowPolicy`（执行策略闭集，
  `include/mira/workflow_ir.hpp:17`）**无继承、替代或豁免关系**，三者是不同层
  的三个概念；glossary 新词条（Temporal Policy / ReactiveRule / PolicyRuntime）
  随 `M26-05` 文档同步落地（AGENTS.md：公共术语以 glossary 为准）。
- **不声明实时性**：延迟、抖动、调度、过载与实时性声明被 RULE-10 与 T6 门禁封死；
  本阶段全部门禁为确定性管线行为断言。
- **不写 EventStore**：T1 事件经宿主供给 sink 发射；`EventEnvelope` 需要
  Runtime/Session 帧与序列推进（`include/mira/event_store.hpp:48-60`），T1 为
  无帧独立组件，事件桥接归 T2+ Runtime 集成阶段。
- 无真实模型调用、无网络、无凭据、不修改任何既有公开契约（新表面为纯加法，
  DEC-002 版本化）。

## 3. 设计与决策依据

- [DEC-037](../decisions/DEC-037-temporal-policy.md)（方向、统一 Policy 抽象、
  感知边界、归纳纪律、Executor 强制、DEC-011 关系）——本文件 §4 是其验证方式
  条款指定的「Stage T1 立项时在里程碑文件内冻结正式契约」承载面
- [Temporal Policy 设计](../design/temporal_policy_design.md) §4（数据契约草案，
  本文件 §4 冻结正式形态）、§6（条件策略契约与执行语义、动作三层模型归属）、
  §7（归纳生命周期与 authority 纪律）、§10（Executor 路由表——生产形态）、
  §11（事件族）、§12（T1 闭环证明与指标分层）、§13（Stage T1–T6 门禁表）
- [DEC-002](../decisions/DEC-002-public-contract-versioning.md)（新公共契约版本化：
  schema 版本策略、加法演进、wire schema 命名）
- [DEC-004](../decisions/DEC-004-security-authority-confirmation.md)（PolicyEngine
  授权边界——规则触发的非豁免语义锚点）、[DEC-021](../decisions/DEC-021-workflow-tool-channel.md)
  （「工具标记不构成授权豁免」先例）、[DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)
  §影响与风险（对相邻决策「注记但不承诺公开契约」的先例）
- 评估与测试口径先例：M16–M20 评估 harness（冻结确定性数据集 + digest 锚定 +
  `--report` 跨进程字节一致）、[M24](m24-context-curator-stage-w5.md)/[M25](m25-host-integration-round.md)
  的 `mira_add_mNN_test` CMake 函数与 `integration;mNN` label、契约/评估双目标拆分
  （`tests/CMakeLists.txt:639-688`）
- `RULE-07`（派生投影可重建）、`RULE-08`（容量与预算上界）、`RULE-09`（外部内容
  不提升为授权）、`RULE-10`（指标口径诚实）、`RULE-02`（异步工作归 Executor）
- AGENTS.md Executor 强制约束第 3/4/6 条（future 必消费、不得用普通周期任务冒充
  实时控制、关闭顺序）

## 4. 冻结的契约语义（跑前冻结，2026-09-24）

本章为正式契约冻结面（DEC-002 意义上的公共契约；设计 v0.1 草案字段与本章不一致
处，以本章为准并在实现注记中回填设计文档）。代码片段为**规范接口**（冻结的签名
级语义），非实现。

### 4.1 TemporalHistory（有界观测环，RULE-07/RULE-08）

```cpp
struct TemporalHistoryOptions final {
    std::size_t capacity = 64;                     // RULE-08 文档化默认值
    [[nodiscard]] Result<void> validate() const;   // capacity > 0，否则 HistoryCapacityInvalid
};

struct TemporalHistoryEntry final {
    std::uint64_t sequence = 0;        // 环内严格单调递增（按实体独立计数）
    Timestamp sampled_at{};            // 数据集确定性时钟（runtime 不读系统时钟）
    double position_x = 0.0;
    double position_y = 0.0;
    std::string motion;                // 版本化动作标签；空 = 无动作
    double motion_phase = 0.0;         // 0..1
    std::string source_ref;            // 来源观测稳定引用（不复制载荷）
    std::string source_digest;         // 被引用载荷摘要；T1 恒空串（语义见下）
};

class TemporalHistory final {          // 值类型组件；runtime 累计器与重建投影共用
  public:
    explicit TemporalHistory(TemporalHistoryOptions options = {});
    [[nodiscard]] Result<void> append(TemporalHistoryEntry entry);
    //   sequence 必须 > 现有最大 sequence，否则 HistorySequenceNotAdvancing；
    //   超容量按 FIFO 逐出最旧。
    [[nodiscard]] std::span<const TemporalHistoryEntry> entries() const noexcept;
    [[nodiscard]] const TemporalHistoryEntry* latest() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] Sha256Digest digest() const;   // 对 entries 的 canonical JSON 摘要，跨进程一致
};
```

- 大体积原始帧不进入历史：只有稳定引用 + 摘要（Observation 引用纪律延伸）。
- **`source_digest` 的 T1 冻结语义**：恒为空串——T1 输入契约（`TrackedEntity`/
  `PolicyWorldView`，§4.2）不携带载荷 digest 字段，`step()` 累计与
  `rebuild_temporal_histories` 重放两条构造路径一致留空，`digest()`（覆盖该字
  段）的逐字节一致因此良定义（T1-G4）；「审计/去重」语义在 T1 不激活，感知侧
  载荷摘要随 T2+ 输入契约进入，届时该字段开始取值而条目 schema 不变（前向兼
  容，DEC-002 加法语义）。
- 历史是**派生投影**：从 episode 日志重放可逐字节重建（§4.6
  `rebuild_temporal_histories`；门禁 `T1-G4`）。

### 4.2 TrackedEntity 与世界视图（T1 局部输入契约）

```cpp
struct TrackedEntity final {
    std::string entity_key;            // 确定性稳定标识（模拟数据集分配）
    double position_x = 0.0;
    double position_y = 0.0;
    double velocity_x = 0.0;
    double velocity_y = 0.0;
    std::string motion;                // 空 = 无动作
    double motion_phase = 0.0;         // 0..1
    double motion_confidence = 0.0;    // 0..1
    std::string source_ref;            // 当前 tick 观测引用
    [[nodiscard]] Result<void> validate() const;
    //   entity_key 非空；phase/confidence ∈ [0,1]；越界 → EntityInvalid
};

struct PolicyFact final {              // 闭集标量世界事实（如 distance=2.7）
    std::string key;
    double value = 0.0;
};

struct PolicyWorldView final {         // 当前 tick 的确定性世界视图
    std::vector<TrackedEntity> entities;   // 顺序即匹配扫描序（确定性）
    std::vector<PolicyFact> facts;         // key 不得重复
    [[nodiscard]] Result<void> validate() const;   // WorldViewInvalid
};

struct PolicyTickContext final {
    std::uint64_t tick = 0;            // 每 runtime 实例严格单调递增
    Timestamp tick_time{};             // 数据集确定性时钟
};
```

- **草案偏差注记**：设计 §4.2 草案的 `TemporalHistory history` 内嵌成员不进入
  `TrackedEntity`——历史由 runtime 累计器唯一持有（§4.4），内嵌会使每个 tick 的
  世界视图复制 N 份历史（重复状态、破坏单一所有权，协议 §5 拒绝形态）。
- `PolicyWorldView` 是 **T1 局部契约**：DEC-041（Session World State 投影）实现
  未开始，T1 不消费、不预承诺其 `WorldState`；词表（`EntityRef`/`state_id`）对齐
  点随 DEC-038/DEC-041 首阶段冻结处理，T1 的 `entity_key` 为阶段局部标识。

### 4.3 ReactiveRule schema v1（闭集谓词，fail-closed）

```cpp
enum class ReactiveRuleStatus : std::uint8_t { Candidate, Testing, Runtime, Retired };

struct RuleCondition final {
    enum class Field : std::uint8_t { Motion, MotionPhase, MotionConfidence, Fact };
    enum class Compare : std::uint8_t { Eq, Lt, Le, Gt, Ge };
    Field field = Field::Motion;
    Compare compare = Compare::Eq;
    std::string fact_key;              // Field::Fact 专用
    std::string text_value;            // Eq 的字符串操作数（Motion）
    double number_value = 0.0;         // 数值比较操作数
    // 语义：Motion/MotionPhase/MotionConfidence 对锚实体求值；Fact 对世界事实求值
};

struct RuleTransition final {
    std::vector<RuleCondition> when;        // 非空合取，全部成立才匹配
    std::vector<PolicyAction> do_actions;   // 非空
};

struct PolicyAction final {            // 结构化动作标签（T1 零副作用）
    std::string kind;                  // 离散动作标签（如 "dodge"）
    std::string parameter;             // 可选参数（如 "left"）；空 = 无参
};

struct ReactiveRule final {
    std::string rule_id;               // "rr-" + 语义内容摘要前 16 hex（见下），全生命周期不变
    SchemaVersion schema_version{1, 0};
    std::string state;                 // 所属策略状态（宿主激活面按 state 门控）
    std::uint32_t priority = 0;        // 匹配序主键（升序），次键 rule_id 字典序
    ReactiveRuleStatus status = ReactiveRuleStatus::Candidate;
    std::vector<RuleTransition> transitions;   // 非空、数量有界（RULE-08）
    std::vector<std::string> source_refs;      // 归纳证据引用（排序去重；RULE-07 无裸规则）
    std::uint32_t support_count = 0;           // 重复验证证据计数（RULE-10）
    [[nodiscard]] Result<void> validate() const;   // RuleSchemaInvalid：未知枚举、
    //   空 when/do、条件数与 transition 数超 options 上界、source 引用缺失（Candidate 及以后）
    [[nodiscard]] std::string to_json() const;      // wire schema mira.policy.rule.v1，canonical JSON
    [[nodiscard]] static Result<ReactiveRule> from_json(std::string_view text);
    //   DEC-002 版本策略：{1,x} 当前读取、更老 major 拒绝、未知更新 major 拒绝
    [[nodiscard]] Sha256Digest digest() const;      // 语义内容（state+transitions）canonical 摘要
};
// rule_id = "rr-" + hex(digest(语义内容))[0..16]；语义等价的规则跨运行同 id（幂等采纳的基础）。
```

- 条件语言是**闭集**：T1 仅支持当前 tick 的实体字段等值/数值五比较与事实数值
  比较；自由表达式 DSL 与历史条件语言（"motion 持续 k tick"）为非目标（前者
  不可跑前冻结确定性语义，后者归 T3+）。
- schema 对全部五种 `Compare` 通用（测试可手编任意条件）；**参考归纳算法**只发射
  §4.4 归纳语义中的规范形式，两者不混淆。

### 4.4 ReactivePolicyRuntime（所有者、评估与生命周期）

```cpp
struct PolicyRuntimeOptions final {
    std::size_t max_rules = 256;                 // RULE-08
    std::size_t max_rules_per_state = 32;
    std::size_t max_conditions_per_transition = 8;
    std::size_t max_transitions_per_rule = 8;
    std::uint32_t min_support = 3;               // 归纳证据下限（RULE-10）
    std::uint32_t min_test_support = 2;          // 晋升测试通过下限
    TemporalHistoryOptions history_options{};    // per-entity 累计器构造参数（唯一来源）
    [[nodiscard]] Result<void> validate() const; // 0 值/下限为 0 → OptionsInvalid；
                                                 //   含 history_options.validate()
};

enum class PolicyEventType : std::uint8_t {
    PolicyActivated, PolicyDeactivated, RuleCandidateInduced, RuleTestingResulted,
    RulePromoted, RuleDemoted, RuleConflicted, RuleTriggered, PolicyEscalatedToAgent,
};                                               // T1 子集九类；PolicyTransitioned 归 T5
[[nodiscard]] std::string_view policy_event_schema_name(PolicyEventType type);
//   精确返回下方事件表所列九个 wire schema 名（逐一冻结，无推断项）。

struct PolicyEvent final {
    PolicyEventType type{};
    std::uint64_t tick = 0;
    std::string rule_id;               // 仅单规则事件携带目标规则 id；激活/停用/升级/冲突
                                       //   事件置空串（帧/载荷分工见下方冻结段）
    std::string payload_json;          // 该事件 schema 的 canonical JSON 载荷
};

class IPolicyEventSink {               // 宿主供给；同步内联调用
  public:
    virtual ~IPolicyEventSink() = default;
    virtual void on_policy_event(const PolicyEvent &event) = 0;
    // 契约：不得阻塞、不得抛出（T1 无 Observer 隔离层——异常隔离随 T2+ Runtime
    // 集成面冻结）；测试 sink 为 vector 追加。
};

struct PolicyEpisodeSample final {     // 归纳/测试输入（harness 持有，runtime 只读）
    PolicyTickContext tick{};
    PolicyWorldView world{};
    std::string active_state;          // 该 tick 的激活策略状态
    std::optional<PolicyAction> agent_action;   // Agent（T1 为脚本）采取的动作
    bool agent_handled = true;         // false = 升级/未处理（不参与归纳）
};

class IPolicyRuntime {                 // 后端中立最小面（T5 的 HSM/BT 后端加入此处）
  public:
    virtual ~IPolicyRuntime() = default;
    [[nodiscard]] virtual Result<void> activate(std::string_view state) = 0;
    [[nodiscard]] virtual Result<void> deactivate(std::string_view state) = 0;
    [[nodiscard]] virtual bool is_active(std::string_view state) const = 0;
    [[nodiscard]] virtual Result<std::vector<PolicyAction>>
    step(const PolicyWorldView &world, const PolicyTickContext &tick) = 0;
};

class ReactivePolicyRuntime final : public IPolicyRuntime {
  public:
    explicit ReactivePolicyRuntime(PolicyRuntimeOptions options = {},
                                   IPolicyEventSink *sink = nullptr);
    // IPolicyRuntime 全部实现 + 生命周期管理（宿主显式，无自动晋升）：
    [[nodiscard]] Result<std::vector<ReactiveRule>>
    induce_candidate_rules(std::span<const PolicyEpisodeSample> episodes);
    [[nodiscard]] Result<std::size_t>
    adopt_candidate_rules(std::span<const ReactiveRule> rules);   // 返回新采纳数
    [[nodiscard]] Result<RuleTestingReport>
    test_candidate_rules(std::span<const PolicyEpisodeSample> episodes);
    [[nodiscard]] Result<void> promote_rule(std::string_view rule_id);
    [[nodiscard]] Result<void> demote_rule(std::string_view rule_id, std::string_view reason);
    [[nodiscard]] Result<void> retire_rule(std::string_view rule_id);
    // 加性只读访问器（DEC-002 加法演进，§9 第六次记录）：观测 per-entity
    // step() 累计器；实体无累计历史时返回空指针——T1-G4 累计器 digest 腿与
    // T1-G1「step 路径 source_digest 恒空」断言的观测点。
    [[nodiscard]] const TemporalHistory *
    temporal_history(std::string_view entity_key) const noexcept;
};

struct RuleTestingReport final {       // 逐规则结果 + 事件已发射
    struct RuleResult final {
        std::string rule_id;
        std::uint32_t passed_count = 0;        // 触发且动作与记录一致
        std::uint32_t false_trigger_count = 0; // 触发但动作不符/无人处理
        std::uint32_t sample_count = 0;
    };
    std::vector<RuleResult> results;   // 按 rule_id 字典序
    [[nodiscard]] Sha256Digest digest() const;    // 跨进程一致
};

[[nodiscard]] Result<std::map<std::string, TemporalHistory>>
rebuild_temporal_histories(std::span<const PolicyEpisodeSample> episodes,
                           TemporalHistoryOptions options = {});
//   只读重建投影：按样本序重放各实体观测 → 与 runtime 累计器逐字节一致（T1-G4）。
```

**冻结的事件 schema 面（九类全部 DEC-002 v1；载荷为 canonical JSON，键集恰为
所列、不多不少）**：

| PolicyEventType | wire schema | 载荷键集（类型） |
| --- | --- | --- |
| `PolicyActivated` | `mira.policy.policy-activated.v1` | `state`（string） |
| `PolicyDeactivated` | `mira.policy.policy-deactivated.v1` | `state`（string） |
| `RuleCandidateInduced` | `mira.policy.rule-candidate-induced.v1` | `state`（string）、`support_count`（uint32）、`source_ref_count`（uint32） |
| `RuleTestingResulted` | `mira.policy.rule-testing-resulted.v1` | `passed_count`、`false_trigger_count`、`sample_count`（均 uint32） |
| `RulePromoted` | `mira.policy.rule-promoted.v1` | `state`（string）、`support_count`（uint32）、`passed_count`（uint32） |
| `RuleDemoted` | `mira.policy.rule-demoted.v1` | `from_status`（string，闭集 `Runtime`\|`Candidate`）、`to_status`（string，闭集 `Candidate`\|`Retired`）、`reason`（string，闭集 `conflict`\|`host-explicit`） |
| `RuleConflicted` | `mira.policy.rule-conflicted.v1` | `state`（string）、`matched_rule_ids`（string 数组，rule_id 字典序；帧 `rule_id` 置空） |
| `RuleTriggered` | `mira.policy.rule-triggered.v1` | `state`、`entity_key`（string）、`actions`（对象数组，每项恰 `kind`+`parameter`，string） |
| `PolicyEscalatedToAgent` | `mira.policy.policy-escalated-to-agent.v1` | `state`（string）、`reason`（string，T1 闭集仅 `conflict`）、`matched_rule_ids`（string 数组，字典序） |

- 帧/载荷分工：`type`/`tick`/`rule_id` 为 `PolicyEvent` 帧字段，**不重复写入载
  荷**（避免双写漂移）；载荷自含该类型的语义字段。帧字段取值逐一冻结：
  帧 `rule_id` 仅单规则事件携带目标规则 id（`RuleCandidateInduced`/
  `RuleTestingResulted`/`RulePromoted`/`RuleDemoted`/`RuleTriggered`），
  `PolicyActivated`/`PolicyDeactivated`/`PolicyEscalatedToAgent`/`RuleConflicted`
  置空串；帧 `tick` = 事件发射时 runtime 已见的最大 tick（step 内事件为当前
  tick，生命周期事件为最近一次成功 step 的 tick，本实例尚无成功 step 时为 0）。
- `reason`/`from_status`/`to_status` 闭集取值为 DEC-002 v1 稳定字符串；新增取值
  走 minor 升版，不静默扩集。

**冻结语义**：

- **所有者（Q2）**：规则集（含生命周期状态与最近测试证据）与激活状态集由
  `ReactivePolicyRuntime` 实例唯一持有；per-entity `TemporalHistory` 由该实例的
  累计器唯一持有；episode 日志由调用方持有、runtime 只读消费；无全局/静态可变
  状态，无第二写路径（采纳/晋升/降级/退役是规则状态仅有的变更入口）。
- **step() 评估序（Q6）**：(1) 校验 world/tick（`tick` 必须严格大于本实例既往
  最大值，否则 `TickNotMonotonic` 拒绝、零事件、零状态变更）；(2) 逐实体追加
  TemporalHistory（累计器恒以本实例 `PolicyRuntimeOptions::history_options` 构
  造——与 `rebuild_temporal_histories` 的对齐参数唯一；无激活状态也追加——审计
  与重建面完整）；(3) 对 status==Runtime
  且 `state` ∈ 激活集的规则按 (priority 升序, rule_id 字典序) 固定序收集全部匹配；
  匹配判定 = 全部 `Fact` 条件在世界事实上成立，**且**实体作用域条件满足：
  (a) 规则含实体作用域条件（Motion/MotionPhase/MotionConfidence）时，须存在锚
  实体 = `world.entities` 顺序中第一个满足全部实体作用域条件的实体——`entities`
  为空或无满足者 → 该规则不匹配；(b) 规则**仅含 `Fact` 条件**（无实体作用域条
  件）时，事实条件全部成立即匹配，无锚实体、`RuleTriggered` 载荷 `entity_key`
  置空串（键仍在，符合事件表键集）。零匹配
  → 空动作、零事件（非匹配是正常态）；恰一匹配 → 发射 `RuleTriggered`、返回
  `do_actions`；**多匹配 → 冲突 fail-closed**：零动作，发射 `RuleConflicted` +
  `PolicyEscalatedToAgent`，全部匹配规则降级 `Candidate`（各发射 `RuleDemoted`
  reason=conflict）——冲突必须显式可见并回到 Agent 控制（设计 §7.1）。
- **激活面**：`activate`/`deactivate` 幂等（重复激活 NoOp、成功返回）；未知空
  状态名拒绝（`RuleSchemaInvalid`）；停用立即生效、无需规则「同意」，
  不等待任何在途判定（设计 §9；T1 step 为同步有界函数，无在途动作可取消）。
- **归纳（参考算法，确定性、无拟合）**：仅 `agent_handled==true` 且存在锚实体
  （`world.entities` 顺序第一个 motion 非空者）的样本参与；签名 = (active_state,
  锚实体 motion, motion_phase 精确值, motion_confidence 精确值, 事实键值集按 key
  字典序, agent_action)；相同签名计数 ≥ `min_support` → 恰一候选规则：单一
  transition，`when` 按固定序 [Motion Eq 签名值, MotionPhase **Ge** 支持集最小
  值, MotionConfidence **Ge** 支持集最小值, 逐事实键 **Le** 支持集最大值（key
  字典序）]，`do_actions` = [签名 agent_action]，`source_refs` = 支持集锚实体
  source_ref 排序去重，`support_count` = 支持数。低于 `min_support` 不产出。
  同一日志重复归纳产出**逐字节相同**的候选集（幂等，T1-G4）。**规范形式全部取
  自覆盖比较**（Ge/Le/Eq）：支持集样本恒匹配自己参与归纳出的规则——精确签名
  下支持集相位单一，若 MotionPhase 取严格 Gt 最小值则规则对自己支持集零触发，
  晋升门与 T1-G5 不可满足；设计 §6.1 示例的严格 `> 0.55` 为手编规则示意，非归
  纳规范形式。

- **归纳可行性前提（数据集构造约束，冻结——T1-G4/G5 的可达性前提）**：T1 评估
  数据集为人工确定性构造，必须满足：(a) 每个待归纳模式在训练分部含 ≥
  `min_support` 个**签名完全相同**的样本（连续量精确重复由构造保证，非自然数据
  假设）；(b) 每样本事实键数 ≤ `max_conditions_per_transition - 3`（默认 8−3=
  5，3 为固定 Motion/MotionPhase/MotionConfidence 条件）——超界则归纳产物
  validate 失败、不可采纳（契约行为，harness 数据集不得触发）；(c) 测试分部每
  模式 ≥ `min_test_support` 个正例，正例相位/事实值落在支持集闭包内（≥ Ge 阈
  值，含至少一个严格大于阈值的非边界值）；(d) 规则未匹配的样本不产生任何计数
  与事件，其 `agent_handled`/`agent_action` 取值不参与判定（测试分母仅由触发
  样本构成，零误触发门不受非触发样本语义影响）。
- **采纳**：`adopt_candidate_rules` 逐条校验（validate + 上界）后加入规则集为
  `Candidate`；`rule_id` 已存在（任意状态）→ 幂等跳过；超 `max_rules`/
  `max_rules_per_state` → `BoundsExceeded` 整批拒绝、零变更（fail-closed）。
  `RuleCandidateInduced` 在**采纳点**发射（每条实际采纳的非重复候选恰一事件，
  幂等跳过不发）——`induce_candidate_rules` 为纯计算不发射（同一日志重复归纳
  不产生重复事件，事件审计以规则进入受管集为准）。
- **测试**：`test_candidate_rules` 对每条 `Candidate` 规则在样本上**隔离评估**
  （不与其他规则交互）：触发且 (`agent_handled` 且动作一致) → passed++；触发但
  动作不符或无人处理 → false_trigger++；逐规则发射 `RuleTestingResulted`；报告
  结果记为该规则「最近测试证据」。
- **晋升**：`promote_rule` 要求 status==Candidate 且最近测试证据存在、
  `false_trigger_count == 0` 且 `passed_count >= min_test_support`，否则
  `PromotionEvidenceMissing` fail-closed；通过 → `Runtime` + `RulePromoted`。
  无时间窗、无自动晋升（自动治理归 T5）。
- **降级/退役（迁移矩阵，逐一冻结）**：合法迁移仅以下三类，越界一律
  `RuleStateInvalid` fail-closed（终局规则集不变、零事件）：
  (a) `demote_rule(rule_id, reason)`，reason ∈ {`conflict`, `host-explicit`}（未
  知 reason → `RuleSchemaInvalid`）：**Runtime → Candidate**，发射 `RuleDemoted`
  （载荷 from_status=Runtime、to_status=Candidate、reason=同参数）；step 冲突路
  径的自动降级即 (a) 的 reason=conflict 形态（每条匹配规则一事件）。
  (b) `retire_rule(rule_id)`（无 reason 参数）：**Runtime 或 Candidate →
  Retired**（终态），发射 `RuleDemoted`（载荷 reason 固定 `host-explicit`、
  to_status=Retired、from_status=实际原状态）——与 (a) 到 Candidate 的降级分工：
  demote 是"回炉受测"，retire 是"移除出受管集"；对已 Retired 规则重复 retire
  → 幂等 NoOp（返回成功、零事件、零状态变更，终态幂等）。
  (c) `promote_rule`：Candidate → Runtime（证据门见上）。
  其余组合全部非法：对 Candidate 调 demote（无可降级面，移除走 retire）、对
  Retired 调 demote/promote、未知 rule_id → `RuleUnknown`。
- **事件纪律**：事件载荷 canonical JSON，九类 schema 名与逐类载荷键集已冻结
  （上方事件表，设计 §11 的 T1 子集）；载荷不含观测样本等大载荷——样本细节留
  在 episode 日志与规则 `source_refs`，事件只携带语义字段（引用纪律）；规则文
  本与样本不得含敏感内容（脱敏纪律延伸）；sink 在 step/生命周期调用的同步路径
  内联调用。
- **错误面**：沿既有 per-module 错误域先例冻结为 `mira.temporal_policy` 域 +
  `TemporalPolicyDomainCode` 十二码显式 int32 枚举 + 稳定名映射（完整三重映射
  见本节末「错误域冻结面」）；全部错误经既有 `Result`/`Error`（core_contracts）
  返回，不抛异常表达失败。

**错误域冻结面（三重映射逐一冻结，DEC-002 稳定公开值）**：

```cpp
// 域值沿全库 mira.* 点分惯例（核验：grep -rhn '\.domain = "' src 全部既有取值
// 均为 mira.* 形态、无一例外；Error::domain 默认 "mira"、domain_code 为 int32，
// core_contracts.hpp:193-200）。
enum class TemporalPolicyDomainCode : std::int32_t {
    OptionsInvalid = 1,               // options/history 容量等 validate 失败
    BoundsExceeded = 2,               // 规则集/每状态/条件数上界（RULE-08）
    EntityInvalid = 3,                // TrackedEntity 校验失败
    WorldViewInvalid = 4,             // PolicyWorldView 校验失败（重复键等）
    TickNotMonotonic = 5,             // tick 回归/重复
    RuleSchemaInvalid = 6,            // 规则 schema/闭集枚举/provenance 缺失
    RuleUnknown = 7,                  // 未知 rule_id
    RuleStateInvalid = 8,             // 非法生命周期迁移
    PromotionEvidenceMissing = 9,     // 晋升证据门 fail-closed
    EpisodeSamplesInvalid = 10,       // 空/越界批量输入
    HistorySequenceNotAdvancing = 11, // 历史序列未严格推进
    HistoryCapacityInvalid = 12,      // capacity == 0
};
[[nodiscard]] std::string temporal_policy_domain() noexcept;   // "mira.temporal_policy"
[[nodiscard]] std::string temporal_policy_domain_code_name(TemporalPolicyDomainCode code);
//   返回成员名稳定字符串（"OptionsInvalid" 等；context_domain_code_name
//   （context_contracts.hpp:268）同构先例）。
Error make_temporal_policy_error(TemporalPolicyDomainCode code, std::string safe_message,
                                 bool retryable = false);
//   domain = temporal_policy_domain()；domain_code = static_cast<std::int32_t>(code)
//   （make_context_error/make_memory_error 同构，context_contracts.cpp:624-639、
//   memory_contracts.cpp:487-495）；ErrorCode 确定性映射：
//   InvalidArgument ← {OptionsInvalid, EntityInvalid, WorldViewInvalid,
//   RuleSchemaInvalid, RuleUnknown, EpisodeSamplesInvalid, HistoryCapacityInvalid}；
//   InvalidState ← {BoundsExceeded, TickNotMonotonic, RuleStateInvalid,
//   PromotionEvidenceMissing, HistorySequenceNotAdvancing}。
```

枚举 int32 值、域字符串与名字函数输出均为 DEC-002 稳定公开值（事后改名/改值即
破坏性变更）；本文其余处的「错误码」均指上述枚举成员。

### 4.5 Executor 路由与生命周期（Q7，显式冻结）

- **T1 零注册循环**：`step`、归纳、测试、采纳、晋升全部是有界同步纯函数
  （工作量上界 = O(规则数 × 实体数 × 条件数)，由 validate 后的 options 封顶），
  由 harness/调用方同步驱动 tick——**不创建**任何线程、定时器、周期任务或
  realtime/低延迟 lane 注册。
- **设计 §10 生产路由的边界注记**：realtime lane（Reactive 层）与
  LowLatency/lockfree（Policy 层）是 T2+ 感知接入与 T6 连续控制的集成形态；
  Mira 侧 realtime lane 目前仅 M0 基线集成测试先例
  (`tests/integration/executor_lifecycle_test.cpp:160`)，无生产消费者。T1 强行
  注册 realtime lane 属无消费者的新集成面；以普通周期任务冒充高频控制被
  AGENTS.md 第 4 条禁止——故 T1 明确不触达任何 Executor 注册面。
- **编排纪律示范**：评估 harness 的归纳/测试/晋升编排任务经
  `executor.submit_auto()` 提交并**消费 future**（AGENTS.md 第 3 条；DEC-037
  决策 6 的「归纳/编译走 submit_auto」在 harness 侧落地），无 fire-and-forget。
- **句柄与关闭序**：runtime 无线程、无句柄、无独立关闭面；harness 持有
  executor 与 runtime，关闭序 = 停编排生产者 → 消费全部 future → 非 worker 线程
  `shutdown(true)`（AGENTS.md 第 6 条）。
- **时间源**：runtime 不读系统时钟；全部 `Timestamp` 来自数据集确定性时钟——
  跨机器/跨进程逐字节可复现的前提（T1-G4）。

### 4.6 决策记录表

| 决策点 | 选择 | 被否备选 | 依据 |
| --- | --- | --- | --- |
| 契约落点 | 单公共头 `include/mira/temporal_policy.hpp` + `src/temporal/temporal_policy.cpp`（core 模块内，零 policy 差异） | 新独立模块（无独立外部依赖诉求，扩面无据）；散入 context/model 既有头（跨域污染既有契约面） | 协议 §2 Q4；`tools/architecture-policy.json` core roots（本次核验） |
| 世界视图来源 | T1 局部 `PolicyWorldView`（实体 + 闭集标量事实） | 消费 DEC-041 `WorldState`（其实现未开始——投机依赖）；直接消费 Observation/感知输出（引入感知依赖，违反 T1 无感知边界） | 设计 §4.3 草案；DEC-041 状态（总计划 §5 表） |
| TrackedEntity 形态 | 当前 tick 快照，**不内嵌** history；per-entity 历史由 runtime 累计器唯一持有 | 按设计 §4.2 草案内嵌 `TemporalHistory`（每 tick 世界视图复制 N 份历史——重复状态、多写路径形态） | 协议 §5；设计「字段命名以届时契约为准」（§4 前注） |
| 条件语言 | 闭集谓词：实体字段（Motion/MotionPhase/MotionConfidence）+ 事实键 × {Eq,Lt,Le,Gt,Ge}，仅当前 tick | 自由表达式 DSL（解析面/注入面不可跑前冻结）；历史条件语言（"持续 k tick"，归 T3+） | 协议 §2 Q3；RULE-08；设计 §6.1 冻结正式 schema 的要求 |
| 归纳算法 | 精确签名重复计数 + **自覆盖**规范阈值形式（phase Ge min / confidence Ge min / fact Le max——支持集样本恒匹配自己参与归纳的规则；无拟合、无浮点推断） | MotionPhase 严格 Gt min（精确签名下支持集相位单一 → 规则对自身支持集零触发，晋升门与 T1-G5 不可满足）；统计/学习型归纳（跨进程不可复现，RULE-10 无证据门禁）；手编数据集特定阈值（不可迁移、非算法） | 设计 §12 T1 闭环与跨进程 digest 一致性；DEC-037 决策 5 |
| 生命周期矩阵与事件发射点 | demote=Runtime→Candidate（reason ∈ {conflict, host-explicit}）、retire=Runtime\|Candidate→Retired（载荷 reason 固定 host-explicit；终态幂等 NoOp）、`RuleCandidateInduced` 在采纳点发射（induce 纯计算不发射）、帧 `rule_id` 分型与帧 `tick` 取值逐一冻结 | demote 允许 Candidate 起点（无可降级面，移除归 retire）；reason 预留 testing-failed 死值（T1 无 Runtime 重测路径，不进 v1 闭集）；induce 点发射（重复归纳 → 重复事件） | §4.4 冻结语义与事件表；AGENTS.md 终态幂等；协议 §2 Q3/Q6 |
| 晋升纪律 | `min_support` 归纳下限 + 测试分部零误触发 + `min_test_support` 通过下限；宿主显式 promote | 单次验证即晋升（违反 RULE-10 与 DEC-037 决策 5「以重复验证证据为准」）；运行时自动晋升（归 T5 自动治理） | DEC-037 决策 5；设计 §7.1 |
| 冲突语义 | 同 tick ≥2 Runtime 规则匹配 → 零动作 fail-closed + `RuleConflicted`/`PolicyEscalatedToAgent` + 匹配规则全体降级 | 首匹配优先吞掉冲突（冲突不可见，违反设计 §7.1 显式暴露）；动作相同即放行（语义分叉、掩盖重叠前件） | 设计 §3 升级红线与 §7.1；协议 §5 拒绝形态 |
| Executor 路由 | T1 零注册循环：step/归纳/测试为有界同步函数，harness 同步驱动 tick；编排任务经 `submit_auto()` 且 future 必消费；realtime/低延迟 lane 为 T2+/T6 集成形态，T1 不注册 | T1 注册 realtime lane（Mira 侧仅 M0 测试先例 `executor_lifecycle_test.cpp:160`，无消费者的新集成面）；普通周期任务驱动 tick（AGENTS.md 第 4 条禁止的冒充形态） | DEC-037 决策 6；AGENTS.md Executor 强制；设计 §10；协议 §2 Q7 |
| 事件面 | T1 自有九类 `mira.policy.*.v1` 事件 + 宿主供给 sink | 写入 EventStore（`EventEnvelope` 需 Runtime/Session 帧与序列推进，`event_store.hpp:48-60`——T1 无该帧，桥接归 T2+）；无事件面（违反设计 §11 失败可见） | 设计 §11；`event_store.hpp` 结构（本次核验） |
| 交叉点处置 | 范围注记：非授权语义写入 §2、工具通道非目标、术语消歧随 glossary 词条落地 | 契约级预冻结（authority 字段/激活 wire schema——本阶段无消费方的提前承诺，违反 DEC-037 加法纪律）；静默不注记（Policy 三义碰撞进入公开头，PM 规范 §6.1 DoR 存在关键歧义） | DEC-037 §5 与非目标；DEC-015 §影响与风险「注记但不承诺」先例；DEC-021「工具标记不构成授权豁免」；M20 §6「另行冻结」句式 |
| 决策载体 | 不新建 DEC：本文件 §4/§5/§6 即 DEC-037 验证方式条款指定的 T1 正式契约冻结面；新增契约全部为该决策已登记的加法面（TemporalHistory/TrackedEntity/ReactiveRule/Policy Runtime） | 新建 DEC-046（方向、纪律、门禁均已被 DEC-037 冻结，重复登记无新裁决内容） | DEC-037 §验证方式（"Stage T1 立项时在里程碑文件内冻结正式契约"）；PM 规范 §8（无清单外新裁决） |

## 5. 阶段冻结协议必答八问（2026-09-24）

1. **行为**：新增确定性条件策略最小闭环——(a) `TemporalHistory` 有界环与重放
   重建投影；(b) `TrackedEntity`/`PolicyWorldView` 输入契约；(c) `ReactiveRule`
   schema v1（闭集谓词、provenance、版本化 wire）；(d) `IPolicyRuntime`/
   `ReactivePolicyRuntime` 的 step 评估（激活门、固定匹配序、冲突 fail-closed）；
   (e) 宿主显式归纳/采纳/测试/晋升/降级/退役生命周期；(f) T1 子集九类版本化
   事件。对应设计：[Temporal Policy 设计](../design/temporal_policy_design.md)
   §4（数据契约）、§6（条件策略与执行语义）、§7（生命周期与纪律）、§11（事件
   族）、§12（闭环证明）、§13 T1 行。
2. **所有者**：规则集（含生命周期状态与最近测试证据）、激活状态集与 per-entity
   `TemporalHistory` 累计器由 `ReactivePolicyRuntime` 实例唯一持有（§4.6 决策
   表）；episode 日志由调用方持有、runtime 只读；重建函数为只读投影；无全局
   可变状态、无第二写路径。
3. **契约**：最小公共契约面 = `include/mira/temporal_policy.hpp` 单头的 §4.1–
   §4.4 类型清单 + `mira.temporal_policy` 错误域（`TemporalPolicyDomainCode`
   十二码显式 int32 值 + 稳定名映射 + ErrorCode 确定性分配，§4.4「错误域冻结
   面」）+ wire schema `mira.policy.rule.v1` 与九类事件 schema 名及逐类载荷键集
   （§4.4 事件表，全部 DEC-002 版本化）；T1 允许的调用方 = 宿主与测试（Agent
   Loop/Workflow 消费者为后续阶段，§2 显式非目标）。契约四件套：公开头文件 +
   本文件 §4 + `docs/api/temporal-policy.md`（随 `M26-05` 交付）+ `tests/m26/`。
4. **层与依赖**：全部新文件入 core 模块（`include/mira` + `src/temporal/`，均在
   policy core roots 内）；头文件仅依赖既有 mira 公共头（`core_contracts.hpp`/
   `json.hpp`）与标准库；`tools/architecture-policy.json` **零差异**（core
   requires 不变；tests 模块既有 requires 覆盖测试所需）。
5. **复用**：`Result`/`Error`/`Timestamp`/`SchemaVersion` 惯例（core_contracts）、
   canonical JSON 与 SHA-256 摘要（`json.hpp:177-180`
   `canonical_json_string`/`canonical_json_digest`，与 WorkingContextSnapshot 同
   口径——不新增第二套序列化/哈希）、DEC-002 版本策略、M16–M20 评估口径（冻结
   数据集 digest 锚定 + `--report` 跨进程一致）、per-milestone CMake 测试函数
   先例（`tests/CMakeLists.txt:639-688`）。策略评估域无既有承担路径，不构成
   平行路径。
6. **时序**：tick 每 runtime 实例严格单调（回归拒绝 + 零事件）；规则匹配固定序
   (priority, rule_id)；多匹配冲突 fail-closed（零动作 + 事件 + 全体降级）；归纳
   与采纳幂等（同日志同 rule_id）；晋升仅以支持计数 + 最近测试证据为准（无时间
   窗）；停用立即生效不等规则同意（设计 §9）；runtime 不读系统时钟（时间源为数
   据集确定性时钟）。T1 无异步等待点，迟到结果/取消/超时语义不适用；该语义随
   T2+ Executor 化集成冻结（显式非目标，非本阶段可答项）。
7. **Executor 路由**：T1 零 Executor 注册面（§4.5）——全部工作为有界同步函数；
   harness 编排经 `submit_auto()` 且 future 必消费；句柄：runtime 无线程无句柄，
   harness 持有 executor 与 runtime；关闭序 = 停编排生产者 → 消费 future → 非
   worker 线程 `shutdown(true)`（AGENTS.md 第 6 条）。
8. **验证上下文**：评审者最小阅读集 = 本文件 §4、[DEC-037](../decisions/DEC-037-temporal-policy.md)、
   [Temporal Policy 设计](../design/temporal_policy_design.md) §4/§6/§7/§11/§12/§13、
   `include/mira/temporal_policy.hpp`、`tests/m26/m26_temporal_policy_test.cpp`、
   `tests/m26/m26_temporal_policy_eval.cpp`。

## 6. 测试与退出条件

门禁形态说明：本轮无模型调用、无平台依赖，门禁全部为确定性断言；测试的编写、
运行与 sanitizer 取证由 Independent-Verification-Agent 独立完成并复验。CMake
注册沿 M24/M25 先例新增 `mira_add_m26_test` 函数（link `Mira::core` +
`executor::executor`，label `integration;m26`，TIMEOUT 120）。

- [ ] `T1-G1` 契约与序列化（`tests/m26/m26_temporal_policy_test.cpp`，label
  `integration;m26`）：`TemporalHistory` 环语义——append 序列严格推进（回归 →
  `HistorySequenceNotAdvancing`）、容量逐出最旧、`digest()` 跨实例一致；
  `TrackedEntity`/`PolicyWorldView`/`PolicyRuntimeOptions`/`TemporalHistoryOptions`
  validate 全部拒绝路径；`ReactiveRule` fail-closed——空 when/do、未知枚举、超
  上界条件/transition 数拒绝，Candidate 无 source_refs 拒绝（无裸规则）；
  `mira.policy.rule.v1` canonical JSON 往返字节一致 + DEC-002 版本策略（{1,0}
  当前、{1,1} 读取、{0,x}/{2,x} 拒绝）；`mira.temporal_policy` 错误域——
  `TemporalPolicyDomainCode` 12 码每码至少一条断言（构造路径断言
  `domain == "mira.temporal_policy"`、`domain_code == static_cast<std::int32_t>(成员)`
  与 `temporal_policy_domain_code_name` 稳定名三重一致）；九类事件
  `policy_event_schema_name` 逐类精确名断言 + 每类构造一次的载荷键集恰为 §4.4
  事件表冻结清单（canonical 序列化跨进程字节一致）；`TemporalHistoryEntry::
  source_digest` 在两条构造路径（step 累计 / rebuild 重放）下恒空串断言。
- [ ] `T1-G2` step 评估语义（同文件）：未激活状态零评估零事件；tick 非单调拒绝
  且零状态变更零事件；匹配固定序（priority 升序、rule_id 字典序，含同 priority
  用例）；恰一匹配 → `RuleTriggered` + 动作集；多匹配 → 冲突 fail-closed（零动
  作 + `RuleConflicted` + `PolicyEscalatedToAgent` + 匹配规则全体降级 Candidate
  且事件在案）；activate/deactivate 幂等与立即生效（停用后同 tick 世界视图零匹
  配）；`max_rules`/`max_rules_per_state` 采纳上界整批拒绝零变更（RULE-08）；
  纯 `Fact` 条件规则（手编）匹配 → `RuleTriggered` 且载荷 `entity_key` 为空串
  （键在、符合事件表键集）；`entities` 为空时含实体作用域条件的规则零匹配零事
  件；帧 `rule_id` 分型与帧 `tick` 取值按 §4.4 帧/载荷分工逐类断言。
- [ ] `T1-G3` 归纳/测试/晋升生命周期（同文件；超行数预算时按 M24 先例拆分
  lifecycle TU 并在验证记录留痕）：锚实体选择确定性（实体序首个 motion 非空）；
  `min_support` 之下零候选；`agent_handled=false` 样本不参与归纳；候选规范阈值
  形式逐字段断言（自覆盖 Ge/Ge/Le + 支持集极值；支持集边界相位值触发断言）；
  同一日志重复归纳逐字节相同；
  `adopt` 幂等（同 rule_id 跳过）与上界 fail-closed；`test_candidate_rules`
  隔离评估（passed/false_trigger 计数、`RuleTestingResulted` 逐规则）；
  `promote_rule` 无证据/未达 `min_test_support`/误触发非零 →
  `PromotionEvidenceMissing` fail-closed；demote/retire 迁移矩阵逐格断言
  （§4.4：Runtime→Candidate demote、Runtime\|Candidate→Retired retire、越界
  `RuleStateInvalid` 且规则集不变零事件）、retire 终态幂等 NoOp（零事件）、
  `RuleCandidateInduced` 采纳点恰一发射且重复采纳不重发。
- [ ] `T1-G4` 重建与跨进程确定性（`tests/m26/m26_temporal_policy_eval.cpp`，
  label `integration;m26`）：冻结确定性数据集（digest 在 harness 内常量锚定并
  断言，场景 = 设计 §6.1 示例域：`heavy_slash_a` 相位过阈 + 距离过近 → dodge，
  含否定样本与保留测试分部）；`rebuild_temporal_histories` 与 runtime 累计器
  digest 逐字节一致（两者以同一 `PolicyRuntimeOptions::history_options` 构造，
  参数来自 runtime 实例）；完整管线重放（induce → adopt → test → promote）两遍 →
  规则集 digest、测试报告 digest 与 `--report` 全部字节一致；`--report` 跨进程/
  跨构建树字节一致（M16–M20 口径）。
- [ ] `T1-G5` 确定性闭环端到端（同文件，设计 §12 T1 证明）：训练分部脚本 Agent
  处理重复模式 → 归纳候选 → 测试晋升 → 重放分部同一模式由 Runtime 规则执行
  （`agent_handled=false`）且动作与 Agent 历史动作一致、`RuleTriggered` 事件在
  案（重放样本含支持集边界相位值——Ge 阈值自覆盖的直接断言；数据集满足 §4.4
  归纳可行性前提：精确重复签名、事实键数 ≤ 上界−3、测试分部正例分布）；
  Agent 调用次数在重放分部严格低于全 Agent 基线（经验下降曲线的最小断言
  形态）；冲突注入场景 → 升级事件 + 脚本 Agent 恢复处理 + 降级事件在案；全部
  指标为管线行为指标，报告不含任何实时性/语义质量声明（RULE-10）；harness 编排
  任务经 `executor.submit_auto` 托管且 future 被消费；runtime 全程零自建线程/
  定时器/注册循环（行为断言）。
- [ ] `T1-G6` 文档同步与契约四件套：API 手册新页 `docs/api/temporal-policy.md`
  + `docs/api/index.md` 模块地图行；术语表新词条（Temporal Policy / ReactiveRule /
  PolicyRuntime，与 PolicyEngine/WorkflowPolicy 消歧）；设计文档实现注记（草案 →
  正式契约偏差清单：TrackedEntity 不内嵌 history、`WorldState` →
  `PolicyWorldView`、`PolicyTransitioned` 归 T5、事件 T1 子集）；README 能力表
  行；[DEC-037](../decisions/DEC-037-temporal-policy.md) 关联回填；总计划同步。
- [ ] 本地门禁：debug 全量 ctest 全绿（含新增 m26 两目标）、ASAN/UBSAN/TSAN 新
  增目标零报告、`format-check`/`docs-check`/`platform-boundary-check`/
  `sbom-check`/`architecture-check` 通过、clang-tidy 预检被改库源编译单元
  （`temporal_policy.cpp` 等）零违例、本机 NDK 两 ABI 交叉编译预演通过；既有
  套件零回归（新表面纯加法，无既有契约改动）。
- [ ] PR CI（Linux/Windows/Android/sanitizers/quality）全绿后回填验证记录并
  关闭本阶段。

## 7. 工作项

- [ ] `M26-01` 阶段立项与本文件 §4/§5/§6/§7 冻结（时间戳先于任何实现与测试）。
- [ ] `M26-02` 契约与实现：`include/mira/temporal_policy.hpp` +
  `src/temporal/temporal_policy.cpp`（入 `mira_core`），按 §4 冻结语义实现；
  core 模块零 policy 差异；clang-tidy 预检零违例。
- [ ] `M26-03` 契约/生命周期测试矩阵（§6 `T1-G1`–`T1-G3`，`tests/m26/`，CMake
  注册 label `integration;m26`）——测试的编写、运行与 sanitizer 取证由
  Independent-Verification-Agent 独立完成并复验。
- [ ] `M26-04` 确定性闭环评估 harness（§6 `T1-G4`–`T1-G5`，
  `tests/m26/m26_temporal_policy_eval.cpp`，数据集 digest 锚定 + 跨进程一致）——
  同由 Independent-Verification-Agent 独立完成并复验。
- [ ] `M26-05` 文档同步（§6 `T1-G6` 清单）+ 本地全门禁与 PR CI 取证回填；
  全部工作项与门禁复核通过后本里程碑转 `Completed`。

## 8. 风险与阻塞

- 风险：草案 → 正式契约偏差引起设计/实现失配（设计 v0.1 明示「字段命名以届时
  契约为准」）。处置：§4 逐条冻结并在 `T1-G6` 以实现注记回填设计文档，偏差清单
  显式（history 所有权、`PolicyWorldView` 命名、事件子集）。
- 风险：T1 闭环被误读为实时性/端到端能力声明。处置：RULE-10 + §2 非目标 + 全部
  门禁为确定性管线行为断言；实时性声明被 T6 门禁（`MNT-202609-27`）封死。
- 风险：`ReactiveRule` 触发被误读为授权豁免或工具面能力。处置：§2 非目标双注记
  （DEC-004 边界 + DEC-015 通道不触达）；术语消歧随 glossary 落地。
- 风险：Executor 路由注记被误读为「生产路由降级」。处置：§4.5 显式区分 T1 边界
  与设计 §10 生产形态；realtime lane 的生产部署验证随 T2+/T6 立项冻结。
- 风险：归纳规范形式与数据集强耦合（阈值取支持集极值）。处置：schema 与归纳算
  法分离（§4.3 注记）；归纳算法为阶段冻结的参考实现，T4 的 Agent Rule Induction
  全管线可替换归纳策略而不破坏 schema；规范形式全部自覆盖（Ge/Le/Eq，支持集恒
  自匹配），数据集构造约束（精确重复签名、事实键数 ≤ 条件数上界−3、测试分部
  正例分布、非触发样本语义）已在 §4.4 冻结为 T1-G4/G5
  可达性前提，harness 作者无需自行反推隐含前提。
- 外部阻塞：无——不依赖真机、凭据、外部语料或真实模型（纯 Core 确定性闭环，
  DEC-037 决策 7 的常规授权形态）。

## 9. 验证记录

2026-09-24：`M26-01` 立项，本文件 §4/§5/§6/§7 冻结（先于任何实现与测试）。
立项依据：总计划 §4.1 第 7 条与 [DEC-037](../decisions/DEC-037-temporal-policy.md)
决策第 7 条的常规授权条款（Stage E 被 `MNT-202609-27` 外部证据阻塞、平台 Adapter
宿主接入无已冻结验收形态须先补专项决策、DEC-038/DEC-041 首阶段须先做词表对齐与
专项设计，经计划核对 T1 为下一可用入口；与 M21/M22 关闭记录点名的入口一致）。
工作分支 `feat-dec-037-stage-t1`。实现未开始；后续验证记录按日期追加。

立项前核验（本次实际执行，2026-09-24）：

- `grep -rn "TemporalHistory|TrackedEntity|ReactiveRule|IPolicyRuntime|PolicyActions|TemporalPolicy|temporal_policy|TemporalRule" src include tools tests --include=*.cpp --include=*.hpp --include=*.json` →
  零命中（退出码 1）——T1 契约面确无既有代码，属全新公共契约（DEC-002 版本化
  与 policy 登记适用）。
- `cat tools/architecture-policy.json` → core roots `["include/mira","src"]`、
  requires `["executor"]`；tests requires 已含全部外部头族——`include/mira/
  temporal_policy.hpp` + `src/temporal/` 落点零 policy 差异。
- `grep -n "register_realtime|push_realtime|RealtimeChannel|submit_auto|LowLatency"
  third_party/executor/include/executor/executor.hpp` → realtime API 存在于
  `executor.hpp:424`（`register_realtime_task`）/`:481`（`push_realtime_task`）；
  `grep -rn "register_realtime|push_realtime|RealtimeChannel" src include tests` →
  仅 `tests/integration/executor_lifecycle_test.cpp:160` 一处（M0 基线先例），
  生产源零使用——§4.5「T1 不注册 realtime lane」的边界注记依据。
- 术语三义核验：`grep -n "Policy" docs/project/glossary.md` → 仅 L168
  `PolicyEngine` 与 L232 `Merge Policy`，无 Temporal Policy/ReactiveRule 词条；
  `include/mira/workflow_ir.hpp:17` `WorkflowPolicy` 闭集、
  `include/mira/security.hpp:99` `PolicyEngine`——消歧注记（§2）与 glossary
  工作项（`M26-05`）依据。
- `include/mira/event_store.hpp:42-60` → `EventPayload{type,data,classification}`
  与 `EventEnvelope`（需 `RuntimeId`/`SessionId`/序列帧）——T1 事件面与
  EventStore 桥接非目标的边界依据（§2/§4.6）。
- `include/mira/json.hpp:177-180` → `canonical_json_string`/`canonical_json_digest`
  既有 canonical 序列化与 SHA-256 摘要入口——复用而非第二套序列化（§5 第 5 问）。
- 测试基建：`tests/CMakeLists.txt:1-13`（`mira_add_test`）、`:639-660`（M24
  `mira_add_m24_test` label `integration;m24`）、`:660-687`（M25
  `mira_add_m25_test` label `integration;m25`）——`mira_add_m26_test` 注册形态
  先例；`docs/api/index.md` 模块地图七页（新页 `temporal-policy.md` 落点）；
  `README.md:13`「当前能力状态」表（`T1-G6` 能力行落点）。
- `python3 tools/check_docs.py .` → Markdown links and fences: OK（冻结后复验，
  见下条）。

2026-09-24（第二次）：冻结文本自查与链接复验。`python3 tools/check_docs.py .` →
Markdown links and fences: OK；总计划同步（头注近况行、§4 里程碑表 M26 行、
§4.1 第 19 条、§5 DEC-037 冻结点列更新）与
[DEC-037](../decisions/DEC-037-temporal-policy.md) 最小事实性回填（头注「关联
计划」行与状态括注指向 M26、非目标「Stage T1 亦未立项」句补时点注记——决策
语义零变更，随本立项同批提交）。无门禁放宽，无范围变更。

2026-09-24（第三次）：独立计划评审发现三项阻塞缺陷，本次修订并补充核验；冻结
范围、工作项与门禁数量不变（错误修正而非门禁放宽，协议 §6）。

- **B1 错误面与既有 Error 模型不兼容**：原冻结 `domain = "temporal_policy"`
  偏离全库 `mira.*` 点分惯例，且 12 个码以字符串列出而 `Error::domain_code` 为
  `std::int32_t`（`core_contracts.hpp:197`），未定义字符串码 ↔ int32 映射与名
  字函数，T1-G1 的「每码至少一条断言」无可执行断言对象。修订：域值冻结为
  `mira.temporal_policy`；码承载冻结为公开 enum `TemporalPolicyDomainCode`
  （显式 int32 值 1–12）+ `temporal_policy_domain_code_name` 稳定名函数 +
  `make_temporal_policy_error`（`static_cast<std::int32_t>` + ErrorCode 确定性
  分配），沿 `ContextDomainCode`（`context_contracts.hpp:254-268`）/
  `make_context_error`（`context_contracts.cpp:624-639`）与 memory 域同构先例；
  原字符串名单废止，语义场景逐一挂到枚举成员。落 §4.4「错误域冻结面」、§5 第
  3 问、T1-G1。
- **B2 `TemporalHistoryEntry::source_digest` 无取值来源**：该字段直接进入
  T1-G4 的逐字节一致门禁，但 `TrackedEntity`/`PolicyWorldView` 输入契约不携带
  载荷 digest，step() 累计与 rebuild 重放两条构造路径均无定义来源。修订：冻结
  「T1 恒空串，两条构造路径一致留空，`digest()` 逐字节一致因此良定义；审计/
  去重语义在 T1 不激活，感知侧载荷摘要随 T2+ 输入契约进入（字段前向兼容）」。
  落 §4.1。
- **B3 九类事件 schema 声明冻结而实际未冻结**：原文仅一个显式 wire 名（其余
  八个靠推断）、载荷字段集未定义、门禁未钉住。修订：九个 schema 名逐一列表冻
  结 + 每类载荷键集（类型与闭集取值）冻结 + 帧/载荷分工（type/tick/rule_id 不
  入载荷；`RuleConflicted` 帧 `rule_id` 置空、匹配 id 在载荷）+ T1-G1 增加
  schema 名精确断言与载荷键集断言。落 §4.4 事件表、§5 第 3 问、T1-G1。
- 核验（本次实际执行）：`sed -n '185,205p' include/mira/core_contracts.hpp` →
  `std::string domain = "mira"`（:194）、`std::int32_t domain_code = 0`（:197）；
  `grep -rhn '\.domain = "' src | sed 's/.*domain = //' | sort -u` → 33 个取值
  全部为 `mira.*` 点分形态、无一例外；`grep -n -A 14 "enum class
  ContextDomainCode" include/mira/context_contracts.hpp` → 显式 int32 值 1–9 +
  `context_domain_code_name` 公开声明（:268）；`src/context/context_contracts.cpp:624-639`
  与 `src/context/memory_contracts.cpp:487-495` → `make_*_error` 先例（域字符串
  + `static_cast<std::int32_t>(code)`）。修订后 `python3 tools/check_docs.py .`
  → Markdown links and fences: OK。

2026-09-24（第四次）：独立计划评审复验发现四项阻塞缺陷（B-A/B-B/B-C/B-D），
本次修订并补充核验；冻结范围、工作项与门禁数量不变（错误修正而非门禁放宽，
协议 §6）。

- **B-A 累计器参数未冻结**：`ReactivePolicyRuntime` 构造仅收
  `PolicyRuntimeOptions`，step() 累计器与 `rebuild_temporal_histories` 各自使
  用什么 `TemporalHistoryOptions` 未定义——容量不同则 FIFO 逐出后 digest 分叉，
  T1-G4 逐字节一致门在契约层不成立。修订：`PolicyRuntimeOptions` 增加显式字段
  `TemporalHistoryOptions history_options{}`（validate 折入），step()(2) 与
  T1-G4 冻结「两路径以同一 runtime 实例的 history_options 构造」。落 §4.4、
  T1-G4。
- **B-B 迁移矩阵/事件发射点/帧字段残缺**：demote 的 reason→(from,to) 合法配对
  矩阵、retire 的事件与 reason、`RuleCandidateInduced` 发射点（induce 还是
  adopt）、非单规则事件的帧 `rule_id` 与生命周期事件的帧 `tick` 取值均未定义，
  T1-G1/G3 断言无唯一对象。修订：迁移矩阵三类合法迁移逐一冻结（demote=
  Runtime→Candidate reason∈{conflict,host-explicit}；retire=
  Runtime|Candidate→Retired 载荷 reason 固定 host-explicit、终态幂等 NoOp；
  promote=Candidate→Runtime）；reason 闭集移除 testing-failed 死值（事件表同
  步）；`RuleCandidateInduced` 冻结在采纳点发射（induce 纯计算不发）；帧
  `rule_id` 分型（单规则事件携带、其余置空）与帧 `tick`（已见最大 tick，无
  step 时 0）冻结。落 §4.4 事件表与冻结语义、决策表新行、T1-G2/T1-G3。
- **B-C 纯 Fact 条件规则的锚实体未定义**：schema 允许手编仅含 `Fact` 条件的规
  则且可晋升 Runtime，但 step() 锚实体定义只覆盖「含实体作用域条件」情形，
  `RuleTriggered.entity_key` 与 `entities` 为空情形无解。修订：匹配判定拆为
  「Fact 条件全部成立 **且** 实体作用域条件满足」；仅含 Fact 条件的规则事实成
 立刻匹配、无锚实体、`entity_key` 置空串（键仍在）；含实体作用域条件的规则在
  `entities` 为空/无满足者时不匹配。落 §4.4 step 语义、T1-G2。
- **B-D 归纳可行性约束未冻结**：精确签名 + `min_support` 要求精确重复样本、
  规范形式 MotionPhase 严格 Gt 支持集最小值在支持集相位单一时对自身支持集零
  触发（晋升门与 T1-G5 逻辑不可达）、每事实键一条件受条件数上界封顶——三项均
  为 harness 作者需自行反推的隐含前提。修订：规范形式 MotionPhase 改 **Ge**
  支持集最小值（全部自覆盖，设计 §6.1 严格示例注记为手编示意）；新增「归纳可
  行性前提（数据集构造约束）」冻结块——精确重复签名、事实键数 ≤ 条件数上界−3、
  测试分部正例分布、非触发样本不计入分母。落 §4.4 归纳语义与可行性前提块、
  决策表归纳算法行、T1-G3/T1-G5、§8 风险条目。
- 核验（本次实际执行）：`grep -rhn '\.domain = "' src | sed 's/.*domain = //'
  | sort -u | wc -l` → 33（domain 惯例复核）；`grep -n "Gt 支持集最小值\|
  §4.5 的规范形式" docs/plans/m26-temporal-policy-stage-t1.md` → 命中仅为本条
  记录自身的历史描述与命令文本（§1–§8 规范节零命中；`testing-failed` 仅存于
  决策表被否备选列的否决理由与本记录，规范语义节零出现）；
  修订后 `python3 tools/check_docs.py .` → Markdown links and
  fences: OK。

2026-09-24（第五次）：`M26-01` 交付轮（实现起点，仅文档变更，未提交——提交由
工作流统一执行）。里程碑状态转 `In Progress`（沿 M23 立项提交先例——立项并跑
前冻结即置 In Progress；M24/M25 因同日立项即关闭未经过该中间态），总计划同步
四处：头注近况行、§4 里程碑表 M26 行、§4.1 第 19 条、§5 DEC-037 行状态括注；
`M26-01` 工作项与 §6 门禁复选框保持未勾选，待 Independent-Verification-Agent
复验。第四次评审 B-B 残留修正一处：§4.4 规范接口片段 `PolicyEvent.rule_id`
行内注释原写「RuleConflicted 例外置空」，与本节已冻结的帧/载荷分工
（`PolicyActivated`/`PolicyDeactivated`/`PolicyEscalatedToAgent`/
`RuleConflicted` 四类帧 `rule_id` 均置空串）不一致，注释对齐冻结文本——错误
修正而非门禁放宽（协议 §6），T1-G2 帧 `rule_id` 分型逐类断言的对象由此唯一。
第四次评审修订落点复核（本次实际执行）：
`grep -n "history_options" docs/plans/m26-temporal-policy-stage-t1.md` →
6 处——§4.4 `PolicyRuntimeOptions` 字段（:260、:262）、step()(2) 语义（:372）、
T1-G4 门禁（:620）与第四次记录文本（:774-775），规范落点齐备；
`grep -n "testing-failed\|Gt 支持集最小值" …` → 仅决策表被否备选列（:519）与
第四次记录文本（:783、:794、:802、:804），规范语义节零命中；
`grep -n "RuleConflicted 例外置空" …` → 零命中（残留已清）；
迁移矩阵三类合法迁移、retire 终态幂等 NoOp、`RuleCandidateInduced` 采纳点
发射、纯 `Fact` 条件匹配与 `entity_key` 置空、`entities` 为空不匹配、归纳自
覆盖规范形式（Ge/Ge/Le）与数据集构造约束冻结块在 §4.4/决策表/门禁
T1-G2..G5/§8 逐一在案。门禁复核（本次实际执行）：
`python3 tools/check_docs.py .` → Markdown links and fences: OK（exit 0）；
`python3 tools/check_architecture.py .` → architecture policy is clean
（8 modules、baseline 抑制 17、stale 0，exit 0）。本提交零代码、零测试目标
变更：新增 m26 测试目标、评估 harness 与门禁取证随 `M26-02`–`M26-05` 交付，
cmake/ctest 自检对文档轮无新增可跑对象。

2026-09-24（第六次）：`M26-02` 实现交付轮（IVA 报告「`M26-02` 产品实现不存在、
两 m26 目标无法编译」后的实现轮；测试文件与 `tests/CMakeLists.txt` 归 IVA
所有，本轮零改动）。交付：`include/mira/temporal_policy.hpp`（公共契约头）+
`src/temporal/temporal_policy.cpp`（入 `mira_core`，`CMakeLists.txt:108` 登记），
按 §4 冻结语义实现全部表面——错误域 12 码三重映射、`TemporalHistory` 环与
canonical digest、`TrackedEntity`/`PolicyWorldView`/`PolicyRuntimeOptions`
validate（history_options 折入）、`ReactiveRule` schema v1（`mira.policy.rule.v1`
canonical wire 往返 + DEC-002 版本策略 + 语义内容 digest 幂等 rule_id）、九类
事件 schema 名与逐类载荷键集（帧/载荷分工与帧 tick 语义）、step 评估序（校验 →
逐实体累计 → (priority, rule_id) 固定序匹配 → 恰一触发/多匹配冲突 fail-closed
全体降级按固定序发射）、纯 `Fact` 条件匹配与 `entity_key` 置空、归纳规范自覆盖
形式（Ge/Ge/Le + 支持集极值 + source_refs 排序去重 + min_support 门）、采纳点
事件/幂等跳过/整批上界拒绝、隔离测试与最近证据、晋升证据门、迁移矩阵（demote
reason 闭集、retire 终态幂等 NoOp）、`rebuild_temporal_histories` 只读重放
（与累计器同 per-entity 序列规则，T1-G4）。runtime 无线程/定时器/Executor
注册面，全部时间取自调用方确定性时钟（§4.5）。
- **加性 API（IVA 申报契约缺口的处置）**：`ReactivePolicyRuntime::
  temporal_history(entity_key)` 只读访问器（实体无累计历史返回空指针），
  DEC-002 加法演进补齐 T1-G4「runtime 累计器 digest == rebuild digest」腿与
  T1-G1「step 累计路径 source_digest 恒空」断言的观测点；§4.4 规范接口同步
  收录。语义零变更、无门禁放宽（协议 §6）。
- 定向核验（本次实际执行，按 IVA 要求仅针对性构建自查、未跑全量套件）：
  `cmake --build --preset debug --target mira_m26_temporal_policy_eval` →
  mira_core 与目标编译链接通过（100% Built target）；`g++ -std=c++20
  -fsyntax-only` 下 `tests/m26/m26_support.hpp`/`m26_dataset.hpp` 编译通过
  （产品头满足全部夹具面）；`python3 tools/check_architecture.py .` →
  architecture policy is clean（初版 1266 行超 `max_file_lines` 1200 预算，
  已按同语义压缩至 1153 行——错误码/事件名表驱动 + 归纳锚点辅助函数，单文件
  冻结范围不变）；`python3 tools/check_docs.py .` → Markdown links and
  fences: OK。clang-tidy 强门禁（`cmake --preset static-analysis` 构建
  `mira_core`，`--warnings-as-errors=*`）首跑报两处违例——
  `performance-inefficient-vector-operation`（:237 归纳签名 facts 未预分配）
  与 `performance-move-const-arg`（:608 对平凡可拷贝 `PolicyRuntimeOptions`
  的无效 `std::move`），当轮修复（`facts.reserve` + 去除 `std::move`）后复跑
  同一门禁构建通过（mira_core 100% Built target，零违例）；debug 评估目标、
  clang-format `-Werror` 干跑、架构与文档检查同步复验全绿（cpp 1154 行，
  预算内）。
- **遗留测试侧缺陷两项（IVA 所有，非产品缺陷，本记录申报）**：(1)
  `tests/m26/m26_temporal_policy_test.cpp:50` 将 T1-G1/G2 用例函数定义于内层
  匿名命名空间，而 `m26_cases.hpp:95` 起在 `mira::m26` 作用域声明同名函数，
  `main()`（:1060 起）的限定名绑定到头文件声明（无定义）——`-Werror` 下 18 个
  `unused-function` 编译失败（本次构建实测），降级该警告后对象符号表显示定义
  为本地符号 `t mira::m26::(anonymous namespace)::…` 而 `main` 持未定义引用
  `U mira::m26::…`，链接必然失败；修复方向：用例定义移至 `mira::m26` 作用域
  （同 `m26_lifecycle.cpp` 先例）。(2) `m26_support.hpp:244-245` `make_entity`
  固定 position (5.0, 5.0)，而 `m26_temporal_policy_test.cpp:562` 断言 rebuild
  条目 `position_y == position_x * 2.0`——§4.6「按样本序重放各实体观测」的
  逐字段复制语义下 5.0 != 10.0 恒假，与夹具自身矛盾（:86-87 环用例为本地构造
  条目设 y=2x，rebuild 输入实体并不满足）；修复方向：夹具 position_y 改 10.0
  或删除该断言。两项修复前契约矩阵目标（T1-G1..G3）无法链接运行；eval 目标
  （T1-G4/G5）本轮已可编译执行。
