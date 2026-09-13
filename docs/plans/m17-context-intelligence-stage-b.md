# M17：Context Intelligence Stage B——Layer 1 检索召回

> 状态：Completed（2026-09-13：PR #45 合入 `e721d4c`，三轮 CI——前两轮各一处
> clang-tidy 违例修复后——head 双 pipeline 24/24 与合并提交 master run 12/12
> 全绿；真实 embedder 供给方与 AgentLoop 集成为显式非目标，归 Stage C+ 与
> 供应链复核通道）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 第 8 条 Stage B）
> 前置：M4（`IMemory` 三腿检索与 `MemoryQueryQuality` 已交付）；M16（Stage A 基线）；
> DEC-032 与 [Context Intelligence 设计](../design/context_intelligence_design.md) §5.2 已冻结
> 建议发布点：非发布物；是 Stage C（重排对照）的 Embedding Top-K 基线（实验矩阵 B 列）
> 更新日期：2026-09-13

## 1. 目标

按 Context Intelligence 设计 §5.2/§12 交付 Layer 1 检索召回：`IContextEmbedder`
与 `IContextRetriever` 公共契约、Cold History 三类资产（Conversation 段、
`WorkflowEpisodeRecord`、`WorkflowRecoveryLesson`）的索引对象模型、确定性会话窗口
切分、进程内参考检索索引（三腿混合 + 逐腿降级 + ACL + deadline + 有界扫描 + 可重建
失效语义）、检索候选到 Layer 0 `ContextItem` 的准入转换，以及
`ContextMemorySupervisor` 的 Executor 路由包装。向量供给沿用
`MemoryQuery::query_embedding` / `index_embedding` 的外部供给语义：embedder 是新供给方，
不是新的存储权威；Core 不引入任何模型或推理后端（DEC-032 第 5 条）。

同时按 DEC-032「验证方式」第 2 条冻结首轮检索评估数据集与阈值：确定性合成数据集 +
确定性 token-hash embedding 供给方，Recall@K / MRR / ACL 零泄漏 / 确定性四项门禁
（R1–R4，见 §4），作为 Stage C reranker uplift 对照的 B 列基线。

## 2. 范围与非目标

范围：`include/mira/context_retrieval.hpp`（Layer 1 全部契约 + 参考实现）、
`src/context/context_retrieval.cpp`（入 `mira_core`）、`ConversationEntry` 增补
`session_sequence`（DEC-016 投影回填事件序列号，加法式变更）、
`ContextMemorySupervisor::schedule_context_retrieval`（Interactive）、
`tests/m17/` 契约/集成测试与检索评估 harness、基准报告与文档同步。

非目标：不实现 `IContextReranker`/`ISemanticConsolidator`/`IPromptCompressor`/
`ContextIntelligenceService`（Stage C–F）；不引入 embedder/reranker 模型、ONNX 或
ANN 索引库（向量腿为有界线性 cosine 扫描）；不做向量索引持久化（索引是可重建投影，
重启后按 EventStore/资产重建）；不修改 `StandardContextManager` 语义；不把检索接入
`AgentLoop::build_request`（调用点变更按 DEC-002 另行评审，归后续里程碑或宿主）；
不以 FTS5 落库（进程内词法腿为参考实现，耐久部署沿用 `IMemory` 三腿）；检索评估
不使用真实语义模型（token-hash embedding 是确定性供给方，指标限定为管线行为回归
基线，不外推为语义质量声明——`RULE-10`）。

## 3. 设计与决策依据

- [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md) 第 2/5/6/7/9 条
  （Layer 1 只负责候选召回、模型不硬编码、检索对象扩展、Layer 0 自足、行为指标验收）
- [Context Intelligence 设计](../design/context_intelligence_design.md) §2/§4/§5.2/§7/§8/§11/§12/§13
  （检索层契约草案、三级生命周期、实现约束、Executor 路由、故障降级、评估设计、
  Stage B 门禁、实验矩阵 B 列）
- [Context 与 Memory 架构设计](../design/context_and_memory_design.md)（三腿检索与
  `MemoryQueryQuality` 逐腿降级语义、scope ACL、token packing 口径）
