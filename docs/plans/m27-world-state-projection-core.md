# M27：会话 World State 投影首阶段——实体词表对齐与确定性投影核心

> 状态：In Progress（2026-09-28 立项并跑前冻结（`M27-01` 交付）；实现按 §7 工作项
> 自 `M27-02` 起推进，门禁验证完成前工作项保持未勾选）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（[DEC-041](../decisions/DEC-041-session-world-state-projection.md)
> 首阶段；[DEC-038](../decisions/DEC-038-unified-behavior-trace.md)/DEC-041
> 「实体词表对齐」共同前置的承载轮——门禁前缀 `WS` 为本文件引入的阶段标签，
> 仅用于工作项与门禁命名，非 DEC-041 冻结的阶段分类）
> 前置：DEC-041 方向冻结（已满足，2026-09-15）；实体词表对齐与专项设计
> （已满足——随本立项以 [DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)
> 冻结收录范围，词表唯一承载 [实体词表设计](../design/entity_vocabulary_design.md)，
> 消费方专项设计 [World State 投影设计](../design/world_state_projection_design.md)
> v0.1 同批交付）；常规授权（已满足——总计划 §4.1 第 8 条方向登记「契约随首阶段
> 里程碑冻结、里程碑文件在进入 `Planned` 前创建」与 M16–M25「依设计与计划推进」
> 同一授权模式；本阶段为纯 Core 确定性投影核，无平台、无感知、无模型、无外部
> 证据依赖；真实模型轮与 DEC-032 Stage E 受 `MNT-202609-27` 外部证据阻塞、平台
> Adapter 宿主接入须先补专项决策，均非本阶段前置且不被本立项解锁）
> 建议发布点：非发布物；DEC-038 首阶段（引用同一词表，另行立项）与 Temporal
> Policy T2+ 世界视图映射的契约锚点
> 更新日期：2026-09-28（立项并跑前冻结）

## 1. 目标

按 [DEC-041](../decisions/DEC-041-session-world-state-projection.md) 冻结的方向，
交付会话 World State 投影的确定性核心（纯 Core，无平台、无感知、无模型依赖）：

- **实体词表对齐**（DEC-038/DEC-041 共同前置，[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)）：
  四组既有契约实体集（ElementRef 族 / App Model `state_id` 族 / toolref 引用族 /
  L0 事件引用族）的单一冻结承载与词表目录测试。
- **投影契约**：`WorldState` 值类型——前台应用认知、页面状态假设（`Believed`/
  `Stale`/`Unknown` 显式三态）、存活实体引用表（容量有界）、最近状态变化环
  （容量有界）；wire schema `mira.worldstate.v1`（DEC-002）。
- **纯函数更新语义**：冻结输入记录集（六类，含显式过期指令）+ 更新算子清单
  （全部显式携带 `WorldStateOptions`）——当前投影 + 输入记录 + 显式时间 →
  新投影；不读时钟；观测冲突显式留痕、证据过期显式标记、无供给进入显式
  `Unknown`（RULE-09/RULE-10 精神）。
- **重建与淘汰**：`rebuild_world_state` 从输入记录序列（六类）以同一 options
  重放重建（同输入序列 + 同选项 → 同投影 digest）；实体表超容量按显式确定性
  淘汰序逐出，淘汰过程可从重放验证（`W-03`/RULE-07/DEC-041 §决策 8）。
- **错误域**：`mira.world_state` 显式 int32 枚举 + 稳定名映射，词表外引用
  fail closed。

## 2. 范围与非目标

范围：`include/mira/world_state.hpp`（单一新公共头）与
`src/worldstate/world_state.cpp`（入 `mira_core`；core 模块内，
`tools/architecture-policy.json` 零差异）；`tests/m27/` 词表目录与投影契约/重建/
淘汰测试矩阵（CMake label `integration;m27`；词表测试目标
`mira_m27_entity_vocabulary_test` 额外链接 `Mira::workflow`——tests 模块 policy
requires 已含 workflow，零 policy 差异——以调用权威 `parse_tool_reference` 做
词表 §2.3 登记一致性冒烟，core 产品代码不复刻语法解析）；文档同步——[实体词表设计](../design/entity_vocabulary_design.md)
与 [World State 投影设计](../design/world_state_projection_design.md)（已随本立项
交付）、API 手册新页、术语表词条、设计实现注记、README 能力表、总计划与
DEC-038/DEC-041/DEC-046 关联回填。

非目标（交叉点注记含在内，均不因本立项解锁）：

- **不做 DEC-038 L1/L2**：Behavior Trace 首阶段另行立项（不预分配编号）；其 L1
  实体引用、源事件引用与工具调用引用必须引用 [实体词表设计](../design/entity_vocabulary_design.md)
  同一冻结面，不得定义第二套引用形态（[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)
  §影响与风险）；L1/L2 schema、抽取编排 API 与预算缺省值随其里程碑冻结
  （DEC-038 §非目标）。
- **不做感知与跨帧身份**：不实现识别、不维护跨帧视觉身份；`ElementRef` 是证据
  不是永久身份（`include/mira/observation.hpp:343-344`），跨帧 identity 归感知侧
  （DEC-041 §非目标）；`Fused` 来源生产者随 [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)
  （`MNT-202609-27` 证据门禁），本阶段只冻结契约形状（RULE-10 如实披露）。
- **不接 Runtime/Session 与 EventStore**：M27 为无帧独立投影核——不写
  `EventEnvelope`（需要 Runtime/Session 帧与序列推进，`include/mira/event_store.hpp:48-60`，
  [M26](m26-temporal-policy-stage-t1.md) §2 同型留痕）、不发射事件、不进入观察
  热路径；Runtime 对投影的认可留痕、checkpoint/恢复集成与观察路径增量更新随
  消费接线里程碑立项冻结（[World State 投影设计](../design/world_state_projection_design.md) §10）。
- **消费者零接线**：Context（DEC-032/035）、Recovery（DEC-031）、Workflow 导航
  （DEC-028）、Temporal Policy（DEC-037 T2+）均不在本阶段接入；T1 的
  `PolicyWorldView` 局部契约不改写，`entity_key` 映射规则随 T2+ 消费立项冻结
  （[实体词表设计](../design/entity_vocabulary_design.md) §3）。
- **投影不是授权来源**（RULE-09 延伸，DEC-041 §决策 6）：`Believed` 状态不构成
  [PolicyEngine](../../include/mira/security.hpp)（DEC-004）输入以外的任何授权
  依据，不提供 capability/permission 语义出口；任何真实副作用仍经既有
  capability/freshness/权限校验。本阶段无动作路径，本条为消费接线阶段的绑定
  约束，随其进入测试面。
- **不修改既有契约**：Observation、App Model、workflow 事件、temporal policy
  契约零触碰（新表面纯加法，DEC-002 版本化）；词表基线条目只登记不改写
  （[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md) 决策 2）。
- **不声明实时性与更新延迟**：投影贴近观察热路径的成本声明被 RULE-10 封死；
  本阶段只冻结容量上限缺省值（暂定默认值，见 §4.1），更新延迟与预算实测归
  消费接线里程碑。
- 无真实模型调用、无网络、无凭据、无真机依赖（纯 Core 确定性投影核）。

## 3. 设计与决策依据

- [DEC-041](../decisions/DEC-041-session-world-state-projection.md)（投影身份、
  纯函数更新、App Model/Observation/Trace 边界、消费者只读、生命周期、有界性）
  ——本文件 §4 是其验证方式条款指定的「首阶段立项时在里程碑文件内冻结」承载面
- [DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md) +
  [实体词表设计](../design/entity_vocabulary_design.md)（词表收录范围、身份域、
  等价规则、映射规则、版本化）
- [World State 投影设计](../design/world_state_projection_design.md) v0.1
  （系统上下文、模块职责、边界、Executor 路由、分阶段；正式契约以本文件 §4 为准）
- [DEC-002](../decisions/DEC-002-public-contract-versioning.md)（wire schema
  major.minor、加法演进、reader fail closed）、[DEC-003](../decisions/DEC-003-event-sourced-persistence.md)
  与 `W-03`（[agent_harness 设计](../design/agent_harness_and_workflow_architecture.md) :154：
  投影可重建、EventStore 唯一事实源）、[DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)
  （置信纯函数纪律、宿主识别边界 §3）、[DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)
  与 [Observation 坐标设计](../design/observation_coordinate_android_host.md) §4.3
  （ElementRef 冻结定义）
- 形态与口径先例：[M26](m26-temporal-policy-stage-t1.md) §4（跑前冻结正式契约、
  零 Executor 注册面、错误域三重映射、数据集确定性时钟）、M16–M20 评估口径
  （digest 锚定 + 跨进程一致）、[M24](m24-context-curator-stage-w5.md)/[M25](m25-host-integration-round.md)
  的 `mira_add_mNN_test` CMake 函数（`tests/CMakeLists.txt:639-721`）
- `RULE-07`（可重建投影）、`RULE-08`（容量上界）、`RULE-09`（外部内容不提升）、
  `RULE-10`（声明须证据）、`W-01`/`W-03`/`W-05`/`W-06`（[agent_harness 设计](../design/agent_harness_and_workflow_architecture.md)
  :152-159）
- AGENTS.md Executor 强制约束第 3/6 条（future 必消费、关闭顺序）、事件纪律
  （引用 + 摘要，不在多条目复制大内容）

## 4. 冻结的契约语义（跑前冻结，2026-09-28）

本章为正式契约冻结面（DEC-002 意义上的公共契约；[World State 投影设计](../design/world_state_projection_design.md)
v0.1 草案字段与本章不一致处，以本章为准并在实现注记中回填设计文档）。代码片段为
**规范接口**（冻结的签名级语义），非实现。

### 4.1 选项与投影值类型

```cpp
struct WorldStateOptions final {
    std::size_t max_entities = 128;               // RULE-08 文档化默认值
    std::size_t max_recent_changes = 64;          // RULE-08 文档化默认值
    std::size_t max_change_summary_bytes = 2048;  // 对齐 kWorkflowEventMaxSummaryBytes
                                                  // （workflow_events.hpp:26）
    std::chrono::milliseconds stale_after{30000}; // 陈旧判定界；暂定默认值（见下注）
    [[nodiscard]] Result<void> validate() const;  // 任一容量为 0 → OptionsInvalid
};
```

