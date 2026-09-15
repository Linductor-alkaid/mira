# Context Intelligence Working Context 自动触发评估 v1（Stage W3 首轮）

> 状态：Active（首轮评估，登记于 2026-09-15）
> 负责人：Mira Maintainers
> 适用范围：[DEC-035](../decisions/DEC-035-context-curator-working-context.md)
> Stage W3（[M22](../plans/m22-working-context-stage-w3.md)）；
> `WorkingContextAutoCurator`（触发策略 + coalescing + forced flush + 失败
> 回退）→ `schedule_working_context_curate`（Supervisor Deferrable）→
> `ProviderContextCurator`（脚本化确定性模型）→ `commit_working_context`
> 对冻结合成信号数据集的管线行为评估；门禁 W3-G1–G6（M22 §4.3 跑前冻结）
> 复现命令见文末

## 1. 目的与范围

这是 [M22](../plans/m22-working-context-stage-w3.md) 产出的 Working Context
自动触发管线首轮评估：冻结合成信号数据集（SplitMix64 seed
`0x4d49'5232'3257'3343`（"MIR22W3C"），`dataset_digest`
`6f2ab2e57b93e1d66846100f92b91563a83f5ffd1f05b5dbf6682dc5f4911535`，评估内
断言 digest 相等后运行；8 会话 × 每会话 40 信号，每信号对话推进 0–10、执行
事件增量 0–12，checkpoint 在确定性水位标记处提交；数据集保证 Watermark 与
EventCount 两种燃点各 ≥ 12 次可观测，并含零对话推进高执行事件段与
checkpoint 空窗段），模型侧沿用 M21 的**脚本化确定性 Curator provider**
（另含确定性闸门变体与五类失败注入变体）。全部指标度量**确定性管线行为**：
触发判定对纯函数的保真、coalescing、forced flush、失败回退、previous 选择、
shutdown 与跨运行确定性——本评估**无真实模型、无网络、无凭据**，不度量、
不声明语义整理质量、token 收益或 continuation correctness（`RULE-10`；归
Stage E 与 `MNT-202609-27` 证据通道）。

## 2. 方法与环境

- Harness：`tests/m22/m22_auto_trigger_eval.cpp`（M22-03，测试的编写、运行
  与 sanitizer 取证由 Independent-Verification-Agent 独立完成），数据集
  digest 断言锚定；门禁 W3-G1–G6（M22 §4.3，跑前冻结）；curation 全部经
  Supervisor Deferrable 路由执行；coalescing/flush 场景用确定性闸门
  （mutex/cv）同步，无 sleep 时序等待。
- 默认策略阈值（`watermark_interval=8`、`event_count_interval=16`、
  `max_tracked_sessions=64`）；单遍 8 会话 × 40 信号后同进程全量重放一遍
  做逐快照比对；JSON 报告经 argv[1] 落盘，跨进程与跨构建 `cmp` 验证。
- 环境：Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、CMake
  3.28.3、CMake `debug` 预设；全轮次 ≈ 3 s。

## 3. 首轮结果（摘要）

| 场景 | 结果 | 违例 |
| --- | --- | --- |
| 纯函数判定表 | 13/13 正确 | 0 |
| 自动链触发保真 | 640 次信号判定（两遍），206 次燃点逐一等于纯函数预测（kind/prediction 失配 0）；非燃信号零调度 | 0 |
| 燃点分列（单遍 103 次） | watermark 72、event count 31（均 ≥ 12）；空窗追燃 36、`checkpoint ≤ settled` 燃点 0 | 0 |
| coalescing（8 会话闸门轮） | 吸收 24/24、阻塞期调度 0、释放后以最新 checkpoint 恰好重燃一次（陈旧输入 0） | 0 |
| forced flush | 强制燃 16/16 低于阈值照常提交；已落库边界 NoOp 8/8（`auto-refresh-current`、committed == store latest、零 curator 调用）；终态迟到全丢弃、store 零移动 | 0 |
| 失败回退（5 类 × 8 组） | 40/40 future 以正确错误码 resolve、store 逐字节不变；紧重试 0；重试间隔恒为 3 个信号；重试成功后失败连击复位 40/40；失败边界 flush 真实重试 40/40 | 0 |
| previous 选择 | 新链 24/24 `nullopt`（transcript 无 `prev:` 段）；同链 8/8 == store latest；epoch 新链 settled 归零后同水位可燃 | 0 |
| 拒绝路径 | 会话不一致 / 容量超限拒绝全部命中 | 0 |
| shutdown | 8/8 在途 Cancelled、关闭后 on_signal/flush 拒绝且零调度 | 0 |
| 长会话确定性 | 重放 103/103 对快照 id/digest/归一化 JSON 字节一致；8/8 settled 轨迹一致；跨进程与 ASAN/UBSAN/TSAN 构建报告全部字节一致（`cmp` 验证） | 0 |

- **W3-G1–G6 全部通过**（`gate_failures` 为空）。
- 报告口径数字（只报告不判定）：curator 调用合计 448 次（链 206 +
  forced 16 + 失败回退/previous/shutdown 场景）；燃点 future 交付缺陷
  （caller future 无关联状态）0；每会话 settled 水位轨迹已入报告 JSON
  （`settled_trajectories`）。

## 4. 发现与限制

- 管线在「宿主信号 → 双轴触发策略 → unsettled 闸门 → coalescing →
  supervisor Deferrable 路由 → 脚本 Curator → 五元组提交 → 失败重臂」全
  链路上：触发判定与纯函数零失配、在途信号零重复调度、同水位零重
  curate、失败后零紧重试且重试间隔恒定、终态后零复活、重放字节级一致
  ——M22 §4 冻结的触发与回退契约全部得到量化。
- 本轮测试期间发现并修复一处实现缺陷：燃点 `shared_future` 二次 `share()`
  返回空句柄（调用方 `get()` 抛 `std::future_error`）；修复为单次
  `share()`（提交内修复，未跨提交跟踪），契约套件以
  `caller_defects == 0` 严格断言取证。
- 限制：脚本化确定性供给方口径——所有指标为确定性管线行为，不得外推为
  语义整理质量、token 收益或 continuation correctness；真实模型接入与
  issue #48 对照指标归真实模型轮与 DEC-032 Stage E（`MNT-202609-27`
  证据通道）。
- 观察项（非冻结契约场景）：单一协调器承载多会话且多会话闸门同时阻塞时，
  Executor 有限 worker 被阻塞的 curation 占用，后续被接纳任务可能推迟执行
  ——这是 Executor 有界 worker 的预期排队行为（非缺陷）；评估 harness 的
  并发闸门场景按会话隔离协调器。宿主不应长时间阻塞 `curate`。
- Memory promotion（W4）与 subagent fork/merge（W5）为显式非目标。

## 5. 补跑与后续条件

- 本地复现：`cmake --preset debug && cmake --build --preset debug --target
  mira_m22_auto_trigger_eval && cd build/debug && ./tests/mira_m22_auto_trigger_eval
  report.json`（门禁失败非零退出；报告 JSON 字节级确定，跨进程 `cmp` 一致）。
- CI：`mira_m22_auto_trigger_test` 与 `mira_m22_auto_trigger_eval` 入 Linux
  测试与 sanitizer 矩阵（Android 为编译级门禁既有范围）；Windows/Release/
  quality 由 PR CI 回填（M22-06）。
- Stage W4（Memory Promotion，经 `MemoryConsolidator` 既有纪律）立项时以
  本自动触发管线为底座；Stage E 真机评估矩阵将以当时的 Curator 形态分列。
