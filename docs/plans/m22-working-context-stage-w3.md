# M22：Working Context Stage W3——Supervisor 自动触发（watermark / event count / task boundary、coalescing、forced flush、失败回退）

> 状态：In Progress（2026-09-15 立项；维护者指示「依设计与计划推进下一步开发」，
> 与 M8–M21 同一授权模式）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-035](../decisions/DEC-035-context-curator-working-context.md) 第 4 条
> Stage W3）
> 前置：M21 / Stage W2 关闭（已满足，PR #53 于 2026-09-15 合入）；快照链在长
> 会话基线上可复现（已满足，[curation 评估 v1](../benchmarks/context-intelligence-working-context-curation-v1.md)：
> 12 会话 × 5 轮链恢复 60/60 id/digest/归一化字节一致、跨进程报告字节级一致）；
> [Context Curator 设计](../design/context_curator_design.md) §8/§13 Stage W3
> 触发策略随本文件 §4 冻结
> 建议发布点：非发布物；产出 Working Context 自动维护的确定性触发基线
> （Stage W4 Memory promotion 与 Stage E 真机评估矩阵的输入形态锚点）
> 更新日期：2026-09-15（立项）

## 1. 目标

按 Context Curator 设计 §8/§9/§13 交付 Stage W3（确定性验证，无真实模型）：

- **触发策略（纯函数，冻结）**：`WorkingContextTriggerPolicy` +
  `evaluate_working_context_trigger`——以两个独立陈旧度轴决定"是否刷新"：
  - **watermark 轴**：会话对话序列距离
    （`current_watermark - last_attempt_watermark >= watermark_interval`）；
  - **event count 轴**：宿主上报的自上次尝试以来的执行事件增量
    （`events_since_attempt >= event_count_interval`，EventStore 活动与对话
    序列正交）。
  设计 §8 的"token watermark"措辞按本节细化为**序列水位**：请求体量预算仍在
  `ContextCurationOptions`（M21），不引入 tokenizer 依赖（确定性、跨平台、
  无供应商耦合）；频率预算由阈值 + coalescing 承担。
- **自动触发协调器 `WorkingContextAutoCurator`**：宿主控制面状态机（不拥有
  线程，不隐藏全局状态），把宿主上报的信号转化为经
  `ContextMemorySupervisor::schedule_working_context_curate`（既有 Deferrable
  路由，关闭顺序与取消语义不变）的刷新。每会话链至多一个在途 policy 刷新；
  在途期间信号被**吸收（coalescing）**——不排队副本、不并发第二跳，落定后
  在下一信号按阈值以最新输入重燃。
- **forced flush（task boundary）**：`flush()` 是任务边界的显式屏障——先有界
  排干在途刷新，再对"未落库的新 checkpoint"强制刷新（无视阈值）并返回必须被
  消费的 future；宿主须在翻转 terminal 前等待它。对已落库边界，
  flush 立即以 `IdempotentNoOp`（reason `auto-refresh-current`）resolve，
  不产生同水位重 curate（同水位不同输入按 §5.2 必然 fail-closed，禁止制造）。
- **失败回退**：curator 失败（provider 错误/畸形 JSON/refusal/deadline/取消）
  → future 以错误 resolve、store 不动（设计 §9：保留上一已提交快照）、
  阈值在燃点已重臂（无紧重试循环）、`consecutive_failures` 递增并在
  committed/NoOp 后重置；下次阈值越过（或 forced flush）重试。不做确定性
  投影自动回退：同水位混源必然 `conflicting-watermark`，且三 section 投影会
  静默丢失五个 Curator section；宿主如需 W1 兜底可显式调用
  `schedule_working_context_commit`。
- 按 §4 产出自动触发管线的确定性评估基线：冻结数据集 + 脚本化确定性 Curator
  （可注入确定性闸门），门禁 W3-G1–G6（跑前冻结）。

## 2. 范围与非目标

范围：新公开头文件 `include/mira/context_working_context_auto.hpp` 与
`src/context/context_working_context_auto.cpp`（入 `mira_core`）——
`WorkingContextTriggerPolicy`、`WorkingContextTriggerKind`/`Decision`、
`evaluate_working_context_trigger` 纯函数、`WorkingContextRefreshInput`、
`WorkingContextAutoCurator`（`on_signal`/`flush`/`drain`/`session_view`/
`stats`，每会话有界状态）、统计结构；`tests/m22/` 契约/集成测试与评估
harness、基准报告与文档同步。