- **暂定默认值注记**（项目管理规范 §8）：`max_entities`/`max_recent_changes`/
  `stale_after` 为暂定默认值，负责人 Mira Maintainers，最迟冻结里程碑 = 消费接线
  里程碑（观察路径集成立项时以真实负载校准；本阶段禁止任何基于这些取值的性能
  声明，RULE-10）。

```cpp
enum class WorldBeliefStatus : std::uint8_t { Believed, Stale, Unknown };

struct WorldForegroundApp final {      // 前台应用认知（词表 §2.1 AppContext 域）
    WorldBeliefStatus status = WorldBeliefStatus::Unknown;
    std::string package_name;          // status == Unknown 时必须为空串
    std::string activity_name;
    bool sensitive = false;
    Timestamp observed_at{};           // 调用方传入的确定性时间
    [[nodiscard]] Result<void> validate() const;   // ForegroundInvalid：
    //   Unknown ⇔ package_name 为空串；非 Unknown 时 package_name 非空
};

struct WorldPageAssumption final {     // 页面状态假设（词表 §2.2 (app_id, state_id)）
    WorldBeliefStatus status = WorldBeliefStatus::Unknown;
    std::string app_id;                // status == Unknown 时必须为空串
    std::string state_id;              // 同上；非 Unknown 时非空（词表身份域）
    double confidence = 0.0;           // [0,1]；Unknown 恒 0
    Timestamp recognized_at{};
    [[nodiscard]] Result<void> validate() const;   // PageAssumptionInvalid：
    //   Unknown ⇔ app_id/state_id 为空串且 confidence == 0；
    //   非 Unknown 时 app_id 与 state_id 均非空、confidence ∈ [0,1]
};

struct WorldEntity final {             // 存活实体条目（词表 §2.1 ElementRef）
    ElementRef element;                // 七字段身份（observation.hpp:345-353）
    double confidence = 0.0;           // [0,1]；观察来源初始 1.0（暂定默认值，同上注）
    bool stale = false;
    Timestamp last_seen_at{};
    Sha256Digest source_event_digest{};// 源引用摘要（词表 §2.4；M27 无事件帧形态见 §4.3）
    [[nodiscard]] Result<void> validate() const;   // EntityInvalid：
    //   element.observation_id 为零值或 space 无效 → 词表违规（见 §4.5 错误域）；
    //   confidence ∉ [0,1] → EntityInvalid
};

enum class WorldChangeKind : std::uint8_t {
    PageAssumed, EntityObserved, EntityStaled, NavigationObserved,
    ToolSettled, VerificationSettled, ConflictMarked, EntityEvicted,
};

struct WorldChange final {             // 最近状态变化环条目
    WorldChangeKind kind = WorldChangeKind::PageAssumed;
    std::string summary;               // 调用方预脱敏有界摘要（≤ max_change_summary_bytes）
    Sha256Digest source_event_digest{};
    Timestamp changed_at{};
    std::uint64_t change_sequence = 0; // 投影实例内严格单调递增（回归 → StateInvalid，
                                       //   经 validate 检测；输入记录不携带本字段，
                                       //   算子写入路径恒递增）
};

struct WorldState final {
    SchemaVersion schema_version{1, 0};
    WorldForegroundApp foreground_app;
    WorldPageAssumption current_page;
    std::vector<WorldEntity> entities;
    std::vector<WorldChange> recent_changes;
    [[nodiscard]] Result<void> validate() const;   // StateInvalid：嵌套校验失败一律
    //   折叠为本码（含 change_sequence 回归、confidence 越界、枚举非法与词表类
    //   值非法——validate 不区分词表语义，见 §4.5 折叠规则）；容量与摘要上界
    //   不是值不变量——见下注
    [[nodiscard]] std::string to_json() const;     // mira.worldstate.v1 canonical JSON
    [[nodiscard]] static Result<WorldState> from_json(std::string_view text);
    //   DEC-002 版本策略：{1,x} 当前读取；更老 major（{0,x}）与未知更新 major
    //   （{2,x} 及以上）拒绝 → SchemaUnsupported；JSON 解析失败、未知成员、缺
    //   成员 → PayloadMalformed；成员结构合法但值非法（含嵌套折叠）→
    //   StateInvalid；载荷键集见 §4.4
    [[nodiscard]] Sha256Digest digest() const;     // canonical JSON 摘要，跨进程一致
};
```

- **options 归属冻结**：`WorldState` 值类型**不内嵌** `WorldStateOptions`、wire
  键集不含 options（§4.4）——容量/时限是策略不是投影状态；全部 §4.3 算子以
  `const WorldStateOptions &` 显式参数取得容量（`max_entities`/
  `max_recent_changes`）、摘要上界（`max_change_summary_bytes`）与陈旧界
  （`stale_after`），算子负责维护「实体数 ≤ max_entities、变化环 ≤
  max_recent_changes、summary ≤ max_change_summary_bytes」的**算子不变量**；
  `WorldState::validate()` 只查结构不变量（不重复容量校验，wire 读回投影无需
  options）。选项不同的两次推进不得互相比较 digest（选项进重建配方，见下）。
- 投影是**派生可重建投影**（`W-03`/RULE-07）：同一输入序列（§4.2 六类，含显式
  过期指令）以**同一 options** 经 §4.3 算子增量推进与 `rebuild_world_state`
  重放，`digest()` 必须逐字节一致（`WS-G3`）。
- 默认构造 `WorldState` 合法：`Unknown` 前台/页面、空实体表、空变化环——「无供给
  ≠ 猜测」的类型化表达（DEC-041 §决策 2）。

### 4.2 输入记录（词表消费面，闭集）

```cpp
struct WorldObservationInput final {       // 观察记录元数据（Observation 侧）
    ObservationId observation_id;
    EnvironmentEpoch environment_epoch = 0;
    AppContext app;                        // package_name 空 = 无前台应用供给
    std::vector<ElementRef> elements;      // size ≤ options.max_entities
    Timestamp observed_at{};
};

struct WorldScreenStateInput final {       // 宿主识别（DEC-027 §3 ScreenStateSnapshot 同型）
    std::string app_id;                    // 非空
    std::string state_id;                  // 词表 §2.2；空 → 词表违规
    double confidence = 0.0;               // [0,1]
    Timestamp recognized_at{};
};

struct WorldNavigationInput final {        // mira.workflow.navigation-observed.v1 同型
    std::string app_id;                    // 导航所属应用域（词表 §2.2 二元组前项；
                                           // 空 → 词表违规——事件载荷无 app 域，
                                           // 调用方从会话上下文供给）
    std::string from_state;                // 词表 §2.2
    std::string to_state;                  // 词表 §2.2
    std::string transition_id;
    bool success = false;
    double confidence = 0.0;               // 事件载荷 confidence ∈ [0,1]
    Timestamp observed_at{};
};

struct WorldToolSettledInput final {       // 工具结算（词表 §2.3）
    std::string tool_ref;                  // toolref: 语法 v1 规范形态（非空）
    std::string disposition;               // 闭集 completed|skipped|failed|stale
    Sha256Digest source_event_digest{};
    Timestamp settled_at{};
};

struct WorldVerificationInput final {      // 验证结算（首阶段范围 = 页面假设）
    std::string outcome;                   // 闭集 confirmed|refuted|inconclusive
    double confidence = 0.0;               // confirmed/refuted 时的新置信 ∈ [0,1]
    Timestamp verified_at{};
};

struct WorldExpiryInput final {            // 显式过期指令（expire_stale 的重放形态）
    Timestamp now{};                       // 过期判定时刻（调用方确定性时间源）
};

using WorldStateInput = std::variant<WorldObservationInput, WorldScreenStateInput,
    WorldNavigationInput, WorldToolSettledInput, WorldVerificationInput,
    WorldExpiryInput>;
```

- 输入记录是**唯一更新入口**：不存在绕过算子的字段级写路径（协议 §5 拒绝形态）。
- **过期指令入闭集**：过期判定以 `WorldExpiryInput` 形态进入输入
  序列（算子面冻结为 `apply_expiry`），使含过期的推进可重放（重建等价性见
  §4.3 `rebuild_world_state` 行）；
  M27 无事件帧，过期指令与五类更新记录同地位（消费接线时映射到事件形态，
  `source_event_digest` 语义同 §4.3 注）。
- `WorldObservationInput.elements` 引用 [实体词表](../design/entity_vocabulary_design.md)
  §2.1：`observation_id` 零值或 `space` 无效 → 词表违规（§4.5 `VocabularyViolation`）。

### 4.3 更新算子清单（冻结；全部纯函数，按名一一冻结，不加不减）

```cpp
[[nodiscard]] Result<WorldState> apply_world_observation(WorldState state,
                                                         const WorldObservationInput &input,
                                                         const WorldStateOptions &options);
[[nodiscard]] Result<WorldState> apply_screen_state(WorldState state,
                                                    const WorldScreenStateInput &input,
                                                    const WorldStateOptions &options);
[[nodiscard]] Result<WorldState> apply_navigation(WorldState state,
                                                  const WorldNavigationInput &input,
                                                  const WorldStateOptions &options);
[[nodiscard]] Result<WorldState> apply_tool_settled(WorldState state,
                                                    const WorldToolSettledInput &input,
                                                    const WorldStateOptions &options);
[[nodiscard]] Result<WorldState> apply_verification(WorldState state,
                                                    const WorldVerificationInput &input,
                                                    const WorldStateOptions &options);
[[nodiscard]] Result<WorldState> apply_expiry(WorldState state,
                                              const WorldExpiryInput &input,
                                              const WorldStateOptions &options);
[[nodiscard]] WorldState prune_entities(WorldState state,
                                        const WorldStateOptions &options);
[[nodiscard]] Result<WorldState> rebuild_world_state(
    std::span<const WorldStateInput> inputs,
    const WorldStateOptions &options);                       // 重放重建（只读投影）
```

- **签名冻结注记**：全部算子显式携带 `const WorldStateOptions &`（§4.1
  options 归属冻结）——容量淘汰（`max_entities`）、变化环 FIFO
  （`max_recent_changes`）、摘要上界（`max_change_summary_bytes`）与陈旧界
  （`stale_after`）的取值来源唯一且可见；算子先 `options.validate()`（失败
  `OptionsInvalid`）再校验输入。`expire_stale(state, now)` 冻结为
  `apply_expiry`（输入即 `WorldExpiryInput`），使含过期的推进可经
  `rebuild_world_state` 重放（下表）。

