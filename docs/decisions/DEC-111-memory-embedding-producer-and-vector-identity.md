# DEC-111：记忆向量腿的 embedding 生产者实现与向量可比性身份

> 状态：Accepted
> 日期：2026-11-17
> 负责人：Mira Maintainers
> 冻结里程碑：2026-11 记忆 embedding producer 维护轮
> 替代/被替代：修订 [DEC-060](DEC-060-memory-embedding-provider.md) 备选方案第 3 条
>（"在 Core 里放一个 OpenAI 兼容的 `/embeddings` 客户端：拒绝"）与第 5 条
>（"给向量加 profile/模型身份并在存储里比较：拒绝（首期）……profile 身份留给后续"）；
> DEC-060 其余决议继续有效
> 关闭的缺口：设计 §14.3 声明的向量腿在默认部署下恒为空——仓库内
> `IEmbeddingProvider` 只有测试替身；换 embedding 模型后旧向量与新查询静默不可比

## 背景与问题

DEC-060 交付了生产者契约、投影面与驱动组件，向量腿的结构（存储、三腿打分、降级
语义、有界修复）全部就位，但两条缺口使它在实践中仍是死代码：

1. **没有生产者实现**。仓库内 `IEmbeddingProvider` 唯一实现是测试 fake
   （`HashingProvider`）；模型平面已有 `OpenAiCompatibleProvider` 协议适配先例，
   记忆平面没有对应物。DEC-060 备选方案第 3 条拒绝在 Core 放 `/embeddings`
   客户端，理由是"Core 没有 HTTP 栈"。该理由已失效：Core 现在有注入式传输边界
   `IHttpTransport`（`model_transport.hpp`），socket/TLS 实现仍在 `adapters/net`，
   Core 不含任何 socket 代码——协议适配在 Core、HTTP 栈经注入，与
   `OpenAiCompatibleProvider` 是同一分工。
2. **向量没有身份**。`memory_embeddings` 不记录产出向量的模型，`MemoryQuery`
   的查询向量也没有身份字段。不同 embedding 模型的向量不可比；换模型后旧向量
   对新查询静默失配（当前实现表现为向量腿"查不到"而非可诊断错误），唯一的
   恢复手段是宿主记住"该手动 `clear_embeddings()`"。DEC-060 备选方案第 5 条
   把 profile 身份显式"留给后续"——本轮就是那个后续。

## 决策

### 1. 向量身份：`EmbedderId`，由 canonical descriptor 确定性派生

- 新增强类型 `EmbedderId`（Id128，与 `MemoryId` 同族），标识"一个 embedding
  生产者配置"。
- 身份**不由宿主任意指定**，由 provider 从 canonical descriptor 确定性派生：
  descriptor = `协议族 | endpoint host | model_selector | expected_dims`，
  经 SHA-256 取前 16 字节（镜像 `context_asset_id_from_seed` 的
  `digest_string` → `Id128::Bytes` 先例）。同一配置恒一同身份；换模型或换
  endpoint host 自动换身份——**旧向量无需宿主纪律即自动失配**，由身份守卫
  （决策 4）与 index lag 如实表达。

### 2. 查询契约结构化：`MemoryQuery::query_embedding` 携带身份

裸 `std::vector<float>` 改为 `MemoryEmbedding{values, embedder}`。
`validate()` 强制配对：values 非空 ⇔ embedder 非 nil；维度上限（4096）与
非有限值拒绝语义不变。无身份的查询向量是不再存在的状态——不存在"能否比较"
的歧义，只有"同一身份才比较"。

### 3. 投影面携带身份：`IMemoryEmbeddingIndex` 四方法加身份参数

- `index_embedding(record, embedder, vector)`：向量按 `(record_id, embedder)`
  归属（表列 `embedder TEXT`，存 `EmbedderId::to_string()`）。一条记录同时只保存
  **一个**向量——投影仍是 statement 的单一投影；新身份的写入会把该记录重新归属
  （re-attribute），旧身份随即如实计入自己的 lag。
- `has_embedding(record, embedder)` / `index_lag(embedder)` /
  `clear_embeddings(embedder)`：按身份过滤——index lag 从此回答"多少 Active
  记录缺**当前身份**的向量"，换模型后 lag 如实回升。
