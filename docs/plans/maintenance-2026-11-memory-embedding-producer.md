# 维护计划：记忆向量腿的 embedding 生产者实现与向量身份（2026-11）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M36 后维护轮）
> 前置：[MNT-202610-31…34 embedding 生产者契约](maintenance-2026-10-memory-embedding-provider.md)（Completed）、
> [MNT-202611-MS-02 FTS/vector 额度修复](maintenance-2026-11-memory-storage.md)（Completed）
> 建议发布点：Host contract alpha 修订（公开契约破坏性变更，见 DEC-111 影响与风险）
> 更新日期：2026-11-17

## 1. 目标

关闭 DEC-111 点名的两条缺口：记忆向量腿有契约无实现（`IEmbeddingProvider`
仓库内只有测试替身，默认部署向量腿恒空），以及向量无身份（换 embedding 模型
后旧向量与新查询静默不可比）。本轮交付：

1. `OpenAiCompatibleEmbeddingProvider`——Core 协议适配器，HTTP 栈经注入的
   `IHttpTransport`，与 `OpenAiCompatibleProvider` 同构；
2. 向量可比性守卫——`EmbedderId` 确定性派生 + schema 1.2 身份列 + 查询端
   失配可见（`vector_errors`）；
3. 召回路径的查询向量接线（opt-in，失败只降级）；
4. 离线示例与全链测试。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/memory_embedding.hpp`：`EmbedderId`、`MemoryEmbedding`、
  `IEmbeddingProvider::embedder_id()`、`IMemoryEmbeddingIndex` 四方法携带身份、
  `MemoryEmbeddingIndexer::embed_query` 返回身份向量。
- `include/mira/memory_contracts.hpp`：`MemoryQuery::query_embedding` 结构化 +
  validate 配对规则。
- `include/mira/sqlite_memory_store.hpp` / `src/storage/sqlite_memory_store.cpp`：
  schema {1,2}、`embedder` 列、链式迁移（1.0→1.1→1.2，迁移清空旧向量）、
  投影四方法与查询向量腿的身份语义。
- 新增 `include/mira/embedding_provider.hpp` / `src/model/embedding_provider.cpp`：
  `EmbeddingProfile`、`embedding_identity_from_descriptor`、
  `OpenAiCompatibleEmbeddingProvider`。
- `src/context/memory_agent*.cpp` / `include/mira/memory_agent.hpp`：
  `set_recall_embedder` opt-in 接线与降级语义。
- `tests/m3/m3_embedding_provider_test.cpp`（新）、`tests/m4` 两文件扩展、
  m36 测试扩展、8 处既有裸赋值点同步。
- 新增 `examples/embedding_recall_host_consumer.cpp`（离线，scripted transport）。
- `CMakeLists.txt`（mira_core 源列表、示例目标）、`tests/CMakeLists.txt`、
  `tests/headers/CMakeLists.txt`。
- 文档：DEC-111（新增）、DEC-060（两条备选方案修订注记）、设计 §14.3/§14.4/§16、
  API 文档、术语表、memory-storage-reference-study §2、总计划注记、README 状态行、
  本计划。

### 2.2 非目标

- 不做 ANN/向量索引升级（等真实规模基准，`memory-storage-reference-study` §5.5）。
- 不把 embed 调用接入 `BudgetLedger` 计费（修复有界 ≤512/pass；归 M31 容量轮）。
- 不桥接 `IContextEmbedder`（context 资产侧继续宿主供给）。
- 不接写入热路径（DEC-060 §6 维持）；不自动触发修复，宿主经
  `schedule_embedding_repair` 驱动。
- 不在 Core 实现本地 ONNX/离线模型；不在 Core 引入 socket/TLS 代码。
- 不动 M30 范围（其 §2.2 非目标"不做向量索引生产化"由 DEC-111 修订口径承接，
  不改 M30 计划文件）。
- 不登记 Executor 反馈台账：不涉及 Executor 能力边界。

## 3. 设计与决策依据

- 决策记录：[DEC-111](../decisions/DEC-111-memory-embedding-producer-and-vector-identity.md)
  （修订 [DEC-060](../decisions/DEC-060-memory-embedding-provider.md) 备选方案
  第 3、5 条）。
- 设计基线：`docs/design/context_and_memory_design.md` §14.3（首期后端）、
  §14.4（两条宿主边界）、§16（可重建投影）、§23（拒绝同步写 embedding）；
  `docs/design/context_intelligence_design.md` §5.2（context 侧
  `ContextEmbedding.profile_id` 身份比对先例）。
- 复用：`IHttpTransport`/`SecretRef`/`TransportDeadlines`（model_transport/
  model_profile）；`map_http_error_status` + `WireHttpResponse`（model_dialect
  公开面）；`parse_json`/`canonical_json_digest`/`digest_string`（json.hpp）；
  `context_asset_id_from_seed` 的 SHA-256→Id128 派生模式；m3_support 测试基建；
  `open_reference_store` 迁移机制与 1.0→1.1 迁移模板。

## 4. 工作项（跑前冻结）

- [x] `MNT-202611-EP-01` 立项与跑前冻结：DEC-111 落成、DEC-060 修订注记、
      本计划冻结工作项与门禁。
- [x] `MNT-202611-EP-02` 契约层：`EmbedderId`/`MemoryEmbedding`、provider 与
      投影接口签名、`MemoryQuery` 结构化与 validate 配对；8 处既有裸赋值点同步。
- [x] `MNT-202611-EP-03` 存储层：schema {1,2}、`embedder` 列、链式迁移
      （清空旧向量）、投影四方法与查询向量腿的身份过滤/失配计数。
- [x] `MNT-202611-EP-04` 生产者：`EmbeddingProfile`（validate/endpoint_url/
      canonical_descriptor）、`embedding_identity_from_descriptor`、
      `OpenAiCompatibleEmbeddingProvider`（fail-closed 解析、
      `map_http_error_status` 复用、批量 index 排序、expected_dims 断言）；
      CMake 登记。
- [x] `MNT-202611-EP-05` 生产者契约测试：请求形状/凭据解析/成功与畸形解析/
      非 2xx 映射/维度断言/批量排序/身份确定性（MockHttpTransport）。
- [x] `MNT-202611-EP-06` 存储与身份测试：1.1→1.2 迁移用例、身份守卫、
      语义腿 plumbing 用例（改述命中，注明为管线证据）。
- [x] `MNT-202611-EP-07` 召回接线：`set_recall_embedder` opt-in、查询向量
      注入与 embed 失败降级（`degraded` 置位）；m36 测试扩展。
- [x] `MNT-202611-EP-08` 离线示例：scripted transport 全链演示
      （provider→indexer→repair→recall 带向量与降级两轮）。
- [x] `MNT-202611-EP-09` 文档同步：设计 §14.3/§14.4/§16、API 文档、术语表
      （Embedder Identity 词条）、reference-study §2 表述、总计划注记、
      README 状态行、本计划验证记录。

### 跑前冻结八问

1. **行为**：DEC-111 决策 1-7；设计 §14.4 增补身份语义。
2. **所有者**：宿主拥有 provider/transport/executor；Core 拥有契约与协议适配器。
3. **契约面**：`memory_embedding.hpp`、`memory_contracts.hpp`（query 字段类型）、
   `sqlite_memory_store.hpp` 覆写签名、`embedding_provider.hpp`（新）、
   `memory_agent.hpp`（新 setter）；store schema 1.1→1.2。
4. **层与依赖**：core 新增文件仅 include std/`<mira/...>`（policy 无需登记）；
   无新外部头族。
5. **复用**：transport/错误映射/digest/迁移机制/m3_support 全部复用，不新增
   平行 HTTP 或哈希路径。
6. **时序幂等**：迁移失败回滚不盖版本戳（既有事务纪律）；mutation/repair 均
   幂等（既有语义）；`index_embedding` upsert 幂等。
7. **Executor 路由**：embed 为阻塞调用，只在宿主 worker（repair 经 supervisor
   Deferrable、recall 经 memory job）执行；查询 embed 失败只降级。
8. **验证上下文**：离线全链测试 + 门禁目标；真实供应商冒烟为补跑项（见 §7）。

## 5. 风险与阻塞

- **公开契约破坏**（`MemoryQuery` 字段类型、投影接口签名、provider 接口新增
  纯虚）：安装面消费者需同步；版本内无外部消费者承诺，风险可接受（DEC-111）。
- **存量库升级即全量重嵌**：迁移清空 `memory_embeddings`，修复 pass 重新消费
  模型调用。宿主可暂缓 repair 推迟成本；期间词法腿照常（DEC-111 决策 5）。
- **查询路径新增阻塞模型调用**：仅 opt-in `set_recall_embedder` 的部署受影响；
  失败/超时降级，不 fail recall。
- **真实供应商行为差异**：/v1/embeddings 各家响应形状细节（index 字段有无、
  usage 形状）可能偏离；解析 fail-closed + 冒烟补跑覆盖。
- 无 Executor、平台、凭据阻塞（真实冒烟待用户提供 key）。

## 6. 测试与退出条件

- [x] `m3_embedding_provider_test`：请求形状、凭据解析、成功/畸形解析、
      非 2xx 映射、expected_dims 断言、批量排序、身份确定性全绿。
- [x] `m4_sqlite_memory_test` 迁移用例：1.1 手工降版 → `Migrated` + 向量清空
      + lag 回升 → 再开 `UpToDate`。
- [x] `m4_memory_embedding_test` 身份守卫：跨身份互不可见、失配计
      `vector_errors`、lag/has/clear 按身份、换身份后 repair 重嵌。
- [x] m36：`set_recall_embedder` 接线 + embed 失败降级（degraded 置位、词法
      腿照常）。
- [x] 全量 `ctest --preset debug` 通过（含 8 处裸赋值点同步后的既有测试回归）。
- [x] `architecture-check`、`check_platform_boundary`、`check_docs`、
      `format-check` 通过；clang-tidy（static-analysis preset）改动文件零告警。
- [x] `embedding_recall_host_consumer` 正常退出（0）。
- [ ] 真实供应商 `/v1/embeddings` 冒烟：**未执行**，见 §7 补跑条件。

## 7. 验证记录

### 2026-11-17：立项与跑前冻结

- DEC-111 落成；DEC-060 备选方案第 3、5 条加修订注记；本计划冻结。
- 未执行项：全部实现工作项（EP-02…09）与真实供应商冒烟。

### 2026-11-17：实现、测试与文档同步（EP-02…09）

- **范围**：`EmbedderId`/`MemoryEmbedding` 契约、生产者与投影接口签名、
  `MemoryQuery::query_embedding` 结构化、store schema 1.2（`embedder` 列 +
  链式迁移清空旧向量）、查询向量腿身份守卫、`OpenAiCompatibleEmbeddingProvider`
  （`embedding_provider.hpp`）、`MemoryAgentService::set_recall_embedder` 接线与
  降级、离线示例、全部既有裸赋值点同步（m4×4 / m13 / consumer；m17/m18 为
  ContextQuery 无需改）与 `m4_store_open_test` 版本戳断言（链式迁移终态 1.2）。
- **依据**：[DEC-111](../decisions/DEC-111-memory-embedding-producer-and-vector-identity.md)。
- **环境**：Linux 7.0.0-43-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；
  clang-tidy 18（miniconda）；14 核；基线提交 `be44e36`（DEC-110 kairo pin，
  PR #127 合并后的 master）。工作树未提交，随 PR 提交。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警 |
  | `ctest --preset debug -j 14` | **184/184 通过**（含新增 m3 embedding provider 契约、m4 迁移/身份守卫/改述 plumbing、m36 recall 接线、consumer 示例） |
  | `cmake --preset static-analysis` + `cmake --build --preset static-analysis -j 14` | 通过；改动文件 clang-tidy 0 告警（修复 1 处 move-of-const） |
  | `cmake --build build/debug --target architecture-check` | `architecture policy is clean` |
  | `cmake --build build/debug --target platform-boundary-check` | `Mira Core platform boundary is clean` |
  | `python3 tools/check_docs.py .` | `Markdown links and fences: OK` |
  | `cmake --build build/debug --target format-check` | 通过（381 文件） |
  | `mira_embedding_recall_host_consumer` | 退出 0：repair 2/2、改述召回命中、降级轮 degraded |

- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 本轮未执行（改动路径已被 184 项离线测试覆盖；按测试分层，
    全量独立验证留待发版前委派）。补跑：`cmake --build --preset {asan,ubsan,tsan}`
    + 对应 `ctest --preset`。
  - Windows / Android 目标未本地构建：新增 Core 源文件与示例均无平台相关代码
    路径，随 PR CI 覆盖。
  - **真实供应商 `/v1/embeddings` 冒烟未执行**（用户确认暂缓）：见下方补跑条件；
    语义腿的"真实模型检索质量"证据归属该冒烟，本轮测试只证明管线（身份、预算、
    降级、改述命中）。
  - 语义相似度作为召回先验的增益未做量化评估（LongMemEval 式基准未建）。
- **同步**：`docs/design/context_and_memory_design.md`（§0、§14.4）、
  `docs/api/context-memory.md`、`docs/api/model-agent-loop.md`、`docs/api/index.md`、
  `docs/project/glossary.md`（Embedder Identity 与向量可比性守卫词条）、
  `docs/design/memory-storage-reference-study.md`（§2 表）、
  `docs/plans/mira-implementation-plan.md`（顶部注记）、`README.md`
  （Project status 增补）、`docs/decisions/DEC-060-…`（修订注记）、
  `docs/decisions/DEC-111-…`（新增）、本计划。

**补跑条件（真实供应商冒烟）**：
- 前置：可用 OpenAI-compatible `/v1/embeddings` 端点 API key（如 SiliconFlow
  `BAAI/bge-m3`），仅注入环境变量，不入库不入文档。
- 网络：`export https_proxy=http://127.0.0.1:7897`（本机固定代理端口）。
- 命令：以 `examples/embedding_recall_host_consumer` 的真实 transport 变体或
  专用 probe 执行一轮 embed→index→recall，断言维度/usage/事件无凭据泄漏。
- 结果：⬜ 未执行（负责人：Mira Maintainers；条件：用户提供 key）。
