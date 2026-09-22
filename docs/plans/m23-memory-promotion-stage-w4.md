# M23：Context Curator Stage W4——Memory Promotion（Working Context 经 `MemoryConsolidator` 既有纪律晋升长期记忆）

> 状态：In Progress（2026-09-22 立项并跑前冻结本文件 §4/§5/§6/§7；同日实现
> 启动）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-035](../decisions/DEC-035-context-curator-working-context.md) 第 4 条
> Stage W4）
> 前置：Stage W2 关闭（已满足，[M21](m21-context-curator-stage-w2.md)，PR #53
> 2026-09-15 合入）；W3 已交付（[M22](m22-working-context-stage-w3.md)，PR #54
> 同日合入，非本阶段硬前置）；Working Context 与长期 Memory 边界测试随本文件
> §6 跑前冻结（设计 [Context Curator 设计](../design/context_curator_design.md)
> §13 Stage W4 门禁的第二项，随本立项满足）
> 建议发布点：非发布物；产出 Working Context 与长期 Memory 之间唯一受控晋升
> 通道的契约与边界测试（Stage W5 subagent fork/merge 与宿主集成轮的输入形态
> 锚点）
> 更新日期：2026-09-22（立项）

## 1. 目标

按 Context Curator 设计 §2 原则 3 与 §13 Stage W4 交付 Memory Promotion
（确定性验证，无模型调用）：

- **唯一晋升通道**：`WorkingContextSnapshot` 中具有跨任务耐久价值的语句，经
  确定性投影 `memory_candidates_from_working_context` 变为 `MemoryCandidate`，
  再交由 `MemoryConsolidator` **既有**纪律管线（marker 过滤、record 校验、
  scope 内冲突检索、duplicate 判定、高危人工审批、mutation 计划与 apply）
  落入长期 Memory。快照与 Curator 仍永不直接写 Memory（DEC-035 决策第 3 条）。
- **共享管线，不设第二写路径**：`MemoryConsolidator` 新增公共入口
  `consolidate_candidates`，接收预先抽取的候选并执行与 `consolidate` 逐字相同
  的策略管线；`consolidate` 重构为"抽取（事件 + 可选模型钩子）→
  `consolidate_candidates`"，管线逻辑只存在一份（阶段冻结协议 §5：不为同一
  状态引入第二写入路径）。
- **验证等级纪律**：晋升候选一律 `Unverified` + `model_assisted = true` +
  `source_namespace = "working-context"`（模型介导派生投影，RULE-09；沿用
  `consolidate` 对模型提案的既有降级纪律），永不 `HumanConfirmed`；人工确认
  只能经既有审批流（`apply_pending`）发生在 Memory 侧。
- **无降级副本**：共享管线 exact-duplicate 判定由"验证等级相等"收紧为"已存
  副本验证等级 ≥ 提案等级"（§4.2）——已被人确认为 `HumanConfirmed` 的记录
  不得被同语句的 `Unverified` 晋升副本 Supersede 降级。
- **显式宿主触发**：本阶段不实现自动晋升（不接入 W3 `WorkingContextAutoCurator`、
  不新增触发器与后台循环）；晋升是宿主显式操作（典型时点：任务终态边界），
  经既有 `ContextMemorySupervisor::submit` 泛型 Deferrable 路由执行。

## 2. 范围与非目标

范围：新公开头文件 `include/mira/context_working_context_promotion.hpp` 与
`src/context/context_working_context_promotion.cpp`（入 `mira_core`）——
`WorkingContextPromotionPolicy`、投影函数与投影结果、晋升报告、
`promote_working_context_to_memory` 组合入口；`memory_consolidation.hpp/.cpp`
加法式重构（`consolidate_candidates` 公共入口 + exact-duplicate 判定收紧，
管线语义除 §4.2 一处收紧外不变）；`tests/m23/` 契约/集成测试；文档同步。