- [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)（会话投影
  事实源与 `build_conversation_view`）、[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)
  （Episode/Lesson 声明面净化纪律）
- `RULE-02`（新路由经既有 Executor 监督者）、`RULE-07`/`RULE-09`（索引为可重建投影、
  检索命中不提升权威）、`RULE-08`（全部容量/预算有界）、`RULE-10`（指标口径诚实）

## 4. 冻结的检索评估 profile 与门禁（跑前冻结，2026-09-13）

**评估对象**：`InMemoryContextIndex`（三腿混合，进程内；无 FTS5、无模型）。

**embedding 供给方（评估用，确定性）**：token-hash embedding——文本按
`[A-Za-z0-9_]+` 小写化切分，每 token 经 FNV-1a 散列到 64 维并按 hash 奇偶取 ±权值，
L2 归一化；query 与资产共用同一函数与 profile id。这是可复现的确定性供给方
（非模型），使向量腿等价于有符号 token 重叠相似度；指标含义限定为管线行为基线。

**数据集（冻结）**：splitmix64 固定种子生成 48 个查询，每查询锚定 1 个相关资产
（三类资产各 16 个；Conversation 段由合成会话切分而来，Episode/Lesson 声明面仅含
净化标识符与 reason code，DEC-029 §5 同源纪律）；每个相关资产共享查询的稀有
token；另生成 384 个干扰资产（同 ACL 域内随机 token 文本）。查询 ACL 只授予锚定
资产所在 session/scope；同时生成跨域干扰（其他 session/scope）验证零泄漏。
`dataset_digest` 为数据集资产文本的排序 SHA-256。

**门禁 R1–R4（跑前冻结；理由随条给出）**：

| 门禁 | 断言 | 依据 |
| --- | --- | --- |
| R1 召回 | 混合三腿 Recall@10 == 1.0（48/48 锚定资产进入各自查询 Top-10） | 相关资产与查询共享稀有 token，exact/词法/向量三腿中至少一腿必命中；确定性管线下达不到 1.0 即实现缺陷 |
| R2 排序 | MRR ≥ 0.90（48 查询平均倒数排名） | 干扰资产与查询无共享 token，锚定资产应稳居首位；容忍 10% 边缘次序损失 |
| R3 ACL 零泄漏 | 全部查询结果中跨 session/scope 候选数为 0；降级路径（无 embedding）同样为 0 | ACL 是过滤器而非排序特征（Context/Memory §6.1）；泄漏即安全缺陷，零容忍 |
| R4 确定性 | 同进程两次完整评估（建索引 + 全查询）输出报告逐字节相等 | 数据集、供给方与索引全确定性；Stage C 对照要求 B 列基线可重复 |

**报告指标（只报告不判定）**：每腿命中率、向量腿扫描数与跳过数、降级路径
（无 embedding、维度失配、deadline=0）下的 Recall@10/MRR、`index_lag`、
tokens_estimate 分布。失败处置同 M16 §4：实现缺陷登记 `BUG-YYYYMMDD-NNN`，
阈值或口径错误先修订本文件与设计文档再复跑。

## 5. 工作项

- [x] `M17-01` 里程碑立项与本节 profile/门禁冻结（本文件；时间戳先于任何正式评估运行）。
- [x] `M17-02` Layer 1 契约：`IContextEmbedder`/`IContextRetriever`、
  `ContextQuery`/`RetrievalBudget`/`ContextCandidate`/`ContextIndexAsset`、
  候选 JSON 序列化（`mira.context.candidate.v1`，DEC-002 版本纪律）与确定性
  资产 id 派生。
- [x] `M17-03` 会话分段：`ConversationEntry` 回填 `session_sequence`；确定性窗口
  切分（有界文本、provenance、序列水位、可重建的确定性资产 id）。
- [x] `M17-04` 参考检索索引 `InMemoryContextIndex`：三类资产注册/失效/重建、
  三腿混合排序、逐腿降级、ACL 默认拒绝、deadline 部分结果、有界向量扫描、
  token packing、禁止标记 fail-closed。
