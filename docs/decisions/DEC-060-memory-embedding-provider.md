# DEC-060：记忆向量腿的 embedding 生产者与可重建投影

> 状态：Accepted
> 日期：2026-10-23
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 记忆 embedding 供给维护轮
> 替代/被替代：无
> 关闭的缺口：设计 §14.3 点名的 `IEmbeddingProvider` 在实现中不存在，向量腿没有任何生产者

## 背景与问题

设计 §14.3 写明「后续可用本地 ONNX embedding model，仍通过 `IEmbeddingProvider` 隔离」，
但仓库里**没有 `IEmbeddingProvider`**，也没有任何等价的记忆侧供给契约：

- 记忆侧唯一的向量入口是 `SqliteMemoryStore::index_embedding(MemoryId, span<float>)`——
  一个存储专属的具体方法，宿主必须自己持有具体存储类型才能调用。
- 查询侧的 `MemoryQuery::query_embedding` 同样要宿主自己填。
- 没有任何办法知道**哪些**记录缺向量：`index_lag()` 只是一个计数，宿主无法据此逐条修复。
- 因此向量腿在默认部署下恒为空：exact 与 lexical 腿承担全部召回。

这与 Mira 其他边界不一致：`IModelProvider` 与 `IContextEmbedder` 都是 Core 契约、由宿主
提供实现（`IModelProvider` 在仓库内同样没有实现，只有测试 fake），唯独记忆侧缺了这道缝。

同时暴露一个正确性问题：向量是 statement 的投影，但没有任何机制保证**存下来的向量仍然
描述该记录**。记录更新后旧向量仍在，语义查询会用上一版文本作答。

## 决策

### 1. 两个宿主边界：生产者与投影

```cpp
class IEmbeddingProvider {              // 生产者：Core 不做推理
    virtual Result<std::vector<float>> embed(std::string_view text) = 0;
};

class IMemoryEmbeddingIndex {            // 投影：没有向量腿的存储不必实现
    virtual Result<void> index_embedding(MemoryId, std::span<const float>) = 0;
    virtual Result<bool> has_embedding(MemoryId) const = 0;
    virtual Result<std::size_t> index_lag() const = 0;
    virtual Result<std::size_t> clear_embeddings() = 0;
};
```

- **生产者放 Core**：Core 定义契约，宿主提供实现并拥有它的 Executor 路由。Core 本身
  不含任何推理实现——与 `IModelProvider`、`IContextEmbedder` 同一分工。放存储选项里
  会让向量腿随存储类型漂移。
- **投影是独立的窄接口**，不并入 `IMemory`：`IMemory` 是持久记忆契约，而向量索引是可选
  投影（一个没有向量腿的存储不必实现它）。查询契约本来就报告 index lag 与
  vector_degraded，投影的写入/探测面则由这个窄接口补齐。`SqliteMemoryStore` 同时实现
  两个接口。
- **不并入 `IMemory` 的另一个理由**：并入会让 7 个 `IMemory` 实现者（真实实现 + 6 个
  测试替身）全部承担四个与持久语义无关的方法。

### 2. `MemoryEmbeddingIndexer` 把两者接起来

驱动组件提供三件事：为一条记录产出并写入向量（`index_record`）、按 scope 做一次有界
修复（`repair`）、为查询产出向量（`embed_query`）。修复是**有界**的：它只看 scope 内
的活记录，受 `max_records` 限制；Active 记录多于该上限的 scope，宿主用 `index_record`
逐条补齐。

### 3. 投影是 statement 的投影

存储在**记录内容改变**时丢弃该记录的向量，记录随即重新计入 index lag。记录离开活跃面
（Supersede 关闭前驱、Tombstone 退役）时同样丢弃。理由：向量索引是可重建投影
（RULE-07），保留一个不再描述该记录的向量，会让语义查询用旧文本作答——这是正确性
问题，不是召回质量问题。丢弃后语义腿只是少一条候选，exact / lexical 腿继续回答。

不为此改动 schema：向量的新鲜度由「记录是否还挂着向量」表达，index lag 已经是这个
信号的既有载体。

### 4. kind 决定是否值得嵌入

§6.3 的 per-kind 策略表把结构化 kind（Episode / Procedure / RecoveryLesson /
SkillHint）排除在语义召回之外。为这类记录产出向量是一次没有查询会用的模型调用。因此：

