# 维护计划：kairo v0.7.0 升级轮（2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：[maintenance-2026-10-kairo-migration.md](maintenance-2026-10-kairo-migration.md)（PR #86 已合并）
> 建议发布点：依赖升级修订
> 更新日期：2026-10-12

## 1. 目标

将 kairo pin 从 `ef821dc`（`v0.6.0-12-gef821dc`）升级到 v0.7.0
（`32027c2`，additive：Scheduling Metrics、FeedbackAggregator、SchedulingPipeline、
AdaptiveScheduler），零代码迁移；同时完成新调度系统对本项目的适用性评估并形成
采纳决策（[DEC-054](../decisions/DEC-054-kairo-0.7.0-upgrade.md)）。

## 2. 范围与非目标

### 2.1 范围

- submodule：`third_party/kairo` → v0.7.0；`dependencies.lock.json`、
  `docs/supply-chain/sbom.cdx.json`、`docs/supply-chain/direct-dependencies.md` 同步。
- 适用性评估：梳理 Mira 的 Executor 使用面（约 10 个 `submit_auto` 提交点、
  单一默认池、无 QoS 定制、无 GPU 注册），对照上游适用指南形成 DEC-054
  的分级采纳决策（即刻/暂缓附触发条件/不适用）。

### 2.2 非目标

- 不注入 `AdaptiveScheduler`、不改任何 `submit_auto` 调用点的 `TaskOptions`
  （DEC-054：暂缓，触发条件见决策）。
- 不在本轮把 `get_scheduling_metrics()` 接入 Runtime 诊断出口（后续维护项）。
- 不修改 `third_party/kairo` 源码；台账无 Open 条目，无跟进行。

## 3. 设计与决策依据

- [DEC-054](../decisions/DEC-054-kairo-0.7.0-upgrade.md)：本轮全部决策
- [DEC-048](../decisions/DEC-048-kairo-0.6.0-migration.md)：0.6.0 迁移框架（不变）
- [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md)：所有权与关闭顺序约束不变
- 上游迁移指南：`third_party/kairo/docs/MIGRATION.md` §0.7.0（"无必需迁移"）
- 上游适用指南：`third_party/kairo/website/zh/guides/adaptive-scheduling.md`

## 4. 工作项

- [x] `MNT-202610-10` submodule 升级到 v0.7.0 并同步 lock、SBOM、
  direct-dependencies。
- [x] `MNT-202610-11` 新调度系统适用性评估与 DEC-054。
- [x] `MNT-202610-12` 全量回归（Independent-Verification-Agent 执行）与
  `architecture-check`/`docs-check`/SBOM 目标。
- [ ] `MNT-202610-13` CI 全矩阵绿（PR 验证）后回填证据。

## 5. 风险与阻塞

- 0.7.0 内部重构面较大（快照发布的标准库回落、增量拓扑排序、分片
  TaskMonitor、executor.cpp 拆分），但公开语义声明与 0.6.1 逐项一致；
  行为差异由全量 ctest 与 CI sanitizer 组合兜底。
- 本机仅 Linux GCC Debug；Windows/Android/sanitizer 由 PR CI matrix 覆盖。

## 6. 测试与退出条件

- [x] 本地全量 ctest 通过；`architecture-check`、`docs-check`、`check_sbom.py` 通过。
- [ ] CI 全矩阵绿（含 Windows/Android/sanitizers/quality）。
- [x] 锁定信息与实际依赖形态一致（`check_sbom.py` 比对通过）。

## 7. 验证记录

2026-10-12：实现轮（工作树基于 master `698fc77`）。

- submodule：`ef821dc` → `32027c2`（`v0.7.0`）；lock（version `0.7.0`）、SBOM
  （bom-ref/purl/version）、direct-dependencies 同步；`check_sbom.py` 通过
  （kairo@32027c247dd4）。
- 构建：`cmake --preset debug && cmake --build build/debug -j`（GCC 13.3.0，
  C++20）全目标（含 kairo、mbedtls、sqlite、Mira 全部库/测试/示例）零错误，
  零代码迁移。
- 治理：`docs-check`、`architecture-check`（policy clean）通过。

2026-10-12：回归轮（Independent-Verification-Agent 执行）。

- 全量 ctest：**105/105 通过，0 失败**（`ctest --output-on-failure -j14`，
  33.67s；contract 52、integration 43、consumer 4、benchmark 2 等）。
- 0.7.0 新诊断接口冒烟（最小程序链本构建树 `libkairo.a`，仓库零改动）：默认
  （DefaultScheduler）配置下 `get_scheduling_metrics()`/`get_feedback_snapshot()`
  在未初始化、运行中（8 个 `submit_auto` 后 `accepted_count` 精确 +8）、
  `shutdown(true)` 后三阶段 13/13 检查通过，计数单调；默认配置反馈快照为空属
  预期（`DefaultScheduler::wants_feedback() == false`，无样本流入）。
- Mira 自有编译单元（src/include/adapters/examples/tests）touch 后全量重编：
  零警告零错误。
- 范围限制（不构成回归证据）：kairo 上游自带测试套件未在 Mira 构建树注册
  （Mira 仅以库方式集成），AdaptiveScheduler/反馈聚合的上游完整测试未随本次
  ctest 覆盖；`wants_feedback()` 注入路径未在本仓库环境实测。Windows/Android/
  sanitizer 由 PR CI matrix 覆盖（`MNT-202610-13`）。
