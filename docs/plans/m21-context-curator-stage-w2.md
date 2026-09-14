# M21：Context Curator Stage W2——IContextCurator 契约与模型供给参考实现

> 状态：Completed（2026-09-15：PR #53 合入 `6b7e377`，首轮 CI 双 pipeline
> 24/24 全绿；真实模型接入与 issue #48 对照指标为显式非目标，归真实模型轮与
> Stage E 证据通道；自动触发归 Stage W3）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-035](../decisions/DEC-035-context-curator-working-context.md) 第 4 条
> Stage W2；维护者指示「依设计与计划推进下一步开发」，与 M8–M20 同一授权模式）
> 前置：M20 / Stage W1 关闭（已满足，PR #49 于 2026-09-14 合入）；
> 可用源模型供给（已满足，[DEC-036](../decisions/DEC-036-consolidation-model-supply.md)
> 修订：经 `IModelProvider` 注入既有可用源模型，含主 Agent 模型，不要求专用小模型，
> 无新增供应链项）；[Context Curator 设计](../design/context_curator_design.md)
> §6/§13 Stage W2 增量 merge 语义随本文件 §4 冻结
> 建议发布点：非发布物；产出模型介导快照管线的确定性行为基线（Stage W3 自动触发
> 与 Stage E 真机评估矩阵的输入形态锚点）
> 更新日期：2026-09-15（立项）

## 1. 目标

按 Context Curator 设计 §4.1/§4.2/§6/§8/§13 交付 Stage W2（模型介导，确定性验证）：

- 快照契约加法式 schema minor 升级（v1.0 → v1.1）：新增五个 Curator 填充 section
  （`active_tasks` / `verified_facts` / `failed_attempts` / `important_refs` /
  `next_actions`，issue #48 数据模型扣除 `goal`——设计 §4.1 刻意偏离）与
  `generated_by` 模型 profile 标注（DEC-036 第 3 条：`generated_by` 记录实际模型
  profile）；`validate()` / `state_digest()` / JSON 序列化与 Layer 0 转换同步扩展；
  v1.0 载荷继续可读（同 major 内向后兼容），确定性投影
  `working_context_from_checkpoint` 语义不变、产出三 section + 空 section 的
  v1.1 快照。
- `IContextCurator` 契约：以
  `previous snapshot + new checkpoint + recent events` 为输入产出快照候选，
  经 §5.2 既有提交管线落库；增量 merge 语义（保留 / supersede / 冲突保留）随
  本文件 §4 冻结为"模型指令契约 + 机械校验"，运行时不执行编辑策略。
- model-backed 参考实现 `ProviderContextCurator`：完整复用 M19 已验证的模型边界
  范式——StrictJsonSchema 输出契约、编号输入（previous / checkpoint / events
  三段编号空间）、provenance 绑定（引用越界即丢弃该条，绝不修复）、标记过滤、
  confidence 下限、有界输出、deadline/cancellation、fail-closed 解析；模型经
  `IModelProvider` 注入（DEC-036 口径：任意可用源模型，Core 不绑定模型）。
- Executor 路由：`ContextMemorySupervisor::schedule_working_context_curate`
  （Deferrable 类，与 W1 `schedule_working_context_commit` 同一关闭顺序与取消
  语义；取消探针经 options 透传；Curator 失败不提交、保留已存快照，future 以
  错误 resolve，调用方按设计 §9 继续使用上一已提交快照）。
- 按 §4 产出模型介导管线的确定性评估基线：冻结数据集 + 脚本化确定性 Curator
  模型，门禁 W2-G1–G6（跑前冻结）。

## 2. 范围与非目标

范围：`include/mira/context_working_context.hpp`（v1.1 加法扩展）、新公开头文件
`include/mira/context_curator.hpp` 与 `src/context/context_curator.cpp`（入
`mira_core`）、`ContextMemorySupervisor::schedule_working_context_curate`、
`tests/m21/` 契约/集成测试与评估 harness、基准报告与文档同步。