逐算子冻结语义（全部先 options/输入校验再更新；时间一律取自输入/参数，不读时钟）：

| 算子 | 冻结语义 |
| --- | --- |
| `apply_world_observation` | 前台：`app.package_name` 非空 → `foreground_app` 置 `Believed`（package/activity/sensitive/observed_at 逐字段覆盖）；为空 → `Unknown`。实体：逐 `elements` 按 `ElementRef` 七字段全等查找——命中则刷新（`confidence = max(旧, 1.0)` 即置 1.0、`last_seen_at = observed_at`、`stale = false`、`source_event_digest` 更新）；未命中插入新条目（`confidence = 1.0` 暂定默认值，§4.1 注）。插入超 `options.max_entities` 先按 §4.4 淘汰序执行淘汰再插入（淘汰序实现与 `prune_entities` 同一）。变化环：前台变化 → 一条 `PageAssumed`；实体新增/刷新合计 → 一条 `EntityObserved`（summary 含新增/刷新计数）；summary 超 `options.max_change_summary_bytes` → `PayloadTooLarge` 拒绝且投影零变更 |
| `apply_screen_state` | `state_id`/`app_id` 为空 → 词表违规。`current_page` 置 `Believed`（app_id/state_id/confidence 取输入、`recognized_at = input.recognized_at`）+ `PageAssumed` 条目；识别是权威新证据，后到者生效——页面切换由 `PageAssumed` 条目留痕，**不记冲突**（冲突触发唯一来源见 `apply_navigation` 对账）；环/摘要上界同上 |
| `apply_navigation` | `app_id` 为空 → 词表违规（二元组前项无来源）。**from_state 对账（`ConflictMarked` 唯一触发）**：`current_page` 非 `Unknown` 且现值二元组 ≠ (`input.app_id`, `input.from_state`) → 追加一条 `ConflictMarked`（summary 含投影现值与声称出发态的有界摘要）；对账只留痕假设与导航前提的断裂，不改变后续转移语义。`success == true` → `current_page` 置 `Believed`（`app_id = input.app_id`、`state_id = to_state`、confidence 取输入、`recognized_at = input.observed_at`）+ `NavigationObserved` 条目。`success == false` → `current_page` 非 `Unknown` 时置 `Stale`（置信与 `recognized_at` 不变——陈旧判定继续自原识别时刻起算）+ `NavigationObserved` 条目；`Unknown` 时零状态变更仅追加条目。环/摘要上界同上 |
| `apply_tool_settled` | `tool_ref` 冻结检查 = **登记级检查**：非空且 ≤ 256 字节（词表 §2.3 冻结的总长上界），违规 → 词表违规；语法解析不在 core 复刻——权威解析器 `parse_tool_reference`（`include/mira/tool_reference.hpp:76`）实现在 workflow 模块（`CMakeLists.txt:153` 编入 `mira_workflow`、:163 链接 `Mira::core`），core 反向依赖被依赖方向禁止，语法一致性由调用方/持有解析器一侧保证（词表 §2.3「解析器归属」行）；`disposition` 闭集外 → `RecordInvalid`；仅追加 `ToolSettled` 条目（上界同上），不改前台/页面/实体（v1 范围冻结） |
| `apply_verification` | `outcome` 闭集外 → `RecordInvalid`；`current_page.status == Unknown` → `StaleTargetMissing`（显式失败，不静默插入目标）。`confirmed` → `Believed` + `confidence = input.confidence`；`refuted` → `Stale` + `confidence = input.confidence`；两者均同时 `recognized_at = input.verified_at`（刚结算的假设不得立即被既有陈旧界判死）；`inconclusive` → 状态、置信与 `recognized_at` 全部不变。三者均追加 `VerificationSettled` 条目（上界同上） |
| `apply_expiry` | 对每个 `last_seen_at`（实体）/`recognized_at`（页面）距 `input.now`（按 `Timestamp::wall` 差值）超过 `options.stale_after` 且当前非 `Stale` 的条目：置 `Stale`、`confidence` 减半（下限 0，暂定默认值同 §4.1 注；已 `Stale` 条目不重复衰减）。实体逐条产生 `EntityStaled` 条目；页面产生一条（上界同上）。无超界条目时零变化零条目 |
| `prune_entities` | `entities.size() > options.max_entities` 时按淘汰序逐出至容量内：`stale` 为真者优先 → `last_seen_at` 早者优先 → `confidence` 低者优先 → `ElementRef` canonical JSON 字典序升序（全序确定性）。每次逐出产生 `EntityEvicted` 条目。容量内调用为 NoOp。**非重建语义**：本算子是 §4.3 淘汰序的具名公开实现与调用方便利/审计入口，不在 `rebuild_world_state` 重建配方内（重建配方 = §4.2 六类输入按序折叠；插入溢出的淘汰在 `apply_world_observation` 内以同一冻结序自动发生）——独立调用 prune 后的投影不参与 `WS-G3` 等价断言 |
| `rebuild_world_state` | `options.validate()` 失败 → `OptionsInvalid`；空输入 → 默认 `WorldState`（合法）；否则按序折叠输入序列：五类更新记录走对应 `apply_*`、`WorldExpiryInput` 走 `apply_expiry`（插入溢出淘汰在观察算子内以同一冻结序发生，无独立 prune 调用）。结果与同 options 增量推进 `digest()` 逐字节一致（`WS-G3`） |

- `recent_changes` 环：追加超 `options.max_recent_changes` 按 FIFO 逐出最旧
  （对齐 `TemporalHistory` 环语义，[M26](m26-temporal-policy-stage-t1.md) §4.1）。
- `change_sequence` 严格单调；任何算子失败（返回错误）时投影保持原值不变
  （强一致失败语义，无部分更新）。
- **源引用摘要的 M27 冻结语义**：`source_event_digest` 取输入记录自身
  `canonical_json_digest`（`include/mira/json.hpp:175-180`）——M27 无事件帧，
  词表 §2.4 的 `EventEnvelope` 信封定位形态随消费接线立项启用，届时该字段改载
  信封摘要而条目 schema 不变（前向兼容，DEC-002 加法语义；对齐 [M26](m26-temporal-policy-stage-t1.md)
  §4.1 `source_digest` T1 冻结语义的同一处理模式）。

### 4.4 wire schema `mira.worldstate.v1`（键集冻结）

`to_json()` 产出 canonical JSON（对象键字典序，`json.hpp:175-180` 同口径）；顶层
成员与嵌套键集冻结如下，`from_json` 对未知成员、缺成员 fail closed：

- 顶层：`schema_version`、`foreground_app`、`current_page`、`entities`、
  `recent_changes`。
- `foreground_app`：`status`（`believed|stale|unknown` 小写名闭集）、
  `package_name`、`activity_name`、`sensitive`、`observed_at`（wall 时间戳）。
- `current_page`：`status`、`app_id`、`state_id`、`confidence`、`recognized_at`。
- `entities[i]`：`element`（七字段：`observation_id`、`environment_epoch`、
  `source`（枚举名闭集）、`stable_hint`、`bounds`、`space`、`evidence_digest`）、
  `confidence`、`stale`、`last_seen_at`、`source_event_digest`。
- `recent_changes[i]`：`kind`（八值小写名闭集）、`summary`、
  `source_event_digest`、`changed_at`、`change_sequence`。
- 版本策略：`{1,x}` 当前读取；`{0,x}`/`{2,x}`+ 拒绝（`SchemaUnsupported`）；
  解析失败/未知成员/缺成员 → `PayloadMalformed`；canonical 往返字节一致
  （`WS-G6`）。

### 4.5 错误域 `mira.world_state`（冻结面）

沿 `mira.*` 点分域惯例与显式 int32 枚举 + 稳定名映射先例
（`ContextDomainCode`/`make_context_error`，`context_contracts.hpp:254-268` 同构；
[M26](m26-temporal-policy-stage-t1.md) §4.4 错误域纪律）：

```cpp
enum class WorldStateDomainCode : std::int32_t {
    OptionsInvalid      = 1,  // WorldStateOptions::validate 失败（任一容量为 0）
    StateInvalid        = 2,  // WorldState::validate 失败（结构不变量，含嵌套折叠）
    RecordInvalid       = 3,  // 输入记录结构非法（数值越界、disposition/outcome
                              //   闭集外、元素数超上限）
    VocabularyViolation = 4,  // 词表外引用（仅算子输入路径）：observation_id
                              //   零值、space 无效、state_id/app_id 空、
                              //   tool_ref 空或超 256 字节；validate/from_json
                              //   不产生本码——嵌套词表类值非法折叠为
                              //   StateInvalid（§4.1 折叠规则）
    PayloadTooLarge     = 5,  // summary 超 max_change_summary_bytes（拒绝，不截断）
    SchemaUnsupported   = 6,  // from_json：major 不在 {1,x} 支持域
    PayloadMalformed    = 7,  // from_json：解析失败、未知成员、缺成员
    StaleTargetMissing  = 8,  // apply_verification 目标页面为 Unknown
};
[[nodiscard]] std::string_view world_state_domain_code_name(WorldStateDomainCode code);
[[nodiscard]] Error make_world_state_error(WorldStateDomainCode code, std::string detail);
//   domain = "mira.world_state"、domain_code = static_cast<std::int32_t>(code)、
//   ErrorCode 确定性分配（沿 make_context_error 先例）
```

### 4.6 Executor 路由与生命周期（Q7，显式冻结）

- **M27 零注册面**：全部算子与重建是有界同步纯函数（工作量上界由
  `WorldStateOptions` 容量封顶：O(max_entities × 输入元素数) 与 O(输入序列长)），
  由调用方/harness 同步驱动——不创建任何线程、定时器、周期任务或 realtime/
  低延迟 lane 注册。
- **生产形态边界注记**：观察热路径的增量更新、水位与预算控制、Runtime/Session
  生命周期集成随消费接线里程碑冻结（[World State 投影设计](../design/world_state_projection_design.md)
  §6/§10）；M27 强行注册执行面属无消费者的新集成面。
- **编排纪律示范**：测试 harness 的输入序列驱动任务经 `executor.submit_auto()`
  提交并**消费 future**（AGENTS.md 第 3 条），无 fire-and-forget。