非目标：不实现 Memory promotion（W4）与 subagent fork/merge（W5）；不引入
新持久化格式与隐藏后台循环/定时器（信号由宿主上报，工作全部经既有
Supervisor Deferrable 路由——AGENTS.md Executor 纪律）；不修改
`IContextCurator`/`ProviderContextCurator` 契约、`commit_working_context`
五元组纪律、`schedule_working_context_curate` 路由语义与 Layer 0 既有语义；
不修改 M19 checkpoint 契约；不声明语义质量、token 收益或 continuation
correctness（RULE-10，归真实模型轮与 Stage E，`MNT-202609-27` 通道）；本
里程碑无真实模型网络调用（评估与测试全部使用脚本化确定性 provider）；不新增
请求体量 token 预算 API（体量预算 = M21 `ContextCurationOptions` 既有上界，
频率预算 = 本里程碑阈值 + coalescing）；运行时 Agent Loop 的接线（宿主集成
示例）不在本里程碑。

## 3. 设计与决策依据

- [DEC-035](../decisions/DEC-035-context-curator-working-context.md)
  第 4 条（W3 范围：自动触发、coalescing、forced flush）、第 6 条（Executor
  纪律：快照提交经 Supervisor Deferrable 路由，关闭顺序与取消语义不变）
- [Context Curator 设计](../design/context_curator_design.md)
  §8（Executor 路由与 W3 归属）、§9（Curator 失败降级——自动触发的失败回退
  底型）、§5.2（提交纪律——单调提交是在途并发的兜底）、§13（Stage W3 门禁）
- [M21](m21-context-curator-stage-w2.md) §4（增量 merge 语义与退化防护——
  刷新的模型侧契约不变；§6 风险"请求级 token 预算与 coalescing 归 W3"的
  处置收敛于本文件 §1/§2）与
  [curation 评估 v1](../benchmarks/context-intelligence-working-context-curation-v1.md)
  （W3 进入门槛证据）
- [M20](m20-working-context-stage-w1.md)（快照契约、五元组提交管线——
  自动触发的落库纪律）
- `RULE-02`（调度经既有 Executor 监督者）、`RULE-07`（快照是可重建投影）、
  `RULE-08`（输入与输出全部有界）、`RULE-09`（模型介导派生投影永不提升
  authority）、`RULE-10`（指标口径诚实）

## 4. 冻结的契约语义、评估 profile 与门禁（跑前冻结，2026-09-15）

### 4.1 触发策略（纯函数）

```cpp
struct WorkingContextTriggerPolicy final {
    std::uint64_t watermark_interval = 8;     // 会话对话序列距离
    std::uint64_t event_count_interval = 16;  // 上报的执行事件增量
    std::size_t max_tracked_sessions = 64;    // 每协调器有界会话数
    [[nodiscard]] Result<void> validate() const;  // 三者 > 0
};

enum class WorkingContextTriggerKind : std::uint8_t { None, Watermark, EventCount };

struct WorkingContextTriggerDecision final {
    bool refresh = false;
    WorkingContextTriggerKind kind = WorkingContextTriggerKind::None;
};

// 纯函数：watermark 距离达标优先（强信号），其次执行事件增量；都不达标为 None。
[[nodiscard]] WorkingContextTriggerDecision evaluate_working_context_trigger(
    const WorkingContextTriggerPolicy &policy, std::uint64_t last_attempt_watermark,
    std::uint64_t events_since_attempt, std::uint64_t current_watermark);
```

### 4.2 信号、状态机与 coalescing（`WorkingContextAutoCurator`）

信号输入（宿主上报，全部有界拷贝）：

```cpp
struct WorkingContextRefreshInput final {
    ConversationCheckpoint checkpoint;                   // 最新已提交 checkpoint
    std::vector<ConversationSegmentEntry> recent_events; // M21 语义的有界尾部
    WorkingContextIdentity identity;
    WorkingContextCommitState live;
    std::uint64_t reported_events = 0;  // 自上次信号以来的执行事件增量
};
```

