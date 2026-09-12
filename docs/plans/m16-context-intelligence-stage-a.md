# M16：Context Intelligence Stage A——long-session 基线

> 状态：In Progress（2026-09-13 依总计划 §4.1 第 6 条方向登记与既有「依设计与计划
> 推进下一步开发」授权模式立项实施，与 M8–M15 同源）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 第 8 条 Stage A）
> 前置：M4（`StandardContextManager` 已交付）；DEC-032 与
> [Context Intelligence 设计](../design/context_intelligence_design.md) 已冻结；
> `MNT-202609-28` 评估 profile 已冻结（其基线纪律由 M15 交付）
> 建议发布点：非发布物；是 Stage B–F 的对照基线（实验矩阵 A 列参考点）
> 更新日期：2026-09-13

## 1. 目标

按 Context Intelligence 设计 §12 Stage A 建立 long-session 基线：以 100/500/1000 轮
合成负载驱动现有 Layer 0（`StandardContextManager` + `ConservativeTokenCounter`，
不引入任何模型或新契约），证明并量化「Session 生命周期与 Model Context 生命周期
解耦」在纯确定性 Reduce 下的实际行为——Model Request token 趋于有界区间波动而非
线性增长——并固化选择/丢弃审计基线（按 kind 的 disposition 混合、引用替换与压缩
计数、约束保持），作为 Stage B–F 引入检索/固化后对照的 A 列参考点。

## 2. 范围与非目标

范围：`tests/m16/` long-session 基线 harness（独立可执行目标，进 ctest 与 CI 的
Linux/Windows 测试矩阵及 sanitizer 矩阵）、确定性合成负载生成器（设计 §11 七类
内容混合）、宿主呈现纪律模型、跑前冻结门禁 G1–G6、JSON 报告与 `dataset_digest`、
基线轮运行与 `docs/benchmarks/` 登记。

非目标：不实现 Layer 1–4 任何接口（`IContextEmbedder`/`IContextRetriever`/
`IContextReranker`/`ISemanticConsolidator`/`IPromptCompressor`）；不引入模型、ONNX
或向量索引；不修改 `StandardContextManager` 语义与任何公开契约；不做 miracle 派生
负载回放（其原始会话语料归 `MNT-202609-27` 外部证据通道，本轮登记补跑条件）；
不产出 Constraint Recall 等语义指标（无模型参与，归 Stage B+）；阈值与口径的任何
变更须先修订本文件与设计文档再复跑（DEC-034 同源纪律）。

## 3. 设计与决策依据

- [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 第 7/8/9 条
  （Layer 0 自足、Stage A 先行建立基线、验收以行为指标为准）
- [Context Intelligence 设计](../design/context_intelligence_design.md) §2/§4/§11/§12/§13
  （long-session 门禁定义、三级生命周期、实验矩阵 A 列）
- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)（Layer 0 所在；
  M4 交付的分区选择、水位、引用替换、图片/工具预算语义）
- `RULE-07`/`RULE-08`/`RULE-10`（预算上限、诚实声明）

## 4. 冻结的 workload profile 与门禁（跑前冻结，2026-09-13）

**驱动对象**：`StandardContextManager`（`ConservativeTokenCounter` 默认配置；不挂
exact counter、不调用 `record_usage`——估计值为保守上界，非 Provider 实测 token，
报告按 `RULE-10` 声明）。

**模型预算 profile（冻结）**：`context_window_tokens=48'000`、
`reserved_output_tokens=8'192`、`safety_margin_tokens=1'024`、
`provider_overhead_tokens=0`，其余字段取 `ContextLimits` 默认值
（`max_image_tokens=20'480`、`max_tool_schema_tokens=8'192`、trim=0.70、
checkpoint=0.85、hard=0.95）。派生：`input_budget=38'784` tokens，
checkpoint 水位界 `floor(0.85×38'784)=32'966`。工具面为固定 4 个
`ExposedToolSpec`（schema 合计远低于工具预算）。

**会话长度与采样（冻结)**：`N ∈ {100, 500, 1000}`；测量点为 `t ≤ 100` 每轮、
`t > 100` 每 10 轮；warm-up `W = 32`（依据：每轮新增原始 token 约 2.5k–3.2k，
32 轮累计已超过 `input_budget` 两倍，稳态必然进入；稳态断言只作用于 `t > W` 的
采样点）。