- `MemoryEmbeddingIndexer` 构造签名不变：身份来自注入 provider 的
  `embedder_id()`，`embed_query` 返回 `Result<MemoryEmbedding>`。

### 4. 查询端身份守卫：不匹配计 `vector_errors`，绝不混算余弦

向量腿读取时逐行比对 `embedder` 列与查询身份：不匹配 → `++vector_errors`
（与维度不匹配同一可见路径），不进入打分。镜像 `InMemoryContextIndex` 的
`ContextEmbedding.profile_id` 比对语义（context_retrieval）。理由：身份失配
几乎总是宿主配置错误（两处 provider 配置不一致），应当可见而非静默降级为
"没查到"。

### 5. schema 1.1 → 1.2：加列并清空旧向量

- `kMemoryStoreSchema{1,2}`；`memory_embeddings` DDL 加
  `embedder TEXT NOT NULL DEFAULT ''`（fresh 库直生终态）。
- 迁移在既有共享事务内执行（`open_reference_store` 纪律：迁移不自行
  BEGIN/COMMIT，失败回滚不盖版本戳）：`ALTER TABLE` 加列后
  `DELETE FROM memory_embeddings`。
- 清空而非保留旧向量为 legacy 身份：向量是可重建投影（RULE-07），带未知
  身份的旧行永远不会被任何新查询命中，只占空间并让 lag 假性为零；清空后
  lag 立即如实反映"全部待重嵌"，修复 pass 用新 provider 补齐。无 provider
  的部署行为与今天一致（词法腿回答，lag 可见）。

### 6. Core 放 OpenAI 兼容 `/v1/embeddings` 协议适配器（修订 DEC-060 第 3 条）

- 新增 `include/mira/embedding_provider.hpp` 与
  `src/model/embedding_provider.cpp`：`EmbeddingProfile`（endpoint/prefix/
  model/expected_dims/credential SecretRef/deadlines/批量上限）+
  `OpenAiCompatibleEmbeddingProvider : IEmbeddingProvider`。
- HTTP 栈经构造注入的 `IHttpTransport`（Core 无 socket 代码）；凭据只传
  `SecretRef`，明文仅在 transport 边界解析；错误映射复用公开的
  `map_http_error_status`；请求/响应解析沿用 `parse_json` + `find` 的
  fail-closed 模式；`expected_dims` 声明即断言（响应维度不符 → 拒绝）。
- 与 `OpenAiCompatibleProvider` 同一分工：**协议适配在 Core，HTTP/TLS 实现
  在 `adapters/net`，宿主组装并拥有 Executor 路由**。本地 ONNX 等离线
  生产者仍由宿主实现 `IEmbeddingProvider` 注入，不在 Core 范围。

### 7. 召回路径接入查询向量：失败只降级

`MemoryAgentService` 新增 `set_recall_embedder()`（initialize 前 opt-in）：
recall 的 `query_records` 在 embedder 在位时先产出查询向量再查询；embed
失败（含超时）**不使 recall 失败**——不设向量、词法腿照常回答、
`result.degraded` 置位。阻塞调用运行在 Executor worker 上（memory job 本就
由 supervisor 路由），时限由 profile 的 transport deadlines 限定。

### 8. 维持不变的部分

- 不接写入热路径（DEC-060 §6 原样有效）：索引与修复仍由宿主经
  `schedule_embedding_repair`（Deferrable）驱动。
- per-kind 语义腿准入、候选池边界、`max_vector_scan` 有界线性扫、
  空-答案-优于-scope-倾倒等检索纪律全部不变。
- 语义相似仍只是打分先验（0.30/0.40 权重项），不是 claim 等价、不是授权。

### 非目标

- ANN 索引（等规模基准，见 `memory-storage-reference-study` §5.5）。
- embed 调用接入 `BudgetLedger` 计费（修复有界 ≤512/pass，后续随 M31）。
- `IContextEmbedder` 桥接（context 资产侧继续由宿主供给）。
- 本地 ONNX/离线模型进树。

## 备选方案

