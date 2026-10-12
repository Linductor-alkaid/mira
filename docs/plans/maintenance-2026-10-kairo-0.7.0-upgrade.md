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
- 后续轮（同分支追加，2026-10-12 用户确认）：
  - `MNT-202610-14` 修复存量问题：`WorkflowRuntime::drive()` 的撤回监视从
    池线程忙等（`submit_auto` + `sleep_for(5ms)` 循环，整个 drive 期间占死
    一个 worker）改为 kairo timer 驱动的 `submit_delayed` tick 链（5ms 节奏
    不变，互斥锁交接 + 栈帧退出守卫 join，覆盖异常路径）。
  - `MNT-202610-15` DEC-054"即刻采纳"落地：`MiraRuntime::scheduling_metrics()`
    返回 mira 自有 `SchedulingMetricsSnapshot`（kairo `get_scheduling_metrics()`
    的逐字段映射，不暴露 kairo 类型），初始化前/关闭后调用安全返回零值。

### 2.2 非目标

- 不注入 `AdaptiveScheduler`、不改任何 `submit_auto` 调用点的 `TaskOptions`
  （DEC-054：暂缓，触发条件见决策）。
- 不改控制面命令（pause/resume/cancel）在调用方线程上的有界收敛等待
  （受 `command_timeout` 约束，非池线程忙等，不属本次存量问题）。
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
- [x] `MNT-202610-13` CI 全矩阵绿（PR 验证）后回填证据。
- [x] `MNT-202610-14` 撤回监视改 kairo timer tick 链（含 IV-Agent 全量回归、
  定向复跑与 TSAN 定向验证）。
- [x] `MNT-202610-15` `MiraRuntime::scheduling_metrics()` 诊断出口接入。

## 5. 风险与阻塞

- 0.7.0 内部重构面较大（快照发布的标准库回落、增量拓扑排序、分片
  TaskMonitor、executor.cpp 拆分），但公开语义声明与 0.6.1 逐项一致；
  行为差异由全量 ctest 与 CI sanitizer 组合兜底。
- 本机仅 Linux GCC Debug；Windows/Android/sanitizer 由 PR CI matrix 覆盖。

## 6. 测试与退出条件

- [x] 本地全量 ctest 通过；`architecture-check`、`docs-check`、`check_sbom.py` 通过。
- [x] CI 全矩阵绿（含 Windows/Android/sanitizers/quality）。
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

2026-10-12：`MNT-202610-14`/`MNT-202610-15` 实现轮（同分支追加提交）。

- 构建：GCC 13.3.0 全目标零警告零错误；`architecture-check`（policy clean）、
  `docs-check` 通过。

2026-10-12：`MNT-202610-14`/`MNT-202610-15` 回归轮（Independent-Verification-Agent 执行）。

- 全量 ctest：**105/105 通过**（`ctest --output-on-failure -j14`，37.70s）。
- 监视器定向复跑：drive/暂停/取消/撤回覆盖集（`m9_control`、
  `agent_harness`、`m14_recovery`、`m24_fork_merge` 共 5 测试）×5 轮
  **25/25 全绿**。
- TSAN 定向：`tsan` 预设构建成功；本机内核高熵 ASLR 触发
  `unexpected memory mapping`（环境项，`setarch -R` 规避）后，5 个定向
  测试 4 轮 + 全量复跑 3 轮**零 data race 报告**（tick 链 5ms 续期 +
  exit guard 抢占路径干净）。
- 新接口冒烟（最小程序链 `libmira_core.a`，仓库零改动）：
  (a) 未初始化调用 `scheduling_metrics()` 全零且安全；(c) `finish_shutdown`
  后调用安全、单调。**结构性观察**：(b) 经 MiraRuntime 公开 API 计数恒为
  零——runtime 自身 executor 仅用于控制面 `submit_on_with_handle`
  （`src/runtime/runtime.cpp` 唯一提交点），不产生路由决策；13 字段映射
  经编译期指定初始化器与 kairo `types.hpp` 逐项核对一致，同一 kairo 构建
  直接 `submit_auto` 对照组计数正常。该出口反映 MiraRuntime 自有 executor
  的调度健康度；workflow/模型层的路由计数归其注入 executor 的所有方。
- 静态契约核对：`TimerHandle::cancel()` noexcept、析构不取消、
  `TimerSubmission` 形态与 exit guard 用法匹配；guard 声明晚于栈上
  `DriveFlags`（逆序析构保证 join 先于 flags 销毁）；`monitor_failures_`
  两处失败递增均落入既有诊断出口。

2026-10-12：PR #89（分支 `mnt/kairo-070-scheduling`）CI 验证。

- 第 1 轮（commit `6a834d1`）：quality job 失败——clang-tidy 18.1.8
  （warnings-as-errors）报 `RuntimeBaseline::Impl` 填充 87 字节（23 最优）：
  kairo 0.7.0 的 `kairo::Executor` 新增 alignas(64) 调度计数器后按值持有面
  填充 profile 恶化，触发 `optin.performance.Padding`。属升级连带影响而非
  本轮代码缺陷。修复（`8fa9f39`）：按 clang-tidy 建议最优序重排该 Impl 成员
  （无初始化顺序依赖，零行为变化），并对分支触碰文件执行 clang-format。
- 本地预防性复验（与 CI 同版本工具）：clang-format 18.1.8 format-check
  260 文件通过；clang-tidy 18.1.8 静态分析全树构建 exit 0、Mira 侧 TU 零
  告警（36 条告警全部来自 third_party 上游源，构成同 0.6.0 轮）；baseline
  ctest 通过。
- 第 2 轮（commit `8fa9f39`，runs
  [`38229572399`](https://github.com/Linductor-alkaid/mira/actions/runs/38229572399) /
  [`38229577564`](https://github.com/Linductor-alkaid/mira/actions/runs/38229577564)）：
  **24/24 检查全部通过**——Linux GCC/Clang Debug+Release、Windows Debug+Release、
  Android arm64+x86_64、ASAN/UBSAN/TSAN、quality（clang-tidy、clang-format、
  docs、SBOM、平台边界）。
- 合并：master `99461b3`（merge commit）；工作分支远端/本地均已删除。