- `index_record()` 对这类 kind 直接拒绝（`InvalidRecord`），错误信息说明原因；
- 修复 pass 计数跳过（`ineligible`），不视为失败。

### 5. 失败只降级，不阻塞

生产者失败或向量写入失败时，投影保持缺口，exact / lexical 腿继续回答，记录本身仍是
权威源，修复 pass 下一轮重试。`repair()` 的报告分别给出 repaired / failed / ineligible。

### 6. 不接写入热路径

`MemoryConsolidator`、学习路径与 procedure 索引**不**自动嵌入：每次写入同步调用模型会
增加控制面延迟，而向量索引是可重建投影（设计 §23 明确拒绝同步写 embedding）。宿主在
自己的 Executor worker 上驱动索引与修复。

## 备选方案

- **把 embedding 方法并入 `IMemory`**：拒绝。向量索引是可选投影，并入会让没有向量腿的
  存储也被迫实现四个方法，并让 7 个实现者全部改动。
- **在 Core 里放一个默认的确定性 embedding（例如 token hash）**：拒绝。那是词面重叠的
  近似，FTS 腿做得更好，却会让「向量腿已工作」成为假象。
- **在 Core 里放一个 OpenAI 兼容的 `/embeddings` 客户端**：拒绝。Mira core 没有 HTTP
  栈（`adapters/net` 只有 TLS 与 socket），模型供应商适配本身也是宿主职责。
- **把 embedding 写进每次 mutation 的同步路径**：拒绝。同 §23：增加热路径延迟，且向量
  是可重建投影。
- **给向量加 profile/模型身份并在存储里比较**：拒绝（首期）。那需要给
  `memory_embeddings` 加列并做一次 schema 迁移。首期用维度检查加 `clear_embeddings()`
  覆盖换模型的情况；profile 身份留给后续。
- **用记录版本号判断向量是否过期**：拒绝。等价方案，但需要把版本写进向量表；直接丢弃
  旧向量用既有信号（index lag）表达同一件事，且不需要 schema 变更。

## 影响与风险

- **向量腿仍需宿主提供生产者才非空**：Core 不做推理，这与 `IModelProvider` 同一分工。
  本轮交付的是缺失的那道缝（契约 + 驱动 + 投影面），不是推理实现。
- **新增可观察失败**：`index_record()` 对结构化 kind 返回 `InvalidRecord`。宿主需要按
  `memory_kind_policy(kind)` 判断是否值得嵌入。
- **记录更新后语义召回变少**：statement 改变即丢弃向量，直到修复 pass 补齐。这是刻意
  的：宁可少一条语义候选，也不用旧文本作答。
- **修复 pass 的覆盖有界**：scope 内 Active 记录超过 `max_records`（默认 512）时，宿主
  需用 `index_record` 逐条补齐剩余记录。
- `IMemory` 的公开方法签名不变；新增的是 Core 契约、一个 Core 组件，以及
  `SqliteMemoryStore` 上四个投影方法（其中三个已存在，语义不变）。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

- `tests/m4/m4_memory_embedding_test.cpp` 新增六个用例：
  - `provider_and_index_contract`：attach / probe / lag / clear 四个动作；结构化 kind 被
    拒绝且不花模型调用；清空后记录仍可读。
  - `semantic_leg_answers_from_the_projection`：写入向量后，带查询向量的查询由向量腿
    命中该记录。
  - `repair_embeds_records_without_vectors`：修复补齐 3 条记录、跳过 1 条结构化记录、
    第二轮无事可做。
  - `producer_failure_degrades_the_leg`：生产者失败时修复报告 failed、向量腿缺席，
    lexical 腿照常回答。
  - `stale_vector_is_dropped_when_the_statement_changes`：statement 改变后向量被丢弃、
    记录重新计入 lag，修复 pass 重新补齐。
  - `tombstone_drops_the_vector`：退役记录时向量被丢弃，记录本身仍可按 id 读取。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-memory-embedding-provider`](../plans/maintenance-2026-10-memory-embedding-provider.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §0、§6.3、§14.1、§14.3、§14.4、§16.2、§23
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 相关：[DEC-059](../decisions/DEC-059-memory-kind-policy.md)（per-kind 策略表；本记录
  的 kind 准入规则来自那张表）
