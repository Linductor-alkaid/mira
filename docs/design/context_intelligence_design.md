# Mira Context Intelligence 设计（长会话语义上下文管理）

> 状态：Active（方向与契约草案冻结；Stage A 基线已由 [M16](../plans/m16-context-intelligence-stage-a.md) 交付——
> [long-session 基线 v1](../benchmarks/context-intelligence-long-session-v1.md)；Layer 1–4 契约草案未实现，进入里程碑的门槛见 §12）  
> 版本：0.1  
> 更新日期：2026-09-13  
> 负责人：Mira Maintainers  
> 上位设计：[Context 与 Memory 架构设计](context_and_memory_design.md)、  
> [Agent Harness 与 Workflow 架构设计](agent_harness_and_workflow_architecture.md)  
> 决策依据：[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)  
> 需求来源：[Issue #39](https://github.com/Linductor-alkaid/mira/issues/39)  

## 1. 文档目的

本文把 issue #39 提出的 Context Intelligence Pipeline 固化为 Mira 的专项设计：在
`StandardContextManager` 现有确定性压缩之上，增加 Retrieve（检索召回）、Rerank（重排）、
Consolidate（语义固化）与可选 Compress（提示压缩）四层能力，并正式定义 Hot/Warm/Cold
三级上下文生命周期。目标是：User-visible Session 可以持续增长，但每次 Model Request
的有效上下文保持有界，同时保留长期任务连续性、用户约束、重要决策与相关历史。

本文所有代码片段均为**契约草案或伪代码**，未实现；标注"现有"的组件以 M4 交付代码为准。
实现进入里程碑前，本文与头文件注释、示例、测试按 DEC-002 同步。

## 2. 设计原则

沿用 issue #39 的五条原则，并与既有约束对齐：

1. **Session lifetime != Context lifetime**。模型可见上下文不随会话长度线性增长。
2. **Deterministic first, semantic second**。能确定性完成的压缩不调模型；Layer 0
   自足（DEC-032 第 7 条），语义层失败不得阻塞请求闭合。
3. **Retrieval != Compression**。Embedding 只解决"哪些历史值得回来"，不负责生成摘要。
4. **Summary != Truth**。语义固化产物必须带 provenance（`source_events`），可回溯
   EventStore 重新验证或重新生成（RULE-07/RULE-09 延伸）。
5. **语义组件无准入权**。`ContextIntelligenceService` 只建议候选；最终 token/safety
   admission 归 `StandardContextManager`。

## 3. 现状基线（以代码为准）

五层中各层的现状（详见 DEC-032 背景与
[Context 与 Memory 架构设计](context_and_memory_design.md)）：

| 层 | 现状 | 承载组件 |
| --- | --- | --- |
| Layer 0 Reduce | 已交付 | `StandardContextManager`（P0–P5 分区、水位、引用替换、图片/工具 schema 预算、最小执行集、审计与 `selection_digest`） |
| Layer 1 Retrieve | 半有 | `IMemory::query` 三腿混合检索（FTS/exact/embedding），但仅覆盖 `MemoryRecord`；embedding 只能外部供给（`MemoryQuery::query_embedding`、`SqliteMemoryStore::index_embedding`），无 embedder；有界线性 cosine 扫描 |
| Layer 2 Rerank | 半有 | `RetrievalWeights` 固定线性加权与多样性约束；无独立 Reranker 接口与模型重排 |
| Layer 3 Consolidate | 对象错位 | `MemoryConsolidator` 是 Event -> 长期记忆写入管线；无会话级语义固化，无 `ConversationCheckpoint` |
| Layer 4 Compress | 缺 | 仅结构化引用/压缩 marker；无提示压缩接口 |

会话侧投影现状：`build_conversation_view`（`UserMessageInjected` + `LoopSettled` 重建，
DEC-016）与 `TaskCheckpoint`（确定性 reducer，`CheckpointBuilder`）是本设计的直接基座。

## 4. 三级上下文生命周期

```text
Hot Context（每次请求直接进入）
    近期对话、当前用户消息、Current Observation、活动 Workflow 状态、
    pending Tool Result、Uncertain Side Effect
        —— 对应现有最小执行集 + P0–P2 与 pinned 项语义
Warm Context（已固化投影，经准入进入）
    TaskCheckpoint、ConversationCheckpoint、活动约束、相关记忆、近期重要事件
        —— 对应 P3/P4 与检索产物
Cold History（默认不进入模型请求）
    EventStore、ArtifactStore、完整会话历史、Episode/Lesson、历史 Observation 与 Tool Result
        —— 经 Layer 1 检索才可重新进入
```

与 [Context 设计 §5 记忆层级](context_and_memory_design.md)的映射：Hot ≈ Request
Context；Warm ≈ Working Memory + Task/Conversation Checkpoint + 已检索记忆；Cold ≈
Episodic/Semantic/Procedural 之上未被检索的历史事实与工件。分区（P0–P5）描述**组装
优先级**，三级描述**存储与进入路径**，二者正交：Warm 中的检索候选仍按其 kind 占据
对应分区，由 Layer 0 统一裁决。

Cold 进入 Warm 的唯一路径是检索命中并经 Layer 0 准入；Warm 固化为 Cold 侧新事实
（如偏好候选入 Memory）走既有 `MemoryConsolidator` 管线，不经 ContextManager。

## 5. 五层管线总体架构

```text
                        ┌──────────────────────────────────────────┐
                        │        ContextIntelligenceService        │
                        │  (编排；只有建议权，无准入权)              │
                        └──────────────────────────────────────────┘
   Layer 1 检索          Layer 2 重排(可选)      Layer 3 固化(异步)   Layer 4 压缩(可选)
   IContextRetriever    IContextReranker        ISemanticConsolidator IPromptCompressor
   IContextEmbedder                             -> IModelProvider
        │                   │                        │                  │
        └───────── 候选 ContextItem ─────────────────┘                  │
                                    │                                   │
                                    ▼                                   │
                    ┌─────────────────────────────┐    仍超预算且允许压缩 ┘
                    │  Layer 0：StandardContext   │──────────────┐
                    │  Manager（现有，最终裁决）    │              ▼
                    └─────────────┬───────────────┘      压缩后候选再准入
                                  ▼
                          PreparedModelContext -> ModelGateway
```

冷路径独立于热路径运行：

```text
Conversation/Events -> Semantic Consolidator -> ConversationCheckpoint + Long-term Memory
```

### 5.1 Layer 0 — 确定性 Reduce（现有，不变）

`StandardContextManager` 继续承担 token 估算（`ConservativeTokenCounter`）、最小执行集
保护、分区选择、水位动作、引用替换、图片与工具 schema 预算、checkpoint 推荐与审计。
本设计**不修改**其任何既有语义；语义层的输出以普通 `ContextItem` 候选形式进入
`ContextRequest`，由现有 Phase 1–5 管线统一选择。

### 5.2 Layer 1 — 检索召回（IContextRetriever / IContextEmbedder）

职责：对 Cold History 建立 semantic index，按当前 Goal + Task + 近期用户消息的查询
召回 Top-K 候选。**只保证 recall，不负责排序终局**。

索引对象从 `MemoryRecord` 扩展为三类：Conversation 段（DEC-016 事件序列的窗口切分）、
`WorkflowEpisodeRecord`、`WorkflowRecoveryLesson`（DEC-029）。索引条目是可重建投影：
`(asset_id, embedding, model_profile_id, indexed_at_sequence)`，源事件变更后按
`through_event_sequence` 失效重建。

```cpp
// 契约草案（未实现）
class IContextEmbedder {
public:
    virtual Result<EmbeddingVector> embed(const ContextEmbeddingInput &input,
                                          const OperationContext &context) = 0;
};

class IContextRetriever {
public:
    // 返回候选及其 provenance；embedding 腿缺失时退化为 exact+FTS（复用
    // MemoryQueryQuality 逐腿降级语义）。
    virtual Result<std::vector<ContextCandidate>> retrieve(
        const ContextQuery &query, const RetrievalBudget &budget) = 0;
};
```

实现约束：

- 向量供给沿用 `MemoryQuery::query_embedding` / `index_embedding` 的外部供给语义；
  embedder 是新供给方，不是新的存储权威。
- 首期不引入 ANN 索引库；沿用有界线性扫描（`max_vector_scan`），超过阈值再按证据立项
  （Context 设计 §14.3 的后续项，与本设计 Stage B 产出挂钩）。
- 查询侧带 `deadline` 与 `token_budget`，超限返回部分结果并标记降级，不阻塞请求。

### 5.3 Layer 2 — 重排（IContextReranker，可选）

Embedding 优先保证 recall，Top-K（30~100）交给 reranker 收敛到 5~20。必须满足：

- Reranker 缺席时系统完整可用（embedding-only 或纯 Layer 0 配置均为合法配置）。
- 排序信号与 `RetrievalWeights` 的既有因子（verification、confidence、recency）融合，
  不整替现有排名语义。
- Android 侧优先 benchmark 小型 multilingual cross-encoder / 量化 ONNX 方案；具体模型
  未定，禁止硬编码。

```cpp
// 契约草案（未实现）
class IContextReranker {
public:
    virtual Result<std::vector<RankedContextItem>> rerank(
        const ContextQuery &query, std::span<const ContextCandidate> candidates) = 0;
};
```

### 5.4 Layer 3 — 语义固化（ISemanticConsolidator -> ConversationCheckpoint）

会话中跨多条消息才成立的信息（约束、决策、未决线索、偏好候选）由 consolidator 固化
为结构化状态。固化模型经 `IModelProvider` 配置（主模型 / 廉价云模型 / 本地小模型 /
专用模型皆可），不绑定 Agent 主模型。

```cpp
// 契约草案（未实现）
struct ConversationConstraint {
    std::string content;              // 如"发送给张三前必须获得用户确认"
    std::vector<EventId> source_events;
    float confidence;
};

struct ConversationCheckpoint {
    std::string summary;              // 叙事摘要，不参与投影 digest 的权威部分
    std::vector<ConversationConstraint> constraints;
    std::vector<ConversationDecision> decisions;
    std::vector<ConversationThread> unresolved_threads;
    std::vector<PreferenceCandidate> preferences;   // 沿用 MemoryConsolidation 的人工审批纪律
    std::vector<EventId> source_events;
    ModelProfileId generated_by;
    float confidence;
    std::uint64_t through_event_sequence;
    SessionId session_id;             // 提交校验五元组（DEC-032 第 4 条）
    TaskId task_id;
    TaskEpoch task_epoch;
    EnvironmentEpoch environment_epoch;
};

class ISemanticConsolidator {
public:
    virtual Result<ConversationCheckpoint> consolidate(
        const ConversationSegment &segment, const ConsolidationOptions &options) = 0;
};
```

与既有实体的关系：

- **TaskCheckpoint**（任务域、确定性 reducer）保持不变；ConversationCheckpoint 是会话
  域投影，与 TaskCheckpoint 并列进入 Warm，二者由 Layer 0 分别按 kind 准入。
- **ConversationLog** 仍是 RULE-07 投影；ConversationCheckpoint 的输入段由其派生，
  事实永远回 EventStore 取。
- **MemoryConsolidator** 不变；Preference 候选从 ConversationCheckpoint 进入 Memory
  时沿用其 scope/sensitivity 分类、冲突检索与人工审批管线。
- 固化输出中的约束以 P1/P2 级候选进入请求（受 Layer 0 分区规则约束），**不得**自我
  提升为 SystemPolicy（RULE-09）。

### 5.5 Layer 4 — 提示压缩（IPromptCompressor，可选实验）

仅当检索+重排后候选仍超预算时，对**允许压缩的内容**做 token 级 keep/remove 分类
（如 LLMLingua-2 类模型，评估后再定）。禁止压缩：System/Safety Policy、活动用户约束、
Tool Schema、各类 ID 与结构化状态、Uncertain Side Effect、安全敏感指令——该集合与
Layer 0 最小执行集保护语义对齐。压缩是可选优化：无压缩组件时由 Layer 0 既有裁剪
闭合。

### 5.6 ContextIntelligenceService

```cpp
// 契约草案（未实现）
class ContextIntelligenceService {
public:
    Result<ContextSuggestions> suggest(const ContextQuery &query);  // 候选上下文
    Result<ConversationCheckpoint> consolidate(const ConversationSegment &segment);
    Result<CompressedContext> compress(const ContextCompressionRequest &request);
};
```

调用点：宿主在组装 `ContextRequest` 前调用 `suggest`，把建议并入请求；AgentLoop 的
`build_request` 是否直接集成由实现里程碑决定（涉及公共调用面变更，需按 DEC-002 评审）。
Service 不持有 `IContextManager` 引用、不写 EventStore、不执行动作。

## 6. 触发、异步与竞态

### 6.1 Compaction 滞回

```text
利用率 ~60–70%：后台预备固化候选（不提交）
利用率 ~80–85%：提交 compaction（校验通过后替换 Warm 中旧 checkpoint）
目标回落     ：~40–55%，之后可长时间运行
```

禁止"每到阈值即调模型"的高频 compaction。滞回参数为配置默认值，非冻结契约。

### 6.2 异步固化与提交校验

异步固化的候选在提交前必须校验五元组（`session_id / task_id / task_epoch /
environment_epoch / through_event_sequence`）；任一不匹配即丢弃候选并保留旧 checkpoint，
禁止旧会话摘要覆盖新状态。固化任务受 Executor 管理、可协作取消；会话/任务终态后到达的
固化结果一律丢弃（终态幂等）。

### 6.3 冲突处理

ConversationCheckpoint 与近期对话冲突时优先级：近期显式用户指令 > 已验证任务状态 >
ConversationCheckpoint > 检索记忆。明显冲突时允许：失效派生记录 -> 取回 `source_events`
-> 重新固化。冲突检测在 Layer 3 输出与 Layer 0 准入两处各做一次（后者兜底）。

## 7. Executor 路由与生命周期

沿用 `ContextMemorySupervisor` 模式（`SupervisedOpClass`）：

| 工作 | 路由 | 取消 |
| --- | --- | --- |
| Embedding/重排推理 | 专属推理 worker（CPU 有界任务，`submit_auto` + future 消费） | 协作式；deadline 到即返回降级结果 |
| 检索查询 | `schedule_memory_query`（Critical/Interactive 分级） | 既有 deadline 语义 |
| 语义固化（模型调用） | 模型调用走既有 Provider 超时/取消面；编排为 Deferrable | 五元组校验兜底迟到结果 |
| 索引重建 | Deferrable，有界速率 | shutdown 时放弃，重启后按 sequence 重建 |

关闭顺序并入 `ContextMemorySupervisor::begin_shutdown` 既有链路：停生产者 -> 取消
Deferrable（含索引重建与预备固化）-> 有界等待 Critical -> 消费 future。若 Executor
公开能力不足（如推理 worker 的亲和性需求），按 AGENTS.md 登记反馈台账，不引入平行
生命周期。

## 8. 故障与降级

| 故障 | 行为 |
| --- | --- |
| Embedder 缺失/失败 | 检索退化为 exact+FTS（`vector_degraded` 语义），请求照常闭合 |
| Reranker 缺失/超时 | 使用检索序 Top-K |
| Consolidator 失败 | 无 ConversationCheckpoint 更新，Layer 0 既有水位动作兜底 |
| 向量索引损坏 | 可重建投影，从 EventStore 重建（RULE-07）；期间退化为词法检索 |
| 压缩组件失败 | Layer 0 既有裁剪路径闭合 |
| 固化候选五元组不匹配 | 丢弃候选，保留旧 checkpoint，记诊断事件 |

## 9. 安全与隐私

- 固化与嵌入输入复用现有脱敏纪律：API key、Authorization、密码与输入法敏感内容不得
  进入 embedding 索引或 checkpoint（`ConsolidationPolicy.forbidden_markers` 同源规则）。
- 检索候选受 `MemoryQuery.scopes` ACL 约束；Conversation 段候选受会话归属过滤。
- 外部内容（对话、工具输出）进索引时是不可信数据（RULE-09）：检索命中不提升权威，
  固化产物不提升为 policy。
- 训练导出边界不变（RULE-12）：embedding 索引不是训练数据集。

## 10. 可观测性

新增诊断事件（沿用现有脱敏 Diagnostic 事件面）：检索执行（腿、K、耗时、降级）、重排
执行（输入/输出数、模型 profile）、固化提交/丢弃（五元组校验结果、provenance 计数）、
压缩执行（前后 token、内容类别）。事件载荷使用稳定引用，不复制被检索原文。

## 11. 评估设计

- **Long-session continuity（Stage A 门禁）**：100/500/1000 轮合成 + miracle 派生
  负载（对话、工具调用与结果、截图、UI 树、Workflow Run、恢复事件、用户纠正），断言
  Model Request token 趋于有界（如 20k–38k 区间波动而非线性增长）。
- **Retrieval**：query -> 相关历史事件数据集；Recall@K、MRR（平均倒数排名）、
  nDCG（归一化折损累计增益）、reranker uplift、延迟、内存；重点关注
  Constraint/Decision/Unresolved-Thread Recall 而非泛化相似度。
- **Consolidation**：Constraint Recall/Precision（最高优先级）、Decision Recall、
  Unresolved Thread Recall、Contradiction Rate、Hallucination Rate、Provenance
  Accuracy、Compression Ratio。
- **Device（Stage E）**：miracle 真机测 embedding/重排/固化延迟、峰值 RAM、模型存储、
  电池与热影响；FP16/INT8/INT4 与 CPU/NNAPI/GPU/NPU 后端按可用性分列。

指标体系并入[评估与基准体系](evaluation_and_benchmark_design.md)，运行载体衔接
`MNT-202609-28/29`。

## 12. 分阶段落地与门禁

| Stage | 内容 | 门禁 |
| --- | --- | --- |
| A | Long-session benchmark 基线（不引入模型）——**已交付**（[M16](../plans/m16-context-intelligence-stage-a.md)，2026-09-13，基线见[long-session v1](../benchmarks/context-intelligence-long-session-v1.md)：token 有界与 N 无关、约束全保留、对话/工具历史稳态全逐出） | 依赖 `MNT-202609-28` profile；产出 token 趋势与选择/丢弃审计基线 |
| B | `IContextEmbedder`/`IContextRetriever`，先覆盖 Conversation/Episode/Lesson | Stage A 基线可重复 |
| C | `IContextReranker` 对照实验 | Embedding Top-K vs +Reranker 召回/成本数据 |
| D | `ISemanticConsolidator` + `ConversationCheckpoint` + provenance + 冲突处理 | 经 `IModelProvider` 配置小模型 |
| E | miracle 真机评估 | `MNT-202609-27` 证据通道 |
| F | 提示压缩实验 | 只有 token 收益不以任务成功率/约束召回为代价才进正式 Runtime |

每 Stage 进入实现前创建里程碑文件（不预分配编号），测试矩阵至少覆盖：正常完成、组件
缺席降级、异步固化五元组竞态、终态后迟到固化、shutdown、检索超时与 ACL 拒绝。

## 13. 实验矩阵

| 变体 | 检索 | 重排 | 固化 | 提示压缩 |
| --- | --- | --- | --- | --- |
| A | 无 | 无 | 无 | 无 |
| B | Embedding | 无 | 无 | 无 |
| C | Embedding | 有 | 无 | 无 |
| D | Embedding | 有 | 小模型 | 无 |
| E | Embedding | 有 | 小模型 | LLMLingua 类 |

对照指标：任务成功率、上下文 token、检索召回、约束保持、模型调用数、延迟、成本、RAM、
人工介入次数。

## 14. 候选技术与供应链

首轮候选（**未选定**；选用前经许可证与 provenance 复核并登记
[直接依赖与许可证](../supply-chain/direct-dependencies.md)）：embedding `BAAI/bge-small-zh-v1.5`
（issue #39 主张 MIT、约 24M 参数，需复核）；reranker multilingual MiniLM cross-encoder
或 BGE reranker 家族的小型量化方案；固化 Qwen ~0.6B 级或既有 Provider 廉价模型；
压缩 LLMLingua-2 类。任何候选不得成为 Mira ABI/API 的一部分。

## 15. 关联文档

- [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)
- [Context 与 Memory 架构设计](context_and_memory_design.md)（Layer 0 与检索/固化的
  既有语义）
- [Agent Harness 与 Workflow 架构设计](agent_harness_and_workflow_architecture.md)
- [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)、
  [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)
- [评估与基准体系](evaluation_and_benchmark_design.md)
- [阶段 F 后续维护计划](../plans/maintenance-2026-09-post-stage-f.md)
- [Issue #39](https://github.com/Linductor-alkaid/mira/issues/39)
