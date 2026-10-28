# 维护计划：kairo 未发布 master pin 升级轮（2026-10）

> 状态：In Progress（实现与本地验证完成，待 CI 全矩阵取证后关闭）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M28 立项轮附带的依赖升级维护轮）
> 前置：[DEC-073](../decisions/DEC-073-kairo-master-pin-subject-quota.md)（已冻结）；
> 上游 PR [#223](https://github.com/Linductor-alkaid/kairo/pull/223) 与
> [#224](https://github.com/Linductor-alkaid/kairo/pull/224) 已合并入上游 master
> 建议发布点：依赖升级修订
> 更新日期：2026-10-28

## 1. 目标

将 kairo pin 从 `32027c2`（`v0.7.0`）升级到上游 master 的 `78c6f557`
（`v0.7.2-14-g78c6f55`），以消费 DEC-069 §1/B2 所需的**分主体在途份额**能力
（上游 PR #223/#224）；同时核对跨越 v0.7.1/v0.7.2 两个未消费发布带来的
公开 API 差异，形成
[DEC-073](../decisions/DEC-073-kairo-master-pin-subject-quota.md) 的
升级决策与分级采纳姿态。

## 2. 范围与非目标

### 2.1 范围

- submodule `third_party/kairo` → `78c6f55753316769a0ae8140048bbd897f1f046c`。
- 依赖记录同步：`dependencies.lock.json`、`docs/supply-chain/sbom.cdx.json`、
  `docs/supply-chain/direct-dependencies.md`。
- 暴露面核对：v0.7.1/v0.7.2 被移除或语义变更的公开 API 在 Mira 全仓的使用点检索。
- 差异性质判定：`v0.7.2 → 78c6f557` 的 `include/` 差异是纯增量还是含移除。
- 分级采纳：分主体份额即刻采纳（不改 Mira 调用点）；调度类能力暂缓并记录触发条件。
- 未发布 master 的 pin 纪律与发版后复核约定。

### 2.2 非目标

- **不**在本轮迁移任何 `submit_auto` 调用点到带键形态——按 DEC-069 的实施约束，
  属 [M31](m31-capacity-admission-and-archival-recovery.md) 的 `M31-03` 工作项。
- **不**注入 `AdaptiveScheduler`、不启用 `DeadlinePolicy` / `ResourceLease` /
  能力快照缓存 / 任务意图自动推断（DEC-073 决策 3 的暂缓项）。
- **不**修改 `third_party/kairo` 源码。
- **不**实现 DEC-069/M31 的容量准入、归档与遗忘本体。

## 3. 设计与决策依据

- [DEC-073](../decisions/DEC-073-kairo-master-pin-subject-quota.md)：本轮全部决策。
- [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
  §1/B2：分主体份额是本轮升级的**目标能力**；其三条实施约束（消费原生能力、
  迁移带键提交形态、记录弱一致与代际语义）落在 M31。
- [DEC-054](../decisions/DEC-054-kairo-0.7.0-upgrade.md)：上一次 kairo 升级的
  分级采纳姿态与 pin 纪律。
- [DEC-048](../decisions/DEC-048-kairo-0.6.0-migration.md)：依赖承载与迁移框架。
- 上游：PR #223/#224 与冻结设计
  `../../third_party/kairo/docs/design/subject_bucket_admission.md`
  （D-1～D-5 + 修订 R-1/R-2/R-3）。

## 4. 工作项

- [x] `MNT-KAIRO-01` 暴露面核对：v0.7.1/v0.7.2 被移除或语义变更的公开 API
      （`submit_priority`、`submit_batch_priority`、`submit_fast`、`TaskRouter` /
      `IScheduler` / `set_scheduler`、`ResourceLease` / `try_reserve`）在 Mira 全仓
      使用点检索——**全部为 0**。
- [x] `MNT-KAIRO-02` 差异性质判定：`v0.7.2 → 78c6f557` 的 `include/` 差异为
      **纯增量**；唯一被移除的行是私有成员（`AdmissionReleaser` 构造签名、
      `try_admit_submission` 内部参数表及内部调用点），无公开 API 移除或改签名。
- [x] `MNT-KAIRO-03` submodule pin 升级与依赖记录同步（lock / SBOM / 直接依赖表）；
      `python3 tools/check_sbom.py .` 通过。
- [x] `MNT-KAIRO-04` 本地构建与全量回归（见 §6）。
- [x] `MNT-KAIRO-05` 分级采纳决策与未发布 master 的 pin 纪律（DEC-073 决策 3、4）。
- [ ] `MNT-KAIRO-06` CI 全矩阵取证（linux gcc/clang × Debug/Release、
      windows Debug/Release、android arm64/x86_64 NDK 交叉、sanitizers
      ASAN/TSAN/UBSAN、quality）——见 §6 验证记录。
- [ ] `MNT-KAIRO-07` 上游发版后复核并改 pin 到正式 tag（或复核后仍保留 master commit
      并记录理由）。**触发条件**：上游发布包含 PR #223/#224 的 tag。
      本项在发版前保持未完成。

## 5. 风险与阻塞

| 风险 | 说明 | 缓解 / 归属 |
| --- | --- | --- |
| `MNT-KAIRO-R1` 未发布 master 漂移 | pin 落在发布 tag 之后 14 个提交，master 是移动目标 | pin 精确到 commit 且不浮动；新提交进入 Mira 需重新走 DEC-073 的核对流程；`MNT-KAIRO-07` 保留发版后复核。DEC-073 风险 D-1 |
| `MNT-KAIRO-R2` 破坏性变更落在消费面外 | v0.7.2 移除 `submit_priority` | Mira 零使用；若未来采纳优先级标注须走 `submit_auto(TaskBuilder(fn).priority(p))`。DEC-073 风险 D-2 |
| `MNT-KAIRO-R3` CI 平台差异 | 本地仅覆盖 Linux x86_64/gcc | Windows MSVC 与 Android NDK 交叉由 CI 矩阵取证；`MNT-KAIRO-06` 未完成前本轮不关闭 |

**外部阻塞**：无。上游 PR #223/#224 已合并且上游 CI 绿。

## 6. 验证记录

### 2026-10-28：实现、本地验证完成

- 环境：Linux x86_64、gcc 13.3.0、CMake 3.28.3、Ninja、C++20；
  kairo pin `78c6f55753316769a0ae8140048bbd897f1f046c`（`v0.7.2-14-g78c6f55`）。
- 命令与结果：
  - `cmake --preset debug` → Configuring/Generating done；
  - `cmake --build build/debug --parallel 14` → 100%，零错误零告警；
  - `ctest --test-dir build/debug --output-on-failure` → **仅 `mira_docs_test` 失败**，
    原因是 `docs/supply-chain/direct-dependencies.md` 引用了本轮尚未创建的
    DEC-073（文档链接门禁按设计拦截悬空引用）；补齐 DEC-073 后复跑通过。
  - `python3 tools/check_sbom.py .` → `Dependency lock and SBOM: OK
    (kairo@78c6f5575331, mbedtls@068ff080b369, sqlite@3.53.4)`。
  - `python3 tools/check_architecture.py` → `architecture policy is clean`
    （零新增违规）。
- 限制：本轮**未**在本地执行 Windows MSVC 与 Android NDK 交叉构建、
  也没有本地跑 ASAN/TSAN/UBSAN 三条 sanitizer 套件——
  这些由 CI 矩阵承担，`MNT-KAIRO-06` 在 CI 全绿前保持未完成。
  上游侧证据：PR #223/#224 的上游 CI 146/146 全绿、TSAN 0 报告
  （上游 PR 正文记录）。
- 未执行的验证项与补跑条件：见 `MNT-KAIRO-06`（CI 全矩阵）与 `MNT-KAIRO-07`
  （上游发版后复核）。