每会话链状态（控制面私有，`session_view()` 可查）：`last_attempt_watermark`
（阈值重臂锚点，燃点更新——**失败也重臂**，保证无紧重试循环）、
`settled_watermark`（最近一次 Committed/IdempotentNoOp 落库水位；失败与丢弃
不推进；输入 identity 的
session/task/task_epoch/environment_epoch 四元组与会话上次燃点不一致时归零
——epoch 新链不被旧链水位误判为"已落库"）、`events_since_attempt`（吸收信号
继续累计）、`in_flight` 所有权、`consecutive_failures`。

- `on_signal(session, input)`：
  1. **drain**：在途 future `wait_for(0)` 就绪则落账（Committed/NoOp →
     `settled_watermark` 推进、`consecutive_failures` 重置；Discarded* →
     不推进不重置；错误 → `consecutive_failures` 递增）。
  2. **仍 在途 → 吸收**：累计 `events_since_attempt += reported_events`、
     `absorbed` 计数，返回 `nullopt`（coalescing：不排队、不并发第二跳）。
  3. **评估**：`evaluate_working_context_trigger` 为 None → 返回 `nullopt`；
     为真且 **`checkpoint.through_event_sequence > settled_watermark`**
     （有未落库的新 checkpoint 才燃——同水位重 curate 要么 NoOp 要么冲突，
     两者都不制造）→ 燃点重臂（`last_attempt_watermark = current_watermark`、
     `events_since_attempt = 0`），`previous` = `store.latest(session)` 且
     四元组（session/task/task_epoch/environment_epoch）与输入 identity 匹配
     才用（epoch 新链自动 `nullopt`，M21 §4.1 语义一致），经
     `schedule_working_context_curate` 调度并把 future 交调用方消费。
     checkpoint 未前进时**不重臂**（距离继续累计，checkpoint 一到即追燃）。
- `flush(session, input)`（task boundary 屏障）：
  1. **有界排干**在途 future（预算 = `ContextCurationOptions::deadline`），
     落账同上。
  2. `checkpoint.through_event_sequence > settled_watermark` → 无视阈值强制
     调度（重臂、previous 选择同上），future 交调用方；与在途 policy 刷新的
     并发由 §5.2 单调提交兜底（后到的低水位 `DiscardedStale`，终态后
     `DiscardedTerminal`，不重激活）。
  3. 否则以 `IdempotentNoOp`（reason `auto-refresh-current`，
     `committed` = store latest 或 nullopt）立即 resolve 返回的 future，
     零 curator 调用。
  宿主契约：**先等待 flush 的 future，再翻转 session/task terminal**；终态
  后的迟到结果按 §5.2 设计丢弃。
- `drain(session)`：显式落账就绪的在途 future（宿主 shutdown/巡检用）。
- 析构：对仍持有的在途 future 做总预算 `2 × deadline` 的有界 drain，未落定者
  释放（supervised 工作照常在 Executor 落定，Result 不吞异常，统计可见）；
  宿主应在丢弃协调器前显式 `flush`/`drain`。
- 统计：`WorkingContextAutoStats`（signals/absorbed/fires_watermark/
  fires_event_count/forced_flushes/flush_noops/committed/idempotent_noops/
  discarded_stale/discarded_terminal/errors/consecutive_failures 汇总）。

构造校验：`policy.validate()` + `ContextCurationOptions::validate()` 失败即
`InvalidArgument`；`max_tracked_sessions` 超限的会话信号以 `ResourceExhausted`
错误 future resolve，不静默扩容。输入 checkpoint 的 session 与信号会话不一致
→ 该信号以 `InvalidArgument` 错误 future resolve（curate 既有前置校验在
supervised op 内兜底，future 统一消费）。

### 4.3 冻结评估 profile

**对照设计**：确定性合成数据集 + 脚本化确定性 Curator 模型（SplitMix64，seed
`0x4d49'5232'3257'3343ULL`（"MIR22W3C"））。8 会话 × 每会话 40 信号：每信号
对话推进 0–10、执行事件增量 0–12（数据集保证 Watermark 与 EventCount 两种
触发各 ≥ 12 次可观测、每会话至少一段"零对话推进高执行事件"相与一段
"checkpoint 空窗"），checkpoint 在确定性的水位标记处提交（默认阈值
`watermark_interval=8`/`event_count_interval=16`）。脚本 Curator 沿用 M21
固定合法策略（保留/supersede/新增三类 merge 关系可见），另含**确定性闸门**
变体（原子标志阻塞 `curate` 返回，供 coalescing/flush 无竞态观测）与**失败
注入**变体（provider 错误/畸形 JSON/refusal/deadline/取消五类）。数据集与
脚本输出以 `dataset_digest` 断言锚定。管线为
`WorkingContextAutoCurator`（注入 `ProviderContextCurator` + 脚本 provider）
→ `schedule_working_context_curate` → `commit_working_context`。