- **句柄与关闭序**：投影核无线程、无句柄、无独立关闭面；harness 持有 executor，
  关闭序 = 停编排生产者 → 消费全部 future → 非 worker 线程 `shutdown(true)`
  （AGENTS.md 第 6 条）。
- **时间源**：投影不读系统时钟；全部 `Timestamp` 来自输入记录/参数（测试用数据集
  确定性时钟）——跨机器/跨进程逐字节可复现的前提（`WS-G2`/`WS-G3`）。

### 4.7 决策记录表

| 决策点 | 选择 | 被否备选 | 依据 |
| --- | --- | --- | --- |
| 词表载体 | [DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md) + [实体词表设计](../design/entity_vocabulary_design.md) 单一承载；本文件与 DEC-038 首阶段里程碑只引用 | 词表分头写进 DEC-038/DEC-041 各自里程碑（双冻结主体 = DEC-038 §备选否决的漂移形态）；并入 agent_harness 总纲（跨域词表稀释总纲职责，且绕不开 §8 决策环节） | 总计划 §4.1 第 19 条；DEC-038 §影响与风险 / DEC-041 §影响与风险；项目管理规范 §8 |
| 阶段切分 | DEC-041 首阶段先行（本里程碑）；DEC-038 首阶段随后另行立项、引用同一词表 | 两投影同一里程碑承载（违反规范 §4.2「可独立验收能力增量」，评审面翻倍）；L1 先行（实体引用先于存活/置信语义落地，漂移风险前置，且 L2 压缩带 DEC-036 模型供给与 eval profile 缺口） | DEC-041 §影响与风险「任何一侧先行都必须预留对齐点」；DEC-038 §影响与风险 eval 基准缺口；M16–M26「一个方向决策→专项设计→阶段里程碑序列」惯例 |
| 词表 v1 基线 | 四组既有已冻结契约实体集（ElementRef / state_id / toolref / L0 事件引用） | 连同 Memory `subject_id`、T1 `entity_key` 一起冻进 v1（收录面与评审面膨胀，二者各有更晚的消费者冻结点）；等 DEC-033 跨帧身份再冻（外部证据门禁卡死无依赖阶段） | DEC-046 决策 2/3；词表设计 §2/§3 |
| 契约落点 | 单公共头 `include/mira/world_state.hpp` + `src/worldstate/world_state.cpp`（core 模块内，零 policy 差异） | 新独立模块（无独立外部依赖诉求）；散入 observation/workflow 既有头（跨域污染既有契约面） | 协议 §2 Q4；`tools/architecture-policy.json` core roots（本次核验，§9） |
| 前台与页面分离 | `foreground_app`（AppContext 域）与 `current_page`（词表 §2.2 (app_id, state_id)）两个独立认知块 | 合并为单一「页面假设」（observation 只有 app 无 state_id，会被迫引入空 state_id 的词表违规态） | DEC-041 §决策 1「前台应用与页面假设」并列原文；词表 §2.2 身份域 |
| 假设性表达 | `WorldBeliefStatus{Believed, Stale, Unknown}` 显式三态贯穿前台/页面；`Unknown` 为默认构造态 | 布尔标记组合（三态语义被两个布尔隐式编码，`Unknown` 与「有供给但已过期」不可区分）；optional 缺省表达（`nullopt` 语义含混，DEC-041 要求「显式 Unknown」） | DEC-041 §决策 2；协议 §2 Q3 |
| 实体身份 | `ElementRef` 七字段全等（含 `evidence_digest`）；跨 Observation 不合并条目 | 以 `stable_hint` 单独为键跨帧合并（宣称了词表不承载的跨帧身份——DEC-041 §非目标与词表 §6 明确排除）；哈希键（等价规则不可执行） | DEC-046 决策 4；词表 §2.1 等价规则 |
| 更新入口 | 六类输入记录（含显式过期指令）+ 八个具名算子闭集（全部显式携带 options），强一致失败语义（失败零变更） | 直接消费原始 `EventEnvelope`（M27 无帧，且 DEC-041 事件桥接归消费接线）；可变引用原地更新（失败中途态难保证，非纯函数形态） | DEC-041 §决策 2；协议 §5；[M26](m26-temporal-policy-stage-t1.md) §4.6 同型决策 |
| options 归属 | 投影值类型**不内嵌** `WorldStateOptions`、wire 键集不含 options；八个算子全部以 `const WorldStateOptions &` 显式参数取得容量/摘要上界/陈旧界，容量为算子不变量而非值不变量 | 嵌入 `WorldState` 并入 wire 键集（策略入状态：wire 往返携带策略、跨选项投影不可比、读回需选项语义）；退化为编译期常量（丢失 DEC-041 §决策 8「显式可配置策略」）；算子省略 options 参数（取值无来源，契约不可实现） | 协议 §2 Q3；DEC-041 §决策 8；[M26](m26-temporal-policy-stage-t1.md) §9 B-A 同型修订 |
| 过期重放形态 | `WorldExpiryInput{now}` 为第六类输入，`expire_stale` 冻结为 `apply_expiry`；重建配方 = 六类输入按序折叠，含过期的推进可重放 | rebuild 收「输入 + 过期指令」分离的算子迹（输入闭集膨胀为命令流，偏离 DEC-041「从事件序列重建」形态）；等价断言限定无过期前缀（过期是 DEC-041 §决策 2 显式语义，排除后投影不可重建，违反 `W-03` 立项验收目标） | DEC-041 §决策 1/§验证方式（重建确定性为立项验收目标）；协议 §2 Q6 |
| toolref 校验面 | core 侧仅登记级检查（非空 + ≤ 256 字节，词表 §2.3 冻结上界）；语法解析权威在 workflow 模块 `parse_tool_reference`，由调用方/持有解析器一侧执行；词表冒烟由链接 `Mira::workflow` 的测试目标断言 | core 复刻语法校验（对 M7 TR0 权威解析器形成第二实现/漂移面，违反词表 §2.3 单一权威）；core requires workflow（反向依赖，违反依赖方向与 `tools/architecture-policy.json`）；契约静默不查（空串/超长无任何拒绝面） | 词表 §2.3「解析器归属」行；`CMakeLists.txt:153/:163`（本次核验，§9）；协议 §2 Q4 |
| 导航 app_id 与时间戳刷新 | `WorldNavigationInput` 增 `app_id`（空 → 词表违规）；导航成功以输入 app_id 置 `Believed`（`recognized_at = observed_at`）；验证 confirmed/refuted 刷新 `recognized_at = verified_at`，failure/inconclusive 不刷新 | `app_id` 沿用现值（Unknown 出发产出空 app_id 的 Believed 态——与词表 §2.2 二元组身份域及 §4.5 词表违规定义矛盾）；`recognized_at` 不随结算刷新（刚确认的假设立即被陈旧界判死，`WS-G3` digest 锚不确定） | 词表 §2.2；DEC-041 §决策 2（显式表达）；协议 §2 Q6 |
| 冲突语义 | `ConflictMarked` 触发冻结为唯一来源——导航 `from_state` 对账（投影现值二元组 ≠ 声称出发二元组：假设链断裂留痕，转移语义照常）；识别/导航的页面切换是正常更新，由 `PageAssumed`/`NavigationObserved` 条目留痕、不记冲突 | 覆盖前值二元组不同即冲突（正常 A→B 导航每次触发，冲突语义被转移噪音淹没）；首写优先吞掉冲突（冲突不可见）；置信加权融合（融合代数无证据支撑，属未冻结策略） | DEC-041 §决策 2「冲突显式表达」；协议 §2 Q6 |
| 淘汰序 | `prune_entities` 显式全序：stale 优先 → `last_seen_at` 早优先 → confidence 低优先 → `ElementRef` canonical JSON 字典序；变化环 FIFO；prune 为淘汰序具名实现与便利/审计入口，**不在重建配方**（插入溢出淘汰在观察算子内自动发生） | LRU/按引用频次（需要额外访问跟踪状态，破坏纯函数输入闭集）；容量满即拒绝新实体（观察热路径上拒绝不可用，DEC-041 §决策 8 要求可配置淘汰）；独立 prune 计入重建等价（调用方插入的便利调用会破坏重放等价性） | DEC-041 §决策 8「显式可配置策略、淘汰可从事件重放验证」；RULE-08；协议 §2 Q6 |
| 验证结算范围 | 首阶段仅页面假设（confirmed/refuted/inconclusive 闭集） | 实体级验证语义（词表外身份归并问题未决，推迟至消费接线按证据立项）；`Unknown` 目标静默忽略（吞失败） | DEC-041 §决策 6（验证结果为重建输入）× v1 最小面；协议 §2 Q3 |
| 源引用摘要 | M27 恒为输入记录 canonical 摘要；信封定位随消费接线启用、schema 不变 | M27 直接承载 `EventEnvelope` 定位（无帧可取，字段无来源——M26 B2 同型缺陷）；恒空串（丢掉重建核对锚点） | [M26](m26-temporal-policy-stage-t1.md) §4.1 `source_digest` 冻结语义先例；词表 §2.4 |
| 错误域 | `mira.world_state` 八码显式 int32 枚举 + 稳定名 + `make_world_state_error` | 字符串码（`Error::domain_code` 为 int32，`core_contracts.hpp:197`，无可执行断言对象——M26 B1 同型缺陷）；复用 `mira` 通用域（词表违规不可寻址） | [M26](m26-temporal-policy-stage-t1.md) §9 B1 修订先例；`mira.*` 域惯例（本次核验 33 个取值全部点分，M26 §9） |
| Executor 路由 | 零注册面：纯函数 + harness `submit_auto()` 编排且 future 必消费 | 注册周期任务驱动投影（无消费者、AGENTS.md 第 4 条禁止的冒充形态）；fire-and-forget 预热（禁止形态） | [M26](m26-temporal-policy-stage-t1.md) §4.5 同型冻结；AGENTS.md Executor 纪律 |
| 决策载体 | 词表收录 = 新建 DEC-046（跨模块公开契约变化，规范 §8 强制）；投影正式契约不新建 DEC：本文件 §4 即 DEC-041 验证方式条款指定的首阶段冻结承载面 | 词表只写设计文档不建 DEC（违反规范 §8）；投影契约另建 DEC（DEC-041 已指定里程碑承载，重复登记无新裁决内容） | 项目管理规范 §8；DEC-041 §验证方式/§非目标；[M26](m26-temporal-policy-stage-t1.md) §4.6「决策载体」行同型 |

