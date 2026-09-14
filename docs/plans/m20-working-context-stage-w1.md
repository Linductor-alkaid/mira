# M20：Context Curator Stage W1——WorkingContextSnapshot 确定性契约

> 状态：Completed（2026-09-14：PR #49 合入 `48f4781`，两轮 CI——第二轮
> 首轮全绿——head 双 pipeline 24/24 全绿；`IContextCurator` 模型供给与
> AgentLoop 集成为显式非目标，归 Stage W2 与供应链复核通道）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-035](../decisions/DEC-035-context-curator-working-context.md) 第 4 条 Stage W1）
> 前置：M19 关闭（已满足，PR #47 于 2026-09-14 合入）；
> [Context Curator 设计](../design/context_curator_design.md) §4/§5/§7/§8 已冻结
> 建议发布点：非发布物；产出 Working Context 生命周期行为基线（Stage W2–W5 的
> 提交管线与输入形态锚点）
> 更新日期：2026-09-14（立项）

## 1. 目标

按 Context Curator 设计 §4/§5/§7/§8/§13 交付 Stage W1（无模型）：

- `WorkingContextSnapshot` 公共契约：task/session 导向的当前状态视图——身份五元组
  （`session_id / task_id / task_epoch / environment_epoch / through_event_sequence`）、
  确定性派生 id、排除时间与身份的 `state_digest`、三个 section（constraints /
  decisions / open_issues）与 `source_checkpoints` 溯源。
- 确定性投影：`working_context_from_checkpoint(checkpoint, options)` 从已提交
  `ConversationCheckpoint` 全有或全无地导出快照候选（约束/决策/未决线索逐条携带
  provenance、真实时序与 confidence；超限整体拒绝，不做部分截断）。
- 提交管线：`IWorkingContextStore` / `InMemoryWorkingContextStore`（会话内水位
  单调、有界保留环）与 `commit_working_context`（五元组校验、终态幂等、幂等
  NoOp、同水位 digest 冲突 fail-closed）。
- Layer 0 候选转换：`context_items_from_working_context`（约束 → P1
  `UserConstraint`、决策/未决 → P3 `CheckpointSummary`、全部 `UntrustedExternalData`
  authority、id 空间与 checkpoint 条目分离、summary/preferences 不转换）；
  不修改 `StandardContextManager`。
- Executor 路由：`ContextMemorySupervisor::schedule_working_context_commit`
  （Deferrable、取消探针透传、shutdown 拒绝与在途取消 resolve Cancelled）。
- 按 §4（本文件）产出生命周期行为基线：冻结数据集 + 确定性 checkpoint 供给方，
  门禁 W1-G1–G6（跑前冻结）。

## 2. 范围与非目标

范围：`include/mira/context_working_context.hpp`、`src/context/context_working_context.cpp`
（入 `mira_core`）、`ContextMemorySupervisor::schedule_working_context_commit`、
`tests/m20/` 契约/集成测试与评估 harness、基准报告与文档同步。

非目标：不实现 `IContextCurator` 与模型调用、previous-snapshot 增量 merge（W2）；
不实现自动触发、coalescing、forced flush（W3）；不实现 Memory promotion（W4）与
subagent fork/merge（W5）；不引入新 `ContextItemKind`、不修改 Layer 0 与 M19
checkpoint 契约；不引入新持久化格式（W1 store 为 volatile 可重建投影，RULE-07）；
无真机/真模型证据（归 DEC-032 Stage E，`MNT-202609-27` 通道）；评估指标为确定性
管线行为，不得外推为 token 收益、语义质量或任务成功率（`RULE-10`）。

## 3. 设计与决策依据

- [DEC-035](../decisions/DEC-035-context-curator-working-context.md)
  第 2/3/4/5/6 条（投影非权威、Memory 边界、W1–W5 分阶段、W1 语义边界、
  Executor 纪律）
- [Context Curator 设计](../design/context_curator_design.md)
  §2/§3/§4/§5/§6/§7/§8/§9/§10/§11/§12/§13（三平面模型、契约草案、提交纪律、
  确定性 merge、Layer 0 路径、Executor 路由、故障降级、恢复、安全隐私、评估、
  Stage W1 门禁）
- [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)
  第 2/3/4 条与 [M19](m19-context-intelligence-stage-d.md)
  （五元组提交纪律、终态幂等、fail-closed 冲突处置、supervisor Deferrable
  路由模式、评估 harness 方法学——W1 全部沿用）
- [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)
  （Working Context 与长期 Memory 的边界来源）