非目标：不实现 subagent fork/merge（W5）；不实现自动触发或把晋升挂接进 W3
自动链（宿主显式调用，无隐藏后台循环/定时器——AGENTS.md Executor 纪律）；
不修改 `IContextCurator`/`ProviderContextCurator`、`commit_working_context`
五元组纪律、Layer 0 既有语义、M19 checkpoint 契约与 `ConsolidationPolicy`
默认内容（审批种类、marker 清单不变）；不做跨 store 擦除编排（已晋升记录受
Memory 侧既有 retention/erasure 治理，宿主按需经既有 `IMemory` erasure 编排，
见 §4.4）；不声明语义质量、记忆收益或任务成功率（RULE-10，无模型无数据集，
归真实模型轮与 Stage E 证据通道）；不新增评估 benchmark 报告（本阶段交付物
是契约与边界测试，见 §6 门禁形态说明）；运行时 Agent Loop 接线（宿主集成
示例）不在本阶段；本阶段无真实模型网络调用。

## 3. 设计与决策依据

- [DEC-035](../decisions/DEC-035-context-curator-working-context.md)
  第 3 条（与 Memory 的边界：Curator 可产生 Memory candidate，不得直接写
  Memory；跨任务知识只能经 `MemoryConsolidator` 既有纪律）、第 4 条（W4 范围
  归属）
- [Context Curator 设计](../design/context_curator_design.md)
  §2（原则 3：Working Context 不是第二份 Memory）、§13（Stage W4 行与
  "每 Stage 进入实现前创建里程碑文件"纪律）
- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)
  §6.2（`MemoryKind` 语义）、冷路径（`MemoryConsolidator` 为 Verified
  Event → Memory 的既有管线）、§Executor 路由（Memory consolidation 归
  delayed/低优先级，终态不等待非关键候选）
- [M22](m22-working-context-stage-w3.md)（显式非目标"Memory promotion 归
  Stage W4"的出处；泛型 Deferrable 路由先例）、
  [M21](m21-context-curator-stage-w2.md)（快照 schema 1.1 八 section 语义——
  晋升映射的输入面）、[M20](m20-working-context-stage-w1.md)（快照契约、
  provenance 与 validate 边界）、[M13](m13-memory-and-learning-loop.md)
  （DEC-029/030 记忆组织与学习契约——`RecoveryLesson` 等既有 kind 消费方）
- TR2 先例（[M7 文件](m7-tools-evaluation-platform-v1.md) §4.7）：确定性
  record/mutation id 从 seed 派生、Add 必带事件 evidence、幂等重放
- `RULE-07`（快照是可重建投影，晋升不改变其事实源地位）、`RULE-08`（输入
  输出全部有界）、`RULE-09`（模型介导派生投影永不提升 authority）、
  `RULE-10`（指标口径诚实）

## 4. 冻结的契约语义（跑前冻结，2026-09-22）

### 4.1 晋升映射（纯函数投影）

```cpp
struct WorkingContextPromotionPolicy final {
    // 晋升把任务导向状态固化为跨任务长期记忆，置信度门槛默认取多数线；
    // 文档化默认值（非冻结契约，随 ConsolidationOptions 先例）。
    double min_confidence = 0.5;
    // 投影阶段候选上限；超出按固定 section 顺序截断（与 consolidator
    // 管线的 max_candidates_per_run 截断语义一致，截断后计数可见）。
    std::size_t max_candidates_per_run = 32;

    [[nodiscard]] Result<void> validate() const;  // 0 <= min_confidence <= 1；bound > 0
};

struct WorkingContextPromotionProjection final {
    std::vector<MemoryCandidate> candidates;  // 固定 section 顺序，详见下表
    std::size_t dropped_low_confidence = 0;   // item.confidence < min_confidence
    std::size_t dropped_missing_evidence = 0; // item.source_events 为空
};
```

| 快照 section | MemoryKind | 理由（冻结） |
| --- | --- | --- |
| `constraints` | `Preference` | 用户约束与禁止项是典型跨任务偏好；默认 `ConsolidationPolicy::approval_required_kinds` 含 `Preference` → 天然落入 `PendingApproval` 人工审批门 |
| `decisions` | `ApplicationFact` | 含理由的决策是关于应用/环境的耐久事实 |
| `verified_facts` | `EnvironmentFact` | 已核验事实的直接对应物 |
| `failed_attempts` | `RecoveryLesson` | 失败经验与 M13 `RecoveryLesson` 学习闭环同源（DEC-029/030） |
| `active_tasks` / `next_actions` / `open_issues` | 不晋升 | 任务导向当前状态，随任务终态失效；晋升会向长期记忆注入陈旧状态 |
| `important_refs` | 不晋升（v1） | 外部引用的耐久价值未经验证（RULE-10）；待真实模型轮证据再议 |