- [x] `M17-05` 候选→`ContextItem` 转换（P4/`RetrievedMemory`，provenance 透传）
  与 Layer 0 准入路径复验（检索候选作为普通 item 进入 `StandardContextManager`）。
- [x] `M17-06` Executor 路由：`schedule_context_retrieval`（Interactive、future
  必须消费）与 shutdown 后提交拒绝测试。
- [x] `M17-07` 契约/集成测试矩阵（§7）与检索评估 harness（R1–R4、JSON 报告、
  `dataset_digest`）、本地全门禁。
- [x] `M17-08` 文档同步与 CI 取证：总计划索引与注记、设计 §3/§5.2/§12 状态、
  README 能力表、API 手册（`context-memory.md` 与模块地图）、基准报告登记；
  PR CI 全绿后回填关闭（PR #45 head 双 pipeline 24/24 + master run 12/12，
  见验证记录）。

## 6. 风险与阻塞

- 风险：进程内词法腿与耐久 FTS5 腿语义存在差异（分词、排名口径）。处置：头文件
  与 API 手册显式声明参考实现定位，耐久检索仍走 `IMemory`；不宣称等价。
- 风险：deadline 检查依赖墙上时钟节拍，测试可能抖动。处置：deadline=0 用作确定性
  「立即超时」语义（与 `SqliteMemoryStore` 同一构造），其余测试用充分大 deadline；
  门禁不依赖真实耗时。
- 风险：token-hash embedding 的指标被误读为语义质量。处置：报告与设计文档显式
  限定指标口径（管线行为回归基线），语义召回声明留待真实 embedder 证据（Stage E
  设备评估或供应链复核后的供给方，DEC-032 第 5/9 条）。
- 外部：无（本里程碑不依赖真机、凭据或外部语料）。

## 7. 测试与退出条件

- [x] 契约/集成测试进 ctest（label `integration;m17`）：分段确定性与边界；
  三类资产正常检索；组件缺席降级（无 embedding→exact+词法，`vector_degraded`/
  `index_lag` 语义）；维度/profile 失配降级；deadline=0 部分结果不阻塞；ACL
  默认拒绝与跨 session 零泄漏；注册内容禁止标记 fail-closed；序列前进触发
  embedding 失效与重建；token budget packing；确定性排序（同输入同结果）；
  候选 JSON 往返；候选→`ContextItem` 后经 `StandardContextManager::prepare`
  完成准入；`schedule_context_retrieval` 路由与 `begin_shutdown` 后拒绝。
- [x] 检索评估 harness 进 ctest（label `integration;m17`，跑前冻结 R1–R4，
  失败非零退出）；首轮报告落
  [context-intelligence-retrieval-v1](../benchmarks/context-intelligence-retrieval-v1.md)
  （环境、命令、摘要、限制）。
- [x] 本地门禁：debug 全量 ctest 72/72、ASAN/UBSAN/TSAN m17 目标零报告、
  `format-check`/`docs-check`/`platform-boundary-check`/`sbom-check` 通过。
- [x] 文档同步完成（§5 `M17-08` 清单；与实现同一变更提交）。
- [x] PR CI（Linux/Windows/Android 编译级/sanitizers/quality）全绿后回填验证
  记录并关闭本里程碑（PR #45 三轮：head 双 pipeline 24/24、master run 12/12）。

## 8. 验证记录

2026-09-13：`M17-01`～`M17-07` 本地实现与首轮评估（分支
`feat/m17-context-intelligence-stage-b`）。

