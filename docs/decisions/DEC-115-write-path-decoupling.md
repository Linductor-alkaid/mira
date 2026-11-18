# DEC-115：Memory Core 写入面分层解耦（留存 / 证据积累 / 权威晋升）

> 状态：Accepted
> 日期：2026-11-18
> 负责人：Mira Maintainers
> 冻结里程碑：2026-11 写入面演进维护轮
> 替代/被替代：修订 [DEC-059](DEC-059-memory-kind-policy.md) 系设计 §13.3 时序
> 不变量与 §13.5 冲突语义（观察时间 → 纪元时间；subject-only 冲突 → 分类分析）；
> [DEC-109](DEC-109-memory-preference-approval.md) 的提案-审批 API 与
> `require_explicit_approval` 语义不变（变的只是 `ConsolidationPolicy` 的 kind
> 审批默认）；安全门（Forbidden/Injection/Invalid）、kind floors、CAS/scope/台账
> 语义全部不变
> 关闭的缺口：写入管道把「信息留存」与「权威事实更新」耦合在同一保守闸门——
> 同 claim-key 不同值冲突一律 Supersede（Unverified 模型候选也能取代权威事实）、
> 同值重复一律零写入（新证据被丢弃）、Preference 默认一律审批、VerifiedEvent
> 陈旧性按观察时间判定、超限候选静默丢弃

## 背景与问题

现有写入管道（`MemoryConsolidator::consolidate_candidates`）对冲突的处置只有
两个出口：Supersede（覆盖）或 Reject（拒绝）。这使尚未充分验证但具有长期价值
的信息被提前不可逆丢弃：

1. **冲突即覆盖**：同 claim-key 不同值的候选一律规划 Supersede——包括
   Unverified 的模型/提升候选取代 Verified 在位权威（管道不区分取代意图与
   并存观点）。
2. **重复即丢弃**：同 claim 同值的重复投递（常见于事件重放、多源观测）一律
   duplicate-noop，新独立证据没有入账通道，可信度无法随证据积累提升。
3. **审批一刀切**：`approval_required_kinds` 默认 `{Preference}` 把用户显式
   表达的偏好、working-context 提升的 constraints、agent 记录的偏好全部挡进
   PendingApproval。
4. **陈旧性看错时间**：VerifiedEvent 陈旧保护比较 `recorded_at`（观察时间），
   历史纪元的晚观察事实会覆盖新纪元事实（或被错误拒绝）。
5. **截断不可见**：`max_candidates_per_run` 超限候选被 resize 静默丢弃，
   `candidates_examined` 记录的是截断后数量。

关键实现事实（探索确认）：同 claim-key 的多条 Active 记录在 store 中**本来
就可以共存**（无唯一约束，claim key 查询时现算）——保守性完全在 consolidator
管道层；`memory_mutations.applied_type` 是 TEXT 列，新变异类型无需 schema
迁移；`Update` 变异类型已存在但 consolidator 从不规划。

## 决策

### 1. 变异模型扩展：Add、AppendEvidence、Supersede、PreserveAlternative、NoOp

（Update 保留为既有类型，本轮管道开始使用其一；Tombstone 不变。）

- **`AppendEvidence`**（wire `"append-evidence"`）：同 claim 同值的证据追加。
  `target + expected_version` 必填（CAS 语义保持：核对但不消费）；`evidence`
  必填且增量（只带在位者没有的 EventId）；`proposed.statement` 必须与 target
  一致；**版本链不动**（result_version = 现版本）——事实身份与 bitemporal
  纪元不变，provenance 合并幂等（INSERT OR IGNORE）。
- **`PreserveAlternative`**（wire `"preserve-alternative"`）：并存主张落库。
  fresh id（不得与 target 混淆）+ `MemoryRecord.alternative_of` 必须指向
  Active 在位记录；不携带 target/expected_version——**不触碰在位者**，不
  参与任何 CAS。存取走 document JSON（record 文档 minor {1,1}→{1,2}，读取
  方容忍未知字段；无需 store schema bump、无需 claim-key 列——claim-key
  查询现算即可同返多条并存记录）。
