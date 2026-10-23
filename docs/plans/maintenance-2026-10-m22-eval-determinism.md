# 维护计划：M22 自动触发评估的确定性（2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：[M22 Stage W3](m22-working-context-stage-w3.md)（Completed）、
> [MNT-202610-39…42 保留期清扫](maintenance-2026-10-memory-retention-sweep.md)（Completed）
> 建议发布点：测试稳健性修订
> 更新日期：2026-10-23

## 1. 目标

关闭一个被观察到的测试抖动：`mira_m22_auto_trigger_eval` 的 G2 门在 CI 的
`linux (clang, Debug)` 上失败过一次（`coexistence mismatches 1`），而同一份代码的
下一个 run 通过——两份 run 只差一个纯文档提交。

本轮把它从「一次抖动」变成「结构上不可能」：找出根因、修掉、并补一个**确定性**的
回归守卫（抖动只能被概率性地抓到，根因可以被确定性地抓到）。

## 2. 范围与非目标

### 2.1 范围

- `tests/m22/m22_auto_trigger_eval.cpp`：
  - 脚本化 provider 的**脚本在调用到达时绑定**（在闸门等待之前捕获），脚本状态加锁。
  - 移除 coexistence 场景里的 1e8 次自旋等待：两个 future 都已 resolve，
    而受监督任务在 resolve 之前完成提交，store 状态此时已是终态。
  - 新增确定性守卫：coexistence 场景断言两个并发刷新**被服务了两个不同的计划**。
- 文档：M22 计划补勘误、新增本计划与 `DEC-063`。

### 2.2 非目标

- 不改 `WorkingContextAutoCurator` / `commit_working_context` / Supervisor 的任何
  行为：这一轮修的是**评估 harness**，不是实现。
- 不改 M22 门禁的判定口径（W3-G1–G6 的条件与门禁 JSON 的计数器集合均不变）。
- 不改已发布的评估报告：修复前后的报告 JSON **逐字节相同**（见 §7）。
- 不引入新的 Executor 能力使用方式；不登记 Executor 反馈台账。

## 3. 设计与决策依据

- 决策记录：[DEC-063](../decisions/DEC-063-m22-eval-determinism.md)。
- 相关设计：[Context Curator 设计](../design/context_curator_design.md) §8–§9；
  M22 计划 §4（触发与回退契约）与「测试矩阵」条目（**确定性闸门（mutex/cv）
  同步，无 sleep 时序等待**）。
- 相关文档：[auto-trigger 评估 v1](../benchmarks/context-intelligence-working-context-auto-trigger-v1.md)。

### 3.1 根因

脚本化 provider 只有一个脚本槽位（`response_`），而它在**闸门等待之后**才被读取：

```cpp
if (behavior == Behavior::Gate && gate != nullptr) {
    gate->wait_at_gate();          // 调用在此阻塞
    ...
}
...
text.text = ... : script;          // 脚本在这里才被读取
```

coexistence 场景**故意**在第一个调用停在闸门里的时候装入第二个计划（先
`gate.wait_entered(1)`，再 `set_script(coexist-flush)`，再发强制 flush）。于是两个
并发刷新**共享同一个计划**：

- 「策略刷新」从未用它自己的（水位 8 的）计划跑过——场景退化；
- 谁先提交由**调度器**决定，而不是由场景决定。抢到的那次提交（Committed），
  落败的那次是同水位的 `IdempotentNoOp`。当强制 flush 抢输了，它的 outcome 就不是
  `Committed`，G2 门判失败——这正是 CI 上观察到的那一次 `coexistence mismatches 1`
  （8 个会话里 1 个）。

附带一处**数据竞争**：脚本槽位、behavior、闸门指针由测试线程写、由 worker 线程读，
此前没有任何同步。

## 4. 工作项

- [x] `MNT-202610-43` 脚本绑定：provider 在调用到达时捕获脚本（响应、条目数、
      behavior、闸门指针），脚本状态加锁；调用因此被服务「它到达时的那份计划」，
      两个并发刷新各得自己的计划，coexistence 场景恢复它声称要验证的东西（单调提交
      纪律把 store 落在 flush 水位上，较旧的那条 claim 被丢弃）。
- [x] `MNT-202610-44` 等待与守卫：移除 1e8 次 yield 自旋（两个 future resolve 后
      store 已是终态，自旋既烧 CPU 又会在争用的 runner 上饿死它等的写者）；新增
      「两个并发刷新被服务了两个不同计划」的确定性断言，共享计划直接计为 coexistence
      失配。

## 5. 风险与阻塞

- **评估报告不变**：修复前后的报告 JSON 逐字节相同，已发布的评估报告继续有效，
  「跨进程/跨构建报告字节级一致」的口径不受影响。
- **门禁口径不变**：W3-G1–G6 的条件与计数器集合都没动；共享计划被计入既有的
  `coexistence_final_mismatches`（语义从「终态不对/flush 没提交」扩到
  「场景没有按规格发生」），因此报告 JSON 不变。
