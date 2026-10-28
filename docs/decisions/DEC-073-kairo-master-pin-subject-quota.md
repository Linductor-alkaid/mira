# DEC-073：kairo pin 升级至上游 master 并采纳分主体在途份额

> 状态：Accepted
> 日期：2026-10-28
> 负责人：Mira Maintainers
> 冻结里程碑：维护轮（2026-10，kairo master pin 升级）
> 替代/被替代：无（延续 [DEC-054](DEC-054-kairo-0.7.0-upgrade.md)、
> [DEC-048](DEC-048-kairo-0.6.0-migration.md) 的依赖承载与迁移框架）
> 来源：[Issue #111](https://github.com/Linductor-alkaid/mira/issues/111) §6 与
> [`EXE-20261028-001`](../executor_feedback/ledger.md)（上游
> [kairo#222](https://github.com/Linductor-alkaid/kairo/issues/222) /
> [PR #223](https://github.com/Linductor-alkaid/kairo/pull/223) /
> [PR #224](https://github.com/Linductor-alkaid/kairo/pull/224)）

## 背景与问题

### 为什么必须跳到未发布的 master

Mira 的 kairo pin 停在 `32027c2`（`v0.7.0`）。分主体在途份额（DEC-069 §1/B2
要求的「分主体份额」准入）由上游 PR #223/#224 交付，两个 PR 均合并在 **v0.7.2
之后**的 master 上：

- 上游最新 release 仍是 `v0.7.2`（2026-10-19）；
- PR #223（merge `d493a69c`）与 PR #224（merge `78c6f557`）分别于 2026-10-28 合并，
  上游明确「随下一个 release tag 发布」；
- ⇒ **没有任何可用 tag 承载该能力**。

DEC-069 的分主体份额若要落地，只能消费 master 上的实现。

### delta 的真实范围（不只一个特性）

pin 从 `32027c2` 到 `78c6f557` 共 **38 个提交**，其中跨越了两个未消费的发布
（v0.7.1、v0.7.2）。必须把这部分一并核对，而不能只看 PR #223/#224：

| 版本 | 内容 | 对 Mira 的影响面 |
| --- | --- | --- |
| v0.7.1 | 能力快照 TTL 缓存、`DeadlinePolicy`、`ResourceLease` 准入期资源预留、任务意图自动推断与有界派发自动选择 | 全部 **opt-in / 默认关闭**；Mira 未注入调度器、未注册 GPU、未声明资源需求 |
| v0.7.2 | **优先级语义转移到调度器**（0.7.x 唯一破坏性变更）：`Executor::submit_priority` **已移除**，改由 `submit_auto(TaskBuilder(fn).priority(p))` 表达 | Mira **零使用** `submit_priority` |
| PR #223/#224 | 分主体在途份额（设计 `subject_bucket_admission.md` D-1–D-5 + 修订 R-1/R-2/R-3） | DEC-069 §1/B2 的目标能力 |

### Mira 的暴露面核对（2026-10-28 静态核对）

对全部被移除或语义变更的公开 API 做全仓检索：

| 符号 | Mira 使用点 |
| --- | --- |
| `submit_priority` | **0** |
| `submit_batch_priority` | **0** |
| `submit_fast`（v0.7.1 移除） | **0** |
| `TaskRouter` / `IScheduler` / `set_scheduler` | **0**（Mira 不注入自定义调度器） |
| `ResourceLease` / `try_reserve` | **0** |
| `task_inference` 相关公开面 | **0** |

Mira 的全部并发使用面是：默认调度器的单一异步池 + `submit_auto` 裸 callable 形态
（约 11 处）+ `SerialExecutionContext` + blocking I/O worker。

### 公开 API 差异的性质

`v0.7.2 → 78c6f557` 的 `include/` 差异是**纯增量**：

- 新增 `set_subject_max_in_flight_tasks` / `get_subject_max_in_flight_tasks` /
  `get_subject_in_flight_submissions` / `get_subject_quota_status`；
- 新增 `SubjectKey` / `kairo::subject()` / `TaskOptions::subject` / `TaskBuilder::subject()`；
- 新增 `submit_auto(SubjectKey, F&&, Args&&...)` 重载；
- 新增 `SubjectQuotaStatus::admitted_total` 与 `ExecutorFailureStatus::subject_quota_exhausted_count`
  与 `ExecutorFailureEvent::subject`；
- **唯一被移除的行**是私有成员：`AdmissionReleaser` 的构造签名与
  `try_admit_submission` 的内部参数表（新增 subject 形参）及随之更新的内部调用点。
  无任何**公开** API 被移除或改签名。

## 决策

1. **pin 升级到 `78c6f55753316769a0ae8140048bbd897f1f046c`**
   （`v0.7.2-14-g78c6f55`，MIT 不变）。
   `dependencies.lock.json`、`docs/supply-chain/sbom.cdx.json`、
   `docs/supply-chain/direct-dependencies.md` 同步。
   架构策略的 kairo 外部头族按 `kairo/` 前缀整族登记，新增头文件
   （`subject_quota_registry.hpp` 为上游**内部**头，`include/kairo/` 下仅
   `task_options.hpp`/`types.hpp`/`executor.hpp` 增量）无需改策略。
   **零代码迁移**：Mira 未触及任何被移除或语义变更的 API。
2. **即刻采纳（零成本、additive）**：分主体在途份额的运行期接口
   （`set_subject_max_in_flight_tasks` 等）与 `kairo::subject()` 裸形态带键重载。
   上游声明全部为 additive，默认行为与 v0.7.2 逐项一致（未配置主体与未带键提交
   零热路径开销）。**本轮不改 Mira 调用点**——按 DEC-069 的实施约束，迁移
   属 [M31](../plans/m31-capacity-admission-and-archival-recovery.md) 的工作项
   （`M31-03`），且 M31 的前置条件是「含 PR #223/#224 的可用版本已发布」。
3. **暂缓采纳并记录触发条件（不是拒绝）**：v0.7.1/v0.7.2 的调度能力
   （能力快照 TTL 缓存、`DeadlinePolicy`、`ResourceLease`、任务意图自动推断、
   优先级经调度器表达、`AdaptiveScheduler` 注入）——沿 [DEC-054](DEC-054-kairo-0.7.0-upgrade.md)
   的分级姿态：Mira 当前不注入调度器、不注册 GPU、不声明资源需求，缺少作用对象；
   触发条件为出现可复现的高价值任务被后台任务挤占的场景
   （`SchedulingMetrics` 拒绝计数异常或实测 queue wait 偏离）。
   退出通道：`set_scheduler(nullptr)` 回基线。
4. **未发布 master 的 pin 纪律**：本 pin 落在发布 tag 之后 14 个提交，
   属**未发布的上游 master**。因此：
   - pin **精确到 commit**，**不浮动**（不使用分支或 tag 范围）；
   - `dependencies.lock.json` 的 `version` 记录 `v0.7.2-14-g78c6f55`，
     使「未发布」这一事实本身可被机器读出；
   - 上游发版后**必须**复核并改 pin 到正式 tag（或复核后仍保留 master commit 并记录理由）；
   - 本轮不承诺 master 上的**后续**提交；任何新提交进入 Mira 都需重新走本决策的
     核对流程（暴露面检索 + 差异性质判定 + 全量回归）。
5. **不修改 `third_party/kairo` 源码**；缺口台账无 Open 条目
   （`EXE-20261028-001` 状态为 `Accepted`）。

## 备选方案

- **等上游发版再升级**：否决。DEC-069 的分主体份额是 M31 的硬前置，
  等 tag 会把 M31 的排期绑在一个上游发布节奏上；上游已明确该能力随下个 release
  发布，而 master 的 delta 已被上游 CI 覆盖（146/146、TSAN 0 报告）。
- **只挑 PR #223/#224 的提交做 cherry-pick 到 submodule**：否决。submodule 必须
  指向上游真实提交；本地拼出的提交无法被上游追溯，也会在下次 rebase 时冲突。
- **停留在 v0.7.2**：否决。该 tag 不含分主体份额，DEC-069 §1/B2 无法落地。
- **同时采纳 v0.7.1/v0.7.2 的调度能力**：否决（非拒绝，是暂缓）。见决策 3 的
  触发条件；缺作用对象时采纳只增加耦合面而无收益。

## 影响与风险

- **依赖面**：kairo pin 从 `32027c2` 前进 38 个提交，越过两个未消费发布。
  公开 API 差异为纯增量；唯一移除的是私有成员。
- **风险 D-1（未发布 master 的漂移风险）**：master 是移动目标；未来提交可能
  引入未评审变更。缓解：pin 精确到 commit 且**不浮动**；本决策记录了「任何新提交
  进入 Mira 都需重新核对」；发版后必须复核。
- **风险 D-2（v0.7.2 的破坏性变更落在消费面外）**：`submit_priority` 移除是
  0.7.x 唯一破坏性变更；Mira 零使用，但**若未来采纳优先级标注**，
  必须走 `submit_auto(TaskBuilder(fn).priority(p))` 而非已移除的 API。
- **风险 D-3（分桶键的泄漏面）**：分桶键被逐字复制进拒绝异常消息、
  `ExecutorFailureEvent::subject` 与状态快照，三者都流向日志
  （上游 R-2 约定）。缓解：M31 必须以**不可识别的派生标识**作键，
  不得直接使用 `TenantId`/`UserId` 原文（DEC-069 风险 X-R8）。
- **风险 D-4（覆盖路径窄导致配额静默失效）**：分桶键只在带键形态上被消费；
  Mira 现有 11 处 `submit_auto` 全是裸 lambda 形态。缓解：上游 PR #224 已提供
  `submit_auto(kairo::subject(key), fn)` 裸形态重载；M31 迁移到该形态，
  并用 `admitted_total` 诊断面逐主体确认键确实流过了接纳点。
- **风险 D-5（跨代低报）**：上游按成本取舍不暴露 `retired_in_flight`，
  配额查询在解除后重建的窗口内**低报**真实在途（差额上界 = 旧份额）。
  缓解：监控口径按「超额上界 + 自愈时间」而非「是否为零」；
  配额判定不得把该查询当精确值使用。

## 验证方式

- 本地（Linux x86_64、gcc 13、C++20）：`cmake --preset debug` +
  `cmake --build build/debug --parallel` + `ctest --test-dir build/debug`
  全量通过（含 `mira_docs_test`、`supply-chain` 标签）。
- `python3 tools/check_sbom.py .` 确认 submodule commit、lock 与 SBOM 三者一致。
- `python3 tools/check_architecture.py` 确认零新增违规。
- CI 矩阵（linux gcc/clang × Debug/Release、windows Debug/Release、
  android arm64/x86_64 NDK 交叉、sanitizers ASAN/TSAN/UBSAN、quality）全绿。
- 上游侧证据：PR #223/#224 的上游 CI 146/144 → 146/146 全绿、TSAN 0 报告。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-kairo-master-pin`](../plans/maintenance-2026-10-kairo-master-pin.md)
- 相关决策：[DEC-054](DEC-054-kairo-0.7.0-upgrade.md)（v0.7.0 升级与分级采纳姿态）、
  [DEC-048](DEC-048-kairo-0.6.0-migration.md)（依赖承载与迁移框架）、
  [DEC-068](DEC-068-interaction-coordinator-and-principal-input-entry.md)、
  [DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md)（分主体份额的实施约束）
- 依赖记录：[`dependencies.lock.json`](../../dependencies.lock.json)、
  [直接依赖表](../supply-chain/direct-dependencies.md)、
  [SBOM](../supply-chain/sbom.cdx.json)
- Executor 反馈：[`EXE-20261028-001`](../executor_feedback/ledger.md)
