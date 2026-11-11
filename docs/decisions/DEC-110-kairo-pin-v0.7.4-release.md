# DEC-110：kairo pin 升级到正式 release v0.7.4（履行 DEC-073 发版复核义务）

> 状态：Accepted
> 日期：2026-11-11
> 负责人：Mira Maintainers
> 冻结里程碑：维护轮（2026-11，kairo 定版 pin 升级）
> 替代/被替代：无（履行 [DEC-073](DEC-073-kairo-master-pin-subject-quota.md)
> 决策 4 的发版后复核义务；延续 [DEC-054](DEC-054-kairo-0.7.0-upgrade.md)、
> [DEC-048](DEC-048-kairo-0.6.0-migration.md) 的依赖承载与迁移框架）
> 来源：[DEC-073](DEC-073-kairo-master-pin-subject-quota.md) 决策 4 与
> [maintenance-2026-10-kairo-master-pin](../plans/maintenance-2026-10-kairo-master-pin.md)
> 的跨轮开放项 `MNT-KAIRO-07`

## 背景与问题

[DEC-073](DEC-073-kairo-master-pin-subject-quota.md) 于 2026-10-28 把 kairo pin
升到上游未发布 master 的 `78c6f557`（`v0.7.2-14-g78c6f55`），并约定：pin 精确到
commit 且不浮动；**上游发版后必须复核并改 pin 到正式 tag**（决策 4）。
该复核的触发条件——上游发布包含 PR #223/#224 的 tag——已由
`v0.7.3`（2026-10-29）满足并在 2026-11-06 确认（台账 `EXE-20261028-001`
与 [M31](../plans/m31-capacity-admission-and-archival-recovery.md) 前置条件
同步确认）；`v0.7.4`（2026-11-11）为该能力线上的收尾版本。
本决策把 pin 从「未发布 master」收敛到「正式 release」，消除风险 D-1
（未发布 master 漂移）。

### delta 的真实范围（78c6f557 → v0.7.4，23 个提交，跨两个发布）

| 版本 | 内容 | 对 Mira 的影响面 |
| --- | --- | --- |
| v0.7.3 | `TaskResult<T>` 轻量 Future 与 `submit_v2`；valued `CpuGpuTask`（`ResultAdapter` + `ExecutionReport`）；分主体份额 admission 正式发布（PR #223/#224 随版）；deadline-drop 异常分类竞态修复（`0564157`）；clang 14 value-path 修复 | 全部为新增 API 或修复；pin 快照已含分主体份额，差异为修复与新面 |
| v0.7.4 | `submit_v2` 更名 `submit_task_result`（`[[deprecated]]` 别名保留至 0.9.0 前）；admission scope 改 `string_view` 直传（维持 4 allocs）；**空 `cpu_affinity` 默认从自动绑核 `[0..hw-1]` 改为 OS 自由调度**（唯一行为变更，显式设值路径不变） | 更名落在 Mira 从未消费的 API 上；默认调度语义变更**适用**于 Mira（见下） |

### Mira 的暴露面核对（2026-11-11 静态核对）

| 符号 | Mira 使用点 |
| --- | --- |
| `submit_v2` / `submit_task_result` / `TaskResult` | **0**（`submit_v2` 为 v0.7.3 新增、v0.7.4 更名，pin 快照从未包含，不存在消费面） |
| `CpuGpuTask` / `make_cpu_gpu_task` | **0**（valued 改造移除 void 形态 `submit_auto(CpuGpuTask)` 重载，无影响） |
| `cpu_affinity`（`ThreadPoolConfig` / `ExecutorConfig`） | **0**（Mira 不显式设值，走默认路径） |
| `affinity_mismatch` 指标 | 保留（`src/runtime/runtime.cpp` 读取 `affinity_mismatch_count`）；未绑核时 advisory 检查自然跳过、计数保持 0，字段存在性与语义不变 |
| 既有提交面（`submit_auto` / `submit_after` / `submit_batch`，17 处） | 签名与语义不变 |

## 决策

1. **pin 升级到 `10e870bcb94b109a1a1f32eab8ef0d3d9f01055b`**（`v0.7.4`，
   2026-11-11，MIT 不变）。`dependencies.lock.json`（`version` 回归 tag 形态
   `v0.7.4`，「未发布」状态消除）、`docs/supply-chain/sbom.cdx.json`、
   `docs/supply-chain/direct-dependencies.md` 同步。
   **零代码迁移**：Mira 未触及任何被移除或更名的 API。
2. **接受空 `cpu_affinity` 默认 OS 自由调度**（上游动机：静态编号绑核在
   big.LITTLE / P-E 混合架构上系统性把 worker 钉在小核，且 pinned worker
   无法迁移到空闲大核）。Mira 的默认异步池、blocking I/O 与 realtime 路径
   均不显式设 affinity，故默认路径语义变更对 Mira 生效；对 Android
   宿主（big.LITTLE 常态）为正向收益。若未来实测需要恢复旧默认，显式传入
   全核列表（`cpu_affinity = {0..hw-1}`）即可，语义与上游文档一致。
   realtime 路径的 round-robin 自动绑核语义上游本次未变，Mira 无需动作。