- **fail-closed 修正**：`memory_mutation_from_json` 对未知 `"type"` 从静默落
  `Add` 改为报错（document 兼容性注记：旧文档的合法类型不受影响）。
- **前向兼容如实记录**：旧二进制重放未知 `applied_type` 台账行落 Noop
  （既有行为，本轮不改；新类型行由旧 reader 重放会得到 Noop 幂等结果而非
  错误——可接受，记录在案）。

### 2. 冲突分析：分类处理取代单一 Supersede/Reject

冲突检索扩为有界候选集分析（max_results 16→64），按 `memory_same_claim`
（key+value 归一化全等）分类：

- **duplicate 族（key+value 全等）**：
  - 候选证据 ⊆ 在位者 provenance 且验证不更高 → **duplicate-noop**（不变）；
  - 存在新独立 EventId → **AppendEvidence**（`Applied/"evidence-appended"`，
    entry 带 incumbent id）；
  - 候选验证级别严格更高且有证据支撑 → 原地 **Update**：提升验证级别 + 合并
    证据（只升不降；**HumanConfirmed 在位者终态不可触碰**，跳过晋升）→
    `Applied/"verification-promoted"`。这是「证据积累逐步提升可信度」的权威
    晋升通道：晋升是严格的（需更高验证 + 证据），且永不降级。
- **alternative 族（同 key 不同值）**：
  - **纪元接续判据**：候选 `validity.valid_from` 严格晚于在位者且验证级别
    ≥ 在位者，且 reason ∈ {VerifiedEvent, HumanCorrection} → **Supersede**
    （时代链接语义不变——值真正变化时新纪元及时接续）；
  - `HumanCorrection` 恒 Supersede（用户纠正即权威意图）；
  - **模型候选（model_assisted）永不 Supersede**——Unverified 文本不能取代
    任何在位权威（修复现状缺陷：旧管道对同 key 不同值的模型候选也规划
    Supersede）；
  - 其余 → **PreserveAlternative**（并存，`Applied/"alternative-preserved"`，
    entry 带 incumbent id）；同 key 并存 alternative 数达
    `ConsolidationPolicy::max_alternatives_per_claim`（默认 4）→
    `RejectedConflict/"alternative-cap-reached"`（显式报告，不静默）。
- **无界/有界保护**：无界提案覆盖有界在位者由 `RejectedConflict` 改为
  PreserveAlternative（`"legacy-coexists-bounded"`）——信息保留，覆盖禁止。
- VerifiedEvent 陈旧保护改按**事实纪元**：`validity.valid_from` 严格更早的
  提案是历史纪元事实 → PreserveAlternative（不再是拒绝）；同纪元重放按
  duplicate 族处理。观察时间只用于 duplicate 族内 incumbent 选择与证据
  新旧，不再决定覆盖。

### 3. Preference 审批默认取消（DEC-109 API 不变）

`ConsolidationPolicy::approval_required_kinds` 默认 `{Preference}` → `{}`：
确定性事实映射的 Preference、working-context 提升的 constraints、agent
Record 的模型偏好全部自动写入（模型候选仍 Unverified + 注入门约束 +
kind/scope 导入契约）。DEC-109 的 `propose_preference`/`decide_preference`
API、工具面 `needs_human_approval`、其 `require_explicit_approval = true`
强制覆盖**全部不变**——提案-审批流是显式 API 的语义，不是管道默认。宿主
可用 policy 把任何 kind 加回审批。高风险敏感内容已由 forbidden 标记前置
阻断（不变）；「写入权限 ≠ 行为执行权限」的原有边界不变。

### 4. 截断可见化

`ConsolidationReport.candidates_truncated` 报告超限数量，
`candidates_examined` 改为截断前全量；提升投影的 32 条静默跳过改为
`dropped_overflow` 计数。本轮不做持久化 pending 队列（显式报告后由宿主
决定重投，登记为后续项）。

### 安全边界（全部不变）