投影逐条固定语义（顺序 = 上表顺序，section 内按向量序）：

- `record.statement = item.content`；`record.kind` 按上表；
  `record.scope` = 调用方显式提供的 `MemoryScope`（ACL 由宿主权威给定，Core
  不发明 tenant/subject 策略）；`record.confidence = float(item.confidence)`；
  `record.validity.valid_from = snapshot.created_at`；`recorded_at` 留空由
  管线盖 `now`（既有语义）；`record.sensitivity = Sensitivity::Internal`。
- `record.verification = Unverified`、`candidate.model_assisted = true`、
  `candidate.reason = MutationReasonCode::Consolidation`、
  `record.source_namespace = "working-context"`——全部不可被调用方覆盖
  （RULE-09 的晋升面固化）。
- `candidate.evidence = item.source_events`（provenance 贯通）；为空则丢弃
  该条（`dropped_missing_evidence`）——TR2 确立的"M4 Add 必带事件 evidence"
  上位契约优先。
  （2026-09-22 实现注记：`WorkingContextSnapshot::validate()`（W1 契约）本就
  拒绝无 provenance 的条目，故该丢弃分支经公共 API 不可达——空 evidence 条目
  在投影入口即整体 `InvalidArgument`。分支保留为纵深防御，测试冻结该可观测
  行为。）
- `record.id` 从 seed 确定性派生：
  `mira.memory.promotion|<scope_kind_name>|<scope.subject_id>|<section_tag>|<content>`
  （TR2 `learning_id_from_seed` 同款 sha256 截断模式，seed 空间独立）；同语句
  重复晋升派生同一 id，跨进程投影字节可复现。mutation id 维持管线既有
  `MutationId::generate()`（M4 行为不变；幂等性由语句 key + 冲突检索的
  `duplicate-noop` 承担，不依赖 mutation id）。
- 输入 snapshot 先过 `validate()`，失败整体报错（无部分投影）；
  `max_candidates_per_run` 截断按固定顺序执行，无随机性。

### 4.2 共享管线的一处收紧：无降级副本

`MemoryConsolidator` 管线的 exact-duplicate 判定由

`conflict->statement == proposed.statement && conflict->verification == proposed.verification`

收紧为

`conflict->statement == proposed.statement && conflict->verification >= proposed.verification`

（`MemoryVerification` 枚举序 `Unverified < Observed < Verified < HumanConfirmed`）。
理由：晋升候选恒为 `Unverified`；相等判定会使已 `HumanConfirmed` 的记录被同
语句的未验证晋升副本 Supersede **降级**。收紧后"已存副本验证等级不低于提案"
即为 noop，杜绝验证等级回退。对既有来源的影响：`VerifiedEvent` 抽取（恒
`Verified`）与模型提案（恒 `Unverified`）重放时等级相等，行为不变；唯一行为
变化是"已存等级更高"这一病态分支从降级变为 noop，属缺陷级收紧，由 §6
W4-G3 回归覆盖。除本条外管线逐行不变（marker 过滤、record 校验、冲突检索、
审批门、mutation 计划与 apply 全部维持 M4/M13 冻结语义）。

### 4.3 公共入口

```cpp
// memory_consolidation.hpp（加法）：
// 对预先抽取的候选执行与 consolidate() 相同的策略管线；不做事件抽取、
// 不挂模型钩子——候选的 provenance 由调用方负责（晋升管线据此复用）。
[[nodiscard]] Result<ConsolidationReport>
consolidate_candidates(IMemory &memory, std::vector<MemoryCandidate> candidates,
                       const MemoryScope &scope, const Timestamp &now) const;
// consolidate() 重构为：抽取 + 模型增强 + consolidate_candidates(...)，
// 管线体只保留一份。
```