- **宿主侧实现协议适配器（严格遵循 DEC-060 原文）**：拒绝。每个宿主重复
  实现 HTTP+TLS+重试+解析，契约测试无法覆盖安装面；Core 已有 `IHttpTransport`
  注入边界与 `OpenAiCompatibleProvider` 先例，"Core 没有 HTTP 栈"的前提
  不再成立。修订而非绕开，是治理要求（规范 §2：契约变化先改决策记录）。
- **身份由宿主构造 indexer 时显式指定**：拒绝。两个事实源（provider 配置与
  身份标签）必然漂移；确定性派生让"换模型"自动换身份，无需纪律。
- **身份用 provider/model 可读字符串而非 Id128**：拒绝。字符串身份会诱使
  查询端做前缀/模糊匹配；强类型全等比较是唯一合法操作（与
  `ContextEmbedding.profile_id` 一致）。可读描述保留在 profile 的
  descriptor 里，用于诊断与日志。
- **迁移保留旧向量为 legacy 身份**：拒绝。见决策 5：假性零 lag 比诚实回
  升更危险。
- **查询向量保持裸 `vector<float>` + 伴生身份字段**：拒绝。允许"向量有、
  身份无"的中间态存在，validate 只能事后拒绝；结构化字段让非法状态不可
  表达。

## 影响与风险

- **公开契约破坏**：`MemoryQuery::query_embedding` 类型、
  `IMemoryEmbeddingIndex` 四方法签名、`IEmbeddingProvider`（新增纯虚
  `embedder_id()`）均为破坏性变更；仓库内受影响点已全部 enumerated（8 个
  测试文件的裸赋值点 + 全部 provider/indexer 实现者），随本轮一并更新。
  安装面消费者需同步（版本内尚无外部消费者承诺）。
- **存量库升级即全量重嵌**：1.1 库打开后 `memory_embeddings` 清空，修复
  pass 会重新消费模型调用。这是刻意行为（决策 5），宿主可暂缓启动 repair
  来推迟成本；期间向量腿缺席、词法腿照常。
- **换模型/换 endpoint host 即换身份**：镜像部署（同模型不同 host）也被视为
  不同身份。保守方向：失配可见优于错误可比。
- **查询路径新增一次阻塞模型调用**：仅当宿主 opt-in `set_recall_embedder`；
  失败与超时只降级。未 opt-in 的部署零变化。
- 不涉及 Executor 能力边界，不登记反馈台账；不引入新外部头族，无
  architecture-policy 变更。

## 验证方式

- `tests/m3/m3_embedding_provider_test.cpp`（新）：请求形状（URL/凭据解析/
  body）、成功解析、形状畸 fail-closed、非 2xx 映射、`expected_dims` 断言、
  批量 index 排序、身份确定性（同 descriptor 同 id、改 model 换 id）。
- `tests/m4/m4_sqlite_memory_test.cpp`：1.1→1.2 迁移用例（手工降版本 →
  reopen 断言 `Migrated`、向量清空、lag 回升 → 再开 `UpToDate`）。
- `tests/m4/m4_memory_embedding_test.cpp`：身份守卫（跨身份互不可见、失配
  计 `vector_errors`、lag/has/clear 按身份、换身份后 repair 重嵌）。
- m36 记忆代理测试：`set_recall_embedder` 接线与 embed 失败降级。
- `examples/embedding_recall_host_consumer.cpp`（新，离线）：scripted
  transport 演示 provider→indexer→repair→recall 全链与降级轮。
- 真实供应商冒烟（`/v1/embeddings` 经真实 endpoint）：本轮未执行，作为
  补跑条件记录于维护计划验证表（需 API key 与出网代理）。

## 关联文档和工作项

- 工作项：[`maintenance-2026-11-memory-embedding-producer`](../plans/maintenance-2026-11-memory-embedding-producer.md)
- 被修订：[DEC-060](DEC-060-memory-embedding-provider.md)（备选方案第 3、5 条）
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §14.3、§14.4、§16
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 相关：[DEC-032](DEC-032-context-intelligence-layered-context.md)（context 侧
  embedder 契约与 Stage 评估通道）、[DEC-077](DEC-077-shared-embedding-similarity.md)
  （cosine 数值规则）