**内容混合（冻结；对应设计 §11 七类）**：每轮 1 条用户消息 + 1 条助手回复
（`HistoricalPayload` P5，replaceable，带 provenance 事件）；每轮 1 条观察（截图
`ImagePart` 150–400KB + UI 树文本 2–6KB；当前轮 `CurrentObservation`，次轮起降格为
`HistoricalPayload` P5 并置 replaceable）；约 60% 轮次 1 对 `ToolCall`/`ToolResult`
（`tool_call_key` 配对；当前轮结果 `consumed=false` 进入最小执行集，次轮起置
consumed；结果 replaceable 且带 `ArtifactRef` payload）；每 10 轮 1–2 条 Workflow
Run 事件文本（`HistoricalPayload`）；每 25 轮 1 条恢复事件（`HistoricalPayload`
+ `RecentError` P3 + `UncertainSideEffect` P2，副作用 3 轮后解除）；每 15 轮 1 条
`UserConstraint` P1（永不删除，累积）；每 20 轮 1 条 `RetrievedMemory` P4；每轮
1 条 `RecentAction` P3；每 50 轮更新 1 条 `CheckpointSummary` P3（仅保留最新一条）；
`SystemPolicy`/`Goal`/`TaskLimits` 静态各 1 条。**宿主呈现纪律**：除上述轮换规则
（消费翻转、副作用解除、checkpoint 替换、观察降格）外，全部历史每请求完整重
presented——这是对 Layer 0 的最坏情形压力，不预设任何滑动窗口。

**确定性（冻结）**：splitmix64（固定种子）；全部 ID 经 `Id128` 计数器构造（不使用
`generate()` 的随机设备）；无时钟、无线程、无环境访问。**Executor 路由**：无新增
异步路径——benchmark 是单线程同步纯函数驱动，无任务句柄、无关闭序列
（`RULE-02` 平凡满足）。

**门禁 G1–G6（跑前冻结；理由随条给出）**：

| 门禁 | 断言 | 依据 |
| --- | --- | --- |
| G1 请求闭合 | 全部采样 `prepare()` 成功；`utilization ≤ 1.0`；终态 watermark ∈ {Normal, Trim, Checkpoint}（不得 Hard）；审计零 `RejectedMinimumSet` | 贪心装配与水位动作的契约上界；Hard/拒绝意味着长会话下请求无法闭合 |
| G2 稳态有界带 | `t > W` 的全部采样：`19'392 ≤ estimated_tokens ≤ 32'966`（即 utilization ∈ [0.50, 0.85)） | 上界：trim(0.70) 先行、checkpoint(0.85) 截断循环的退出条件；下界：贪心填充后单项粒度 ≤ ~3.3k tokens（<8.5% budget），水位动作不会击穿半程 |
| G3 有界与 N 无关 | 三个 N 使用同一绝对界（32'966）全部通过；`S_max(1000) ≤ 1.25 × S_max(100)`；原始 presented tokens `R(1000) ≥ 5 × R(100)` | 稳态平台与 N 无关（N=100 已过 budget 两倍）；末项为负载确实线性增长的 sanity，防「有界」退化为平凡 |
| G4 约束保持 | 全部采样中每条 `UserConstraint` 的 disposition 为 Selected 且 reason 为 `minimum_set` | P1 属最小执行集的契约语义；这是 Stage D 语义召回对照的 Layer 0 精确保留基线 |
| G5 确定性 | N=100 全序列（tokens/selected/dropped/replaced/compressed/`selection_digest`）同进程两次运行逐采样相等 | 纯函数 + 确定性输入的复现性要求（基线可重复是 Stage A 门禁，设计 §12 Stage B 前置） |
| G6 审计完备 | 每采样 `item_audit` 条数 == items 条数；id 与输入一一对应且无重复；disposition 属闭集；未消费 `ToolResult` 必为 Selected | 审计是选择/丢弃基线的数据源，逐条可核对是其有效前提 |

**报告指标（只报告不判定）**：per-N token 序列统计（min/p50/p95/max，稳态段）、
终末请求按 kind 的 disposition 混合与 token 份额、presented vs selected tokens、
replaced/compressed 计数、`checkpoint_recommended` 率、最小执行集 token 随
`UserConstraint` 累积的增长（Layer 0 精确约束保持的诚实成本）。

失败处置：首轮若有门禁失败，先归因——实现缺陷登记 `BUG-YYYYMMDD-NNN`；阈值或
口径错误须先修订本文件与设计文档并记录理由后复跑，不得静默放宽。

## 5. 工作项

- [x] `M16-01` 里程碑立项与本节 profile/门禁冻结（本文件；时间戳先于任何正式运行）。
- [x] `M16-02` 合成负载生成器与确定性驱动：七类内容混合、宿主呈现纪律、采样策略、
  计数器 ID 与固定种子文本。
- [x] `M16-03` 基线 harness：G1–G6 断言、审计基线指标采集、JSON 报告与
  `dataset_digest`、门禁失败非零退出。
- [x] `M16-04` 基线轮运行与登记：首轮正式运行、报告落
  [context-intelligence-long-session-v1](../benchmarks/context-intelligence-long-session-v1.md)
  （环境、命令、摘要、限制与补跑条件）。