## 5. 阶段冻结协议必答八问（2026-09-28）

1. **行为**：新增确定性会话投影核——(a) [实体词表](../design/entity_vocabulary_design.md)
   四组身份域的词表目录一致性测试面（DEC-046 验证方式落地）；(b) `WorldState`
   值类型与 `mira.worldstate.v1` wire schema（前台/页面/实体表/变化环，三态假设
   性）；(c) 六类输入记录（五类更新 + 显式过期指令）+ 八个纯函数更新算子
   （全部显式携带 options；含确定性淘汰）；
   (d) `rebuild_world_state` 重放重建；(e) `mira.world_state` 错误域。对应设计：
   [World State 投影设计](../design/world_state_projection_design.md) §3/§4/§7/§8
   （正式形态以本文件 §4 为准）、[实体词表设计](../design/entity_vocabulary_design.md)
   §2/§7。
2. **所有者**：投影状态由 `WorldState` 值类型承载、调用方唯一持有（值语义，无
   共享可变状态）；更新只经 §4.3 八个具名算子，无第二写路径；输入记录由调用方
   持有、算子只读；无全局可变状态。消费侧（Context/Recovery/导航/Temporal
   Policy）按 DEC-041 §决策 6 只读，接线随其立项。
3. **契约**：最小公共契约面 = `include/mira/world_state.hpp` 单头的 §4.1–§4.3
   类型与算子清单（算子显式携带 `WorldStateOptions`，§4.1 options 归属冻结）
   + `mira.world_state` 错误域（八码显式 int32 + 稳定名 + 构造函数，§4.5）+
   wire schema `mira.worldstate.v1` 键集与 DEC-002 版本策略（§4.4）；允许的
   调用方 = 宿主与测试（Context/Recovery/导航/Temporal Policy 消费者为后续
   阶段，§2 显式非目标）。契约四件套：公开头文件 + 本文件 §4 +
   `docs/api/world-state.md`（随 `M27-05` 交付）+ `tests/m27/`。
4. **层与依赖**：全部新文件入 core 模块（`include/mira` + `src/worldstate/`，均在
   policy core roots 内）；产品头仅依赖既有 mira 公共头（`core_contracts.hpp`/
   `observation.hpp`/`event_store.hpp`（`Sha256Digest`，:23）/`json.hpp`）与
   标准库（`variant`/`span`/`chrono`）；**不含** `tool_reference.hpp`（权威
   解析器 `parse_tool_reference` 实现在 workflow 模块——`CMakeLists.txt:153`
   编入 `mira_workflow`、:163 链接 `Mira::core`，core 反向依赖被依赖方向禁止，
   toolref 只做词表 §2.3 登记级检查）、不含 `workflow_navigation.hpp`
   （`WorldScreenStateInput` 为 `ScreenStateSnapshot` 同型注记，非同类型）；
   `tools/architecture-policy.json` **零差异**（core requires 不变；词表测试
   目标额外链接 `Mira::workflow` 落在 tests 模块——其 requires 已含 workflow，
   本次核验见 §9）。
5. **复用**：`Result`/`Error`/`Timestamp`/`SchemaVersion` 惯例（core_contracts）、
   `ElementRef`/`AppContext`（observation.hpp:341-375）、canonical JSON 与
   SHA-256（`json.hpp:175-180` `canonical_json_string`/`canonical_json_digest`，
   与 M26/WorkingContextSnapshot 同口径——不新增第二套序列化/哈希）、toolref
   词表与解析纪律（M7 TR0 已冻结，`tool_reference_and_skill_design.md` §4）、
   DEC-002 版本策略、错误域三重映射先例（`ContextDomainCode`/`mira.temporal_policy`）、
   per-milestone CMake 测试函数先例（`tests/CMakeLists.txt:639-721`）。投影域无
   既有承担路径（`grep -rn world_state include src tests` 零命中，§9）——
   App Model 是长期资产、Working Context 是内容投影，均不承担「当前 believed
   环境认知」（DEC-041 §备选方案已否决同型方案），不构成平行路径。
6. **时序**：更新顺序 = 输入记录应用序（调用方决定，重放可复现）；过期判定
   以第六类输入 `WorldExpiryInput` 显式进入序列，含过期的推进可重放；
   `change_sequence` 投影实例内严格单调——输入记录不携带该字段、算子写入
   路径恒递增，回归只可能出现在手工构造或 wire 读回，经 `validate()`
   拒绝（`StateInvalid`，§4.1 折叠规则）；陈旧判定以 `Timestamp::wall` 差值
   对照 `options.stale_after`（时间由调用方传入，不读时钟）；页面假设的
   `recognized_at` 刷新规则冻结——导航成功取 `input.observed_at`、验证
   confirmed/refuted 取 `input.verified_at`、failure/inconclusive 不刷新
   （陈旧判定继续自原识别时刻起算）；冲突 = 导航 `from_state` 对账不一致
   留痕（`ConflictMarked` 唯一触发），转移语义后到者生效照常；淘汰全序确定
   且插入溢出自动发生（独立 prune 调用不在重建配方）；算子失败强一致
   （零变更）。取消/deadline 语义不适用于无异步等待点的纯函数核；Runtime
   集成后的取消、迟到结果与热路径预算随消费接线立项冻结（显式非目标，非
   本阶段可答项）。
7. **Executor 路由**：M27 零 Executor 注册面（§4.6）——全部工作为有界同步纯
   函数；harness 编排经 `submit_auto()` 且 future 必消费；句柄：投影核无线程无
   句柄，harness 持有 executor；关闭序 = 停编排生产者 → 消费 future → 非 worker
   线程 `shutdown(true)`（AGENTS.md 第 6 条）。
8. **验证上下文**：评审者最小阅读集 = 本文件 §4、[DEC-041](../decisions/DEC-041-session-world-state-projection.md)、
   [DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)、
   [实体词表设计](../design/entity_vocabulary_design.md)、
   [World State 投影设计](../design/world_state_projection_design.md)、
   `include/mira/world_state.hpp`、`tests/m27/m27_entity_vocabulary_test.cpp`、
   `tests/m27/m27_world_state_test.cpp`。

## 6. 测试与退出条件

门禁形态说明：本轮无模型调用、无平台依赖，门禁全部为确定性断言；测试的编写、
运行与 sanitizer 取证由 Independent-Verification-Agent 独立完成并复验。CMake
注册沿 M24/M25/M26 先例新增 `mira_add_m27_test` 函数（link `Mira::core` +
`executor::executor`，label `integration;m27`，TIMEOUT 120；
`tests/CMakeLists.txt:639-721` 先例）。

- [x] `WS-G1` 词表目录一致性与 fail-closed 负向矩阵
  （`tests/m27/m27_entity_vocabulary_test.cpp`，label `integration;m27`，目标
  额外链接 `Mira::workflow`）：词表 §2.4 **两层断言**——(a) 类型名闭集：
  `is_workflow_event_type`（`workflow_events.hpp:28`）对词表 §2.4 表 20 个
  CamelCase 类型名逐一接受、闭集外名拒绝；(b) schema 名清单：词表 §2.4 表
  20 个 `mira.workflow.<kebab>.v1` 串逐一经对应事件序列化产物（`to_event_payload`
  的 `schema` 成员）精确断言（M26 T1-G1 事件表断言同型；词表 §2.4 表即单一
  冻结清单），`mira.policy.*.v1` 九类经 `policy_event_schema_name`
  （`temporal_policy.hpp:242`）逐一比对；词表 §2.1——`ElementRef`
  关键字段缺失（`observation_id` 零值、`space` 无效）经投影输入路径拒绝
  （`VocabularyViolation`）；词表 §2.2——输入侧空 `state_id`/`app_id` 拒绝；
  词表 §2.3 对齐冒烟——经 `Mira::workflow` 链接调用权威 `parse_tool_reference`
  （`tool_reference.hpp:76`）断言词表登记一致（接受/拒绝样例；非复刻实现，
  权威解析矩阵在 M7 TR0 既有测试），core 登记级检查——`tool_ref` 空或超
  256 字节经 `apply_tool_settled` 拒绝；
  `MemoryScope.subject_id`/`entity_key` 不进词表目录的反向断言（映射规则登记，
  词表 §3——投影 API 不接受 `entity_key` 形态输入的编译面确认）。
- [x] `WS-G2` 纯函数性与更新算子矩阵（`tests/m27/m27_world_state_test.cpp`，
  label `integration;m27`）：`WorldStateOptions`（含容量为 0 → `OptionsInvalid`）/
  `WorldForegroundApp`/`WorldPageAssumption`/`WorldEntity`/`WorldState`
  validate 全部拒绝路径；八算子逐个正负用例（§4.3 表逐行：观察刷新/插入/溢出
  自动淘汰、识别更新零冲突（页面切换仅 `PageAssumed` 留痕）、导航——
  `Unknown` 出发带 `app_id` 成功置 `Believed`、`app_id` 缺失词表违规、failure
  置 `Stale` 且 `recognized_at` 不变、`from_state` 对账不一致 → `ConflictMarked`
  且转移照常、正常 A→B 导航零 `ConflictMarked`（冲突触发唯一性断言）、
  toolsettle 登记级检查与 `disposition`
  闭集拒绝、验证三 outcome 与 `StaleTargetMissing`、`apply_expiry` 超界置
  Stale + 置信减半且已 Stale 不重复衰减、导航成功/验证结算对 `recognized_at`
  的刷新断言、`prune_entities` 淘汰序逐格）；纯函数性——同输入序列 + 同
  options 在不同进程内重复应用 `digest()` 逐字节一致，且投影更新不读系统时钟
  （固定输入 + 变换宿主墙钟不可观测：digest 与时间注入无关）；算子失败零变更
  强一致断言。
- [x] `WS-G3` 重建与跨进程确定性（同文件）：冻结确定性输入序列（digest 在测试
  内常量锚定并断言，覆盖六类输入——五类更新 + `WorldExpiryInput`——与冲突/
  过期/溢出淘汰路径）；同 options 下 `rebuild_world_state` 与逐算子增量推进的
  `digest()` 逐字节一致；同输入前缀截断重放 → 前缀投影 digest 与全量推进至
  该前缀一致；独立 `prune_entities` 调用的投影不在等价断言内（§4.3 非重建
  语义的负向确认）；完整管线重放两遍 → digest 跨进程字节一致（M16–M20 口径）。
