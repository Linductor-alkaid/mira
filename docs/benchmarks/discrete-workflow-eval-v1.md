# 离散动作与 Workflow 评估基线 v1（recorded 首轮）

> 状态：Active（首轮基线，登记于 2026-09-12）
> 负责人：Mira Maintainers
> 适用范围：[最小评估 Profile](../design/discrete_workflow_eval_profile.md) v1.0 的
> recorded 确定性轮（DEC-034）；live canary 与真实平台组不在本报告内
> 复现命令见文末

## 1. 目的与范围

这是 `MNT-202609-29`（[M15](../plans/m15-eval-harness-and-baseline.md)）产出的
recorded 首轮基线：四组对照臂（`agent-direct` / `workflow-strict` /
`recovery-no-memory` / `recovery-with-memory`）、17 个 EvalCase、seed {0,1,2} 加
seed-0 三重复，共 **170 个 run 单元**；硬门禁 G1–G6 与全部预算护栏通过。本报告
只声明 Simulator + recorded Provider 环境下的结论，不外推到真实平台（归
`MNT-202609-27`）与 live Provider（canary 轮单独 manifest）。

## 2. 方法与环境

- Harness：`tests/m15/m15_eval_profile_harness.cpp`（M15-01～03），全部经 Mira 公共
  API 驱动（`AgentLoop::run`、`WorkflowRuntime::execute_run`/`wait_run`/
  `agent_continuation`/`record_recovery_lesson`、`WorkflowRecoveryOrchestrator::
  attempt_recovery`），串行执行，每 run 单元独立装配 executor/MiraRuntime/
  Simulator 会话/事件存储，关闭按 profile §11 顺序。
- 环境：Ubuntu 24.04 x86_64、Intel Core Ultra 5 225H（14 线程）、GCC 13.3.0、
  CMake 3.28.3、CMake `debug` 预设（无优化）。
- `dataset_digest`：
  `9bd42d8d4c2e75394216779b1d2da94aaac33e087489931fb8a613e11aef9382`
  （canonical case 表的 SHA-256，实现于 harness；本 digest 为首轮基线锚点）。
- 判定：宿主 oracle（环境终态 + 输入/派发计数 + Run 终态），recorded Provider
  脚本决策；token 按 `UsageQuality=ProviderReported` 口径（每响应 10/5）。

## 3. 基线结果（摘要）

| 臂×家族 | 成功（Wilson 95%） | 模型调用合计 | 人工介入 | run 级 P50/P95（ms） | 步级 P95（ms） |
| --- | --- | --- | --- | --- | --- |
| A×R（30 run） | 30/30（0.886..1.000） | 55 | 0 | 69.7 / 127.7 | 127.7 |
| A×F（15） | 15/15（0.796..1.000） | 15 | 0 | 21.5 / 42.7 | 42.7 |
| B×W（20） | 20/20（0.839..1.000） | 0 | 0 | 0.50 / 0.84 | 0.40 |
| B×F（5） | 5/5 | 0 | 0 | —（关闭探针） | — |
| C×W（20） | 20/20（0.839..1.000） | 20 | 5 | 0.52 / 7.69 | 8.75 |
| C×L（10） | 10/10（0.722..1.000） | 15 | 0 | 7.44 / 7.56 | 7.22 |
| C×F（20） | 20/20（0.839..1.000） | 15 | 5 | —/205.2* | 204.9 |
| D×W（20） | 20/20（0.839..1.000） | 20 | 5 | 0.66 / 9.33 | 8.91 |
| D×L（10） | 10/10（0.722..1.000） | 15 | 0 | 7.53 / 13.63 | 11.55 |
| D×F（20） | 20/20（0.839..1.000） | 15 | 5 | —/205.3* | 205.0 |

\* F4 的 205 ms P95 是注入的 store 延迟（200 ms 旋钮）本身，属预期。F2 关闭序列
P50/P95/max = 0.52 / 0.84 / 0.93 ms（护栏 120 s）。