- `RULE-02`（新路由经既有 Executor 监督者，Deferrable 类）、`RULE-07`
  （快照是可重建投影，EventStore/checkpoint 仍是事实源）、`RULE-08`
  （section 计数、文本字节、provenance 全部有界）、`RULE-09`（快照语句继承
  checkpoint 的模型介导语义，永不提升 authority）、`RULE-10`（指标口径诚实）

## 4. 冻结的评估 profile 与门禁（跑前冻结，2026-09-14）

**对照设计**：确定性合成 checkpoint 数据集（SplitMix64，seed `0x4d49'5232'3057'4354ULL`
（"MIR20WCT"）；12 个会话 × 每会话 5 个已提交 `ConversationCheckpoint`，水位
8/16/24/32/40 递进；每 checkpoint 植入 6 约束 / 4 决策 / 4 未决线索，语句内容含
会话 token、类别与链内序号（相邻 checkpoint 语句集不同，替换语义可见），
`source_events` 取自本会话 40 个事件的事件空间，位置逐会话确定性打乱），数据集
以 `dataset_digest` 断言锚定（口径同 M17/M18/M19；首轮钉定后记录于 §8）。
供给方为确定性 checkpoint 构造器（无模型）：直接构造已提交 checkpoint 输入；
投影管线为 `working_context_from_checkpoint` → `commit_working_context`。

**场景轮（全部 12 会话）**：递进链提交（5 轮/会话）；同水位幂等重放（最终
checkpoint 重合并 → 全部 IdempotentNoOp）；同水位冲突（改写一条语句 → 全部
`conflicting-watermark` 丢弃且已存快照逐字节保留）；陈旧水位（最终轮后重提交
前一轮 → 全部 `stale-watermark` 丢弃）；身份不匹配四联（task / task_epoch /
environment_epoch / session 各 12/12 丢弃）；终态迟到（session_terminal 与
task_terminal 各 12/12 丢弃）；epoch 失效链（`task_epoch` 提升后新链提交、
`latest` 返回新链、旧链可按水位回查）；恢复（空 store 重导出 → id 与
`state_digest` 与丢失前一致）；越界拒绝（单 section 超上限的 checkpoint →
merge 整体拒绝、0 提交）。

**门禁 W1-G1–G6（跑前冻结；理由随条给出）**：

| 门禁 | 断言 | 依据 |
| --- | --- | --- |
| W1-G1 投影保真 | 全部已提交快照三 section 与输入 checkpoint 逐条一致（content、source_events、source_sequence、confidence），条目数相等，fidelity = 1.0 | W1 快照是 checkpoint 的确定性投影（设计 §4.2/§6）；丢条目、改内容或丢 provenance 即投影缺陷 |
| W1-G2 身份与水位绑定 | 已提交快照 `through_event_sequence` == 输入 checkpoint 水位；`source_checkpoints` == 输入 checkpoint id；同输入重导出 id 与 digest 一致 | 确定性身份与 RULE-07 可重建性（设计 §5.1）：同输入必须幂等同 id |
| W1-G3 提交纪律 | 幂等重放 12/12 NoOp 且 store 逐字节不变；同水位冲突 12/12 fail-closed 丢弃且已存快照保留；陈旧水位 12/12 丢弃；身份不匹配四联 12/12×4 丢弃；终态迟到 12/12×2 丢弃；除递进链外 0 提交 | 设计 §5.2 表：终态幂等、五元组校验、同水位冲突不静默覆盖——W2 Curator 非确定性行为的兜底必须在 W1 冻结并取证 |
| W1-G4 epoch 失效隔离 | epoch 提升后新链 12/12 提交，`latest` 返回新链快照；旧链快照仍可按水位回查且条目携带旧 epoch（Layer 0 stale-build 可识别）；store 环有界 | 设计 §5.3：epoch 变化开启新链，身份隔离表达失效；陈旧投影必须可检测而非静默混入 |
| W1-G5 Layer 0 纪律 | 全部转换条目 authority == `UntrustedExternalData`；约束 → P1 `UserConstraint`、决策/未决 → P3 `CheckpointSummary`；id 确定性且与 checkpoint 条目 id 零冲突；越界 candidate 0 提交 | DEC-035 第 2/5 条与设计 §7：快照不自准入、RULE-09 authority、id 空间分离可审计；全有或全无边界（RULE-08） |
| W1-G6 确定性与恢复 | 同进程重复轮聚合逐项相等；恢复轮 id 与 `state_digest` 与丢失前一致；跨进程 JSON 报告字节级一致（`cmp` 验证） | 数据集、投影、提交全确定性（设计 §10）；恢复 = 幂等重导出 |