3. **分主体份额消费姿态不变**：v0.7.3/v0.7.4 未改变 PR #223/#224 的语义面；
   [M31](../plans/m31-capacity-admission-and-archival-recovery.md) `M31-03`
   的带键形态迁移与临时方案移除仍按原计划执行，本决策不改动其约束。
4. **不修改 `third_party/kairo` 源码**；缺口台账无新增 Open 条目。
5. **pin 纪律延续**：正式 tag 上的 pin 仍精确到 commit 且不浮动；
   后续升级沿用本决策与 DEC-073 的核对流程（暴露面检索 + 差异性质判定 +
   全量回归）。

## 备选方案

- **保留 `78c6f557` master commit 并记录理由**：否决。该状态的存在意义
  （承载未发布能力）已随 v0.7.3/v0.7.4 发布消失；继续停留等于自愿承担
  DEC-073 风险 D-1 而无对应收益。
- **跳过 v0.7.4、直接 pin 到未来版本**：否决。无目标能力诉求时的 pin
  升级以「收敛到已复核的正式 release」为原则，不为不存在的需求引入
  未消费 delta。
- **回退空 `cpu_affinity` 旧行为（显式传全核列表）**：否决。静态绑核在
  Mira 的目标硬件（Android 大小核）上是负优化；接受上游默认并保留
  显式恢复路径（决策 2）。

## 影响与风险

- **依赖面**：kairo pin 从 `78c6f557` 前进 23 个提交到 `10e870bc`
  （v0.7.3 + v0.7.4 两个正式发布）。公开 API 差异为纯增量 + 一次落在
  零消费面上的更名；唯一语义变更是空 `cpu_affinity` 默认值（决策 2 接受）。
- **风险 R-1（默认调度语义变更的观测缺口）**：worker 由绑核转为自由调度
  可能改变吞吐/延迟特征。缓解：本轮全量回归 + CI 矩阵取证；上线后通过
  `ThreadPoolStatus::cpu_affinity_applied`（新增，未绑核时为 false）与
  既有 metrics 观测；实测劣化时按决策 2 的恢复路径回退。
- **风险 R-2（M31 引用的 pin 快照表述过期）**：M31 与台账的历史记录中
  「当前 pin `78c6f55`」为 2026-11-06 时点证据，不回溯改写；本决策与
  供应铰记录为当前状态的唯一权威来源。

## 验证方式

- 本地（Linux x86_64、gcc 13、C++20）：`cmake --preset debug` + 全量构建 +
  `ctest` 全量回归，结果见
  [maintenance-2026-11-kairo-pin-release](../plans/maintenance-2026-11-kairo-pin-release.md)
  验证记录（测试执行由 Independent-Verification-Agent 独立完成并回报证据）。
- `python3 tools/check_sbom.py .` 确认 submodule commit、lock 与 SBOM 三者一致。
- `python3 tools/check_architecture.py` 确认零新增违规。
- CI 矩阵（linux gcc/clang × Debug/Release、windows Debug/Release、
  android arm64/x86_64 NDK 交叉、sanitizers ASAN/TSAN/UBSAN、quality）全绿——
  **已取证**：[PR #127](https://github.com/Linductor-alkaid/mira/pull/127)
  head `02e4045`，两个 workflow run 各 12 个 job 全 success，合计 **24/24**
  （[push](https://github.com/Linductor-alkaid/mira/actions/runs/42974127379)、
  [pull_request](https://github.com/Linductor-alkaid/mira/actions/runs/42974175498)）。

## 关联文档和工作项

- 工作项：[maintenance-2026-11-kairo-pin-release](../plans/maintenance-2026-11-kairo-pin-release.md)
  （履行 `MNT-KAIRO-07`）
- 相关决策：[DEC-073](DEC-073-kairo-master-pin-subject-quota.md)（被履行的
  发版复核义务）、[DEC-054](DEC-054-kairo-0.7.0-upgrade.md)、
  [DEC-048](DEC-048-kairo-0.6.0-migration.md)（依赖承载与迁移框架）、
  [DEC-069](DEC-069-bounded-capacity-and-forgetting-lifecycle.md)（分主体份额
  消费约束，姿态不变）
- 依赖记录：[`dependencies.lock.json`](../../dependencies.lock.json)、
  [直接依赖表](../supply-chain/direct-dependencies.md)、
  [SBOM](../supply-chain/sbom.cdx.json)
- 上游：kairo `v0.7.3` / `v0.7.4` release notes 与 `docs/MIGRATION.md`
  升级注意（submodule 内随版）