- [ ] `M16-05` 文档同步与 CI 取证：总计划索引与方向登记注记、设计状态注记、
  README 能力表；PR CI（Linux/Windows/sanitizers/quality）全绿后回填关闭。

## 6. 风险与阻塞

- 风险：合成负载的「用户纠正累积进最小执行集」使最小集线性增长（N=1000 约 60–70
  条）。处置：如实报告为 Layer 0 诚实成本；若首测超出 budget 推导（触发 G1 失败）
  则按第 4 节失败处置归因，不放宽门禁。
- 风险：保守 token 上界与真实 Provider 计数存在系统性偏差。处置：报告显式声明
  估计口径（`RULE-10`），结论限定为趋势与相对结构，不外推为绝对 token 声明。
- 外部：miracle 派生负载回放依赖 `MNT-202609-27` 的原始会话语料（脱敏后）。本轮
  未执行，登记补跑条件（见验证记录），不阻塞合成基线关闭。

## 7. 测试与退出条件

- [x] harness 进 ctest（label integration;m16，TIMEOUT 600）与 CI 矩阵（Linux/Windows
  测试 + ASAN/UBSAN/TSAN；Android 为编译级门禁既有范围，本目标不新增 Android 运行
  要求），门禁失败即测试失败（本地已入套件；CI 取证归 `M16-05`）。
- [x] G1–G6 在 N ∈ {100, 500, 1000} 全部通过（本地 debug 70/70 与三 sanitizer 复验
  一致；CI 复验归 `M16-05`）。
- [x] 基线报告落 `docs/benchmarks/`，含 `dataset_digest`、序列统计、disposition
  混合、限制与 miracle 派生负载补跑条件
  （[context-intelligence-long-session-v1](../benchmarks/context-intelligence-long-session-v1.md)）。
- [x] 文档同步完成（总计划 §4 里程碑索引与 §4.1 第 6 条注记、设计 §12 Stage A
  状态、README 能力表；与实现同一变更提交）。
- [ ] PR CI 全绿后回填验证记录并关闭本里程碑。

## 8. 验证记录

2026-09-13：`M16-01`～`M16-04` 本地实现与首轮基线（分支
`feat/m16-context-intelligence-stage-a`）。

- **交付**：`tests/m16/m16_long_session_baseline.cpp`（单目标
  `mira_m16_long_session_baseline`，链接 core + executor，入 ctest，label
  integration;m16，TIMEOUT 600）。单线程确定性纯函数驱动
  `StandardContextManager::prepare`；七类内容混合与宿主呈现纪律按 §4 冻结口径实现
  （工具对每 5 轮中 3 轮 ≈ 60%）；G1–G6 断言内联于采样循环（约束逐条、审计逐项、
  待定工具对、水位与闭合）；JSON 报告 + `dataset_digest` + 非零退出。
- **首轮基线**（正式运行一次，登记
  [context-intelligence-long-session-v1](../benchmarks/context-intelligence-long-session-v1.md)）：
  G1–G6 全绿；原始 presented tokens 286,579 / 1,453,697 / 2,917,016（N=100/500/1000，
  线性 10.2×），选择 token 三 N 共用平台上界 27,147（utilization 0.629–0.699 <
  trim 0.70）；用户纠正 6/33/66 条全保留，最小执行集 3,135 → 5,797 tokens 线性；
  N=1000 终末请求 3,187 项对话/历史与 600 对工具记录被逐出（连续性损失量化，
  Stage B 动机）；`checkpoint_recommended` 稳态零触发。`dataset_digest`
  `b8b082e7e26d2206505a6e54fafe495a551b4f49145d1ca2c2948f33a77c64a5`。
- **确定性**：G5 同进程重复通过；另行跨进程双跑报告字节级一致（`cmp` 验证）。
- **本地门禁**（Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、
  CMake 3.28.3）：debug 全量 ctest **70/70**（原 69 + 本目标，全套件 ~87 s，本目标
  单轮 ~11 s）；ASAN/UBSAN/TSAN（`setarch -R`）m16 1/1 通过零报告；
  `format-check`、`docs-check`、`platform-boundary-check`、`sbom-check` 通过。
- **限制与未执行项**：miracle 派生负载回放未执行（补跑条件登记于基线报告 §5，
  归 27 语料）；token 为保守上界非 Provider 实计数；Windows/Android/Release/
  quality 由 PR CI 回填后本里程碑方可关闭（`M16-05`）。
- **首轮发现**：见基线报告 §4（有界性闭合、连续性损失量化、约束线性计费、
  checkpoint 水位稳态空转——Stage D 固化触发设计输入、P3 不老化注记）。无门禁
  失败，无阈值修订。