**场景轮（全部 8 会话）**：40 信号自动链（触发判定逐一比对纯函数预测）；
checkpoint 空窗追燃；吸收轮（闸门阻塞下 N 信号吸收、恰好一次调度、释放后
下一越限信号以最新输入重燃）；flush 轮（低于阈值强制燃、边界后终态迟到
丢弃、已落库边界零调用 NoOp）；失败回退轮（五类失败各 8 组：future 错误、
store 逐字节不变、无紧重试、下次越过重试成功并重置）；previous 选择轮
（epoch 新链 previous = nullopt、同链 previous == latest）；shutdown 轮
（在途取消 resolve Cancelled、关闭后信号拒绝）；长会话确定性（全量自动链
同进程重放逐快照 id/digest/归一化字节一致）。

**门禁 W3-G1–G6（跑前冻结；理由随条给出）**：

| 门禁 | 断言 | 依据 |
| --- | --- | --- |
| W3-G1 触发策略正确性 | 纯函数边界（达标/不达标/优先级）100% 正确；自动链每次燃点的 kind 与输入逐一等于纯函数预测；非燃信号零调度；checkpoint 空窗后首个携带新 checkpoint 的越限信号追燃；`checkpoint ≤ settled_watermark` 零燃 | §4.1/§4.2 冻结策略；频率预算 = 阈值 + coalescing 的取证 |
| W3-G2 coalescing | 在途期间吸收 N/N 信号、supervisor 恰好一次调度（curator 调用计数）、不排队副本；落定后下一越限信号恰好一次以最新 checkpoint 重燃；flush 与在途并存时单调提交（最终 store == flush 输入水位，无冲突无回退） | §4.2 coalescing 冻结语义；§5.2 单调提交兜底 |
| W3-G3 forced flush | 低于阈值仍燃；future 必返且被消费；宿主等待后翻 terminal → Committed；其后迟到的 policy 刷新 DiscardedTerminal 且 store 保留 forced 快照（终态幂等）；已落库边界 flush 零 curator 调用、`auto-refresh-current`、committed == store latest | §4.2 flush 屏障语义；设计 §5.2 终态纪律 |
| W3-G4 失败回退 | 五类失败 40/40 future 以正确错误 resolve、store 逐字节不变；失败后无紧重试（阈值未再越过前零燃）；下次越过重试成功且 `consecutive_failures` 重置；失败边界 flush 产生真实重试（非 NoOp） | §4.2 失败回退冻结语义；设计 §9 降级 |
| W3-G5 提交纪律贯通 | 自动路径全场景 disposition 矩阵（Committed/NoOp/Stale/Terminal）逐一符合预期；epoch 新链 previous = nullopt、同链 previous == latest（curator 捕获断言）；`max_tracked_sessions` 超限 ResourceExhausted；会话不一致 InvalidArgument | 设计 §5.2 贯通；M21 §4.1 previous 语义延续 |
| W3-G6 shutdown 与确定性 | `begin_shutdown` 时在途 future resolve Cancelled、drain 落账、关闭后 on_signal/flush 以拒绝错误 resolve 零调度；全量自动链重放 8/8 会话逐快照 id/digest/归一化字节一致；跨进程 JSON 报告字节级一致（`cmp` 验证） | 既有 Deferrable 关闭顺序不变；RULE-07；M19–M21 方法学 |

**报告指标（只报告不判定）**：燃点计数（按 kind/forced/追燃分列）、吸收计
数、disposition 计数、失败计数与重试间隔、每会话 settled 水位轨迹、curator
调用次数。失败处置同 M19–M21：实现缺陷登记 `BUG-YYYYMMDD-NNN`，阈值或口径
错误先修订本文件与设计文档再复跑。

## 5. 工作项

- [x] `M22-01` 里程碑立项与本节契约语义/profile/门禁冻结（本文件；时间戳先于
  任何正式评估运行）。
- [x] `M22-02` 公开契约与实现：`context_working_context_auto.hpp`（策略纯
  函数、`WorkingContextAutoCurator`、统计）+ `context_working_context_auto.cpp`
  （入 `mira_core`）；复用既有 Deferrable 路由，不新增 Supervisor 方法。