```cpp
// context_working_context_promotion.hpp：
struct WorkingContextPromotionReport final {
    WorkingContextPromotionProjection projection;  // 投影产物与丢弃计数
    ConsolidationReport consolidation;             // 既有管线报告（原样透出）
};

// 组合入口：投影 + 共享管线。consolidator 的 policy 即晋升纪律的唯一配置面。
[[nodiscard]] Result<WorkingContextPromotionReport>
promote_working_context_to_memory(const MemoryConsolidator &consolidator, IMemory &memory,
                                  const WorkingContextSnapshot &snapshot,
                                  const MemoryScope &scope, const Timestamp &now,
                                  const WorkingContextPromotionPolicy &policy = {});
```

错误语义：policy 或 snapshot `validate()` 失败 → `InvalidArgument`；
`memory` 缺席由调用方契约排除（引用形参，编译期不可缺席——"组件缺席降级"
在本入口表现为：空快照产出零候选的空报告而非错误；memory 不可用时的
store 层失败经管线既有 `RejectedConflict`/错误传播暴露）。

### 4.4 生命周期与边界

- **快照侧只读**：晋升不写 `IWorkingContextStore`、不产生新快照、不推进
  水位；快照的可重建性（RULE-07）与晋升正交。
- **擦除边界**：`erase_session` 只删快照投影，不触及已晋升记录；已晋升记录
  是长期记忆事实，受 Memory 侧既有 retention/erasure/approval 治理。隐私
  场景的跨 store 编排（会话擦除时连带处理其派生记忆）是宿主职责，头文件
  文档明示；负向测试冻结该边界（W4-G4）。
- **幂等**：同一 snapshot 重复晋升 → 同一投影（确定性 id + 固定顺序）→
  管线按语句 key 命中已存副本 → `duplicate-noop`，零新增记录；已存副本等级
  更高时同样 noop（§4.2）。
- **终态**：晋升没有五元组身份，不存在迟到重激活问题；宿主典型时点为任务
  终态边界（此后晋升仍合法——写的是 Memory，不是任务状态）。
- **Executor 路由**：晋升是 consolidation 家族的低优先级工作，宿主经既有
  `ContextMemorySupervisor::submit<WorkingContextPromotionReport>(…,
  SupervisedOpClass::Deferrable, …)` 泛型路由执行；**不新增 Supervisor
  方法**（M22 先例）。关闭顺序沿用既有：停生产者 → 取消 Deferrable → 有界
  等待 Critical → 消费 future；被取消的在途晋升 resolve `Cancelled`，
  关闭后的提交被拒绝。future 必须被调用方消费（AGENTS.md 第 3 条）。

## 5. 阶段冻结协议必答八问（2026-09-22）

1. **行为**：新增 Working Context → Memory 的确定性晋升投影与组合入口；
   `MemoryConsolidator` 新增 `consolidate_candidates` 公共入口并收紧
   exact-duplicate 判定（§4.2）。对应设计：Context Curator 设计 §2 原则 3、
   §13 Stage W4 行（实现注记随交付回填）。
2. **所有者**：新增可变状态只有 Memory store 侧既有记录（所有者不变）；
   投影函数与组合入口均为无状态纯计算，无新增可变状态、无缓存、无全局量；
   快照 store 全程只读。
3. **契约**：最小公共契约面 = `context_working_context_promotion.hpp` 全部
   （policy/投影/报告/组合入口）+ `memory_consolidation.hpp` 的
   `consolidate_candidates` 一项；错误语义 §4.3；调用方为宿主与测试（Layer
   0/Curator 不感知晋升）。契约四件套：公开头文件 + 本文件 §4 + API 手册
   `docs/api/context-memory.md` 条目 + `tests/m23/`。
4. **层与依赖**：新文件入 `core` 模块（`include/mira` + `src/context`），
   仅依赖既有 `mira` 头与标准库；`architecture-policy.json` 零差异
   （`core` requires 不变，无新外部头族）。
5. **复用**：晋升管线 100% 复用 `MemoryConsolidator` 既有纪律（marker、
   校验、冲突检索、审批、apply）——新增的只有"接收候选"的入口与"投影候选"
   的纯函数，不新增平行写路径（协议 §5 拒绝形态的正面清单）。
