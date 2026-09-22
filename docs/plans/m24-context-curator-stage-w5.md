# M24：Context Curator Stage W5——Subagent Fork / Merge（快照 fork、局部 delta、curated result 与 parent merge policy）

> 状态：Completed（2026-09-23 跑前冻结并同日交付关闭；`W5-G1`–`G6` 全绿，
> [PR #68](https://github.com/Linductor-alkaid/mira/pull/68) CI 24/24 全绿）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-035](../decisions/DEC-035-context-curator-working-context.md) 第 4 条
> Stage W5；场景边界见
> [DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)）
> 前置：Stage W4 关闭（已满足，[M23](m23-memory-promotion-stage-w4.md)，PR #67
> 2026-09-22 合入）；多 Agent 工作流场景冻结（已满足——该门禁原无文档承载，
> 随本立项以 [DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)
> 冻结，本文件 §4 承载其契约面）
> 建议发布点：非发布物；产出 subagent fork/merge 的确定性契约与边界测试
> （宿主集成轮与真实模型轮/Stage E 评估矩阵的输入形态锚点）
> 更新日期：2026-09-23（立项并交付关闭）

## 1. 目标

按 Context Curator 设计 §13 Stage W5 行与
[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md) 冻结的
场景边界，交付 Subagent fork / merge（全部为确定性纯函数，无模型调用）：

- **快照 fork（只读基线继承）**：`fork_working_context` 从父会话已提交快照
  产出子会话 fork 基线——八 section 逐字副本、身份换为子会话五元组、携带
  fork 溯源（父快照 id / 父会话 / fork 点父水位）。父会话 store 永不被子操
  作写入；fork 不复用 M20 task_epoch 新链的「作废」语义（DEC-044 第 3 条）。
- **局部 delta（curated result）**：子会话按既有 W2/W3 管线维护自身快照链
  （fork 基线 = 子链 `previous`，契约零修改）；子代理结束时经机械投影
  `working_context_delta_from_fork` 产出带 provenance 的局部 delta——继承
  条目剔除、同 section 血统交集判定 supersede、其余为 addition。完整探索
  历史不回传父会话。
- **parent merge policy（机械确定性合并）**：`merge_working_context_delta`
  以引用驱动方式把 delta 合入父快照——supersede 原位替换（父先序保持）、
  stale supersede 丢弃并重分类为 addition（计数可见）、addition 按序追加、
  逐 section 固定顺序截断；产物是普通快照候选，经**既有** §5.2 提交管线落
  库。冲突不做语义取舍（设计 §2 原则 1）；rejected = 宿主不调用合并，子内
  容只存子会话可重建投影与 Raw Trace（issue #48 merge policy 二值的机械对
  应）。
- **快照 schema 1.1 → 1.2 加法 minor**：`WorkingContextSnapshot` 增加可选
  fork 溯源字段（仅 fork 基线非 nil）；`state_digest`/JSON/`validate()` 同
  步加法扩展，v1.0/v1.1 载荷保持可读。digest 纪律（可执行冻结断言，见
  §4.2）：fork 字段仅**非 nil** 时进入 canonical 对象；从 v1.0/v1.1 载荷
  读回的快照保留原 `schema_version`，其 digest 与升级前计算逐位一致
  （[DEC-002](../decisions/DEC-002-public-contract-versioning.md)；升级后
  新产出的 fork=nil 快照以 `{1,2}` 参与 digest——加法 minor 的预期行为，
  非回归，诚实记录见 §4.2/§8）。

## 2. 范围与非目标

范围：新公开头文件 `include/mira/context_working_context_fork.hpp` 与
`src/context/context_working_context_fork.cpp`（入 `mira_core`）——fork
seed、fork 溯源、delta 契约、机械投影、合并报告与 merge 入口；
`context_working_context.hpp` schema 1.2 加法扩展（fork 字段 + 校验/digest/
JSON 同步）；**既有套件戳记同步**：`tests/m21` 的 schema 戳记断言与 eval
报告 environment 版本派生随升版同步更新（§4.2，与 schema 升版同一变更）；
`tests/m24/` 契约/集成测试与确定性 eval harness；文档同步。

非目标：**不实现自动 fork/merge 或自动晋升触发**——fork 与 merge 均为宿主
显式操作，不接入 W3 `WorkingContextAutoCurator` 触发链、无隐藏后台循环/
定时器（AGENTS.md Executor 纪律；自动晋升维持 M23 冻结的宿主显式现状，若
未来需要自动化须上位决策，不得实现期顺手带入）；**不做宿主集成接线**——
Agent Loop 接线与宿主集成示例归独立的宿主集成轮（M23 头注将「Stage W5
subagent fork/merge」与「宿主集成轮」并列为两个后续消费方；接线验收形态无
任何已冻结文档，按阶段冻结协议 §2 写为非目标）；**不做 Workflow fork**——
DEC-019 §2 的分支合并/并行/子 Workflow 保持未承诺扩展位，issue #48 强制
flush 清单中的 `workflow fork` 归该扩展链另行决策（DEC-044 第 2 条）；
**不新增子代理执行面**——无 `SubagentRuntime`、无第三平面、无跨会话租约仲
裁（DEC-044 第 1/6 条）；不修改 `IContextCurator`/`ProviderContextCurator`
契约与子链 curation 语义、`commit_working_context` 五元组纪律（§5.2 全表适
用，不放宽同水位冲突 fail-closed）、Layer 0 既有语义、M19 checkpoint 契约、
W3 触发语义与 W4 晋升映射；不做模型介导的语义合并（机械校验层之上的语义
refine 是后续加法增强，需要独立决策）；不声明语义质量、token 收益或
continuation correctness（RULE-10，无模型；issue #48 对照指标归真实模型轮
与 Stage E，`MNT-202609-27` 证据通道）；本阶段无真实模型网络调用。

## 3. 设计与决策依据

- [DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)（场
  景冻结：控制平面绑定、fork=子会话+溯源、curated result=子链产物、机械合
  并、环境动作边界、W3/W4 共存）——本文件 §4 是其契约面
- [DEC-035](../decisions/DEC-035-context-curator-working-context.md) 第 2/3/4
  条（投影非权威、Memory 边界、W5 归属）
- [Context Curator 设计](../design/context_curator_design.md) §2（原则 1）、
  §4.1（schema 加法 minor 先例与「投机 API」纪律）、§5.2/§5.3（提交纪律与
  store 语义）、§13（Stage W5 行与每 Stage 最低测试矩阵）
- [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)（双平
  面；W-06/W-07 不变量）、
  [DEC-019](../decisions/DEC-019-workflow-ir-contract.md) §2（Workflow 扩展
  位声明）、[核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)
  §9（ActionLease：Planner/Reasoner 不需要 lease）
- [M20](m20-working-context-stage-w1.md)（五元组身份、epoch 新链「作废」语
  义与 live 单五元组取证——fork 身份方案的否定性依据）、
  [M21](m21-context-curator-stage-w2.md)（W2 三段编号转录与 supersede 引用
  纪律——血统交集判定的来源；schema 1.1 加法先例）、
  [M22](m22-working-context-stage-w3.md)（每会话链键控——父/子独立链的依
  据）、[M23](m23-memory-promotion-stage-w4.md)（固定顺序截断先例、宿主显
  式触发先例、「契约门禁即验收面」评估形态先例）
- [DEC-002](../decisions/DEC-002-public-contract-versioning.md)（加法 minor
  与 major 条件）
- `RULE-07`（投影可重建：fork 基线与合并产物均可确定性重导出）、`RULE-08`
  （输入输出全部有界）、`RULE-09`（delta 内容全部 `UntrustedExternalData`
  面语义不变）、`RULE-10`（指标口径诚实）

## 4. 冻结的契约语义（跑前冻结，2026-09-23）

### 4.1 场景边界（承载 DEC-044，不重复裁决）

subagent = 宿主创建的子 Session（父会话旁的普通会话）；fork/merge 是
Working State Plane 的投影操作；子代理是推理角色（Developer/Test/Review），
不含动作路径，如需环境动作在其自身会话按既有 ActionLease 纪律执行。本节不
新增语义，负向冻结（同会话 fork 拒绝、子操作零写父 store）由 §6 W5-G4/
W5-G5 落为测试断言。

### 4.2 快照 fork 与 schema 1.2（加法 minor）

```cpp
// context_working_context.hpp（加法）：
struct WorkingContextForkProvenance final {
    WorkingContextSnapshotId base_snapshot_id;        // fork 源父快照
    SessionId parent_session_id;
    std::uint64_t parent_through_event_sequence = 0;  // fork 点父会话水位（≥ 1）
};
// WorkingContextSnapshot 加法可选字段（schema 1.2）：
std::optional<WorkingContextForkProvenance> fork;     // 仅 fork 基线非 nil
```

```cpp
// context_working_context_fork.hpp：
struct WorkingContextForkSeed final {
    SessionId child_session;                 // 子会话（fork 基线的 ACL owner）
    WorkingContextIdentity child_identity;   // task / task_epoch / environment_epoch
    std::uint64_t child_watermark = 0;       // 子会话当前水位（≥ 1，validate 纪律）
};

// 纯函数：parent_base 必须是父会话已提交快照；产出子会话 fork 基线候选。
[[nodiscard]] Result<WorkingContextSnapshot>
fork_working_context(const WorkingContextSnapshot &parent_base,
                     const WorkingContextForkSeed &seed,
                     const WorkingContextMergeOptions &options = {});
```

固定语义：

- **逐字副本**：八 section（constraints / decisions / open_issues /
  active_tasks / verified_facts / failed_attempts / important_refs /
  next_actions，即结构体声明序）按向量序全量复制，条目的 content /
  source_events / source_sequence / confidence 逐一相等；`source_checkpoints`
  逐字继承（跨会话 `EventId`/checkpoint id 引用合法——全局 128 位标识，可
  追溯性保留，擦除治理边界见 §4.5）。
- **身份换轨**：基线身份 = (seed.child_session, child_identity, 
  seed.child_watermark)；`child_watermark ≥ 1`（子会话至少已记录 fork 指令
  事件；快照 `validate()` 的正水位纪律适用，零水位整体拒绝）。基线不占用父
  会话五元组、不推进父水位、不写父 store。
- **fork 溯源**：`fork = {parent_base.id, parent_base.session_id,
  parent_base.through_event_sequence}`；`validate()` 对非 nil fork 要求三者
  有效（id/会话非 nil、父水位 ≥ 1）。
- **schema 与 digest**：`working_context_schema_current()` → `{1, 2}`；
  JSON 增加可选 `fork` 字段，v1.0/v1.1 载荷缺省该字段可读（`validate_schema_version`
  同 major 既有接受语义；DEC-002 加法 minor，无删除/改义/默认安全行为变化，
  不满足升 major 条件）。digest 纪律（2026-09-23 修订后的可执行冻结断言）：
  沿 [M21](m21-context-curator-stage-w2.md) §4.2 既有冻结公式——canonical
  对象**全量无条件包含、无按版本分支**（`schema_version` 本身入 digest，
  `src/context/context_working_context.cpp:258-263`），fork 字段仅**非 nil**
  时进入 canonical 对象。由此两点可执行推论：
  (a) 从 v1.0/v1.1 载荷读回的快照（读取自载荷恢复原 `schema_version`，同
  文件 `:318-332`）digest 与升级前计算**逐位一致**（W5-G1 断言）；
  (b) 升级后**新产出**的 fork=nil 快照（W1 投影 / W2 curation / merge 产物）
  以 `{1,2}` 参与 digest，与 1.1 计算值不同——这是加法 minor 的预期行为而
  非回归：既有 m20–m23 套件对 `state_digest` 的断言全部为运行期双快照重算
  等值（重建/重放/篡改对照，无硬编码快照 digest 常量锚点；2026-09-23 实现
  前核验，记录见 §9），数据集级 `dataset_digest` 锚点只吃 checkpoint
  `"watermark|text"` 行（`tests/m20/m20_working_context_eval.cpp:208-228`
  同型），不受 schema 演进影响。禁止为使命题 (b) 成立而给 digest 加按版本
  分支或把 `schema_version` 移出 digest——两者分别违反 M21 §4.2 冻结公式与
  加法 minor 纪律（[DEC-002](../decisions/DEC-002-public-contract-versioning.md)）。
- **既有套件戳记同步（2026-09-23 第二次评审核验后显式入范围，随 M24-02
  同一变更交付）**：schema 升 1.2 直接触碰 `tests/m21` 的既有版本期望——
  `m21_context_curator_test.cpp` 四处戳记断言（`:392` 对
  `working_context_schema_current()` 断言 `minor == 1`、`:403-404` 投影
  戳、`:421` 往返戳、`:610` curated 戳）与 `m21_curator_eval.cpp` 两处
  （`:757` faithful 判定含 `candidate.schema_version.minor == 1`，失配会
  翻转恢复/绑定计数使 W2 评估门禁不绿；`:1586` 报告 environment 硬编码
  `snapshot_schema_version="1.1"`，会把 1.2 快照谎报为 1.1 污染证据）。
  处置：前五处按新当前版本同步期望；`:1586` 的字面量改为派生自
  `working_context_schema_current()`（不留需随版本维护的第二处字面量）。
  m20/m22/m23 无任何 schema 戳记断言（2026-09-23 全量 grep 核验，见 §9），
  不在同步范围。
- **幂等重建**：同一 `(parent_base, seed)` 永远派生同一 id 与
  `state_digest`（fork 是纯函数）；输入 `parent_base` 未过 `validate()` 或
  越界 → 整体 `InvalidArgument`，无部分基线。

### 4.3 局部 delta 契约（独立 schema v1）

```cpp
enum class WorkingContextDeltaEntryKind : std::uint8_t { Addition, Supersede };

struct WorkingContextDeltaEntry final {
    WorkingContextDeltaEntryKind kind = WorkingContextDeltaEntryKind::Addition;
    std::size_t superseded_base_index = 0;  // Supersede：fork 基线规范编号
    std::string section;                    // 冻结词表 = 快照八 section 名
    std::string content;
    std::vector<EventId> source_events;     // 逐条非空（子会话事件或继承血统）
    SessionSequence source_sequence = 0;
    double confidence = 0.0;
};

struct WorkingContextDelta final {
    SchemaVersion schema_version = /* mira.working_context.delta.v1, {1,0} */;
    WorkingContextSnapshotId fork_base_snapshot_id;   // 必填
    SessionId child_session_id;
    std::vector<WorkingContextDeltaEntry> entries;    // 有界，固定顺序
    std::size_t inherited_skipped = 0;                // 基线继承剔除计数（可见性）
    ModelProfileId generated_by;                      // 子侧 Curator 实际模型（DEC-036）
    [[nodiscard]] Result<void> validate() const;
};

// 机械投影：从子会话最终快照与 fork 基线产出 delta。纯函数。
[[nodiscard]] Result<WorkingContextDelta>
working_context_delta_from_fork(const WorkingContextSnapshot &fork_base,
                                const WorkingContextSnapshot &child_snapshot,
                                const WorkingContextMergeOptions &options = {});
```

- **子链契约零修改**：子会话按既有 W2 管线 curation（fork 基线 = 子链
  `previous`），本阶段不改 `IContextCurator`。delta 内容来自子会话**已提交**
  快照的过滤面，不重复执行标记过滤（与 W1 投影同一信任边界）。
- **基线规范编号**：fork 基线八 section 按声明序 + 向量序展开为
  `0..N-1`（确定性；Supersede 条目的 `superseded_base_index` 指向该编号）。
- **分类规则（按序，逐条）**：
  1. content 与某基线条目逐字节相等 → `inherited`（剔除，`inherited_skipped`
     计数——基线内容不重复转录）；
  2. 否则，`source_events` 与同 section 某基线条目存在交集（W2 编号转录的
     provenance 绑定使「引用基线条目」表现为携带其父会话事件血统；父子会话
     事件空间不相交，交集即血统证据）→ `Supersede`（取编号最小的命中基线
     条目）；
  3. 否则 → `Addition`。
  跨 section 的语义收敛（如 verified_fact 解决 open_issue）不在机械合并内
  裁决，归父侧下一次 curation（诚实口径，见 §8）。
- **validate() 纪律**：section ∈ 冻结八 section 词表；`source_events` 逐条
  非空；`content` 非空且越 `max_item_chars` 拒绝；Supersede 条目
  `superseded_base_index` 指向的 section 由投影保证一致；每 section 条目数
  与总数越界拒绝（`WorkingContextMergeOptions` 既有上界，RULE-08）；整体失
  败无部分 delta。

### 4.4 parent merge policy（机械确定性合并）

```cpp
struct WorkingContextMergeReport final {
    WorkingContextSnapshot merged;   // 父身份候选（schema 1.2，fork 字段 nil）
    std::size_t additions_appended = 0;
    std::size_t supersedes_resolved = 0;       // 原位替换成功
    std::size_t supersedes_dropped_stale = 0;  // 目标已不在父快照，重分类 addition
    std::size_t truncated_entries = 0;         // 固定顺序截断计数
};

// 纯函数：把 delta 合入父快照，产出候选；提交走既有 §5.2 管线（本入口不写 store）。
[[nodiscard]] Result<WorkingContextMergeReport>
merge_working_context_delta(const WorkingContextSnapshot &fork_base,
                            const WorkingContextSnapshot &parent,
                            const WorkingContextDelta &delta,
                            const WorkingContextIdentity &parent_identity,
                            std::uint64_t parent_watermark,
                            const WorkingContextMergeOptions &options = {});
```

裁决顺序（全部固定，无随机性）：

1. **前置校验**：三个输入各自 `validate()` 失败 → 整体 `InvalidArgument`；
   `fork_base.id != delta.fork_base_snapshot_id` → `fork-base-mismatch`；
   `fork_base.session_id == parent.session_id` → `same-session-fork`
   （DEC-044 第 3 条负向冻结）；`parent_identity` 与 `parent` 身份不一致 →
   `InvalidArgument`。
2. **supersede 扫描**（按基线规范编号序）：Supersede 条目在其 section 内找
   第一个 content 相等的父条目 → **原位替换**（父位置与父先序保持，
   `supersedes_resolved`）；无命中（父侧已推进/移除该基线内容）→
   `supersedes_dropped_stale`，条目重分类为 addition。
3. **addition 追加**：剩余 addition 按 delta 顺序追加到对应 section 尾部。
4. **上界截断**：逐 section 超过 `max_items_per_section` 时按固定顺序截断
   （父保留条目与原位替换先于追加的 addition），`truncated_entries` 计数
   （M23 §4.1 固定顺序截断先例，RULE-08）。
5. **候选字段**：身份 = (parent.session_id, parent_identity,
   parent_watermark)；`source_checkpoints` = 父快照逐字继承（子侧溯源由条目
   `source_events` 承载，不并入 checkpoint 链）；`generated_by` =
   **继承 `parent.generated_by`**（2026-09-23 评审修订：`state_digest` 无
   条件包含 `generated_by`，`src/context/context_working_context.cpp:271`，
   置 nil 会使零效果合并候选与 curated 父快照在同水位 digest 恒异、第 7 条
   的 `IdempotentNoOp` 断言不可达；继承后合并产物延续父链 curation 归属，
   子侧模型归属仍由 `delta.generated_by` 承载、合并行为由报告计数承载）；
   `fork` = nil。
6. **提交**：候选经**既有** `commit_working_context` + live 父状态落库，
   §5.2 全表适用、一条不放宽——**合并只在父水位严格前进时可提交**：同水位
   异 digest → `conflicting-watermark` fail-closed（设计 §5.2 纪律，合并产
   物不豁免；宿主操作序为「子返回事件进入父会话 → 父水位前进 → 合并」，
   W5-G4 冻结该负向行为）。无第二提交路径（协议 §5）。
7. **幂等与确定性**：同一输入组永远产出逐字节相同候选与报告计数（跨进程字
   节一致，W5-G3/W5-G6）；零效果 delta（全 inherited）产出与父快照在全部
   digest 覆盖字段上逐字段相同（含继承的 `generated_by`）的候选，提交行为
   由 §5.2 既有规则自然裁决——同水位同 digest → `IdempotentNoOp`（对
   `generated_by` 为 nil 的确定性父快照与 ≠ nil 的 curated 父快照普遍成
   立）；效果非零的合并在同水位仍为 `conflicting-watermark` fail-closed
   （第 6 条不豁免）。
8. **rejected 语义**：merge policy 的 rejected 分支 = 宿主不调用合并；子代
   理内容仅存于子会话可重建投影（子链）与 Raw Trace，父快照不变（RULE-07）。
   合并报告即宿主侧审计与晋升决策依据。

### 4.5 生命周期与边界

- **触发**：fork 与 merge 均宿主显式调用；无自动 fork/merge、无自动晋升、
  不新增 Supervisor 方法——装配后的候选经既有
  `ContextMemorySupervisor::schedule_working_context_commit`（Deferrable）
  提交，future 必须被调用方消费（M22/M23 先例）。
- **W3 共存**：`WorkingContextAutoCurator` 每会话链键控不变；父、子会话链状
  态天然独立（无共享重臂锚点或 in-flight 槽位），宿主自行决定是否为子会话
  运行自动 curation；W5 零触发语义修改（W5-G5 协同断言）。
- **W4 共存**：晋升仍是每快照宿主显式操作；合并产物按 M23 冻结的
  section→kind 映射晋升（映射只依赖 section 名与条目内容，与条目 provenance
  的会话来源无关）；未合并分支内容不进入父快照、不随父晋升（W5-G5）。
- **擦除**：`erase_session(子会话)` 只删子会话快照投影，父快照与已合并内容
  不变；合并条目 provenance 指向的子会话事件属 Trace Plane，受既有 Erasure
  治理；跨会话隐私擦除编排（连同派生内容）归宿主职责（与 M23 §4.4 同款边
  界，W5-G5 负向冻结）。
- **恢复**：空 store 重建——父快照走既有重建路径；子 fork 基线 =
  `fork_working_context(重建的父基线快照, 同一 seed)` 幂等同 id 同 digest；
  子链其余快照按既有规则从子 checkpoint 重导出（RULE-07）。
- **Executor 路由**：fork/delta/merge 本体是有界纯计算；提交经既有
  Deferrable 路由；取消探针透传、被取消的在途工作 resolve `Cancelled` 且
  store 零部分写入、`begin_shutdown` 后提交被拒（§6 W5-G5；supervisor 既有
  §17.2 关闭顺序）。

## 5. 阶段冻结协议必答八问（2026-09-23）

1. **行为**：新增快照 fork（子会话基线 + 溯源）、局部 delta 机械投影、
   parent merge 机械合并三组纯函数与快照 schema 1.2 加法字段。对应设计：
   [Context Curator 设计](../design/context_curator_design.md) §13 W5 行
   （实现注记随交付回填）、DEC-044 全部条目。
2. **所有者**：无新增可变状态——三组入口均为无状态纯函数；快照 store 唯一
   写路径仍是 `commit_working_context`（fork/merge 只产出候选）；子会话与父
   会话快照各以其 `SessionId` 为 ACL owner（store 既有键控）。
3. **契约**：最小公共契约面 = `context_working_context_fork.hpp` 全部
   （seed/溯源/delta/报告/两个入口）+ `context_working_context.hpp` 的
   schema 1.2 加法字段；错误语义 §4.2/§4.3/§4.4（`InvalidArgument`、
   `fork-base-mismatch`、`same-session-fork`）；调用方为宿主与测试（Curator/
   Layer 0/W3/W4 不感知）。契约四件套：公开头文件 + 本文件 §4 + API 手册
   `docs/api/context-memory.md` 条目 + `tests/m24/`。
4. **层与依赖**：新文件入 `core` 模块（`include/mira` + `src/context`），仅
   依赖既有 `mira` 头与标准库；`tools/architecture-policy.json` 零差异
   （`core` requires 不变，无新外部头族）。
5. **复用**：提交复用 §5.2 `commit_working_context`（唯一写路径）；子链
   curation 复用 W2 `IContextCurator` 与 W3 自动触发（零修改）；上界复用
   `WorkingContextMergeOptions`；supersede 血统语义源于 W2 编号转录纪律；
   截断复用 M23 固定顺序先例。不新增平行 merge/commit 路径。
6. **时序**：幂等键 = 五元组 + 水位 + digest（既有 §5.2 表全适用）；合并仅
   在父水位严格前进时可提交，同水位异 digest fail-closed 不豁免（§4.4 第 6
   条）；过期规则 = 终态迟到/五元组不匹配/水位回退照旧丢弃；取消与 deadline
   由既有 Deferrable 路由承担，合并本体是有界纯计算。
7. **Executor 路由**：全部经既有 `schedule_working_context_commit`
   Deferrable（非新路由）；句柄由宿主持有并消费；关闭顺序 = supervisor 既
   有序列（§4.5）。
8. **验证上下文**：评审者最小阅读集 = 本文件 §4、
   [DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)、
   `include/mira/context_working_context_fork.hpp`、
   `include/mira/context_working_context.hpp`（1.2 字段）、
   `tests/m24/m24_fork_merge_test.cpp` 与 `tests/m24/m24_fork_merge_eval.cpp`
   （label `integration;m24`）。

### 决策记录表

| 决策点 | 选择 | 被否备选 | 依据 |
| --- | --- | --- | --- |
| 执行面绑定 | Agent Harness 控制平面：subagent = 父会话旁的子 Session（DEC-044） | Workflow 子运行（DEC-019 §2 分支/并行/子 Workflow 是未承诺扩展位，需 IR 破坏性扩展 + W-07 仲裁）；独立 SubagentRuntime（第三执行平面，无消费场景的投机 API） | DEC-044 第 1/2 条；issue #48 场景主体是推理隔离 |
| fork 身份 | 子会话五元组 + fork 溯源字段（schema 1.2 加法） | 同 session bump task_epoch（M20 epoch 是「作废」语义，live 单五元组阻止并行链，W1-G4 取证）；同 session 兄弟 TaskId（同病：live 校验 + 水位交错 + W-07 + erase 连坐） | §4.2；DEC-044 第 3 条；`context_working_context.hpp` 五元组纪律 |
| delta 形态 | 独立契约 + 自有版本化 schema v1（新公共头） | 局部载荷塞进快照 schema（稀释 `validate()` 与整替换语义，投机 API）；snapshot 升 major 2.0（无 DEC-002 条件） | §4.3；DEC-002；M21 §4.2 加法先例 |
| supersede 判定 | 机械血统交集（同 section、基线规范编号、父子事件空间不相交） | 模型在 merge 时声明 supersede（v1 引入模型介导裁决，破坏确定性门禁口径）；调用方自由声明 supersede（同一裁决两个来源，歧义面） | §4.3；W2 编号转录 provenance 绑定语义 |
| merge 策略 | 机械确定性合并：引用驱动 + stale supersede 重分类 + 固定顺序截断；产物经既有 §5.2 提交 | W2 式模型介导合并作 v1（非确定，同水位 fail-closed 误杀合理合并，门禁需脚本模型口径）；整体替换（回滚父侧推进、退化覆盖富快照）；为 merge 放宽同水位冲突（破坏 §5.2 冻结纪律，第二写路径） | §4.4；设计 §2 原则 1；协议 §5 |
| curated result 形态 | 子侧既有 `IContextCurator` 管线产物 + 机械 delta 投影（fork 基线 = 子链 previous） | 新增子代理专用 Curator 契约（平行路径）；子代理直写父 store（破坏 ACL/水位/审计） | §4.3；DEC-044 第 4 条 |
| 触发方式 | fork/merge 均宿主显式；不新增 Supervisor 方法 | 挂接 W3 AutoCurator 自动 fork/merge（改 W3 冻结键控语义 + 隐式派生子代理）；新增专用路由包装（为单一调用方扩路由面） | §4.5；M23 §5 触发方式先例 |
| 同水位提交 | 不豁免：合并候选同水位异 digest → `conflicting-watermark` fail-closed；合并只在父水位严格前进时可提交 | 为 merge 开同水位提交例外（静默覆盖父侧同水位状态，违反设计 §5.2「冲突不静默覆盖」） | §4.4 第 6 条；W5-G4 负向冻结 |
| 合并候选 `generated_by` | 继承 `parent.generated_by`（digest 无条件包含该字段） | 强制置 nil（零效果合并候选与 curated 父快照同水位 digest 恒异，`IdempotentNoOp` 断言不可达——2026-09-23 评审核验 `src/context/context_working_context.cpp:271/:795-802` 后否决） | §4.4 第 5/7 条；W5-G3 |
| W4 晋升/自动晋升触发 | 晋升映射与时点维持 M23 冻结现状；自动晋升触发为 W5 非目标（如需自动化须上位决策） | 把自动晋升并入 W5（被 M23 §5 决策表留痕否决的形态，动机未论证）；fork 分支单独晋升语义（未合并内容不进父快照即自然不随父晋升，无需新语义） | §2/§4.5；M23 §5 决策表 |
| 宿主集成接线 | W5 非目标，归独立宿主集成轮 | 并入 W5（接线验收形态无任何已冻结文档，八问不可答；冻结面膨胀违反每阶段最小冻结） | §2；M23 头注两消费方分立表述 |
| 评估形态 | 契约/集成测试 + 冻结 fork/merge 链确定性 eval harness（digest 锚定、跨进程字节一致）；无模型无语义声明 | 真实模型对照指标（归真实模型轮与 Stage E，`MNT-202609-27` 通道，RULE-10 不允许无对象声明） | §6；RULE-10；M23 §5 评估形态先例 |

## 6. 测试与退出条件

门禁形态说明：W5 无模型调用（机械合并口径）；契约面落 `tests/m24/` 契约/
集成断言，确定性面落冻结 fork/merge 链数据集 eval harness（digest 锚定），
两目标 label 均为 `integration;m24`，测试的编写、运行与 sanitizer 取证由
Independent-Verification-Agent 独立完成并复验。

- [x] `W5-G1` fork 契约（`tests/m24/m24_fork_merge_test.cpp`）：八 section
  逐字复制（content/provenance/sequence/confidence 逐一相等）；
  `source_checkpoints` 逐字继承；身份 = 子五元组 + 子水位 ≥ 1（零水位拒
  绝）；fork 溯源三字段正确且 `validate()` 冻结；schema 1.2 加法（v1.0/
  v1.1 载荷读回保留原 `schema_version` 且 digest 与升级前计算逐位一致；
  fork 字段仅非 nil 进入 canonical 对象，无按版本分支——沿 M21 §4.2 冻结
  公式）；同输入幂等同 id 同 digest；无效父快照整体 `InvalidArgument`。
- [x] `W5-G2` delta 契约（同文件）：机械投影三分类（inherited 剔除计数、
  同 section 血统交集 → Supersede 取最小编号、其余 Addition）；冻结八
  section 词表（越词表拒绝）；provenance 逐条非空与上界纪律；`fork-base-
  snapshot-id` 必填；delta schema v1 JSON 往返；`generated_by` 记录子侧
  profile；无效子快照整体拒绝。
- [x] `W5-G3` merge 策略（同文件）：supersede 原位替换且父先序保持；
  stale supersede 丢弃计数并重分类 addition；addition 按 delta 序追加；
  逐 section 固定顺序截断与计数；`generated_by` 继承 `parent.generated_by`
  （nil 父与 curated 父两种形态均断言）；前置校验三拒绝（`fork-base-
  mismatch`/`same-session-fork`/身份不一致）；零效果 delta → 与父在全部
  digest 覆盖字段上逐字段相同的候选（两类父快照同水位提交均为
  `IdempotentNoOp`）；效果非零合并在同水位 → `conflicting-watermark`；同
  输入组跨进程候选与报告计数逐字节一致。
- [x] `W5-G4` 提交与竞态贯通（同文件）：合并候选经既有
  `commit_working_context` 在父水位严格前进时提交成功；同水位异 digest →
  `conflicting-watermark` fail-closed 且 store 不变（合并不豁免）；水位回
  退/五元组不匹配/终态迟到照旧丢弃；父链与子链隔离（两会话并行提交互不影
  响，无跨会话水位干扰）；fork 后父侧 epoch 变化开新链、合并到旧身份候选
  被拒。
- [x] `W5-G5` 生命周期与边界（同文件）：Deferrable 路由（正常完成且 future
  被消费；`begin_shutdown` 后提交被拒；在途取消 → `Cancelled` 且 store 零
  部分写入）；`erase_session(子会话)` 后父快照与已合并内容不变、子快照清
  空；W3 协同——父/子会话 AutoCurator 链状态互不重臂、互不抢 in-flight 槽
  位（复用 m22 契约面断言）；W4 协同——合并后父快照按既有映射晋升成功、未
  合并分支内容零晋升路径；子会话全操作对父 store 零写入（场景负向冻结）。
- [x] `W5-G6` 恢复重建与确定性（`tests/m24/m24_fork_merge_test.cpp` +
  `tests/m24/m24_fork_merge_eval.cpp`）：空 store 重建——父快照既有路径 +
  子基线经 fork 纯函数重建同 id 同 digest；eval harness 在冻结 fork/merge
  链数据集（digest 锚定）上跑投影/分类/合并/提交全链，报告跨进程字节一致。
- [x] 本地门禁：debug 全量 ctest 全绿（含新增 m24 两目标）、ASAN/UBSAN/TSAN
  m24 目标零报告、`format-check`/`docs-check`/`platform-boundary-check`/
  `sbom-check`/`architecture-check` 通过、clang-tidy 预检被改库源编译单元
  零违例、本机 NDK 两 ABI 交叉编译预演通过；既有 m20–m23 套件在 `M24-02`
  戳记同步落地后全绿——m21 的五处 schema 戳记断言与 eval 报告 environment
  版本派生随 1.2 升版同步（§4.2；2026-09-23 核验确认这些断言存在且升版必
  碰，见 §9；未同步前该套件必然红灯，不构成「零回归」证据）；既有套件的
  `state_digest` 断言为运行期重算等值、无硬编码快照 digest 锚点，
  `dataset_digest` 数据集级锚点不受 schema 演进影响；m21 eval 报告
  environment 派生修复后不再把 1.2 快照谎报为 1.1。
- [x] 文档同步完成（§7 `M24-04` 清单；与实现同一变更提交）。
- [x] PR CI（Linux/Windows/Android/sanitizers/quality）全绿后回填验证记录
  并关闭本阶段。

## 7. 工作项

- [x] `M24-01` 阶段立项与本文件 §4/§5/§6 冻结 + [DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)
  交付（时间戳先于任何实现与测试）。
- [x] `M24-02` 公开契约与实现：`context_working_context.hpp` schema 1.2 加
  法扩展（fork 溯源字段、validate/digest/JSON 同步）+
  `context_working_context_fork.hpp/.cpp`（seed、delta 契约、机械投影、
  merge 入口与报告）；既有 m21 套件戳记同步随同一变更交付（§4.2：四处
  test 戳记断言按新当前版本同步 + eval faithful 判定同步 + eval 报告
  environment 版本派生自 `working_context_schema_current()`；测试的运行与
  取证由 Independent-Verification-Agent 复验）。
- [x] `M24-03` 契约/集成测试矩阵（§6 W5-G1–G6，`tests/m24/`，CMake 注册
  `mira_m24_fork_merge_test` 与 `mira_m24_fork_merge_eval`，label
  `integration;m24`）——测试的编写、运行与 sanitizer 取证由
  Independent-Verification-Agent 独立完成并复验。
- [x] `M24-04` 文档同步：Context Curator 设计 §13 实现注记、
  [DEC-035](../decisions/DEC-035-context-curator-working-context.md) 验证
  方式与关联计划回填、总计划索引与 §4.1 注记、API 手册
  `docs/api/context-memory.md` 新头文件条目、README 能力表、术语表（词条已
  随本立项登记）。
- [x] `M24-05` 本地全门禁与 PR CI 取证回填。

## 8. 风险与阻塞

- 风险：场景边界与宿主/产品侧对「多 Agent」的真实期待错位（并行环境操作或
  Workflow 编排）。处置：DEC-044 即立项评审的显式确认对象；workflow fork 归
  DEC-019 扩展链另行决策（§2 非目标留痕）。
- 风险：机械合并不做语义裁决——stale supersede 重分类与 addition 追加可能
  造成同 section 内容重复，语义收敛依赖父侧下一次 curation。处置：合并报告
  全计数透出；语义收敛/去重声明归真实模型轮与 Stage E（RULE-10），本阶段只
  冻结机械行为。
- 风险：血统交集判定依赖「父子会话事件空间不相交」（`EventId` 全局唯一
  128 位标识）与 W2 provenance 绑定语义。处置：W5-G2 冻结分类行为；若未来
  provenance 绑定语义变化，须按决策变更复核本节。
- 风险：schema 1.2 使升级后新产出快照（含 fork=nil）的 digest 输入变化，被
  质疑影响既有评估锚点。处置：digest 公式维持 M21 §4.2 冻结的全量无条件包
  含、无按版本分支（不给 digest 加版本分支、不移出 `schema_version`）；
  W5-G1 冻结的可执行断言为「v1.0/v1.1 载荷读回保留原 `schema_version`、
  digest 与升级前逐位一致」+「fork 字段仅非 nil 进入 canonical 对象」；新
  产出 fork=nil 快照以 `{1,2}` 参与 digest 属加法 minor 预期行为——既有
  m20–m23 套件无硬编码快照 digest 锚点、数据集级锚点不受影响（2026-09-23
  实现前核验，见 §9），实现后仍复跑 m20–m23 套件作回归门禁（§6 本地门禁
  条目）。
- 风险：schema 升版必碰既有套件的版本戳记期望（m21 四处 test 断言 + eval
  faithful 判定与报告 environment 字面量），若不在交付范围内会以「未授权红
  灯」阻塞 M24-02 第一步。处置：戳记同步显式纳入 §2 范围与 `M24-02`（§4.2
  逐点列明），eval environment 改为派生自 `working_context_schema_current()`
  不留字面量；m20/m22/m23 无戳记断言（2026-09-23 核验，§9）。
- 风险：合并候选字段选择与 digest 契约交互产生不可达断言（如 `generated_by`
  置 nil 使零效果合并的 `IdempotentNoOp` 对 curated 父快照不可达）。处置：
  候选字段冻结时逐字段对照 digest 覆盖面（`generated_by` 继承父值，§4.4 第
  5 条决策留痕）；W5-G3 对两类父快照形态均断言。
- 外部：无（不依赖真机、凭据、外部语料或真实模型；宿主集成轮与真实模型轮
  为显式非目标，不阻塞本阶段）。

## 9. 验证记录

2026-09-23：`M24-01` 立项，本文件 §4/§5/§6/§7 冻结（先于任何实现与测试），
[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md) 同日交
付。进入门槛复核：Stage W4 已关闭（[M23](m23-memory-promotion-stage-w4.md)，
PR #67 双 pipeline CI 24/24）；设计 §13 Stage W5 门禁第二项「多 Agent 工作
流场景冻结」原无文档承载（全语料仅 §13 W5 行一处占位），随 DEC-044 冻结满
足。工作分支 `feat-m24-context-curator-stage-w5`。实现未开始；后续验证记录
按日期追加。

2026-09-23（第二次）：独立计划评审发现阻塞级矛盾——原冻结断言「fork=nil 时
digest 与 1.1 计算逐位一致」（§1/§4.2/W5-G1/§8）按字面不可实现：
`state_digest()` 的 canonical 对象无条件包含 `schema_version`
（`src/context/context_working_context.cpp:258-263`），schema 升 1.2 后同
一内容的 hash 输入必然不同，且 [M21](m21-context-curator-stage-w2.md) §4.2
已冻结「digest 公式为全量无条件包含（无按版本分支）」；使命题成立只有给
digest 加按版本分支（违反该冻结公式）或把 `schema_version` 移出 digest
（改变全部既有 digest 值，非加法变更），均超出
[DEC-002](../decisions/DEC-002-public-contract-versioning.md) 授权。本次修
订把断言改写为可执行真命题（§1/§4.2/§6/W5-G1/§8）：fork 字段仅非 nil 进入
canonical 对象；v1.0/v1.1 载荷读回保留原 `schema_version`、digest 与升级前
逐位一致；新产出 fork=nil 快照以 `{1,2}` 参与 digest（预期行为，非回归）。
冻结状态与门禁数量不变（未放宽任何门禁，属错误修正而非门禁变更——协议 §6）。
本次实际执行的核验命令与结果：`grep -n "schema_version"
src/context/context_working_context.cpp` → `:258-263`（digest 无条件包含）、
`:318-332`（JSON 读取自载荷恢复 `schema_version` 后过
`validate_schema_version`）；`include/mira/core_contracts.hpp:237-248` 同
major 接受、更新 major 与跨 major 回退拒绝；`grep -rn "state_digest"
tests/m20 tests/m21 tests/m22 tests/m23` → 14 处断言全部为运行期双快照重算
等值（m20:138/146/444、m21 test:445/498-511/711、m21 eval:923/947/1101/
1508）；`grep -rEn '"[0-9a-f]{32,}"' tests/m2{0,1,2,3}` → 仅 m20/m21/m22
eval 的 `dataset_digest` 常量与 m23 晋升 record id 常量（后者 seed 不含
schema_version）；`awk` 复核 `tests/m20/m20_working_context_eval.cpp:208-228`
——dataset digest 输入为排序后的 checkpoint `"watermark|text"` 行，不含快
照 digest 或 schema；`python3 tools/check_docs.py .` → Markdown links and
fences: OK。

2026-09-23（第三次）：独立计划评审发现两项阻塞缺陷，本次修订并补充核验。

- **B1 冻结主张与既有套件冲突、§9 核验不完整**：§4.2 冻结
  `working_context_schema_current()` → `{1,2}`，但既有 m21 套件存在直接断言
  当前 schema 戳记为 1.1 的测试，升版必红，而原「既有 m20–m23 套件零回归」
  的核验集（state_digest / hex 常量 / dataset digest 组成）漏掉了 schema
  戳记断言这一直接影响面，且 `M24-01`–`05` 与 §2 均未包含修改 tests/m21 的
  授权。本次逐点复跑核验属实：`tests/m21/m21_context_curator_test.cpp:392`
  （对 `working_context_schema_current()` 断言 `minor == 1`）、`:403-404`
  （`working_context_from_checkpoint` 投影戳）、`:421`（`make_full_snapshot`
  默认 `schema_version = current`，JSON 往返后断言 `minor == 1`）、`:610`
  （curated 快照戳）；`tests/m21/m21_curator_eval.cpp:757`（eval faithful
  判定含 `candidate.schema_version.minor == 1`，失配翻转恢复/绑定计数使
  W2 评估门禁不绿）、`:1586`（报告 environment 硬编码
  `snapshot_schema_version="1.1"`——测试自洽可通过，但实现后复跑 m20–m23
  作回归门禁时会把 1.2 快照谎报为 1.1，污染证据）。另以
  `grep -rEn "minor ==|major ==|schema_version" tests/m20 tests/m22
  tests/m23`（退出码 1，零命中）核实 m20/m22/m23 无戳记断言。修订：戳记同
  步显式纳入 §2 范围、§4.2 逐点列明与 `M24-02`（含 eval environment 派生
  化），§6/§8 的「零回归/已核验」表述据实改写。
- **B2 merge 候选 `generated_by = nil` 与 `IdempotentNoOp` 冻结断言自相矛
  盾**：`state_digest` 无条件包含 `generated_by`
  （`src/context/context_working_context.cpp:271`），同水位提交以 digest 相
  等判 `IdempotentNoOp`、不等判 `conflicting-watermark`
  （`src/context/context_working_context.cpp:795-802`）；父快照为 curated
  （`generated_by ≠ nil`，父链跑 W2 curation 的常态）时，置 nil 的零效果合
  并候选与父快照 digest 必然不同 → 冻结的 `IdempotentNoOp` 断言不可达。修
  订取评审方案 (a)：候选**继承 `parent.generated_by`**（子侧模型归属仍由
  `delta.generated_by` 承载，合并行为由报告计数承载），零效果候选与父在全
  部 digest 覆盖字段上逐字段相同，`IdempotentNoOp` 对两类父快照普遍成立；
  被否备选 (b)（保留 nil 并把断言限定于 nil 父）落决策记录表留痕。修订落
  §4.4 第 5/7 条、W5-G3、决策记录表（新增一行）与 §8 风险。
- 两次核验均为本次实际执行；`python3 tools/check_docs.py .` → Markdown
  links and fences: OK。冻结状态与门禁数量不变（错误修正而非门禁放宽，协
  议 §6）。
- 2026-09-23（第三次，交付复核）：`M24-01` 关闭前逐点复跑上列证据，结果一
  致——`grep -n "minor == 1" tests/m21/m21_context_curator_test.cpp` →
  `:392/:403/:421/:610` 四处戳记断言、
  `grep -nE "minor == 1|snapshot_schema_version"
  tests/m21/m21_curator_eval.cpp` → `:757`（faithful 判定）与 `:1586`
  （environment 字面量）；`grep -rEn "minor ==|major ==|schema_version"
  tests/m20 tests/m22 tests/m23` 退出码 1（零命中）；`grep -nE
  "generated_by|schema_version" src/context/context_working_context.cpp` →
  `:261-263` 与 `:271`（`schema_version`/`generated_by` 均无条件入
  digest）、`grep -nE "IdempotentNoOp|conflicting-watermark"` →
  `:796/:800`（同水位 digest 等判 `IdempotentNoOp`、异判
  `conflicting-watermark`）。`python3 tools/check_docs.py .` → Markdown
  links and fences: OK；`cmake --preset debug` 配置与
  `cmake --build --preset debug` 构建通过（本工作项仅文档变更，无源码改
  动；`ctest --preset debug -R m24` 无匹配目标——`tests/m24/` 随
  `M24-02`/`M24-03` 交付）。`M24-01` 据此勾选；里程碑状态维持 `Planned`
  （实现未开始，`M24-02` 起进入实现）。


2026-09-23（第四次，交付）：`M24-02`–`M24-04` 同一变更交付（提交 `4e10449`，
[PR #68](https://github.com/Linductor-alkaid/mira/pull/68)）。产品面：
`context_working_context.hpp` schema 1.2 加法扩展（`WorkingContextForkProvenance`
+ 可选 `fork` 字段，validate/digest/JSON 同步——fork 仅非 nil 进入 canonical
对象，v1.0/v1.1 载荷读回保留原戳记且 digest 逐位一致）与新公开契约
`include/mira/context_working_context_fork.hpp` +
`src/context/context_working_context_fork.cpp`（fork 基线、局部 delta 机械投影、
parent merge 机械合并三组纯函数与报告；候选 `generated_by` 继承父值，经既有
§5.2 管线提交）；m21 既有套件戳记同步随同一变更交付（§4.2 六处）。测试矩阵
`M24-03` 由 Independent-Verification-Agent 独立交付：`tests/m24/` 22 用例
（`W5-G1`–`G6`，label `integration;m24`）+ 冻结 fork/merge 链确定性 eval
harness。IVA 循环共发现并修复三处测试侧缺陷（均经升级裁决授权、语义零改动）：
`reject_mutant` lambda 缺结尾 return（编译修复）、w3 夹具全局 gate 双会话
死锁（gdb 取证后改会话化 gate）、父路径漏 drain（按
`context_working_context_auto.hpp` 契约补 `await_auto_settlement`）；产品侧
据评审修订一处 supersede 解析口径（§4.4「在其 section 内找 content 相等的
父条目」扫描语义），并在 clang-tidy 强门禁下移除
`context_working_context.cpp` 对可平凡拷贝 fork 溯源的无效 `std::move`
（`performance-move-const-arg`）。本地取证：`mira_m24_fork_merge_test`
22/22 PASS、`mira_m24_fork_merge_eval` 全绿（`runs_byte_identical:true`）、
`ctest -R "m24|m21"` 4/4 通过；静态分析 preset 下 `mira_core` clang-tidy
（`--warnings-as-errors=*`）零违例；format/docs/sbom/platform-boundary/
architecture 五项检查全绿（architecture 的 max-file-lines 新违规按裁决把
套件按职责拆分为契约 TU + 生命周期 TU + 共享助手头，ctest 仍两目标）。
sanitizer 与 NDK 交叉编译面按 §6 冻结口径由 CI 管线复跑取证（见下条），
本记录不另行声明本地 sanitizer/NDK 取证。

2026-09-23（第五次，关闭）：`M24-05` 完成——
[PR #68](https://github.com/Linductor-alkaid/mira/pull/68)（head `4e10449`）
CI 24/24 全部 SUCCESS：linux（gcc/clang × Debug/Release）、windows
（Debug/Release）、android（arm64/x86_64 NDK 交叉）、sanitizers（ASAN/UBSAN/
TSAN）与 quality 管线全绿，即 §6 本地门禁条目所列各面（含既有 m20–m23
套件在戳记同步后复跑）由 CI 全量复跑证实。`W5-G1`–`G6` 与 `M24-01`–`M24-05`
全部勾选，里程碑转 `Completed`。遗留（非本阶段范围，维持 §2 非目标留痕）：
宿主集成接线、自动 fork/merge 与自动晋升触发、模型介导语义合并、真实模型/
Stage E 语义指标（`MNT-202609-27` 证据通道，RULE-10）。