# DEC-063：M22 自动触发评估的确定性与脚本绑定

> 状态：Accepted
> 日期：2026-10-23
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 测试稳健性维护轮
> 替代/被替代：无
> 关闭的缺口：M22 评估的 coexistence 场景在 CI 上抖动，且它的脚本绑定让场景退化了

## 背景与问题

`mira_m22_auto_trigger_eval` 的 G2 门在 CI 的 `linux (clang, Debug)` 上失败过一次
（`coexistence mismatches 1`），而同一份代码的下一个 run 通过——两份 run 只差一个纯
文档提交。这是一个典型的抖动：结论取决于调度，而不是取决于被测行为。

本轮不接受「又抖了一次」这个结论，去找根因。插桩之后确认了两件事：

### 1. 脚本槽位在闸门之后才被读取

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
`gate.wait_entered(1)`，再 `set_script(coexist-flush)`，再发强制 flush）。于是两个并发
刷新**共享同一个计划**：

- 「策略刷新」从未用它自己的（水位 8 的）计划跑过——场景退化成「两个相同的刷新」；
- 谁先提交由**调度器**决定。抢到的那次是 `Committed`，落败的那次是同水位的
  `IdempotentNoOp`。当强制 flush 抢输了，它的 outcome 就不是 `Committed`，G2 门判
  失败——CI 上观察到的正是 8 个会话里 1 个。

场景注释声称验证的是「单调提交纪律把 store 落在 flush 水位上」；而在那份共享计划下，
根本没有一条更旧的 claim 存在，纪律无从生效。

### 2. 脚本状态存在数据竞争

脚本槽位、`behavior`、闸门指针由测试线程写、由 worker 线程读，此前没有任何同步
（只有 `last_transcript_` 和两个计数器是受保护的）。

### 3. 断言前的自旋等待

coexistence 场景在断言 store 水位前有一个最多 1e8 次 `yield` 的自旋。这个自旋既烧
CPU，又可能在争用的 runner 上饿死它正在等的那次提交——它把一个本该由 future 提供的
确定同步点换成了一个计时等待。

## 决策

### 1. 脚本在调用到达时绑定

provider 在 `infer` 的开头（闸门等待之前）捕获它要服务的那份脚本：响应文本、期望条目
数、behavior、闸门指针。被服务的就是捕获的那一份。

一个停在闸门里的调用因此**不会**捡起它到达之后才装入的脚本——两个并发调用各得自己的
计划，谁先提交不再是调度器说了算。coexistence 场景恢复它声称要验证的东西：策略刷新
提交水位 8 的 claim，强制 flush 提交水位 16 的 claim，单调提交纪律让 store 落在 16，
而更旧的那条 claim 被当作陈旧丢弃（无论两条 claim 哪条先落地，flush 的 outcome 恒为
`Committed`）。

### 2. 脚本状态加锁

脚本槽位、behavior、闸门指针与「已服务的脚本」记录同属一把互斥锁。捕获既是语义，也是
它与装脚本的测试线程之间的 happens-before 边。

### 3. 断言不等待

两个 future 都已 resolve，而受监督任务在 resolve **之前**完成提交——store 状态此时已是
终态。自旋等待因此被移除：断言直接读 store。

自旋的代价不只是「慢」：它把一个由 future 提供的确定同步点换成了计时等待，在争用的
runner 上会烧掉数秒 CPU 并饿死它等的写者。移除后，真实缺陷会以真实失败出现，而不是
以抖动出现。

### 4. 确定性守卫

评估现在断言：coexistence 场景的两个并发刷新**被服务了两个不同的计划**。共享计划被
直接计入既有的 `coexistence_final_mismatches`。

这个守卫的价值在于它是**确定性**的：把脚本绑定改回闸门后读取，评估 **8/8** 失败；
而修复前的断言只能以约 1/8 的概率在 CI 上抓到同一件事。抖动只能被概率性地抓到，
根因可以被确定性地抓到。

## 备选方案

- **把自旋换成有界 sleep 等待**：拒绝。它仍然是计时等待（且与 M22 计划「无 sleep 时序
  等待」的记载相悖），而 future 已经是确定同步点——等待没有存在理由。
- **把两个并发刷新串行化**：拒绝。那就不是 coexistence 场景了。
- **给 provider 一个脚本队列**：拒绝。队列会让「先到先得」重新变成顺序问题；按到达
  绑定脚本不需要队列，而且本仓库内只有 coexistence 场景会在闸门等待期间换脚本。
- **放宽断言（接受 `Committed` 或 `IdempotentNoOp`）**：拒绝。那会把退化后的场景
  合法化，而不是修好它——场景本该验证单调提交纪律。
- **只加断言不改绑定**：拒绝。断言会确定性地失败（这是它该做的），但被测行为仍然是
  退化的。

## 影响与风险

- **只有评估 harness 变化**：`WorkingContextAutoCurator`、`commit_working_context`、
  Supervisor 与所有公开契约不变。W3-G1–G6 的门禁条件与计数器集合不变。
- **已发布的评估报告继续有效**：修复前后的报告 JSON 逐字节相同（已用 `cmp` 与
  `counters` 比对确认）。
- **脚本的可观察语义变了**：停在闸门里的调用不再捡起后装的脚本。本仓库内只有
  coexistence 场景在闸门等待期间换脚本；其余场景都是「先装脚本、后触发」，语义不变。
- **计数器语义微扩**：`coexistence_final_mismatches` 从「终态不对/flush 没提交」扩到
  「场景没有按规格发生」（含共享计划）。报告 JSON 不变（该计数器仍为 0）。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

- `mira_m22_auto_trigger_eval`：修复后 W3-G1–G6 全绿，`gate_failures` 为空。
- **报告稳定性**：修复前后各落盘一次报告 JSON，逐字节相同。
- **变异验证**：把脚本绑定改回「闸门之后读取」，评估 8/8 失败。
- **稳定性**：固定到 2 核并加 8 个 CPU 占用进程，修复后 20/20 通过。
- **工具链**：全量 `ctest --preset debug` 112/112；ASAN / UBSAN / TSAN 下 m22 组通过；
  静态分析、架构、文档、平台边界与格式检查全过。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-m22-eval-determinism`](../plans/maintenance-2026-10-m22-eval-determinism.md)
- 计划：[M22 Stage W3](../plans/m22-working-context-stage-w3.md)（harness 的出处）
- 报告：[auto-trigger 评估 v1](../benchmarks/context-intelligence-working-context-auto-trigger-v1.md)
- 设计：[Context Curator 设计](../design/context_curator_design.md) §8–§9