6. **时序**：幂等键 = 语句 key（kind + 归一化 statement，既有）+ 确定性
   record id（§4.1）；过期结果规则 = 无（无五元组身份，§4.4）；取消与
   deadline 由 Deferrable 路由与调用方 options 承担，晋升本体是有界纯计算
   （§4.1 上限 + 快照 validate 上界）。
7. **Executor 路由**：`submit<T>` 泛型 Deferrable（既有能力，非新路由）；
   句柄由宿主持有并消费；关闭顺序 = supervisor 既有 §17.2 序列（§4.4）。
8. **验证上下文**：评审者最小阅读集 = 本文件 §4、
   `include/mira/memory_consolidation.hpp`、
   `include/mira/context_working_context_promotion.hpp`、
   `tests/m23/m23_promotion_test.cpp`（契约/集成，label `integration;m23`）。

### 决策记录表

| 决策点 | 选择 | 被否备选 | 依据 |
| --- | --- | --- | --- |
| 晋升通道形态 | 快照 → 候选纯投影 + 共享 `consolidate_candidates` 管线 | 合成 `EventEnvelope` 走 `consolidate()`（伪造事件 provenance，耦合抽取启发式）；晋升器自带策略（第二写路径，协议 §5 禁止） | DEC-035 第 3 条；本文件 §1/§4.2 |
| section→kind 映射 | constraints→Preference、decisions→ApplicationFact、verified_facts→EnvironmentFact、failed_attempts→RecoveryLesson；`active_tasks`/`next_actions`/`open_issues`/`important_refs` 不晋升 | 全 section 晋升（向长期记忆注入随任务失效的当前状态）；映射做成宿主可配置位（审计面膨胀，v1 冻结为固定表） | §4.1 表；Context/Memory 设计 §6.2；DEC-029/030 |
| 验证等级 | 恒 `Unverified` + `model_assisted=true` + `source_namespace="working-context"`，不可覆盖 | 沿用快照 item.confidence 推断等级（模型介导投影永不 Verified/HumanConfirmed，RULE-09） | §4.1；管线模型提案既有降级纪律 |
| duplicate 判定 | 收紧为 `conflict->verification >= proposed.verification` | 保持相等判定（HumanConfirmed 记录会被 Unverified 晋升副本降级——缺陷级行为） | §4.2；W4-G3 回归 |
| 确定性 id | record id 从 seed 派生（`mira.memory.promotion|…` 独立 seed 空间）；mutation id 维持 `MutationId::generate()` | 全随机（投影跨进程不可复现）；mutation id 也确定性化（越出 W4 范围的 M4 契约变更） | §4.1；TR2 先例与边界 |
| 触发方式 | 宿主显式调用（典型任务终态边界） | 挂接 W3 AutoCurator 自动晋升（改 W3 冻结语义 + 隐式写长期记忆，风险不可控） | §1/§4.4 |
| Executor 路由 | `submit<T>` 泛型 Deferrable，不新增 Supervisor 方法 | 新增 `schedule_working_context_promotion` 包装（为单一调用方增加路由面） | §4.4；M22 先例 |
| 擦除边界 | `erase_session` 不触及已晋升记录；跨 store 编排归宿主 | 晋升记录携带 session 反向索引由 Core 联动擦除（新增第二治理面，超出既有 Memory 治理语义） | §4.4；W4-G4 负向冻结 |
| 评估形态 | 契约/集成测试门禁（ctest），无 benchmark 报告 | 冻结数据集评估 profile（无模型无指标可测，RULE-10 不允许虚构对象） | §2/§6 |

## 6. 测试与退出条件

门禁形态说明：W4 无模型调用、无数据集与指标对象（RULE-10），门禁全部落为
`tests/m23/` 契约/集成断言（label `integration;m23`），由
Independent-Verification-Agent 独立编写、运行并复验。

- [x] `W4-G1` 投影契约：四类晋升 section 的 kind/statement/scope/confidence/
  validity/provenance 逐一正确；`Unverified`/`model_assisted`/
  `source_namespace` 固化不可覆盖；confidence floor 以下丢弃并计数；空
  evidence 丢弃并计数；确定性 record id（同输入同 id，seed 空间独立）；固定
  section 顺序与 per-run 截断；非晋升四 section 恒零候选；无效 snapshot 整体
  报错。