- [x] `WS-G4` 容量与淘汰（同文件，全部经带 options 的算子断言）：
  `options.max_entities` 溢出触发淘汰序（stale → `last_seen_at` → confidence
  → canonical JSON 字典序四键逐格断言，含全同键稳定性）；被淘汰实体的投影可
  从六类输入序列以同 options 重放逐字节重建（含 `EntityEvicted` 条目）；
  `options.max_recent_changes` FIFO 逐出；`summary` 超
  `options.max_change_summary_bytes` → `PayloadTooLarge` 拒绝且投影零变更
  （不截断冒充）；不同 options 下同输入序列推进不宣称 digest 可比（options
  进重建配方的正向确认）。
- [x] `WS-G5` Unknown/陈旧/冲突 fail-closed 负向矩阵与只读性（同文件）：默认
  构造 `WorldState` 全 `Unknown` 且合法（无供给 ≠ 猜测）；无识别/导航供给时
  页面恒 `Unknown` 且置信恒 0；`apply_expiry` 后消费 `Stale` 条目不回 `Believed`
  （无新证据不升级）；导航 `from_state` 对账冲突场景 `ConflictMarked` 条目
  在案、转移语义照常且可重放；
  只读性——投影核公开面无任何返回授权/capability/permission 类型的出口（头
  面断言 + 架构检查：`world_state.hpp` 不包含 `security.hpp` 依赖）、
  `Unknown`/`Stale` 状态不被算子自动升级为 `Believed`（授权语义本身归 DEC-004
  安全层，非本阶段对象）。
- [x] `WS-G6` wire schema 与错误域（同文件）：`mira.worldstate.v1` canonical
  JSON 往返字节一致 + DEC-002 版本策略（`{1,0}`/`{1,1}` 读取、`{0,x}`/`{2,x}`
  拒绝、未知成员/缺成员 `PayloadMalformed`；成员结构合法但值非法——含嵌套
  词表类值非法（如实体 `observation_id` 零值）与 `change_sequence` 回归——
  折叠为 `StateInvalid`，§4.1 折叠规则断言）；键集恰为 §4.4 冻结清单
  （canonical 序列化跨进程字节一致）；`mira.world_state` 错误域——八码每码
  至少一条断言（构造路径断言 `domain == "mira.world_state"`、`domain_code ==
  static_cast<std::int32_t>(成员)` 与 `world_state_domain_code_name` 稳定名
  三重一致；`RecordInvalid` 以输入路径可达场景断言，`VocabularyViolation`
  以算子输入路径断言、validate/from_json 路径零产生）。
- [ ] `WS-G7` 文档同步与契约四件套：API 手册新页 `docs/api/world-state.md` +
  `docs/api/index.md` 模块地图行；术语表新词条（World State / 实体词表，与
  Behavior Trace 词条消歧衔接）——词表词条随本立项先行落地（§7 `M27-01`），
  本门禁复核其与交付契约一致；设计文档实现注记（[World State 投影设计](../design/world_state_projection_design.md)
  草案 → 正式契约偏差清单、[Temporal Policy 设计](../design/temporal_policy_design.md)
  §16 对齐点回填）；README 能力表行；[DEC-038](../decisions/DEC-038-unified-behavior-trace.md)/[DEC-041](../decisions/DEC-041-session-world-state-projection.md)/[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)
  关联回填；总计划同步。
- [ ] 本地门禁：debug 全量 ctest 全绿（含新增 m27 两目标）、ASAN/UBSAN/TSAN 新
  增目标零报告、`format-check`/`docs-check`/`platform-boundary-check`/
  `sbom-check`/`architecture-check` 通过、clang-tidy 预检被改库源编译单元
  （`world_state.cpp` 等）零违例；既有套件零回归（新表面纯加法，无既有契约改动）。
- [ ] PR CI（Linux/Windows/Android/sanitizers/quality）全绿后回填验证记录并
  关闭本阶段。

## 7. 工作项

- [x] `M27-01` 阶段立项与本文件 §4/§5/§6/§7 冻结（时间戳先于任何实现与测试）；
  同批交付 [DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)、
  [实体词表设计](../design/entity_vocabulary_design.md)、
  [World State 投影设计](../design/world_state_projection_design.md) v0.1、
  术语表词条（World State / 实体词表）、总计划登记与 DEC-038/DEC-041/
  [Temporal Policy 设计](../design/temporal_policy_design.md) §16 事实性回填。
- [x] `M27-02` 契约与实现：`include/mira/world_state.hpp` +
  `src/worldstate/world_state.cpp`（入 `mira_core`），按 §4 冻结语义实现；
  core 模块零 policy 差异；clang-tidy 预检零违例。
- [x] `M27-03` 词表目录与 fail-closed 测试矩阵（§6 `WS-G1`，
  `tests/m27/m27_entity_vocabulary_test.cpp`，CMake 注册 label `integration;m27`，
  目标额外链接 `Mira::workflow` 以调用权威 `parse_tool_reference`）
  ——测试的编写、运行与 sanitizer 取证由 Independent-Verification-Agent 独立
  完成并复验。
- [x] `M27-04` 投影契约/重建/淘汰测试矩阵（§6 `WS-G2`–`WS-G6`，
  `tests/m27/m27_world_state_test.cpp`）——同由 Independent-Verification-Agent
  独立完成并复验。
- [ ] `M27-05` 文档同步（§6 `WS-G7` 清单）+ 本地全门禁与 PR CI 取证回填；
  全部工作项与门禁复核通过后本里程碑转 `Completed`。

## 8. 风险与阻塞

- 风险：实体身份表达力受限（`ElementRef` 是证据非永久身份，跨帧同一性归感知
  侧，`Fused` 来源无生产者）。处置：§4.3 实体身份按七字段全等冻结、不宣称跨帧
  合并；词表 §6 与 §2 非目标如实披露（RULE-10）；感知侧身份随 DEC-033 证据
  立项，不在本阶段解锁。
- 风险：投影被误读为感知、授权或实时能力。处置：§2 非目标三重封死（不做识别、
  非授权来源 RULE-09 注记、不声明延迟 RULE-10）；`WS-G5` 只读性与无升级负向
  矩阵；授权语义归 DEC-004 安全层。
- 风险：容量与陈旧缺省值被误读为实测结论。处置：§4.1 暂定默认值注记（负责人
  + 最迟冻结里程碑 = 消费接线里程碑）；门禁全部为确定性断言，无任何性能声明。
- 风险：`entity_key` 二元性延续（T1 局部标识与词表并存）。处置：词表 §3 映射
  规则已登记、映射随 T2+ 消费立项冻结；`WS-G1` 反向断言投影不接受 `entity_key`
  输入形态，防止提前混用。
- 风险：DEC-038 首阶段立项时偏离词表。处置：DEC-046 §影响与风险已载明其门禁
  必须引用本词表且不得定义第二套引用形态；总计划 §4.1 第 20 条登记该约束。
- 外部阻塞：无——不依赖真机、凭据、外部语料或真实模型（纯 Core 确定性投影核；
  真实模型轮与 Stage E 的 `MNT-202609-27` 门禁与本阶段无耦合，不被本立项解锁
  也不阻塞本阶段）。

## 9. 验证记录

2026-09-28：`M27-01` 立项，本文件 §4/§5/§6/§7 冻结（先于任何实现与测试）。
立项依据：总计划 §4.1 第 19 条——M26 关闭后遗留三项中，平台 Adapter 宿主接入
须先补专项决策、真实模型轮与 DEC-032 Stage E 受 `MNT-202609-27` 外部证据阻塞，
DEC-038/DEC-041 首阶段的阻塞项「实体词表对齐 + 专项设计」已随本立项以
[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md) 与两份专项设计
补齐，且 DEC-041 首阶段为无外部依赖的纯 Core 确定性范围（与 M16–M20/M26
「先交付纯 Core 确定性阶段」节奏一致）；DEC-041 §影响与风险「任何一侧先行都
必须预留对齐点」由词表单一承载满足。工作分支
`dec-038-dec-041-behavior-trace-world-state`。实现未开始；后续验证记录按日期追加。

立项前核验（本次实际执行，2026-09-28）：

- `grep -rn "world_state\|WorldState" include src tests --include=*.hpp
  --include=*.cpp` → 唯一命中 `include/mira/temporal_policy.hpp:121` 注记
  （「DEC-041 WorldState alignment happens in a later stage」）——投影契约面
  确无既有代码，属全新公共契约（DEC-002 版本化与 policy 登记适用）。
- `cat tools/architecture-policy.json` → core roots `["include/mira","src"]`、
  requires `["executor"]`；tests requires 已含全部外部头族——
  `include/mira/world_state.hpp` + `src/worldstate/` 落点零 policy 差异。
- `grep -n "struct ElementRef" include/mira/observation.hpp` → :345（词表 §2.1
  出处）；`grep -n "### 4.3" docs/design/observation_coordinate_android_host.md`
  → :196（权威冻结定义）；`grep -n "toolref:" docs/design/tool_reference_and_skill_design.md`
  → :104-105（语法 v1）；`sed -n '207,219p' src/workflow/workflow_events.cpp`
  → 20 类闭集；`grep -n "policy_event_schema_name" include/mira/temporal_policy.hpp`
  → :242（九类精确名出口）。
- `grep -n "mira_add_m26_test\|integration;m26" tests/CMakeLists.txt` → :701-721
  （`mira_add_m27_test` 注册形态先例）；`ls docs/plans/` → 最大编号 m26（M27
  顺延）；`ls docs/decisions/` → 最大编号 DEC-045（DEC-046 顺延）。
- `python3 -c "import json;d=json.load(open('tools/architecture-policy.json'))…"`
  → core/workflow/tests 模块声明与上述引用一致。

