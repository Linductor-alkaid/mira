# Context Intelligence Working Context 评估 v1（Stage W1 首轮）

> 状态：Active（首轮评估，登记于 2026-09-14）
> 负责人：Mira Maintainers
> 适用范围：[DEC-035](../decisions/DEC-035-context-curator-working-context.md)
> Stage W1（[M20](../plans/m20-working-context-stage-w1.md)）；
> `working_context_from_checkpoint` → `commit_working_context` 确定性投影与提交
> 管线对冻结 checkpoint 链数据集的管线行为评估；门禁 W1-G1–G6（M20 §4 跑前冻结）
> 复现命令见文末

## 1. 目的与范围

这是 [M20](../plans/m20-working-context-stage-w1.md) 产出的 Working Context
生命周期首轮评估：冻结合成 checkpoint 数据集（12 会话 × 每会话 5 个已提交
`ConversationCheckpoint`，水位 8/16/24/32/40 递进；每 checkpoint 植入 6 约束 /
4 决策 / 4 未决线索；`dataset_digest`
`ed81befbc0273b80253c723f9273bf65fc932eac7a7972c9bc39c9d1b894695a`，评估内断言
digest 相等后运行），经确定性投影 `working_context_from_checkpoint` 与五元组
提交 `commit_working_context` 走完整管线。**无模型、无 Curator**（Stage W1
范围）：checkpoint 直接由确定性构造器供给。全部指标度量**管线行为**（投影保真、
身份/水位绑定、提交纪律、epoch 链隔离、Layer 0 转换纪律、恢复、确定性）——
本评估不度量、不声明 token 收益、语义整理质量或任务成功率（`RULE-10`；
Stage W2 起以真实 Curator 另测）。

## 2. 方法与环境

- Harness：`tests/m20/m20_working_context_eval.cpp`（M20-06），数据集 digest
  断言锚定；门禁 W1-G1–G6（M20 §4，跑前冻结）。
- 轮次（全部 12 会话）：递进链提交（5 轮/会话）、幂等重放、同水位冲突（改写
  一条语句）、陈旧水位（回退一轮）、身份不匹配四联（session / task /
  task_epoch / environment_epoch）、终态迟到（session / task 各一轮）、epoch
  失效链（`environment_epoch` 提升后新链提交与旧链回查）、恢复（空 store
  重导出）、越界/无效候选拒绝。
- 环境：Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、CMake
  3.28.3、CMake `debug` 预设；全轮次 < 1 s。

## 3. 首轮结果（摘要）

| 轮次 | 提交 | NoOp | 丢弃 | 存储不变 | 违例 |
| --- | --- | --- | --- | --- | --- |
| 递进链 | 60/60（12 会话 × 5 水位） | 0 | 0 | — | 保真 0、身份 0、转换 0 |
| 幂等重放 | 0 | 12/12 | 0 | 12/12（逐字节） | 0 |
| 同水位冲突 | 0 | 0 | 12/12（`conflicting-watermark`） | 12/12 | 0 |
| 陈旧水位 | 0 | 0 | 12/12（`stale-watermark`） | — | 0 |
| 身份不匹配 ×4 | 0 | 0 | 48/48（四类 reason code 各 12） | — | 0 |
| 终态迟到 | 0 | 0 | 24/24（session/task 各 12） | — | 0 |
| epoch 失效链 | 24（旧链 12 + 新链 12） | 0 | 12/12 旧候选对新 live | — | 0 |
| 越界/无效候选 | 0 | 0 | 全部拒绝（`invalid-candidate` / 边界拒绝） | — | 0 |

- W1-G1–G6 全部通过；跨进程双跑 JSON 报告字节级一致（`cmp` 验证）。
- 投影保真（G1）：全部 60 个已提交快照三 section 与输入 checkpoint 逐条一致
  （content、source_events、source_sequence、confidence，840 条目级断言）。
- 身份与水位绑定（G2）：`through_event_sequence` == checkpoint 水位、
  `source_checkpoints` == `{checkpoint.id}`；恢复轮（G6）空 store 重导出
  12/12 与丢失前 id、`state_digest` 与零时戳 JSON 逐字节一致。
- epoch 失效隔离（G4）：`environment_epoch` 提升后新链 12/12 提交，
  `latest` 返回新链快照，旧链快照可按水位回查且条目携带旧 epoch（Layer 0
  stale-build 可检测）；旧候选对新 live 状态 100% `environment-epoch-mismatch`
  丢弃。
- Layer 0 纪律（G5）：全部转换条目 `UntrustedExternalData`、约束 → P1
  `UserConstraint`、决策/未决 → P3 `CheckpointSummary`；条目 id 与 checkpoint
  转换空间零冲突、重转换确定性；越界候选零提交。
- 报告口径数字（只报告不判定）：链轮快照条目 840（约束 360 / 决策 240 /
  未决 240），零时戳快照 JSON 合计 152,184 字节。

## 4. 发现与限制

- 管线在「确定性投影 → 五元组提交 → 有界保留环」全链路上对真值无损、对
  同水位冲突 fail-closed、对失败与迟到结果不破坏既有状态、对丢失可幂等重建
  ——Stage W1 声明的生命周期行为边界全部得到量化。
- 限制：评估对象为确定性投影（无 Curator、无模型）——所有指标为管线行为，
  不得外推为语义整理质量、token 收益或 continuation correctness；Working
  Context 的价值口径（Continuation correctness、Failed-attempt recurrence
  等，issue #48）归 Stage W2+ 以真实模型与冻结对照数据集另行登记（`RULE-10`）。
- 与 DEC-032 Stage E 的衔接：真机评估矩阵将以当时的 Curator 形态分列
  （`MNT-202609-27` 证据通道）。

## 5. 补跑与后续条件

- 本地复现：`cmake --preset debug && cmake --build --preset debug --target
  mira_m20_working_context_eval && ctest -R mira_m20_working_context_eval`
  （门禁失败非零退出；报告 JSON 经 argv[1] 落盘，跨进程 `cmp` 一致）。
- CI：`mira_m20_working_context_eval` 与契约测试入 Linux 测试与 sanitizer
  矩阵（Android 为编译级门禁既有范围）；Windows/Release/quality 由 PR CI
  回填（M20-07）。
- Stage W2（`IContextCurator` + 模型供给参考实现）立项时以本 store/提交管线
  为底座，增量 merge 语义随其契约冻结后重测并另行登记。