- [x] `W4-G2` 既有纪律贯通：forbidden marker → `RejectedForbidden`；model
  文本 injection marker → `RejectedInjection`；`Preference` →
  `PendingApproval` 且 `apply_pending` 审批后落库；同 scope 同 key 冲突 →
  `Supersede`（版本递增、旧记录 `Superseded`）；store 拒绝 →
  `RejectedConflict`；`consolidate()` 与 `consolidate_candidates()` 走同一
  管线（同一输入候选两类入口 disposition 矩阵一致）。
- [x] `W4-G3` 幂等与无降级：同一 snapshot 重复晋升 `duplicate-noop`、记录数
  不变；已存 `Observed`/`Verified`/`HumanConfirmed` 同语句副本不被 `Unverified`
  晋升降级（noop，版本不变）；M13 `VerifiedEvent` 重放回归仍 noop（§4.2 对
  既有来源无行为变化）。
- [x] `W4-G4` 边界负向：晋升前后快照 store 逐字节不变（零写）；`erase_session`
  后已晋升记录仍在；空快照 → 零候选空报告；policy/snapshot 校验失败 →
  `InvalidArgument`；Curator/快照无任何直接写 Memory 路径（晋升入口是唯一
  桥）。
- [x] `W4-G5` Executor 路由：泛型 Deferrable 提交正常完成并被消费；
  `begin_shutdown` 后提交被拒；在途晋升被取消时 future 以 `Cancelled` resolve
  且 Memory 零部分写入；统计与报告计数一致。
- [x] `W4-G6` 确定性：同输入投影跨进程字节一致（候选 id/statement/顺序/
  丢弃计数；mutation id 为 M4 既有随机 id，不参与字节比对，诚实记录）；
  全部断言在 debug 与三 sanitizer 树下通过。
- [x] 本地门禁：debug 全量 ctest 全绿（含新增 m23 目标）、ASAN/UBSAN/TSAN
  m23 目标零报告、`format-check`/`docs-check`/`platform-boundary-check`/
  `sbom-check`/`architecture-check` 通过、clang-tidy 预检被改库源编译单元
  零违例、本机 NDK 两 ABI 交叉编译预演通过。
- [x] 文档同步完成（§7 `M23-04` 清单；与实现同一变更提交）。
- [ ] PR CI（Linux/Windows/Android/sanitizers/quality）全绿后回填验证记录
  并关闭本阶段。

## 7. 工作项

- [x] `M23-01` 阶段立项与本文件 §4/§5/§6 冻结（时间戳先于任何实现与测试）。
- [x] `M23-02` 公开契约与实现：`context_working_context_promotion.hpp/.cpp`
  （policy、投影、报告、组合入口）+ `memory_consolidation.hpp/.cpp` 加法重构
  （`consolidate_candidates` 共享管线入口、exact-duplicate 收紧）。
- [x] `M23-03` 契约/集成测试矩阵（§6 W4-G1–G6，`tests/m23/`，CMake 注册
  label `integration;m23`）——测试的编写、运行与 sanitizer 取证由
  Independent-Verification-Agent 独立完成并复验。
- [x] `M23-04` 文档同步：Context Curator 设计 §2/§13 实现注记、
  [DEC-035](../decisions/DEC-035-context-curator-working-context.md) 关联
  计划与验证方式回填、总计划索引与 §4.1 注记、API 手册
  `docs/api/context-memory.md` 新头文件条目、README 能力表、术语表
  （如需新词条）。
- [ ] `M23-05` 本地全门禁与 PR CI 取证回填（本地门禁已取证，PR CI 待回填）。

## 8. 风险与阻塞

- 风险：`consolidate_candidates` 重构被质疑改动了 M4/M13 既有行为。处置：
  除 §4.2 一处收紧外管线逐行不变；W4-G3 含 `VerifiedEvent` 重放回归；
  §4.2 的收紧理由（验证等级降级缺陷）已在本文件留痕，可评审回退。
- 风险：晋升被误用为绕过审批的旁路（把 Preference 语句塞进其他 section）。
  处置：映射表冻结且 Core 固定，调用方只能选择是否晋升、不能改写 kind 与
  等级；marker 与审批门在管线内兜底（与来源无关）。
