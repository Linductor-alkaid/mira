# 维护计划：kairo 定版 pin 升级轮（2026-11）

> 状态：In Progress（2026-11-11 立项；升级与决策已冻结，验证取证进行中）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（维护轮）
> 前置：[DEC-110](../decisions/DEC-110-kairo-pin-v0.7.4-release.md)（已冻结）；
> 履行 [maintenance-2026-10-kairo-master-pin](maintenance-2026-10-kairo-master-pin.md)
> 的跨轮开放项 `MNT-KAIRO-07`
> 建议发布点：依赖升级修订
> 更新日期：2026-11-11

## 1. 目标

履行 DEC-073 决策 4 与 `MNT-KAIRO-07` 的发版复核义务：上游已发布包含
PR #223/#224 的正式 tag（v0.7.3，收尾于 v0.7.4），把 kairo pin 从
`78c6f557`（`v0.7.2-14-g78c6f55`，未发布 master）收敛到
`10e870bc`（`v0.7.4`，2026-11-11），消除「未发布 master 漂移」风险
（DEC-073 风险 D-1），并复核 v0.7.3/v0.7.4 两个发布的暴露面与差异性质，
形成 [DEC-110](../decisions/DEC-110-kairo-pin-v0.7.4-release.md)。

## 2. 范围与非目标

### 2.1 范围

- submodule `third_party/kairo` → `10e870bcb94b109a1a1f32eab8ef0d3d9f01055b`
  （tag `v0.7.4`）。
- 依赖记录同步：`dependencies.lock.json`、`docs/supply-chain/sbom.cdx.json`、
  `docs/supply-chain/direct-dependencies.md`。
- 暴露面核对：v0.7.3/v0.7.4 被移除、更名或语义变更的公开 API 在 Mira
  全仓的使用点检索。
- 差异性质判定：`78c6f557 → v0.7.4` 的 `include/` 差异性质；唯一行为变更
  （空 `cpu_affinity` 默认 OS 自由调度）的适用性判定与采纳决策。
- 全量回归与门禁取证。

### 2.2 非目标

- **不**迁移任何 `submit_auto` 调用点到带键形态或 `submit_task_result`/
  `TaskResult` 新面——前者属 [M31](m31-capacity-admission-and-archival-recovery.md)
  `M31-03`，后者无消费诉求。
- **不**回退空 `cpu_affinity` 旧行为（DEC-110 决策 2 接受新默认，保留显式
  恢复路径）。
- **不**修改 `third_party/kairo` 源码。
- **不**回溯改写历史决策/计划中的 pin 快照表述（DEC-110 风险 R-2）。

## 3. 设计与决策依据

- [DEC-110](../decisions/DEC-110-kairo-pin-v0.7.4-release.md)：本轮全部决策。
- [DEC-073](../decisions/DEC-073-kairo-master-pin-subject-quota.md)：被履行的
  发版复核义务与 pin 纪律（决策 4、5）。
- 上游：kairo `v0.7.3`/`v0.7.4` release notes、`docs/MIGRATION.md` §升级注意、
  `fb86413`（cpu_affinity 默认变更）与 `0564157`（deadline-drop 竞态修复）。

## 4. 工作项

- [x] `MNT-KRELEASE-01` 暴露面核对：`submit_v2`/`submit_task_result`/
      `TaskResult`、`CpuGpuTask`、`cpu_affinity` 配置、`affinity_mismatch`
      指标在 Mira 全仓使用点检索——前三项 **0**；`cpu_affinity` 配置 **0**
      （默认路径变更适用）；`affinity_mismatch` 字段在 v0.7.4 保留且语义不变。
- [x] `MNT-KRELEASE-02` 差异性质判定：`78c6f557 → v0.7.4` 公开 API 为
      纯增量 + 一次零消费面更名（`submit_v2` → `submit_task_result`，
      pin 快照从未含 `submit_v2`）；唯一行为变更为空 `cpu_affinity`
      默认 OS 自由调度（DEC-110 决策 2 采纳）。
- [x] `MNT-KRELEASE-03` submodule pin 升级与依赖记录同步（lock / SBOM /
      直接依赖表）；`python3 tools/check_sbom.py .` 通过。