- **数据竞争修复改变了脚本的可观察语义**：闸门里阻塞的调用不再捡起后装的脚本。
  本仓库内只有 coexistence 场景会在闸门等待期间换脚本，其余场景都是「先装脚本、
  后触发」，语义不变。
- 无 Executor 阻塞，无平台条件阻塞，无凭据或设备依赖。

## 6. 测试与退出条件

- [x] `mira_m22_auto_trigger_eval` 修复后全门通过（W3-G1–G6，`gate_failures` 为空）。
- [x] 修复后报告 JSON 与修复前**逐字节相同**（`counters` 完全一致）。
- [x] 变异验证：把 provider 改回「闸门后读脚本」，评估 **8/8** 失败——守卫是确定性的，
      不是概率性的。
- [x] 稳定性：固定到 2 核并加 8 个 CPU 占用进程，修复后 20/20 通过。
- [x] 全量 `ctest --preset debug` 通过；ASAN / UBSAN / TSAN 下 m22 组通过。
- [x] `architecture-check`、`check_docs`、平台边界与格式检查通过。

## 7. 验证记录

### 2026-10-23：根因定位、修复与确定性守卫

- **范围**：provider 的脚本绑定与加锁、coexistence 场景的自旋移除与守卫、
  `DEC-063`、本计划与 M22 计划的勘误。
- **依据**：[DEC-063](../decisions/DEC-063-m22-eval-determinism.md)；M22 计划 §4 与
  「测试矩阵」条目；Context Curator 设计 §8–§9。
- **环境**：Linux 7.0.0-38-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；14 核；
  固定 SQLite amalgamation 3.53.4。基线提交 `e81b493`（PR #105 合并并结项后的
  master）。
- **定位过程**：
  - CI 现象：PR #105 的首个 push run 中 `linux (clang, Debug)` 的
    `mira_m22_auto_trigger_eval` 报 G2 门失败（`coexistence mismatches 1`）；下一个
    run（只差一个纯文档提交）通过 → 判定为既有抖动。
  - 本地复现尝试：`taskset -c 0,1` 固定双核 + 8 个 CPU 占用进程，修复前 12/12 通过
    → 复现不出来（争用不足以触发）。
  - 插桩定位：给 provider 打印每次调用实际拿到的脚本后确认——coexistence 场景的
    **两次并发调用拿到的是同一个计划**（`coexist-flush`），「coexist」计划从未被使用。
    根因即脚本在闸门之后才被读取。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警（改动文件 0 条） |
  | `ctest --preset debug -j 14` | **112/112 通过** |
  | `ctest --test-dir build/debug -R "m22" --output-on-failure` | 2/2 通过 |
  | `cmake --build --preset static-analysis -j 14` | 通过；改动文件 0 条告警（余下全部来自 `third_party/kairo` 与 libstdc++ 头文件实例化） |
  | `cmake --build --preset asan -j 14` + `ctest --test-dir build/asan -R "m22\|m23\|m4" -j 8` | **18/18 通过**，无 ASan 报告 |
  | `cmake --build --preset ubsan -j 14` + `ctest --test-dir build/ubsan -R "m22" -j 8` | **2/2 通过**，无 UBSan 报告 |
  | `cmake --build --preset tsan -j 14` + `setarch x86_64 -R ctest --test-dir build/tsan -R "m22" -j 4` | **2/2 通过**，0 条 ThreadSanitizer 告警 |
  | `cmake --build build/debug --target architecture-check` | `architecture policy is clean`（8 模块，17 条既有 baseline 抑制项，0 条 stale） |
  | `python3 tools/check_docs.py .` | `Markdown links and fences: OK` |
  | `python3 tools/check_platform_boundary.py` | `Mira Core platform boundary is clean` |
  | `cmake --build build/debug --target format-check` | 通过（269 个文件） |

- **变异验证**：把 provider 改回「闸门之后读脚本」，评估 **8/8** 失败（G2 门）——
  新守卫是确定性的：共享计划必然被抓到，而修复前只有 1/8 的概率被 CI 抓到。
- **报告稳定性**：修复前后各跑一次评估并落盘 JSON，`cmp` 与 `counters` 比对均一致
  （逐字节相同），已发布的评估报告继续有效。
- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 仅覆盖 m22 / m23 / m4 测试组；全量 112 项仅在 debug 配置执行。
  - 未在 Windows 与 Android 目标上本地构建验证：本轮改动不含平台相关代码路径。
  - 未在 CI 的双核 runner 上复现原始抖动（本地争用不足以触发）；根因由插桩定位确认，
    守卫的判别力由变异验证给出。
- **同步**：`tests/m22/m22_auto_trigger_eval.cpp`、
  `docs/plans/m22-working-context-stage-w3.md`（勘误）、
  `docs/decisions/DEC-063-m22-eval-determinism.md`（新增）、本计划（新增）。