冻结文本自查与链接复验（本次实际执行）：`python3 tools/check_docs.py .` →
Markdown links and fences: OK；`git status --short` → 变更仅 docs/ 下九个文件
（五改四新），零产品代码与测试变更。总计划同步（头注近况行、§4 里程碑表 M27 行、
§4.1 第 20 条、§5 DEC-038/DEC-041/DEC-046 行）与
[DEC-038](../decisions/DEC-038-unified-behavior-trace.md)/[DEC-041](../decisions/DEC-041-session-world-state-projection.md)/[Temporal Policy 设计](../design/temporal_policy_design.md)
§16 最小事实性回填（状态括注/时点注记指向 M27 与 DEC-046——决策语义零变更，
随本立项同批提交）。无门禁放宽，无范围变更。

2026-09-28（第二次）：独立计划评审发现五项阻塞缺陷（B-A…B-E），本次修订并
补充核验；冻结范围、工作项与门禁数量不变（错误修正而非门禁放宽，协议 §6）。

- **B-A 算子 options 无来源**：原冻结八算子签名不含 `WorldStateOptions`，而
  淘汰/环 FIFO/摘要上界/陈旧界语义处处依赖它，`WorldState` 又不内嵌 options、
  wire 无 options 键——契约按原文不可实现。修订：options 归属冻结为「值类型
  不内嵌、wire 不含 options、八算子全部显式携带 `const WorldStateOptions &`」，
  容量为算子不变量而非值不变量（`validate()` 只查结构不变量，wire 读回无需
  options）；落 §4.1「options 归属冻结」、§4.3 签名块、决策表新行、WS-G2/G4。
- **B-B 重建等价性自相矛盾**：原 `rebuild_world_state` 只收五类输入且明确不含
  `expire_stale`，凡调用过过期的投影无法重建，与 §1/§4.1 无条件等价宣称及
  WS-G3「覆盖过期路径」冲突。修订：输入闭集增第六类 `WorldExpiryInput{now}`、
  `expire_stale` 冻结为 `apply_expiry`（过期指令显式进入序列，含过期推进可
  重放）；`prune_entities` 冻结为非重建语义（便利/审计入口，独立调用不在
  WS-G3 等价断言）；等价措辞统一为「同输入序列（六类）+ 同 options」；落
  §1/§4.2/§4.3、决策表新行、WS-G2/G3/G4。
- **B-C toolref 校验不可链接**：原 `apply_tool_settled` 要求 core 侧做 toolref
  语法校验，但权威解析器 `parse_tool_reference`（`tool_reference.hpp:76`）在
  workflow 模块（`CMakeLists.txt:153` 编入 `mira_workflow`、:163 链接
  `Mira::core`），core 反向依赖被依赖方向禁止。修订：契约弱化为登记级检查
  （非空 + ≤ 256 字节，词表 §2.3 冻结上界），语法一致性归调用方/持有解析器
  一侧；词表 §2.3 增「解析器归属」行、§2.4 增两层目录（见 B-E），词表冒烟
  改由链接 `Mira::workflow` 的测试目标调用权威解析器；落 §4.3/§4.5、§5 第 4
  问、决策表新行、WS-G1。
- **B-D 页面假设三处矛盾**：原 `apply_navigation` 成功时「app_id 沿用现值」
  在 Unknown 出发时产出空 app_id 的 Believed 态（§4.1 合法、§4.5 词表违规、
  算子不拒绝三说不一致），且导航/验证均未冻结 `recognized_at` 更新来源
  （`apply_expiry` 判定依赖它，digest 锚不确定）。修订：`WorldNavigationInput`
  增 `app_id`（空 → 词表违规）、成功以输入 app_id 置 Believed；`recognized_at`
  刷新规则冻结——导航成功取 `observed_at`、验证 confirmed/refuted 取
  `verified_at`、failure/inconclusive 不刷新；§4.1 validate 与 §4.5 词表违规
  注记对齐（非 Unknown 态二元组项均非空）；落 §4.1/§4.2/§4.3/§4.5、§5 第 6
  问、决策表新行、WS-G2/G3。
- **B-E 词表 §2.4 目录事实错误**：`is_workflow_event_type` 闭集是 CamelCase
  类型名而非 `mira.workflow.*.v1` 串（后者内联于各 `to_event_payload`，仓库
  无逐名清单），原 WS-G1「对 20 类 schema 精确名接受」不可执行。修订：词表
  §2.4 重写为两层目录并逐一冻结 20 组（类型名 ↔ schema 名）对应（本次核验：
  `grep -oh '"mira\.workflow\.[a-z-]*\.v1"' src/workflow/workflow_events.cpp |
  sort -u` → 恰 20 个；五处非显然归属经宿主函数 `to_event_payload(const
  WorkflowXxxEvent&)` 相邻行逐一核实）；WS-G1 改两层断言（类型名闭集 ×
  schema 名清单）；落词表 §2.4/§7、DEC-046 决策 2④/验证方式、总计划 §4.1
  第 20 条、WS-G1。
- 核验（本次实际执行）：`grep -n "tool_reference" CMakeLists.txt` → :153
  （`tool_reference.cpp` 入 `mira_workflow` 源清单）；`sed -n '163,163p'
  CMakeLists.txt` → `target_link_libraries(mira_workflow PUBLIC Mira::core
  PRIVATE executor::executor)`；`grep -n "parse_tool_reference"
  include/mira/tool_reference.hpp` → :76/:80；`grep -oh
  '"mira\.workflow\.[a-z-]*\.v1"' src/workflow/workflow_events.cpp | sort -u`
  → 20 个唯一串（`wc -l` = 20）；五处非显然对应（episode/lesson/recovery/
  tool-compat/procedures）经 `grep -n -B 6` 宿主函数行核实。修订后
  `python3 tools/check_docs.py .` → Markdown links and fences: OK。

2026-09-28（第三次）：独立计划评审复验发现两项阻塞缺陷（B1/B2），本次修订并
补充核验；冻结范围、工作项与门禁数量不变（错误修正而非门禁放宽，协议 §6）。

- **B1 from_state 冲突规则无出处**：WS-G2 冻结了「`from_state` 不一致冲突
  条目」用例，但 §4.3 算子表的 `ConflictMarked` 触发只有「被覆盖前值二元组
  不同」，`input.from_state` 无任何算子消费——门禁按字面不可实现。连带发现
  同根缺陷：「覆盖前值二元组不同即冲突」会把正常 A→B 导航每次记为冲突
  （转移噪音淹没冲突语义）。修订：`ConflictMarked` 触发冻结为唯一来源——
  导航 `from_state` 对账（投影现值二元组 ≠ (输入 app_id, from_state) →
  冲突条目，转移语义照常）；`apply_screen_state` 的覆盖冲突条件删除（识别
  是权威新证据，页面切换由 `PageAssumed` 留痕）；落 §4.3 两行、决策表
  「冲突语义」行、§5 Q6、WS-G2/G5。
- **B2 change_sequence 回归双码归属**：字段注记与 §4.5 `RecordInvalid` 清单
  把回归归 `RecordInvalid`，§4.1 `validate` 又归 `StateInvalid`；六类输入
  均不携带该字段，`RecordInvalid`（输入记录结构非法）路径不可达该触发。
  修订：回归唯一归 `StateInvalid`（从 `RecordInvalid` 清单删除）；并明示
  折叠规则——`validate`/`from_json` 的嵌套失败（含词表类值非法）一律折叠
  `StateInvalid`，`VocabularyViolation` 仅产生于算子输入路径，`from_json`
  值层非法 → `StateInvalid`；落 §4.1（validate 注记、from_json 注记）、
  `WorldChange` 字段注记、§4.5 两码注记、§5 Q6、WS-G6（八码可达路径与
  折叠断言）。核验（本次实际执行）：`grep -n "from_state"
  docs/plans/m27-world-state-projection-core.md`（修订前）→ 仅 :242 字段
  声明与 :522 门禁用例两处、算子表零消费；`grep -n "change_sequence" …` →
  :185（RecordInvalid）/ :195（StateInvalid）双归属确认。修订后
  `python3 tools/check_docs.py .` → Markdown links and fences: OK。

2026-09-28（第四次）：`M27-01` 交付轮（实现起点，仅文档变更，未提交——提交由
工作流统一执行）。里程碑状态转 `In Progress`（头注状态行，沿 M26 `M26-01`
交付轮同型先例——立项并跑前冻结即置 In Progress），`M27-01` 工作项与 §6 门禁
复选框保持未勾选，待 Independent-Verification-Agent 复验。总计划同步四处：
头注近况行状态括注、§4 里程碑表 M27 行、§4.1 第 20 条、§5 DEC-041 行状态
括注；[DEC-041](../decisions/DEC-041-session-world-state-projection.md) 头注
「实现未开始」按 DEC-037 同型修订改为「实现进度见该里程碑」（决策语义零变更）。
本提交零代码、零测试目标变更：`include/mira/world_state.hpp`、
`src/worldstate/world_state.cpp`、`tests/m27/` 与 CMake 注册随 `M27-02`–
`M27-04` 交付，本轮无新增可跑对象。门禁复核（本次实际执行）：
`python3 tools/check_docs.py .` → Markdown links and fences: OK（exit 0）；
`python3 tools/check_architecture.py .` → architecture policy is clean
（8 modules、baseline 抑制 17、stale 0，exit 0）；`cmake --preset debug` →
configure/generate 通过（exit 0）；`cmake --build --preset debug` → 全目标
构建通过（exit 0）；`ctest --preset debug -R m27` → No tests were found
（exit 0——m27 测试目标随 `M27-03`/`M27-04` 注册）。B1/B2 修订落点复核
（本次实际执行）：`grep -n "ConflictMarked" docs/plans/m27-world-state-projection-core.md`
→ §4.3 `apply_navigation` 行（:328）、决策表「冲突语义」行（:425）、§5 Q6
（:485）、WS-G2（:532-533）、WS-G5（:558）在案，`apply_screen_state` 行
「不记冲突（冲突触发唯一来源见 `apply_navigation` 对账）」在案（:327）；
`grep -n "change_sequence" docs/plans/m27-world-state-projection-core.md` →
唯一错误归属 `StateInvalid`（:186/:198/:478/:567），§4.5 `RecordInvalid`
清单零命中。