- [x] `MNT-KRELEASE-04` 决策冻结：[DEC-110](../decisions/DEC-110-kairo-pin-v0.7.4-release.md)。
- [x] `MNT-KRELEASE-05` 本地构建与全量回归（Independent-Verification-Agent
      独立执行并回报证据）。
- [x] `MNT-KRELEASE-06` `python3 tools/check_architecture.py` 零新增违规。
- [ ] `MNT-KRELEASE-07` CI 全矩阵取证（linux gcc/clang × Debug/Release、
      windows Debug/Release、android arm64/x86_64 NDK 交叉、sanitizers
      ASAN/TSAN/UBSAN、quality）；完成前本轮不关闭，完成后在 DEC-110
      验证方式与本节回填。

## 5. 风险与阻塞

| 风险 | 说明 | 缓解 / 归属 |
| --- | --- | --- |
| `MNT-KRELEASE-R1` 默认调度语义变更的观测缺口 | worker 由绑核转自由调度，吞吐/延迟特征可能变化 | 全量回归 + CI 矩阵；`cpu_affinity_applied` 可观测；实测劣化走 DEC-110 决策 2 恢复路径 |
| `MNT-KRELEASE-R2` CI 平台差异 | 本地仅覆盖 Linux x86_64/gcc | Windows MSVC 与 Android NDK 交叉由 CI 矩阵取证；`MNT-KRELEASE-07` 未完成前本轮不关闭 |

**外部阻塞**：无。上游 v0.7.4 已发布，上游 CI 全绿。

## 6. 验证记录

### 2026-11-11：暴露面核对与 pin 升级完成

- 暴露面检索（`git grep` 全仓，`src/` `include/` `tests/` `apps/` `tools/`）：
  `submit_v2` / `submit_task_result` / `TaskResult` = 0；
  `CpuGpuTask` / `make_cpu_gpu_task` / `register_gpu` = 0；
  `cpu_affinity` 配置设值 = 0（仅 `runtime.cpp` 读取 `affinity_mismatch_count`）。
- submodule 已 checkout `v0.7.4`（`10e870bc`）；lock / SBOM / 直接依赖表已同步。

### 2026-11-11：本地独立验证完成（Independent-Verification-Agent）

- 环境：Linux x86_64、g++ 13.3.0、Ninja、C++20、Debug preset
  （`MIRA_BUILD_TESTS=ON`、`MIRA_WARNINGS_AS_ERRORS=ON`）；本机无 clang++，
  clang/Release/sanitizer preset 不在本地范围，由 CI 矩阵承担。
- 命令与结果（全新配置构建，`build/debug` 删除重建）：
  - `cmake --preset debug` + `cmake --build build/debug --parallel` →
    EXIT=0，零错误；37 条告警全部来自 `third_party/kairo` 上游源码
    （`-Wunused-parameter`/`-Wshadow`/`-Wredundant-decls`/`-Wsign-conversion`
    等，Mira 自研代码 0 告警；`MIRA_WARNINGS_AS_ERRORS` 仅作用于 Mira
    目标，构建通过）。
  - `ctest --test-dir build/debug --output-on-failure` →
    **`100% tests passed, 0 tests failed out of 182`**（含 `mira_docs_test`、
    `mira_sbom_test`；上轮基线 113+，现 182，无缩减）。执行器相关抽查
    `mira_executor_integration_test`、`mira_m3_tool_executor_test` 复跑均绿。
  - `python3 tools/check_sbom.py .` → `Dependency lock and SBOM: OK
    (kairo@10e870bcb94b, mbedtls@068ff080b369, sqlite@3.53.4)`。
  - `python3 tools/check_architecture.py` → `architecture policy is clean`
    （baseline 16 条既有抑制项，0 新增、0 过期）。
- 说明：kairo 自身测试套不编入 Mira 构建树（Mira 以 add_subdirectory 消费
  其构建目标），上游行为由 Mira 182 项集成回归间接覆盖；`cpu_affinity`
  默认变更的性能影响属性能范畴，本轮仅功能回归，不做基准、不宣称性能保证。
- 未完成：`MNT-KRELEASE-07` CI 全矩阵取证——待变更提交并走 PR 后回填，
  完成前本轮保持 In Progress、不关闭。