非目标：不实现 Supervisor 自动触发、watermark/event count/task boundary 触发、
coalescing、forced flush（W3）；不实现 Memory promotion（W4）与 subagent
fork/merge（W5）；不引入新 `ContextItemKind`（新 section 复用既有 kind 转换，
独立审计粒度经条目 id 的 section 标签区分——设计 §7 加法式预留）；不修改 M19
checkpoint 契约、Layer 0 既有语义与 `working_context_from_checkpoint` 投影映射；
不引入新持久化格式（store 仍为 volatile 可重建投影，RULE-07）；不声明语义质量、
token 收益或 continuation correctness——issue #48 的对照指标（A/B/C 三臂）需要
真实模型证据（RULE-10），归真实模型评估轮与 Stage E（`MNT-202609-27` 通道），
本里程碑评估对象是确定性管线行为；本里程碑无真实模型网络调用（评估与测试全部
使用脚本化确定性 provider；真实模型轮凭据归受控通道）。

## 3. 设计与决策依据

- [DEC-035](../decisions/DEC-035-context-curator-working-context.md)
  第 2/3/4/6 条（投影非权威、Memory 边界、W1–W5 分阶段、Executor 纪律——W2
  模型调用在 `schedule_working_context_commit` 同一路由语义后接入）
- [Context Curator 设计](../design/context_curator_design.md)
  §4.1（schema minor 升级）、§4.2/§6（W2 输入形态与增量 merge 语义）、
  §5.2（提交纪律——同水位冲突分支为 W2 非确定性兜底）、§7（Layer 0 路径）、
  §8（Executor 路由）、§9（Curator 失败降级）、§12（评估口径）、§13（Stage W2
  门禁）
- [DEC-036](../decisions/DEC-036-consolidation-model-supply.md)（模型供给口径：
  可用源模型即可；`generated_by` 记录实际 profile）
