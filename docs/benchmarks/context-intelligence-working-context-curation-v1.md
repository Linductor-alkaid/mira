# Context Intelligence Working Context Curation 评估 v1（Stage W2 首轮）

> 状态：Active（首轮评估，登记于 2026-09-15）
> 负责人：Mira Maintainers
> 适用范围：[DEC-035](../decisions/DEC-035-context-curator-working-context.md)
> Stage W2（[M21](../plans/m21-context-curator-stage-w2.md)）；
> `ProviderContextCurator` → `commit_working_context`（Supervisor
> `schedule_working_context_curate` 路由）对冻结合成数据集 + 脚本化确定性
> Curator 模型的管线行为评估；门禁 W2-G1–G6（M21 §4 跑前冻结）
> 复现命令见文末

## 1. 目的与范围

这是 [M21](../plans/m21-context-curator-stage-w2.md) 产出的 Working Context
模型介导 curation 管线首轮评估：冻结合成数据集（12 会话 × 每会话 5 轮链，
checkpoint 水位 8/16/24/32/40 递进、每 checkpoint 植入 6 约束 / 4 决策 /
4 未决线索、每轮 8 条 recent events；seed `0x4d49'5232'3157'4354`
（"MIR21WCT"），`dataset_digest`
`4e3221cb56f48a2a5db5e77d6057be4dfa3e613970f4a8754341e45fcf3db6b6`，评估内
断言 digest 相等后运行），模型侧由**脚本化确定性 Curator provider** 供给
（固定策略：保留上一快照全部 constraints、每轮对一条旧 decision 做
supersede、新增语句只引用 checkpoint/events、按轮填充新 section；输出契约
digest `08b6f2ed24e2a2baa14d5140591708a6921be5acf8797923d4fd151cbef4c122`）。
全部指标度量**确定性管线行为**：绑定保真、身份/链绑定、提交纪律与失败降级、
增量 merge 语义、Layer 0 转换纪律、v1.0/v1.1 兼容、跨进程确定性——本评估
**无真实模型、无网络、无凭据**，不度量、不声明语义整理质量、token 收益或
continuation correctness（`RULE-10`；真实模型口径归 Stage E 与
`MNT-202609-27` 证据通道）。

## 2. 方法与环境

- Harness：`tests/m21/m21_curator_eval.cpp`（M21-05），数据集 digest 断言
  锚定；门禁 W2-G1–G6（M21 §4.3，跑前冻结）；curation 经 Supervisor
  Deferrable 路由执行。
- 轮次（全部 12 会话）：5 轮递进链 curation + 提交；对抗轮（伪造越界/负引用/
  空 content/超界/forbidden/injection/低 confidence 各 12 组）；同输入重
  curate 幂等；同 candidate 重放；同水位冲突；陈旧水位；身份不匹配四联；
  终态迟到；退化输出（previous 非空、零 previous 引用）；五类 Curator 失败
  （provider 错误 / 畸形 JSON / refusal / deadline / 取消）；恢复（第二次
  全链重放逐对 id/digest/归一化字节比对）。
- 环境：Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、CMake
  3.28.3、CMake `debug` 预设；全轮次 ≈ 3.3 s。

## 3. 首轮结果（摘要）

| 轮次 | 提交 | NoOp | 丢弃/拒绝 | 存储不变 | 违例 |
| --- | --- | --- | --- | --- | --- |
| 递进链 | 60/60（12 会话 × 5 轮，60 次 curator 调用） | 0 | 0 | — | 绑定 0、身份 0、merge 0、转换 0 |
| 对抗输出 | 0 | 0 | 84/84 逐条丢弃（12 × 7 类） | anchor 存活 | 准入 0 |
| 同输入重 curate | — | — | — | — | 幂等 12/12（同 id 同 digest） |
| 同 candidate 重放 | 0 | 12/12 | 0 | 12/12（逐字节） | 0 |
| 同水位冲突 | 0 | 0 | 12/12（`conflicting-watermark`） | 12/12 | 0 |
| 陈旧水位 | 0 | 0 | 12/12（`stale-watermark`） | — | 0 |
| 身份不匹配 ×4 | 0 | 0 | 48/48（四类 reason code 各 12） | — | 0 |
| 终态迟到 | 0 | 0 | 24/24（session/task 各 12） | — | 0 |
| 退化输出 | 0 | 0 | 12/12（`degenerate-merge`） | 12/12 | 0 |
| Curator 失败 ×5 | 0 | 0 | 60/60 future 以正确错误 resolve | 60/60 | 非预期 0 |