- [x] `M22-03` 契约/集成测试矩阵（§7）与评估 harness（W3-G1–G6、JSON 报告、
  dataset digest 锚定）——测试的编写、运行与 sanitizer 取证由
  Independent-Verification-Agent 独立完成并复验。
- [x] `M22-04` 首轮评估运行（正式一次）并登记
  `../benchmarks/context-intelligence-working-context-auto-trigger-v1.md`。
- [x] `M22-05` 文档同步（总计划索引与注记、Context Curator 设计 §8/§13 实现
  注记、README 能力表、API 手册、关联决策链接）与实现同批变更。
- [ ] `M22-06` 本地全门禁与 PR CI（Linux/Windows/Android/sanitizers/quality）
  取证回填。（本地部分已完成，见 §8；PR CI 待回填）

## 6. 风险与阻塞

- 风险：触发策略语义（双陈旧度轴）与设计 §8"token watermark"措辞存在解释
  空间，被质疑为语义漂移。处置：本文件 §1/§4.1 跑前冻结并写明理由（确定性、
  无 tokenizer、体量预算留在 M21 options）；设计文档 §8 同步实现注记，偏差
  可审计。
- 风险：同水位重 curate 制造 `conflicting-watermark` 噪声。处置：§4.2
  `settled_watermark` 燃点闸门 + flush 的 `auto-refresh-current` 短路
  （W3-G1/G3 取证）。
- 风险：coalescing/flush 场景对时序敏感，测试不稳定。处置：脚本 Curator
  确定性闸门（原子标志）替代 sleep 竞态（W3-G2 取证）；harness 禁用任意
  sleep 类同步。
- 风险：协调器 future 所有权泄漏（在途未消费）。处置：所有权规则冻结
  （§4.2 析构有界 drain + 释放语义）；测试断言每次燃点 future 的去向
  （交调用方或被后续 drain 落账）。
- 外部：无（不依赖真机、凭据或外部语料；真实模型轮为显式非目标）。

## 7. 测试与退出条件

- [x] 契约/集成测试进 ctest（label `integration;m22`）：策略纯函数边界与
  validate；on_signal 正常燃/不燃/吸收/追燃；settled 闸门（同水位零燃）；
  flush 三分支（强制燃/终态迟到/NoOp 短路）；失败回退五类与重试重臂；
  previous 选择（epoch 新链/同链）；超限与会话不一致错误；shutdown 在途
  取消与关闭后拒绝；统计与会话视图一致性；析构有界 drain。
- [x] 评估 harness 进 ctest（label `integration;m22`，跑前冻结 W3-G1–G6，
  失败非零退出）；首轮报告落
  `../benchmarks/context-intelligence-working-context-auto-trigger-v1.md`
  （环境、命令、场景轮摘要、门禁结果、限制）。
- [x] 本地门禁：debug 全量 ctest 全绿（新增 2 目标）、ASAN/UBSAN/TSAN m22
  目标零报告、`format-check`/`docs-check`/`platform-boundary-check`/
  `sbom-check` 通过、clang-tidy 预检覆盖全部被修改库源编译单元、本机 NDK
  两 ABI 交叉编译预演通过。
- [x] 文档同步完成（§5 `M22-05` 清单；与实现同一变更提交）。
- [ ] PR CI（Linux/Windows/Android 编译级/sanitizers/quality）全绿后回填验证
  记录并关闭本里程碑。

## 8. 验证记录

2026-09-15：`M22-01` 立项，本文件 §4 契约语义、profile 与门禁冻结（先于任何
正式评估运行）。进入门槛复核：M21/Stage W2 已关闭（PR #53，CI 24/24）；
快照链长会话基线可复现（curation 评估 v1：恢复 60/60 id/digest/归一化字节
一致、跨进程报告 `cmp` 一致）。同日实现前修订两处冻结文本（先于任何测试与
评估）：§4.2 `settled_watermark` 增加 epoch 新链归零规则；§4.1/§4.2 明确
信号以输入 checkpoint 的 `through_event_sequence` 为对话水位观测值（curation
只能推进到 checkpoint 水位，checkpoint 空窗期距离与事件增量继续累计，新
checkpoint 一到即追燃）。