- 风险：宿主把晋升当高频路径滥用，制造记忆膨胀。处置：per-run 截断 +
  confidence floor + 语句 key 幂等；膨胀治理归 Memory 侧既有 retention
  sweep（非本阶段契约）。
- 外部：无（不依赖真机、凭据、外部语料或真实模型）。

## 9. 验证记录

2026-09-22：`M23-01` 立项，本文件 §4/§5/§6/§7 冻结（先于任何实现与测试）。
进入门槛复核：Stage W2 已关闭（M21，PR #53）；设计 §13 Stage W4 门禁第二项
"Working Context 与长期 Memory 边界测试冻结"随本文件 §6 满足。W3（M22）已
交付，非本阶段硬前置（设计 §13 W4 行门禁为"W2 关闭"）。

2026-09-22：`M23-02`–`M23-04` 本地实现、测试与文档同步（分支
`feat/m23-memory-promotion-stage-w4`；测试的编写、运行与 sanitizer 取证由
Independent-Verification-Agent 独立完成并复验，两轮取证）。

- **交付**：`include/mira/context_working_context_promotion.hpp` +
  `src/context/context_working_context_promotion.cpp`（入 `mira_core`）——
  `WorkingContextPromotionPolicy`（floor 0.5 / cap 32 文档化默认值）、
  `memory_candidates_from_working_context` 确定性投影（冻结 section→kind
  映射、逐条 `Unverified`+`model_assisted`+`Consolidation`+
  `"working-context"` 固化、确定性 record id、固定顺序与截断、丢弃计数）、
  `promotion_record_id_from_seed`（`mira.memory.promotion|` 独立 seed 空间）、
  `promote_working_context_to_memory` 组合入口（报告同时携带投影产物与管线
  报告）；`MemoryConsolidator::consolidate_candidates` 共享管线公共入口
  （`consolidate()` 重构为"抽取 + 模型增强 + 本入口"，管线体只保留一份）+
  exact-duplicate 判定收紧（§4.2）。CMake 注册 `mira_m23_promotion_test`
  （label `integration;m23`）。
- **测试矩阵**（`tests/m23/m23_promotion_support.hpp` 确定性构造器 +
  `FaithfulMemory` 测试双；`tests/m23/m23_promotion_test.cpp` 16 用例覆盖
  W4-G1–G6；mutex/cv 闸门同步，无 sleep 时序）。IVA 首轮取证 15/16 绿并
  抓到一处实现缺陷：`promote_working_context_to_memory` 先把候选 move 进
  管线导致 `report.projection.candidates` 恒为空（预期报告保留投影产物）——
  主循环修复为报告先接管投影、管线按拷贝消费（提交内修复），复验 16/16 全绿。
  测试另冻结一处计划内张力（§4.1 实现注记：空 evidence 丢弃分支经公共 API
  不可达，`WorkingContextSnapshot::validate()` 先行整体拒绝）。
- **本地门禁**（Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、
  CMake 3.28.3、NDK r26.3）：debug 全量 ctest **92/92**（原 91 + 本里程碑
  1 目标，m4/m13 既有套件在重构后无回归）；ASAN/UBSAN/TSAN（TSAN 经
  `setarch -R`）m23 目标 16/16 零报告；`format-check`（229 文件）/
  `docs-check`/`platform-boundary-check`/`sbom-check`/`architecture-check`
  通过；miniconda clang-tidy 18.1.8 预检被改库源编译单元
  （`context_working_context_promotion.cpp`、`memory_consolidation.cpp`）
  零违例；本机 NDK r26.3 `android-arm64-release`/`android-x86_64-release`
  两 preset `mira_core` 交叉编译（warnings-as-errors）预演通过。
- **限制与未执行项**：无真实模型调用与 benchmark 报告（本阶段无模型无数据
  集，RULE-10；契约门禁即验收面）；subagent fork/merge（W5）、自动晋升触发、
  宿主集成接线为显式非目标；Windows/Android 编译级/Release/quality 由 PR
  CI 回填后本阶段方可关闭（`M23-05`）。