- [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 与
  [M19](m19-context-intelligence-stage-d.md)（模型边界范式与评估 harness 方法学
  ——StrictJsonSchema、编号输入、provenance fail-closed、supervisor Deferrable
  路由、冻结数据集 + dataset digest 锚定，W2 全部沿用）
- [M20](m20-working-context-stage-w1.md)（快照契约、提交管线与输入形态——W2
  的直接前置）
- `RULE-02`（新路由经既有 Executor 监督者，Deferrable 类）、`RULE-07`
  （快照是可重建投影）、`RULE-08`（输入与输出全部有界）、`RULE-09`（模型介导
  派生投影永不提升 authority）、`RULE-10`（指标口径诚实）

## 4. 冻结的契约语义、评估 profile 与门禁（跑前冻结，2026-09-15）

### 4.1 增量 merge 语义（随 `IContextCurator` 冻结）

输入为三段编号空间（拼接为单一 transcript，编号连续）：

1. **previous 段**：`previous` 快照八个 section 的全部条目，行格式
   `[i|prev:<section>|seq=<source_sequence>] <content>`；省略时该段为空。
2. **checkpoint 段**：新 checkpoint 四个 statement section 的全部条目，行格式
   `[i|ckpt:<section>|seq=...] <content>`。
3. **events 段**：recent events（`ConversationSegmentEntry`），行格式
   `[i|event|seq=...] <text>`。

模型输出契约（StrictJsonSchema，`mira.working_context.curator.output.v1`）：根
对象含 `confidence`（number）与八个 section 数组（`constraints` / `decisions` /
`open_issues` / `active_tasks` / `verified_facts` / `failed_attempts` /
`important_refs` / `next_actions`），每条 `{content, sources, confidence}`，
`sources` 为 transcript 编号数组且逐条非空。

机械校验（fail-closed，逐条丢弃，绝不修复；沿用 M19 `bind_statement` 语义）：

- 内容为空、超 `max_item_chars`、命中 forbidden/injection 标记、confidence 非有限
  或低于 `min_confidence` → 丢弃该条。
- 任一 `sources` 引用越界（不在三段编号空间内）→ 丢弃该条（伪造 provenance）。
- 绑定后 provenance = 被引用输入的 `source_events` 并集（去重保序，首个先占），
  超过 `max_source_events` → 丢弃该条；`source_sequence` = 被引用输入的最小
  sequence；跨 section 上限 `max_items_per_section` 先到先得。
- 每个 section 允许为空数组；条目不得携带输入未出现的内容之外的身份信息。

**保留 / supersede / 冲突保留是指令契约，不是运行时编辑策略**：保留 = 模型引用
previous 编号重申条目（provenance 随引用继承）；supersede = 模型省略旧条目并以
同时引用旧 previous 编号与新证据编号的方式给出替代条目；冲突保留 = 模型同时给出
各自 provenance 绑定的多个变体。运行时只校验上述机械纪律并整快照提交（§5.2），
不裁决内容取舍。

**退化输出防护（M20 风险条款的 W2 处置，机械可校验）**：

- `previous` 非空（任一 section 有条目）时，候选中必须至少有一条绑定条目引用了
  previous 段编号——否则该候选整体拒绝，reason `degenerate-merge`（supersede
  纪律要求引用被替代条目；零 previous 引用说明模型未依据 previous，提交会无保护
  地覆盖富快照）。被拒候选不提交，已存快照保留。
- epoch 失效开启新链时宿主应传 `previous = nullopt`（旧链快照不是新链的增量
  基线）；`previous` 与新 checkpoint 的 session/task/task_epoch/
  environment_epoch 不一致 → 调用前 `InvalidArgument` 拒绝。
- recent events 的 `session_sequence` 不得越过 checkpoint 水位（违反 →
  `InvalidArgument`；候选水位恒等于 checkpoint 水位，同水位不同输入由 §5.2
  同水位冲突分支 fail-closed 兜底）。

### 4.2 快照 schema v1.1（加法式 minor）

- `working_context_schema_current()` = {1, 1}；读者接受同 major 内 {1, 0}/{1, 1}
  载荷（`validate_schema_version` 既有语义）；v1.0 载荷读取时新 section 为空。
- 新增字段：五个新 section（`WorkingContextItem` 形状不变）与 `generated_by`
  （`ModelProfileId`；确定性投影为 nil）。
- `state_digest()` 覆盖全部权威字段：既有覆盖面 + 八个 section + `generated_by`；
  排除 `id` / `created_at` 不变。digest 公式为全量无条件包含（无按版本分支）。
- `working_context_from_checkpoint` 投影映射不变（constraints / decisions /
  unresolved_threads 三段），产出 stamping {1, 1}、新 section 为空、
  `generated_by` 为 nil 的快照；`source_checkpoints` 仍为 `[checkpoint.id]`。
- Curator 候选：身份五元组取新 checkpoint stamping；`source_checkpoints` =
  `previous.source_checkpoints` ∪ {新 checkpoint id}（保序去重，超出 64 上限时
  保留最近 64 个——`validate()` 既有绝对上界）；`created_at` = curation 完成时刻。

### 4.3 冻结评估 profile

**对照设计**：确定性合成数据集 + 脚本化确定性 Curator 模型（SplitMix64，seed
`0x4d49'5232'3157'4354ULL`（"MIR21WCT"）；12 个会话 × 每会话 5 轮链：每轮一个
已提交 `ConversationCheckpoint`（水位 8/16/24/32/40 递进，每 checkpoint 植入
6 约束 / 4 决策 / 4 未决线索，语句含会话 token 与链内序号）+ 每轮 8 条 recent
events（sequence ≤ 该轮水位，事件空间取自本会话 48 个事件，位置逐会话确定性
打乱）。脚本化模型按固定确定性策略产出合法输出：保留上一快照的 constraints
（引用 prev 编号）、以"引用旧 prev 条目 + 新 checkpoint 语句"的方式 supersede
每轮一条决策、对新增语句只引用 checkpoint/events 编号、并按轮次填充
`next_actions` 等新 section——三类 merge 关系（保留 / supersede / 新增）在
每轮均可见。数据集与脚本模型输出以 `dataset_digest` 断言锚定（口径同
M17–M20；首轮钉定后记录于 §8）。管线为
`ProviderContextCurator`（注入脚本 provider）→ `commit_working_context`，
Supervisor 路由场景走 `schedule_working_context_curate`。

**场景轮（全部 12 会话）**：5 轮递进链 curation + 提交；同输入重放（同 candidate
重提交 → 全部 IdempotentNoOp）；同水位冲突（同轮次改写一条脚本输出 → 全部
`conflicting-watermark` 丢弃且已存快照逐字节保留）；陈旧水位（旧 checkpoint
重 curate → `stale-watermark` 丢弃）；身份不匹配四联（task / task_epoch /
environment_epoch / session 各 12/12 丢弃）；终态迟到（session_terminal 与
task_terminal 各 12/12 丢弃）；退化输出（previous 非空、输出零 previous 引用 →
`degenerate-merge` 整体拒绝且已存快照保留）；Curator 失败降级（provider 错误 /
畸形 JSON / refusal / deadline / 取消 → future 以错误 resolve、0 提交、已存
快照不变）；越界与伪造引用（逐条丢弃，计数入报告）；Layer 0 转换（八 section
全转换、authority、id 空间、epoch 标注）。

**门禁 W2-G1–G6（跑前冻结；理由随条给出）**：

| 门禁 | 断言 | 依据 |
| --- | --- | --- |
| W2-G1 绑定保真 | 脚本输出 → 候选逐条一致：content、provenance = 被引用输入事件并集、`source_sequence` = 最小被引用 sequence、confidence clamp、section 归属正确；越界/伪造/超界条目 100% 丢弃且不计入候选 | 模型边界范式（设计 §6、M19 D 系列）：provenance 绑定是机械契约，伪造引用绝不入场（RULE-09） |
| W2-G2 身份与链绑定 | 候选 id = 身份五元组种子派生；水位 == checkpoint 水位；`source_checkpoints` = 累积链（保序去重、有界）；`generated_by` == provider profile id；同输入重 curate 幂等（同 id 同 digest） | 确定性身份（设计 §5.1）与 DEC-036 `generated_by` 口径；RULE-07 可重建性 |
| W2-G3 提交纪律与失败降级 | 递进链 60/60 提交；重放 12/12 NoOp；同水位冲突/陈旧水位/身份不匹配四联/终态迟到 100% 丢弃且已存快照不变；退化输出 12/12 `degenerate-merge` 拒绝；Curator 五类失败（provider 错误/畸形 JSON/refusal/deadline/取消）future 以错误 resolve 且 0 提交 | 设计 §5.2/§9：同水位冲突分支是 W2 非确定性的 fail-closed 兜底；Curator 失败不阻塞 Agent loop |
| W2-G4 增量 merge 语义 | 保留：脚本保留项逐轮在候选中且 provenance 继承 previous；supersede：旧条目消失、替代条目引用含 prev 编号；冲突保留：两变体并存且各自 provenance 正确；previous 非空时零 previous 引用候选 0 提交 | §4.1 冻结的指令契约 + 退化防护；M20 风险条款（退化输出覆盖富快照）的 W2 处置取证 |
| W2-G5 Layer 0 纪律 | 八 section 全部转换；constraints → P1 `UserConstraint`、其余七 section → P3 `CheckpointSummary`；全部条目 authority == `UntrustedExternalData`；条目 id 确定性且 section 标签互异、与 checkpoint 条目 id 零冲突；条目携带快照 epoch 标注 | 设计 §7 与 RULE-09：快照不自准入；不引入新 kind 的 W2 口径 |
| W2-G6 契约兼容与确定性 | v1.0 载荷在 v1.1 读者下解析（新 section 空）；v1.1 JSON 往返逐字段一致；`state_digest` 覆盖新字段且排除 id/created_at；恢复场景（空 store 上以同 checkpoint 链重放确定性投影 → Curator 重 curate）id/digest 与丢失前一致；跨进程 JSON 报告字节级一致（`cmp` 验证） | DEC-002 加法式版本化（设计 §4.1）；RULE-07 恢复；M19/M20 方法学 |

**报告指标（只报告不判定）**：各 section 条目计数（输入/输出/丢弃分类）、
provenance 并集大小、`source_checkpoints` 链长、store 环深、提交/丢弃分类
计数、脚本模型调用次数。失败处置同 M19/M20 §4：实现缺陷登记
`BUG-YYYYMMDD-NNN`，阈值或口径错误先修订本文件与设计文档再复跑。

## 5. 工作项

- [x] `M21-01` 里程碑立项与本节契约语义/profile/门禁冻结（本文件；时间戳先于任何
  正式评估运行）。
- [x] `M21-02` 快照 schema v1.1 加法扩展：五个新 section、`generated_by`、
  `validate()` / `state_digest()` / JSON 往返 / Layer 0 转换扩展、v1.0 兼容读取；
  `working_context_from_checkpoint` 语义不变。
- [x] `M21-03` `IContextCurator` 契约与 `ProviderContextCurator` 参考实现：
  `ContextCurationOptions`（有界配置 + deadline/cancellation/标记过滤）、
  输出 schema `mira.working_context.curator.output.v1`、三段编号输入渲染、
  fail-closed 解析与 §4.1 机械校验（含退化防护）。
- [x] `M21-04` Executor 路由：`ContextMemorySupervisor::schedule_working_context_curate`
  （Deferrable、取消探针透传、shutdown 拒绝与在途取消 resolve Cancelled、
  Curator 失败不提交）。
- [x] `M21-05` 契约/集成测试矩阵（§7）与评估 harness（W2-G1–G6、JSON 报告、
  dataset digest 锚定）、本地全门禁。
- [x] `M21-06` 文档同步（总计划索引与注记、Context Curator 设计实现注记、
  README 能力表、API 手册、基准报告、关联决策链接）与实现同批变更；CI 取证
  已回填（PR #53）。

## 6. 风险与阻塞

- 风险：模型介导输出使快照链不再字节确定，"同水位同 digest"幂等在真实模型下
  不可复现。处置：幂等语义保持在"同候选重放"与"同输入 + 确定性模型"两层
  （W2-G2/W2-G6 以脚本模型取证）；真实模型下由 §5.2 同水位冲突 fail-closed 兜底，
  修复路径为从 `source_checkpoints` 确定性重投影（设计 §9/§10）。
- 风险：退化或劣化输出覆盖富快照。处置：§4.1 退化防护（零 previous 引用整体
  拒绝）+ 同水位冲突 fail-closed（W2-G3/W2-G4 取证）。
- 风险：三段编号输入在长会话下膨胀请求。处置：输入全部有界（`max_recent_events`
  与既有 per-section 上限；RULE-08）；请求级 token 预算与 coalescing 归 W3。
- 风险：schema minor 升级被误解为破坏 v1.0 兼容。处置：DEC-002 加法式变更，
  v1.0 载荷读者语义测试冻结（W2-G6）。
- 外部：无（本里程碑不依赖真机、凭据或外部语料；真实模型轮为显式非目标）。

## 7. 测试与退出条件

- [ ] 契约/集成测试进 ctest（label `integration;m21`）：schema v1.1 往返与 v1.0
  兼容读取、digest 覆盖新字段；Curator 正常 curation（provenance 绑定、三段
  编号输入、`generated_by`、身份五元组、累积 `source_checkpoints`）；标记过滤、
  confidence 下限、逐条越界丢弃、per-section 上限先到先得；退化输出整体拒绝；
  previous/checkpoint 身份不一致与 recent events 越水位 `InvalidArgument`；
  提交竞态（陈旧水位、四联不匹配、幂等 NoOp、同水位冲突 fail-closed）；终态
  迟到丢弃；epoch 新链（previous = nullopt）；store 水位单调与有界保留；
  Curator 失败降级五类（provider 错误/畸形 JSON/refusal/deadline/取消）；
  `context_items_from_working_context` 八 section 映射、RULE-09 authority 与
  id 空间分离；`schedule_working_context_curate` Deferrable 路由、shutdown
  拒绝与在途取消。
- [ ] 评估 harness 进 ctest（label `integration;m21`，跑前冻结 W2-G1–G6，失败
  非零退出）；首轮报告落
  `../benchmarks/context-intelligence-working-context-curation-v1.md`（环境、
  命令、场景轮摘要、门禁结果、限制）。
- [ ] 本地门禁：debug 全量 ctest 全绿（新增 2 目标）、ASAN/UBSAN/TSAN m21
  目标零报告、`format-check`/`docs-check`/`platform-boundary-check`/
  `sbom-check` 通过、clang-tidy 预检覆盖全部被修改库源编译单元、本机 NDK
  两 ABI 交叉编译预演通过。
- [ ] 文档同步完成（§5 `M21-06` 清单；与实现同一变更提交）。
- [x] PR CI（Linux/Windows/Android 编译级/sanitizers/quality）全绿后回填验证
  记录并关闭本里程碑（PR #53 首轮双 pipeline 24/24）。

## 8. 验证记录

2026-09-15：`M21-01` 立项，本文件 §4 契约语义、profile 与门禁冻结（先于任何
正式评估运行）。

2026-09-15：`M21-02`～`M21-06` 本地实现与首轮 curation 评估（分支
`feat/m21-context-curator-stage-w2`；测试的编写、运行与 sanitizer 取证由
Independent-Verification-Agent 独立完成并复验）。

- **交付**：`include/mira/context_working_context.hpp` +
  `src/context/context_working_context.cpp`——快照 schema 加法式升级 1.1
  （`active_tasks` / `verified_facts` / `failed_attempts` / `important_refs` /
  `next_actions` 五 section + `generated_by`；validate/digest/JSON/Layer 0
  转换同步扩展，v1.0 载荷兼容读取；`working_context_from_checkpoint` 映射
  不变）；新公开契约 `include/mira/context_curator.hpp` +
  `src/context/context_curator.cpp`（入 `mira_core`）——`ContextCurationOptions`
  有界配置、`working_context_curation_output_schema()`（根 confidence + 八
  section，StrictJsonSchema）、`IContextCurator` 与 `ProviderContextCurator`
  （三段编号转录 `prev:`/`ckpt:`/`event:`、provenance 并集绑定、逐条
  fail-closed、退化防护 `degenerate-merge`、身份五元组 + 累积
  `source_checkpoints` + `generated_by`）；`ContextMemorySupervisor::
  schedule_working_context_curate`（Deferrable、取消探针透传、shutdown 拒绝、
  Curator 失败不提交）。
- **首轮评估**（正式运行一次，登记
  [curation 评估 v1](../benchmarks/context-intelligence-working-context-curation-v1.md)）：
  W2-G1–G6 全绿——递进链 60/60 提交（60 次 curator 调用）、section 条目
  输入/输出 1,368 相等、对抗输出 84/84 逐条丢弃且零准入；同输入重 curate
  12/12 幂等、重放 12/12 NoOp；同水位冲突 12/12 `conflicting-watermark`、
  陈旧水位 12/12、身份四联 48/48、终态迟到 24/24 全部丢弃且已存快照不变；
  退化输出 12/12 `degenerate-merge` 整体拒绝；五类 Curator 失败 60/60 以
  正确错误码 resolve、零提交、store 不变；恢复 60/60 id/digest/归一化字节
  一致；跨进程双跑报告字节级一致（`cmp` 验证）。`dataset_digest`
  `4e3221cb56f48a2a5db5e77d6057be4dfa3e613970f4a8754341e45fcf3db6b6` 断言
  锚定；输出契约 digest `08b6f2ed…4c122` 记录于报告。
- **测试矩阵**（`tests/m21/m21_context_curator_test.cpp` 10 组、
  `tests/m21/m21_curator_eval.cpp` 门禁 harness，label `integration;m21`）：
  schema v1.1 往返与 v1.0 兼容、Layer 0 八 section 转换与 id 空间、Curator
  正常路径与三段转录、累积链、fail-closed 解析、退化防护、输入校验、提交
  竞态与终态、epoch 新链、supervisor shutdown/取消/失败降级、options 边界。
  期间发现并修复两处：实现 transcript 行缺 `prev:`/`ckpt:` 块前缀（与 §4.1
  冻结格式不符，按冻结文本修实现并补同 tag 同 seq 区分断言）；harness 计数
  器非原子（测试侧 TSAN 竞态，改 `std::atomic`）。
- **本地门禁**（Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、
  CMake 3.28.3）：debug 全量 ctest **80/80**（原 78 + 本里程碑 2 目标）；
  ASAN/UBSAN/TSAN（TSAN 经 `setarch -R`）m21 两目标零报告；
  `format-check`/`docs-check`/`platform-boundary-check`/`sbom-check` 通过；
  miniconda clang-tidy 18.1.8 预检全部三个被改库源编译单元（含新增
  `context_curator.cpp`）零违例（修一处 `bugprone-inc-dec-in-conditions`
  触发后复验通过）；本机同版本 NDK（r26.3）arm64-v8a/x86_64 两 ABI
  `mira_core` 交叉编译与 arm64 installed-consumer 闭包预演通过。
- **限制与未执行项**：脚本化确定性供给方口径，非语义质量、token 收益或
  continuation correctness 声明（`RULE-10`）；真实模型接入与 issue #48 对照
  指标归真实模型轮/Stage E（`MNT-202609-27` 通道）；自动触发与 coalescing
  （W3）、Memory promotion（W4）、subagent fork/merge（W5）为显式非目标；
  Windows/Android 编译级/Release/quality 由 PR CI 回填后本里程碑方可关闭
  （`M21-06`）。

2026-09-15：PR CI（[#53](https://github.com/Linductor-alkaid/mira/pull/53)）
首轮全绿，无需修复轮。

- PR [#53](https://github.com/Linductor-alkaid/mira/pull/53)（head `0ad7218`，
  合并提交 `6b7e377`）push pipeline run
  [`34884298162`](https://github.com/Linductor-alkaid/mira/actions/runs/34884298162)
  与 pull_request pipeline run
  [`34884341364`](https://github.com/Linductor-alkaid/mira/actions/runs/34884341364)
  各 12 项全部通过：Linux GCC/Clang（Debug/Release，两 m21 目标入 Linux 测试
  矩阵）、Windows MSVC（Debug/Release）、Android arm64-v8a 与 x86_64（NDK
  编译级）、ASAN/UBSAN/TSAN、quality（clang-tidy 18 + clang-format +
  docs/sbom/platform-boundary 检查）。§7 退出条件逐项复核后关闭本里程碑。
  遗留（显式非目标，不阻塞关闭）：真实模型接入与 issue #48 对照指标
  （Continuation correctness / Constraint retention / Failed-attempt
  recurrence，A/B/C 三臂冻结数据集；归真实模型轮与 DEC-032 Stage E，
  `MNT-202609-27` 证据通道，DEC-036 已解除供给门禁）；Supervisor 自动触发
  与 coalescing（Stage W3）、Memory promotion（W4）、subagent fork/merge（W5）。