- **W2-G1–G6 全部通过**；跨进程双跑 JSON 报告字节级一致（`cmp` 验证），
  同进程重放链计数逐项相等。
- 绑定保真（G1）：输入条目 1,368 ↔ 输出条目 1,368 逐轮相等；provenance
  并集（最大 5）、`source_sequence` 最小值绑定、越界/伪造/超界 100% 丢弃。
- 身份与链绑定（G2）：候选 id 五元组种子派生、水位 == checkpoint 水位、
  `source_checkpoints` 链最长 5（保序累积）、`generated_by` ==
  provider profile。
- 提交纪律与失败降级（G3）：五类失败（provider 错误/畸形 JSON/refusal/
  deadline/取消）各 12 组全部以对应错误码 resolve、零提交、已存快照逐字节
  不变。
- 增量 merge（G4）：保留 / supersede / 冲突保留三关系逐轮可见且 provenance
  正确；零 previous 引用候选 12/12 整体拒绝。
- Layer 0 纪律（G5）：八 section 全转换（constraints → P1 `UserConstraint`、
  其余七 section → P3 `CheckpointSummary`）、authority 全部
  `UntrustedExternalData`、条目 id 携带 section 标签且与 checkpoint 空间
  零冲突。
- 契约兼容与确定性（G6）：v1.0 载荷兼容读取 60/60、v1.1 JSON 往返 60/60、
  恢复 60/60 与丢失前 id/digest/归一化字节一致；store 环最深 5（有界）。
- 报告口径数字（只报告不判定）：链轮 curator 调用 60 次、快照 section 条目
  合计 1,368（输入/输出相等）、`max_provenance_union` = 5、
  `max_chain_length` = 5、`max_ring_depth` = 5。

## 4. 发现与限制

- 管线在「脚本化 Curator → 三段编号转录 + provenance 绑定 → 退化防护 →
  五元组提交 → 有界保留环」全链路上：对伪造 provenance 与越界输出 fail-closed、
  对同水位冲突不静默覆盖、对 Curator 失败与迟到结果不破坏既有状态、对退化
  merge 显式拒绝、对同输入保持幂等——M21 §4.1 冻结的机械契约全部得到量化。
- 限制：Curator 模型为脚本化确定性供给方——所有指标为确定性管线行为，不得
  外推为语义整理质量、token 收益或 continuation correctness；issue #48 的
  对照指标（Continuation correctness、Constraint retention、Failed-attempt
  recurrence，A/B/C 三臂冻结数据集）需要真实模型证据，归真实模型轮与
  DEC-032 Stage E（`MNT-202609-27` 证据通道；DEC-036 已解除"专用小模型"
  供给门禁，真实模型接入为宿主配置问题，非本方向阻塞项）。
- 请求级 token 预算与自动触发/coalescing 归 Stage W3（M21 显式非目标）。

## 5. 补跑与后续条件

- 本地复现：`cmake --preset debug && cmake --build --preset debug --target
  mira_m21_curator_eval && ctest -R mira_m21_curator_eval`（门禁失败非零
  退出；报告 JSON 经 argv[1] 落盘，跨进程 `cmp` 一致）。
- CI：`mira_m21_context_curator_test` 与 `mira_m21_curator_eval` 入 Linux
  测试与 sanitizer 矩阵（Android 为编译级门禁既有范围）；Windows/Release/
  quality 由 PR CI 回填（M21-06）。
- Stage W3（Supervisor 自动触发：watermark / event count / task boundary、
  coalescing、forced flush）立项时以本 curation 管线与提交路由为底座，
  触发策略与请求预算随其契约冻结后重测并另行登记。
