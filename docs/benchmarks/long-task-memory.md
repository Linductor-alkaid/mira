# M4 长任务与 Memory Benchmark

> 状态：Active
> 版本：1.0
> 更新日期：2026-09-03
> 负责人：Mira Maintainers
> 关联工作项：[`M4-17`](../plans/m4-context-memory-recovery.md)

## 1. 目的与范围

本基准衡量 M4 有状态基线在长任务、记忆检索、压缩与 Provider 切换下的确定性表现。
所有场景离线运行（无网络、无真实 Provider、无平台输入），结论只覆盖本机与 CI 声明的
构建组合，不外推到真机延迟或生产吞吐。

## 2. 方法

- Harness：`tests/benchmark/m4_stateful_benchmark.cpp`（ctest 名
  `mira_m4_stateful_benchmark`，断言保真不变量，回归即测试变红）。
- 输出：单份 JSON manifest（stdout 及可选文件参数）。
- 场景：
  - `long_task_recovery`：125 个事件的会话按水位检查点，逐前缀恢复；统计
    goal/安全约束/未决副作用保真、重复副作用数、检查点写入与恢复延迟分位数。
  - `memory_retrieval`：400 条语料、60 次混合检索（exact+FTS），对比 no-memory
    基线命中（0）与 configured-memory 命中及 p50/p95/p99。
  - `compaction_impact`：200 条历史事件全量入上下文 vs 检查点摘要压缩后的 token
    估算与 P0/P1 保留。
  - `provider_switch`：continuation 绑定失效后的本地检查点重建成本。
- 统计口径：每次运行采样分位数（非跨机均值）；成本代理为 conservative token 上界，
  不是计费 token。

## 3. 基线结果

环境 A（开发机，Ubuntu 24.04，x86_64，GCC 13.3.0，CMake Debug，单线程通道；采样于
2026-09-03，`m4_stateful_benchmark` 输出）：

```json
{
  "long_task_recovery": {
    "checkpoints_written": 2,
    "recoveries": 4,
    "fidelity_failures": 0,
    "repeated_side_effects": 0,
    "checkpoint_put_p50_ms": 13.71,
    "checkpoint_put_p95_ms": 13.71,
    "recovery_p50_ms": 2.75,
    "recovery_p95_ms": 3.03
  },
  "memory_retrieval": {
    "corpus_size": 400,
    "queries": 60,
    "no_memory_hits": 0,
    "configured_memory_hits": 60,
    "query_p50_ms": 16.53,
    "query_p95_ms": 22.51,
    "query_p99_ms": 28.91
  },
  "compaction_impact": {
    "full_history_tokens": 5645,
    "compacted_tokens": 91,
    "full_p0_p1_kept": 2,
    "compacted_p0_p1_kept": 2,
    "token_savings_ratio": 0.984
  },
  "provider_switch": {
    "continuation_invalid_on_switch": 1,
    "rebuild_tokens": 60
  }
}
```

CI 环境（GitHub Actions ubuntu-24.04）在每次 pipeline 以
`ctest -R mira_m4_stateful_benchmark` 复跑同一 harness；CI 数值受共享 runner 影响，
只用于回归对比，不作为性能声明。

## 4. 结论

- 恢复保真：全部恢复点 goal、安全约束、未决副作用一致，重复副作用为 0（满足
  M4 退出条件对副作用重复执行为零的要求）。
- 记忆增益：no-memory 基线命中 0/60，configured-memory 命中 60/60；混合检索在
  400 条语料下 p95 约 22ms（本机 Debug）。
- 压缩收益：检查点摘要使估算 token 从 5645 降至 91（约 98%），P0/P1 项保持完整。
- Provider 切换：continuation 绑定判定失效并回退本地检查点重建（60 token 代理），
  与设计 §15.2 一致。

## 5. 限制

- 全部场景离线确定性运行；未测量真实 Provider 网络往返、SQLite 磁盘碎片或真机
  存储延迟。
- 延迟分位数来自单次运行内采样，样本量小（检查点 2 次、检索 60 次），只用于
  回归监测，不构成统计显著的性能声明。
- token 为保守估算上界（ConservativeEstimate），非 exact count，也不是计费口径。
- watermark 触发的检查点受最小增量策略约束（阶段性恢复点语义），因此
  `checkpoints_written` 不等于事件边界数；显式 Pause 触发补齐尾部覆盖。

## 6. 复现

```sh
cmake --preset debug
cmake --build --preset debug --target mira_m4_stateful_benchmark
./build/debug/tests/mira_m4_stateful_benchmark   # JSON 输出到 stdout
```