**报告指标（只报告不判定）**：各 section 条目计数、快照与语句字节量、
`source_checkpoints` 计数、store 环深、提交/丢弃分类计数。
失败处置同 M19 §4：实现缺陷登记 `BUG-YYYYMMDD-NNN`，阈值或口径错误先修订
本文件与设计文档再复跑。

## 5. 工作项

- [x] `M20-01` 里程碑立项与本节 profile/门禁冻结（本文件；时间戳先于任何正式
  评估运行）。
- [x] `M20-02` Stage W1 契约：`WorkingContextItem`、`WorkingContextSnapshot`
  （身份五元组、确定性 id、`state_digest`、`validate()`、JSON
  `mira.working_context.snapshot.v1`）、`WorkingContextMergeOptions`（有界
  配置）、`working_context_from_checkpoint`（确定性投影、全有或全无边界）。
- [x] `M20-03` 提交管线：`IWorkingContextStore`/`InMemoryWorkingContextStore`
  （水位单调、有界保留环、`erase_session`）、`WorkingContextCommitState`/
  `WorkingContextCommitDisposition`/`WorkingContextCommitOutcome`、
  `commit_working_context`（终态幂等、五元组校验、幂等 NoOp、同水位冲突
  fail-closed）。
- [x] `M20-04` Layer 0 候选转换 `context_items_from_working_context`（P1/P3
  映射、`UntrustedExternalData` authority、确定性 id 与 checkpoint 空间分离、
  summary/preferences 不转换）。
- [x] `M20-05` Executor 路由：`ContextMemorySupervisor::schedule_working_context_commit`
  （Deferrable、取消探针透传、shutdown 拒绝与在途取消 resolve Cancelled）。
- [x] `M20-06` 契约/集成测试矩阵（§7）与评估 harness（W1-G1–G6、JSON 报告、
  dataset digest 锚定）、本地全门禁。
- [x] `M20-07` 文档同步（总计划索引与注记、Context Curator 设计实现注记、
  README 能力表、API 手册、基准报告、Context Intelligence 设计 §15 关联文档）
  与实现同批变更；CI 取证待 PR 全绿后回填关闭。

## 6. 风险与阻塞

- 风险：W1 快照与 checkpoint 投影内容高度重叠，被质疑为重复 API。处置：交付物
  是生命周期基础设施与 W2 输入形态（DEC-035 影响与风险节、设计 §4.2/§6）；
  门禁与文档显式限定 W1 口径（管线行为，`RULE-10`）。
- 风险：整体替换语义下，W2 Curator 的部分退化输出可能覆盖富快照。处置：同水位
  digest 冲突 fail-closed 与五元组校验在 W1 冻结（W1-G3）；W2 的增量合并语义
  届时随 `IContextCurator` 契约另行冻结。
- 外部：无（本里程碑不依赖真机、凭据或外部语料）。

## 7. 测试与退出条件

- [x] 契约/集成测试进 ctest（label `integration;m20`）：正常投影（三 section、
  provenance、水位、digest、JSON 往返）；无效/越界输入整体拒绝（无部分快照）；
  五元组提交竞态（陈旧水位、task/epoch/session 不匹配、幂等 NoOp、同水位冲突
  fail-closed）；终态后迟到提交丢弃（会话/任务）；epoch 失效隔离（新链提交、
  旧链回查、条目 epoch 标注）；store 水位单调与有界保留；恢复重建 id/digest
  一致；`context_items_from_working_context` 映射与 RULE-09 authority 与 id
  空间分离；`schedule_working_context_commit` Deferrable 路由、shutdown 拒绝
  与在途取消。
- [x] 评估 harness 进 ctest（label `integration;m20`，跑前冻结 W1-G1–G6，失败
  非零退出）；首轮报告落
  `../benchmarks/context-intelligence-working-context-v1.md`（环境、命令、
  场景轮摘要、门禁结果、限制）。
- [x] 本地门禁：debug 全量 ctest 全绿（新增 2 目标）、ASAN/UBSAN/TSAN m20
  目标零报告、`format-check`/`docs-check`/`platform-boundary-check`/
  `sbom-check` 通过。
- [x] 文档同步完成（§5 `M20-07` 清单；与实现同一变更提交）。
- [x] PR CI（Linux/Windows/Android 编译级/sanitizers/quality）全绿后回填验证
  记录并关闭本里程碑（PR #49 两轮：第二轮 head 双 pipeline 24/24）。

## 8. 验证记录

2026-09-14：`M20-01`～`M20-07` 本地实现与首轮 working-context 评估（分支
`feat/m20-working-context-stage-w1`）。