2026-09-28（第五次）：`M27-02` 契约与实现交付轮（未提交——提交由工作流统一
执行；IVA 复验前 `M27-02` 工作项与 §6 门禁复选框保持未勾选）。交付：
`include/mira/world_state.hpp`（§4.1–§4.5 冻结面：选项/值类型、六类输入记录、
八算子签名、`mira.world_state` 八码错误域 + 稳定名 + `make_world_state_error`，
头注冻结不变量）与 `src/worldstate/world_state.cpp`（错误域/校验/八算子/
`rebuild_world_state`）+ `src/worldstate/world_state_wire.cpp` +
`src/worldstate/world_state_wire.hpp`（私有头）——wire 序列化、DEC-002 reader
与输入记录摘要按职责拆分为独立编译单元，源起 `tools/architecture-policy.json`
`max_file_lines=1200` 预算（合并单文件 1269 行触发「new architecture policy
violations block the change」；policy 指引按职责拆分，未加基线例外、未改
policy），拆分后三文件 672/598/30 行，全部在 core 模块 roots 内、零 policy
差异；两 TU 均登记入 `mira_core`（`CMakeLists.txt` source 清单）。

实现取舍（冻结文本未定处，逐条留痕供评审/测试对照）：条目
`source_event_digest` 恒为产生该条目的输入记录 canonical 摘要（决策表「源引用
摘要」行字面语义；`WorldToolSettledInput` 自带 digest 字段作为记录成员被覆盖
其中）；前台「变化」判定忽略 `observed_at`（状态/包/Activity/sensitive 四元
组）；观察置 `Unknown` 前台时 `observed_at` 一并复位；空变化环首条
`change_sequence = 1`；观察路径插入溢出淘汰条目携带观察记录摘要，独立
`prune_entities` 条目为零摘要 + 纪元时间（无输入记录可引；prune 为全函数，
其定宽摘要不经 `PayloadTooLarge` 检查）；`apply_expiry` 对 `Unknown` 页面零
操作（无信念可陈旧，否则产出违反 §4.1 validate 的空二元组 `Stale` 态）、页面
过期条目用 `EntityStaled`（八值闭集中唯一陈旧类）；导航空 `from_state`/
`to_state` → `VocabularyViolation`（词表 §2.2 身份域）；wire：belief/kind 用
枚举名小写闭集、`ElementSource` 用精确枚举名、时间为 wall 纳秒整数、id 为
32 hex、digest 为 64 小写 hex；`from_json` 键序：键集 fail closed →
schema_version 形状 → major 门（`{0,x}`/`{2,x}`+ → `SchemaUnsupported`）→
值解析（成员类型错 → `PayloadMalformed`，成员值非法含坏 hex/越界枚举名/
负无符号 → `StateInvalid`）→ `validate()` 折叠复核。

门禁（本次实际执行，按交付轮约定仅针对性构建自查，全量套件与 CI 归工作流
统一执行）：`cmake --preset debug` → exit 0；`cmake --build --preset debug
--target mira_core` → 两 worldstate 编译单元零警告零错误、`Built target
mira_core`（exit 0）；`python3 tools/check_architecture.py .` → architecture
policy is clean（exit 0）；头文件自包含 `g++ -std=c++20 -fsyntax-only
-Iinclude`（仅含 `world_state.hpp` 的 TU）→ 通过。`tests/m27/` 与
`tests/CMakeLists.txt` 零改动（归 Independent-Verification-Agent 所有）；
`WS-G1`–`G6` 断言对象已就绪，待 IVA 按冻结 §4 签名编写并登记后复验。

2026-09-28（第六次）：IVA 复验报告 `WS-G2` 1/28 用例失败
（`observation_operator_matrix`：同应用后到观察未按 §4.3
「package/activity/sensitive/observed_at 逐字段覆盖」刷新前台
`observed_at`）——实现偏差修正，非门禁放宽：`apply_world_observation` 前台
改为无条件逐字段覆盖（空 `package_name` → 置 `Unknown` 同样无条件），
`same_foreground_content`（状态/包/Activity/sensitive 四元组，不含
`observed_at`）仅用于门控是否追加一条 `PageAssumed` 环条目——前台变化条目
语义（§4.3「前台变化 → 一条 PageAssumed」）不变；
`include/mira/world_state.hpp` 的 `apply_world_observation` 注记同步明确该
语义。
连界效应：冻结数据集同应用后到观察（obs2@2s）的 foreground `observed_at`
随之更新，`WS-G3` 数据集 digest 锚（`kFrozenDatasetDigestHex`）漂移——锚点
重钉归 Independent-Verification-Agent（`tests/m27/m27_support.hpp` 归其
所有，本方零改动）。门禁（本次实际执行，按约定仅针对性构建自查）：
`cmake --build --preset debug --target mira_core` → exit 0；
`python3 tools/check_architecture.py .` → architecture policy is clean
（exit 0）；`python3 tools/check_docs.py .` → Markdown links and fences:
OK（exit 0）。全量套件与 CI 归工作流统一执行。

2026-09-28（第七次）：强门禁 clang-tidy 报
`world_state.cpp:133` `performance-move-const-arg`（`evict_one` 内对
`const std::string summary` 做 `std::move` 无效）——去掉该局部变量的
`const` 使既有移动生效（单行修复，无语义变更）。门禁（本次实际执行）：
`cmake --build build/static-analysis --target mira_core`（clang-tidy
warnings-as-errors 增量树，两个 worldstate 编译单元重检）→ 零诊断、
`Built target mira_core`（exit 0）；`cmake --build --preset debug --target
mira_core` → exit 0。

2026-09-28（第八次）：强门禁 format-check 报
`include/mira/world_state.hpp` clang-format 违例——对四个 worldstate 新文件
（公共头 + 三个 src/worldstate/ 文件）按仓库 `clang-format`（18.1.3，与门禁
同二进制）原地格式化，纯格式重排、零语义变更（行数 343→351/672→665/598→587/
30→30）。门禁（本次实际执行）：`cmake --build build/debug --target
format-check` → 249 files 全部通过（exit 0）；`cmake --build build/debug
--target mira_core` 与 `cmake --build build/static-analysis --target
mira_core`（clang-tidy 增量树）→ 均 exit 0。

2026-09-28（第九次）：强门禁第 3 轮唯一失败定位为既有测试
`tests/integration/executor_lifecycle_test.cpp` 的 flaky 竞态（与本里程碑
零代码耦合，原 :76-87 属 M0 基线交付）：原循环 64 次 submit→cancel→wait 并
要求至少一次观察到「排队中取消」，但 `apply` 纯函数瞬时完成，空闲机器上
worker 出队恒快于取消请求——单次尝试取消获胜概率约 2.5%，64 次全败概率约
20%，ubsan 门禁因此间歇性打红（复核：`build/ubsan/Testing/Temporary/
LastTest.log` 唯一失败项 `mira_executor_integration_test`，断言
`executor_lifecycle_test.cpp:87 check failed: saw_queued_cancellation`）。
修复（测试侧，机制覆盖由概率改确定，非取消语义放宽）：循环保留 64 次但只
断言与交错无关的结算契约（`cancel` ∈ {Cancelled, Applied}、`wait(2s)` ∈
{Applied, Cancelled}，超时/拒绝即失败）+ 已观察 id 再取消返回 `NotFound`
的终态幂等断言；「排队中取消」改由文件末尾新增确定性段验证——promise 阻塞
occupier 钉住唯一 worker，victim 必然在队列中被取消
（`RequestedBeforeStart` + future 以 `TaskCancelled` 结算，in_flight 归零）。
Independent-Verification-Agent 复验（本次实际执行）：该测试 ubsan 20/20、
debug 10/10、tsan 5/5（tsan 需 `setarch -R` 规避本机内核高熵 ASLR 与
sanitizer shadow 映射冲突的环境性启动崩溃，约 90% 复现率、与本次修复无关，
CI runner 无此问题）；全量 `ctest`（ubsan 预设）`100% tests passed,
0 tests failed out of 102`（132.06s）。`WS-G1`–`WS-G6` 与 `M27-01`–
`M27-04` 复选框随本轮取证勾选（IVA 三轮复验通过，m27 两目标 35 用例全绿，
7 条门禁有测试覆盖）；`WS-G7`、本地全门禁与 PR CI 复选框保持未勾选，待
`M27-05`（API 手册新页随其交付，CI 取证随 PR 回填）。

2026-09-28（第十次）：PR CI android（NDK libc++）与 windows（MSVC）双目标
编译失败，根因同一处平台精度问题：`Timestamp::wall`（`core_contracts.hpp`
既有公共类型）原声明为 `std::chrono::system_clock::time_point`，其 tick 精度
随标准库平台定义——libstdc++/libc++ 为纳秒，MSVC 为 100 ns。M27 冻结契约
要求 wall 时间按纳秒整数精确往返（§4.4 wire）且陈旧判定 1 ns 粒度
（§4.3/`WS-G4` 边界用例 `make_expiry(30, 1)`），centi tick 表示在 MSVC 上
既编译不过（纳秒→centi 隐式收窄被拒）也无法表示 1 ns 偏移。修复（公共契约
可移植性泛化，非门禁放宽）：`core_contracts.hpp` 新增 `WallTimePoint`
别名（`time_point<system_clock, nanoseconds>`）并将 `wall` 钉到该类型——
纳秒 tick 标准库上与原类型完全同型（Linux/Android 零行为变更），MSVC 上
获得精确纳秒表示；随迁机械适配六处 wall↔memory/context 契约字段桥点
（`memory_consolidation.cpp` `wall_nanos` 形参 + recorded_at 回填、
`context_working_context_promotion.cpp`/`m23_promotion_support.hpp`
显式 `time_point_cast`），并把 checkpoint/context 快照两处 ns 解码从
「cast 到 clock 自身 duration」升级为 `WallTimePoint` 精确重建（MSVC 上
round-trip 由有损变精确）；`temporal_policy.cpp` 的 `sampled_at_wall_ns`
序列化在粗 tick 标准库上随之从 tick 计数修正为真实纳秒（Linux 不变）。
API 手册 `docs/api/core-runtime.md` Timestamp 条目同步。门禁（本次实际
执行）：`cmake --build build/debug`（mira_core/m27 两目标/m23/m13/
executor 集成测试）→ 零错误；`cmake --build build/android-arm64-release
--target mira_core`（NDK aarch64 libc++，复现 CI 失败配置）→ 零错误；
`cmake --build build/ubsan`（同四目标）→ 零错误；`format-check` →
249 files 通过；`check_docs.py`/`check_architecture.py` → OK/clean。
windows（MSVC）本地无工具链，由 PR CI 验证。