2026-09-15：`M22-02`～`M22-05` 本地实现、首轮自动触发评估与文档同步（分支
`feat/m22-working-context-stage-w3`；测试的编写、运行与 sanitizer 取证由
Independent-Verification-Agent 独立完成并复验）。

- **交付**：`include/mira/context_working_context_auto.hpp` +
  `src/context/context_working_context_auto.cpp`（入 `mira_core`）——
  `WorkingContextTriggerPolicy` / `evaluate_working_context_trigger` 双轴
  纯策略（序列水位优先、执行事件增量其次）、`WorkingContextAutoCurator`
  （`on_signal` 落账/吸收/评估/燃点重臂 + unsettled 闸门、`flush` 有界排干
  + 强制燃/`auto-refresh-current` 短路、`drain`/`session_view`/`stats`、
  共享 future 双副本语义、析构 2×deadline 有界 drain）；全部 curation 经
  `schedule_working_context_curate` 既有 Deferrable 路由，未新增 Supervisor
  方法。CMake 注册 `mira_m22_auto_trigger_test` / `mira_m22_auto_trigger_eval`
  （label `integration;m22`）。
- **测试矩阵**（`tests/m22/m22_auto_trigger_test.cpp` 16 用例 +
  `tests/m22/m22_auto_trigger_eval.cpp` 门禁 harness；确定性闸门
  （mutex/cv）同步，无 sleep 时序等待）。期间 Independent-Verification-Agent
  发现并证实一处实现缺陷：燃点 `std::future` 二次 `share()` 返回空句柄，
  调用方 `get()` 抛 `std::future_error`——按其受控实验方案修复为单次
  `share()`（提交内修复），修复后 16/16 全绿；另修两处 clang-tidy
  `performance-move-const-arg`（平凡可拷贝 policy 去 `std::move`）。
- **首轮评估**（最终源码字节上正式运行一次，登记
  [auto-trigger 评估 v1](../benchmarks/context-intelligence-working-context-auto-trigger-v1.md)）：
  W3-G1–G6 全绿——纯函数表 13/13；640 次信号判定、206 次燃点与纯函数预测
  零失配（watermark 72 / event count 31，均 ≥ 12）；空窗追燃 36、同水位
  零重 curate；coalescing 吸收 24/24、阻塞期零调度、释放后以最新输入恰好
  重燃一次；forced 16/16 提交、边界 NoOp 8/8 零 curator 调用；五类失败
  40/40 正确 resolve、store 逐字节不变、零紧重试（重试间隔恒 3）、连击
  复位 40/40；previous 新链 24/24 nullopt、同链 8/8 命中；shutdown 8/8
  在途 Cancelled、关闭后零调度；重放 103/103 对快照 id/digest/归一化字节
  一致。`dataset_digest`
  `6f2ab2e57b93e1d66846100f92b91563a83f5ffd1f05b5dbf6682dc5f4911535`
  断言锚定；跨进程/跨构建报告字节级一致（`cmp` 验证）。
- **本地门禁**（Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、
  CMake 3.28.3）：debug 全量 ctest **82/82**（原 80 + 本里程碑 2 目标）；
  ASAN/UBSAN/TSAN（TSAN 经 `setarch -R`）m22 两目标零报告且报告与 debug
  字节一致；`format-check`/`docs-check`/`platform-boundary-check`/
  `sbom-check` 通过；miniconda clang-tidy 18.1.8 预检被改库源编译单元
  （`context_working_context_auto.cpp`）零违例（修两处
  `performance-move-const-arg` 后复验通过）；本机同版本 NDK（r26.3）
  arm64-v8a/x86_64 两 ABI `mira_core` 交叉编译（warnings-as-errors）预演
  通过。
- **限制与未执行项**：脚本化确定性供给方口径，非语义质量、token 收益或
  continuation correctness 声明（`RULE-10`）；真实模型接入与 issue #48
  对照指标归真实模型轮/Stage E（`MNT-202609-27` 通道）；Memory promotion
  （W4）与 subagent fork/merge（W5）为显式非目标；运行时 Agent Loop 接线
  （宿主集成示例）不在本里程碑。观察项（非契约场景、未登记缺陷）：单一
  协调器多会话且多闸门同时阻塞时，Executor 有限 worker 使后续被接纳任务
  推迟执行——预期排队行为，宿主不应长时间阻塞 `curate`。Windows/Android
  编译级/Release/quality 由 PR CI 回填后本里程碑方可关闭（`M22-06`）。