- **交付**：`include/mira/context_working_context.hpp` +
  `src/context/context_working_context.cpp`（入 `mira_core`）——Stage W1 全部
  契约（快照、确定性投影、store、提交管线、Layer 0 转换）；
  `ContextMemorySupervisor::schedule_working_context_commit`（Deferrable）。
  测试 `tests/m20/m20_working_context_test.cpp`（9 组：确定性投影与 JSON
  往返/越界整体拒绝/五元组提交竞态/终态迟到丢弃/epoch 链隔离/store 单调与
  有界保留/恢复重投影/Layer 0 映射与 id 空间分离/supervisor Deferrable 路由
  与 shutdown 拒绝与顺序重放幂等）与 `tests/m20/m20_working_context_eval.cpp`
  （12 会话 × 5 checkpoint 链冻结数据集，`dataset_digest`
  `ed81befbc0273b80253c723f9273bf65fc932eac7a7972c9bc39c9d1b894695a` 断言锚定；
  冻结 W1-G1–G6、JSON 报告）。
- **首轮评估**（正式运行一次，登记
  [context-intelligence-working-context-v1](../benchmarks/context-intelligence-working-context-v1.md)）：
  W1-G1–G6 全绿——递进链 60/60 提交、840 条目投影保真零违例；幂等重放 12/12
  NoOp 且存储逐字节不变；同水位冲突 12/12 `conflicting-watermark` fail-closed、
  陈旧水位 12/12、身份不匹配四联 48/48、终态迟到 24/24 全部丢弃；epoch 失效链
  新链 12/12 提交且旧链可按水位回查（旧候选对新 live 100% 丢弃）；越界与无效
  候选零提交；恢复 12/12 与丢失前 id/digest/零时戳 JSON 一致；跨进程双跑报告
  字节级一致（`cmp` 验证）。
- **本地门禁**（Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、
  CMake 3.28.3）：debug 全量 ctest **78/78**（原 76 + 本里程碑 2 目标）；
  ASAN/UBSAN/TSAN（`setarch -R`）m20 两目标通过零报告；
  `format-check`、`docs-check`、`platform-boundary-check`、`sbom-check` 通过；
  miniconda clang-tidy 18.1.8 预检库源（CI quality 同口径）零违例；
  本机同版本 NDK 两 ABI（arm64-v8a/x86_64）交叉编译预演通过（chrono duration
  自首版即显式 `duration_cast`，无 M19 型平台转换问题）。
- **限制与未执行项**：确定性投影口径非语义质量、token 收益或 continuation
  correctness 声明（`RULE-10`）；`IContextCurator` 与模型供给、增量 merge、
  自动触发、Memory promotion、subagent fork/merge（W2–W5）与 Stage E 真机
  评估（`MNT-202609-27` 通道）为显式非目标；Windows/Android 编译级/Release/
  quality 由 PR CI 回填后本里程碑方可关闭（`M20-07`）。

2026-09-14：PR CI（[#49](https://github.com/Linductor-alkaid/mira/pull/49)）
一轮修复后复验。

- 第一轮：CI quality（clang-tidy 18）报库源
  `context_memory_supervisor.cpp` 的 `performance-move-const-arg`——lambda
  init-capture 对平凡可拷贝的 `WorkingContextMergeOptions` 使用 `std::move`
  无效果（M19 同型违例；本机 tidy 预检只覆盖了新增源文件，漏了被修改的
  supervisor 编译单元）。修复为按值捕获并保留非平凡 checkpoint 的移动；
  修复后本机对两个被修改编译单元复验 tidy 零违例、debug ctest m20 全绿。
- 第二轮：CI 全绿。PR
  [#49](https://github.com/Linductor-alkaid/mira/pull/49)（head `bf4e100`，
  合并提交 `48f4781`）push pipeline run
  [`34848824021`](https://github.com/Linductor-alkaid/mira/actions/runs/34848824021)
  与 pull_request pipeline run
  [`34848828495`](https://github.com/Linductor-alkaid/mira/actions/runs/34848828495)
  各 12 项全部通过：Linux GCC/Clang（Debug/Release，两 m20 目标入 Linux 测试
  矩阵）、Windows MSVC（Debug/Release）、Android arm64-v8a 与 x86_64（NDK
  编译级）、ASAN/UBSAN/TSAN、quality（clang-tidy 18 + clang-format +
  docs/sbom/platform-boundary 检查）。§7 退出条件逐项复核后关闭本里程碑。
  遗留（显式非目标，不阻塞关闭）：`IContextCurator` 契约与模型供给参考实现、
  previous-snapshot 增量 merge（Stage W2，供应链复核通道）、Supervisor 自动
  触发与 coalescing（W3）、Memory promotion（W4）、subagent fork/merge（W5）、
  DEC-032 Stage E 真机评估（`MNT-202609-27` 证据通道）。