Forbidden/Injection/Invalid 门、kind floors 与 scope 允许表（namespaced
导入契约不变）、model_assisted 强制与 HumanConfirmed 剥离、注入标记只约束
模型文本、CAS/scope-check 先于版本比较、台账 scope 绑定与幂等重放、
single-writer store、凭据类内容写入前阻断、不可信文本永远带来源与信任边界
（Unverified/namespace）不能成为系统指令。

## 非目标（后续轮登记）

基于价值/访问统计的遗忘与聚合（另立 DEC）；NLP 级冲突理解；超限候选的
持久 pending 队列；claim-key 存储列/索引；preference 存储 whitelist 扩展
（仍只 Add/Supersede）；`require_explicit_approval`/DEC-109 API 语义变更；
旧二进制的 forward-compat 升级器。

## 备选方案

- **Supersede 仅限显式权威来源**（VerifiedEvent 也只并存）：拒绝。事实真变化
  （端口 8443→9443）时新旧值长期并存，召回面同时命中两个时代的值且无接续
  语义；纪元接续判据已把「取代」收紧到验证 ≥ + 时间更晚，比现状（任何候选
  都可能 Supersede）更严格。
- **重复验证不自动晋升**：拒绝为默认。「Verified 事件重放已存 Unverified
  副本」是最安全的晋升场景（同 claim 同值、有证据、只升不降、HumanConfirmed
  终态保护）；完全禁止会让可信度只能靠宿主手工 Update。
- **alternative 用独立表/列存储**：拒绝。claim-key 查询现算已能同返并存记录，
  alternative_of 链接经 document JSON 往返即可；为不出现在任何查询谓词中的
  字段加列是提前优化。
- **超限候选入持久队列**：拒绝（本轮）。队列的可见性/生命周期/审查权属是
  新语义面；显式计数报告已消除静默丢失，队列留后续轮按需立项。

## 影响与风险

- 公开契约加法+语义修订：`MemoryMutationType` +2（7 处 parser/switch 同步）、
  `MemoryRecord.alternative_of`（document minor {1,2}）、`ConsolidationEntry/
  Report` 新字段、`ConsolidationPolicy` 默认值变更（**产品默认值**：Preference
  默认审批取消——本记录为据，DEC-109 头部加修订注记）。
- 行为风险：并存主张增加召回面的候选多样性（claim-key 查询同返多条）——
  检索侧已有 claim-key 查询禁用多样性上限 + 打包纪律，冲突裁决归消费方
  （与 DEC-112「reranker 不裁决冲突」同一分层）。
- 供应链/平台/Executor：无变化；无新外部头族；architecture-policy 不变。
- 钉死测试 consciously 更新清单见维护计划 WE-05；m23 无降级矩阵必须保持绿。

## 验证方式

- 新契约测试（m4_write_evolution_test）：AppendEvidence 合并/幂等/CAS/版本
  不动；PreserveAlternative 并存 + claim-key 查询同返 + cap 显式报告；纪元
  接续 Supersede 分区；观察/纪元时间区分；模型候选永不取代；晋升只升不降 +
  HumanConfirmed 终态；truncation 计数；DEC-109 路径仍 Pending；新类型台账
  重放。
- 钉死用例 consciously 更新后全量 ctest 通过；安全门回归原样全绿。
- 门禁四目标 + static-analysis 零告警 + clang-format。

## 关联文档和工作项

- 工作项：[`maintenance-2026-11-write-evolution`](../plans/maintenance-2026-11-write-evolution.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §13.3/§13.4/§13.5
- API：[`docs/api/context-memory.md`](../api/context-memory.md)、
  [`docs/api/memory-agent.md`](../api/memory-agent.md)
- 相关：[DEC-109](DEC-109-memory-preference-approval.md)（API 不变）、
  [DEC-059](DEC-059-memory-kind-policy.md)（floors 不变）、
  [DEC-058](DEC-058-mutation-ledger-scope-binding.md)（台账语义不变）、
  [DEC-114](DEC-114-extraction-model-supply.md)（模型候选永不取代注记）、
  [DEC-113](DEC-113-hybrid-retrieval-default-wiring.md)