**门禁**：G1 重复副作用、G2 运行时缺陷、G4 recorded 确定性、G5 事件关联、
G6 脱敏、预算护栏——全部通过，连续三轮复验一致。**soak**（`--soak 3`）：每轮
门禁全绿，稳态 RSS 11800 → 12040 → 12040 KB，增长 2.0%（阈值 10%）。

**L1 学习传递配对（D vs C，seed 0，报告型——DEC-034 决策 3，无通过线）**：
两臂均完成（机制证据：D 臂 run B 检索命中 Lesson，`lessons_offered≥1/kept≥1`；
C 臂检索为空）；模型调用 2 vs 2；run 级 7.84 vs 7.47 ms。样本量 1 对/seed，
**不构成收益或成本结论**；分布对照待 soak 轮扩样。

**L2 失效过滤**：D 臂 `lessons_offered≥1、stale≥1、kept=0`（digest 漂移的
Episode 被三层过滤的第二层丢弃），lesson-free 修复路径仍完成 Run；C 臂无检索。
W4 修复无效场景：无效 `resume` 后二次升级、`need_user` 上交宿主，Run 保持
`WaitingAgent`，编排/升级预算计数正确，无终态复活。

## 4. 首轮发现

1. **事件交错顺序不是跨生产者契约**：`WorkflowRecoveryAttempted`（编排器路径）
   与 resume 后的 `WorkflowStepStarted/Settled`（驱动 worker）并发发射，两生产者
   的相对顺序在重复运行间可交换（G4 严格位序比较下可复现）。harness 据此把 G4
   细化为：A 臂（单生产者）严格位序；workflow 臂比较有向多重集——内容漂移、
   缺失或多余事件仍然失败。profile §9.1 G4 已同步该口径。**对事件消费者的含义：
   不要依赖跨生产者的事件相对顺序做状态推断，应按关联键（run_id/ordinal/
   request_id/patch_id）重建。**
2. **`Id128` 随机性与 digest 级联**：`profile_digest` 等内容摘要内嵌随机生成的
   标识（Id128 用 `std::random_device`），任何确定性比较都必须把 32/64 位 hex
   token 一并规范化（等值性与不等性都保留）。已固化在 harness 的 G4 规范化中。
3. **`create_run` 在创建时校验步骤工具注册**（"step tool is not registered"）：
   harness 装配顺序教训——工具注册必须先于 run 创建；该校验行为本身符合
   fail-closed 预期。

## 5. 限制

- 每臂×家族格子样本量 3–15（seed×重复），Wilson 区间宽；本文所有比例只按格子
  报告，不聚合成总体成功率，不支撑能力宣称。
- token 为脚本常量（ProviderReported 10/5），成本为 recorded 虚拟口径；live
  canary 未执行（受控凭据未配置，补跑条件：27 的 Provider profile + 单独
  manifest）。
- 每运行 fd 计数未实现（profile §8 的句柄项仅 soak RSS 覆盖）；Windows/Android
  不运行本 harness（CI 中 Android 为编译级，Windows 运行级由 CI 补充取证）。
- 延迟为 Debug 无优化构建 + 本机空载，只作回归锚点，不作物级性能声明。

## 6. 复现

```bash
cmake --preset debug
cmake --build --preset debug --target mira_m15_eval_profile_harness
./build/debug/tests/mira_m15_eval_profile_harness          # 单轮 + JSON 报告到 stdout
./build/debug/tests/mira_m15_eval_profile_harness --soak 3  # soak + RSS 稳态
ctest --test-dir build/debug -R m15                        # 门禁化入口（CI 同款）
```

退出码非零 = 任一 G 门禁或预算护栏失败。机器可读报告为本轮 stdout JSON；
原始 JSON 随本报告方法字段足以复算。

## 7. 关联

- [最小评估 Profile](../design/discrete_workflow_eval_profile.md) v1.0、
  [DEC-034](../decisions/DEC-034-minimal-eval-profile.md)
- [M15 里程碑](../plans/m15-eval-harness-and-baseline.md)（实现与验证记录）
- [长期记忆基准](long-task-memory.md)（仓库既有 benchmark 报告格式先例）