- **交付**：`include/mira/context_retrieval.hpp` + `src/context/context_retrieval.cpp`
  （入 `mira_core`）——Layer 1 全部契约、`InMemoryContextIndex` 参考索引、
  `segment_conversation`、`context_item_from_candidate`、候选 JSON
  （`mira.context.candidate.v1`）与确定性资产 id 派生；`ConversationEntry` 增补
  `session_sequence`（`build_conversation_view` 回填）；`ContextDomainCode` 增补
  `ScopeDenied=8`/`ForbiddenContent=9`（加法式）；supervisor 增补
  `schedule_context_retrieval`（Interactive）。测试
  `tests/m17/m17_context_retrieval_test.cpp`（13 组：分段确定性/三类正常检索/
  组件缺席降级/维度与 profile 失配/零 deadline/ACL 默认拒绝与跨域零泄漏/禁止
  marker fail-closed/水位失效与重建/exact 强制过滤与 budget/有界扫描/
  Layer 0 准入端到端/JSON 往返/supervisor 路由与 shutdown 拒绝）与
  `tests/m17/m17_retrieval_eval.cpp`（冻结 R1–R4、JSON 报告、`dataset_digest`）。
- **首轮评估**（正式运行一次，登记
  [context-intelligence-retrieval-v1](../benchmarks/context-intelligence-retrieval-v1.md)）：
  R1–R4 全绿——混合三腿 Recall@10 = 1.0（48/48）、MRR 0.986；降级（无 embedding）
  Recall@10 = 1.0、MRR 1.000；两轮 ACL 违例 0；同进程双轮与跨进程双跑报告字节级
  一致。`dataset_digest`
  `ffc59f6d0a0a375a6e8730906219339900b616ed1f861b274c437515a367649e`。
- **本地门禁**（Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H、GCC 13.3.0、
  CMake 3.28.3）：debug 全量 ctest **72/72**（原 70 + 本里程碑 2 目标，全套件
  ~34 s）；ASAN/UBSAN/TSAN（`setarch -R`）m17 两目标通过零报告；
  `format-check`、`docs-check`、`platform-boundary-check`、`sbom-check` 通过。
- **限制与未执行项**：token-hash 供给方指标为管线行为基线（非语义质量声明，
  `RULE-10`）；真实 embedder、AgentLoop 集成、向量持久化为显式非目标；
  Windows/Android/Release/quality 由 PR CI 回填后本里程碑方可关闭（`M17-08`）。
- **首轮发现**：见基线报告 §4（降级路径 MRR 反超——hash 供给方噪声特性，Stage C
  对照直接输入；exact 腿零命中属评估设计预期）。无门禁失败，无阈值修订。

2026-09-13：PR CI 证据回填并关闭。PR
[#45](https://github.com/Linductor-alkaid/mira/pull/45)（head `0971ace`，合并提交
`e721d4c`）push pipeline run
[`34743189129`](https://github.com/Linductor-alkaid/mira/actions/runs/34743189129) 与
pull_request pipeline run
[`34743190775`](https://github.com/Linductor-alkaid/mira/actions/runs/34743190775) 各
12 项，合并提交 master pipeline run
[`34743920149`](https://github.com/Linductor-alkaid/mira/actions/runs/34743920149) 12
项全部通过：Linux GCC/Clang（Debug/Release，两 m17 目标入 Linux 测试矩阵）、
Windows MSVC（Debug/Release）、Android arm64-v8a 与 x86_64（NDK 编译级，本机已用
同版本 NDK 预演 CI 目标集双 ABI 通过）、ASAN/UBSAN/TSAN、quality（clang-tidy 18 +
clang-format + docs/sbom/platform-boundary 检查）。三轮迭代：首轮 quality 两处
clang-tidy 违例（`performance-move-const-arg`、`bugprone-branch-clone`，提交
`6bb668c` 修复）、次轮一处（`performance-no-automatic-move`，提交 `0971ace` 修复，
并发现本机 miniconda clang-tidy 18.1.8 可预检——后续轮次可用）、第三轮零违例全绿。
§7 退出条件逐项复核后关闭本里程碑。遗留（显式非目标，不阻塞关闭）：真实 embedder
供给方（供应链复核后接入并重测语义指标）、AgentLoop `build_request` 集成（DEC-002
评审）、向量索引持久化与 ANN（按证据立项）。Stage C 入口门槛「Embedding Top-K vs
+Reranker 召回/成本数据」以[检索评估 v1](../benchmarks/context-intelligence-retrieval-v1.md)
B 列基线为对照起点，进入实现前依设计 §12 创建里程碑文件。
