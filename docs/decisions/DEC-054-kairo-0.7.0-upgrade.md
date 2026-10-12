# DEC-054：随附并发库升级 kairo v0.7.0 与新调度系统采纳姿态

> 状态：Accepted
> 日期：2026-10-12
> 负责人：Mira Maintainers
> 冻结里程碑：维护轮（2026-10，kairo 0.7.0 升级）
> 替代/被替代：无（延续 [DEC-048](DEC-048-kairo-0.6.0-migration.md) 的依赖承载与迁移框架）

## 背景与问题

Mira 的 kairo pin 停留在 `ef821dc`（`v0.6.0-12-gef821dc`）。上游随后发布
v0.6.1（结构化调度决策、`SchedulingMetrics`、反馈通道）与 v0.7.0（Runtime-aware
Scheduling：`FeedbackAggregator`、四阶段 `SchedulingPipeline`、opt-in 的
`AdaptiveScheduler`）。上游明确 0.7.0 为 additive 版本：默认行为与 0.6.1 逐项一致，
0.6.1 契约测试一字不改通过，不迁移任何代码即可升级。继续停留旧 pin 将错过调度
可观测性与后续自适应调度窗口，且升级破坏面只会随版本差距积累。

新调度能力一览（`third_party/kairo/docs/MIGRATION.md` §0.7.0、
`third_party/kairo/website/zh/guides/adaptive-scheduling.md`）：

- **`SchedulingMetrics`**（0.6.1，默认开启）：按决策 status/reason/diagnostics 与
  deadline miss 分类的无锁单调计数，经 `Executor::get_scheduling_metrics()` 读取，
  无需配置。
- **`FeedbackAggregator`**（0.7.0 M2）：worker 线程无锁样本累加 + RCU 快照读，
  按 (backend, executor, qos) 键给出 queue wait / 执行时长 EWMA 与直方图 p99；
  仅当注入的 `IScheduler` 声明 `wants_feedback()` 时才有样本流入，经
  `Executor::get_feedback_snapshot()` 诊断读取。
- **`SchedulingPipeline`**（0.7.0 M1）：约束过滤 → 候选生成 → 评分 → 选择的可组合
  公开组件；`DefaultScheduler` 行为不变。
- **`AdaptiveScheduler`**（0.7.0 M3，`set_scheduler()` 显式注入）：CPU/GPU 历史选择、
  QoS 感知降载（对严格更低 QoS 类返回 `reason = LoadShedding` 结构化拒绝）、
  QoS→priority 有界提升；`set_scheduler(nullptr)` 即回基线。

Mira 侧现状：全树 60 处 `<kairo/executor.hpp>` + 1 处 `<kairo/blocking_io.hpp>`，
全部任务经默认调度器的单一异步池提交（约 10 个 `submit_auto` 提交点），未使用
`TaskOptions` 的 QoS/优先级定制，未注册 GPU 执行器，无自定义 `IScheduler`。

## 决策

1. **升级 pin 到 kairo v0.7.0**（`32027c247dd4b9280d7c995454b732bac0db9095`，MIT
   不变）。零代码迁移：Mira 未做对 `RoutingReason` 的穷举 switch、未实现自定义
   `IScheduler`、按字段名读取 `SchedulingMetrics`，全部命中上游声明的"不受影响"面。
   `dependencies.lock.json`、`docs/supply-chain/sbom.cdx.json`、
   `docs/supply-chain/direct-dependencies.md` 同步；架构策略的 kairo 外部头族按
   `kairo/` 前缀整族登记，新增头文件（`adaptive_scheduler.hpp` 等）无需改策略。
2. **即刻采纳（零成本、additive）**：`get_scheduling_metrics()` 计数随升级可用。
   后续维护项将其接入 Runtime 诊断出口（`IAgentObserver` 事件或诊断快照），服务
   AGENTS.md"队列拒绝必须转化为明确结果与事件"的既有要求；本轮不改代码。
3. **暂缓采纳并记录触发条件（不是拒绝）**：
   - **提交点 QoS 标注**（模型驱动/验证走 `Interactive`/`Critical`，工具调用走
     `Standard`，上下文整理与后台归档走 `BestEffort`）：Mira 任务谱系存在天然
     分层，但当前无排队压力证据；待出现高价值任务被后台任务挤占的可复现场景
     （`SchedulingMetrics` 的 rejected 计数或实测 queue wait 异常）再引入，
     届时仅改 `submit_auto` 调用点的 `TaskOptions`，无接口变更。
   - **`AdaptiveScheduler` 注入**：按上游适用指南，三类决策均以真实 QoS 分层为
     前提；Mira 当前单一默认 QoS，降载与提升没有作用对象；且调用方尚无
     `LoadShedding` 结构化拒绝的退避处理。触发条件：上一项落地后出现持续
     过载形态。退出通道：`set_scheduler(nullptr)` 回基线。
   - **`FeedbackAggregator` 诊断接入**：需注入 `wants_feedback()` 调度器才有
     数据流，与上一项绑定推进；接入形态建议走 Replay 事件流（低频快照）。
4. **不适用项**：CPU/GPU 历史选择（Mira 无 GPU 执行器注册）、
   `SchedulingPipeline` 自定义评分组件（无自定义路由需求，单池形态）。

## 影响与风险

- 行为面：零（默认调度器与 0.6.1/0.6.0-12 逐项一致；0.6.0-12 → 0.7.0 跨越的
  0.6.1 变更同样 additive，Mira 消费面不受影响）。
- 构建/CI：全部目标与 CI matrix 重验（0.7.0 含快照发布的标准库回落、增量拓扑
  排序、分片 monitor 等内部重构，Windows/Android/sanitizer 编译差异由 CI 覆盖）。
- 台账：`EXE-*` 全部条目已 Resolved，本轮无新增缺口记录。

## 验证方式

- 全树构建零警告零错误；全量 ctest 通过；`architecture-check`、`docs-check`、
  `check_sbom.py` 通过（见维护计划验证记录）。
- CI 全矩阵（Linux GCC/Clang、Windows、Android、ASAN/UBSAN/TSAN、quality）绿。

## 关联文档和工作项

- [DEC-048](DEC-048-kairo-0.6.0-migration.md)：0.6.0 迁移框架（不变）
- [DEC-001](DEC-001-runtime-executor-ownership.md)：Runtime-Executor 所有权（不变）
- 维护计划：[maintenance-2026-10-kairo-0.7.0-upgrade.md](../plans/maintenance-2026-10-kairo-0.7.0-upgrade.md)
- 上游：[kairo v0.7.0 release](https://github.com/Linductor-alkaid/kairo/releases/tag/v0.7.0)、
  `third_party/kairo/docs/MIGRATION.md` §0.7.0、
  `third_party/kairo/website/zh/guides/adaptive-scheduling.md`
