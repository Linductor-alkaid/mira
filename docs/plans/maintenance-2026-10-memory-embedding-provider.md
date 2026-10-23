# 维护计划：记忆向量腿的 embedding 生产者与投影（2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：[MNT-202610-27…30 per-kind 策略表](maintenance-2026-10-memory-kind-policy.md)（Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-23

## 1. 目标

关闭设计 §14.3 点名却在实现中不存在的 `IEmbeddingProvider`：记忆向量腿既没有生产者
契约，也没有宿主可用的投影面，向量腿在默认部署下恒为空。

本轮补上两个宿主边界（`IEmbeddingProvider` 生产者、`IMemoryEmbeddingIndex` 投影）、
把它们接起来的 `MemoryEmbeddingIndexer`，并修掉一个正确性问题：向量必须始终描述它
所投影的那条记录。

## 2. 范围与非目标

### 2.1 范围

- 新增 `include/mira/memory_embedding.hpp`：
  - `IEmbeddingProvider`（`embed` / `embed_batch`）。
  - `IMemoryEmbeddingIndex`（`index_embedding` / `has_embedding` / `index_lag` /
    `clear_embeddings`）。
  - `MemoryEmbeddingIndexer`（`index_record` / `repair` / `embed_query`）与
    `MemoryEmbeddingRepairReport` / `MemoryEmbeddingRepairOptions`。
- 新增 `src/context/memory_embedding.cpp`：驱动实现。
- `include/mira/sqlite_memory_store.hpp` / `src/storage/sqlite_memory_store.cpp`：
  - `SqliteMemoryStore` 同时实现 `IMemoryEmbeddingIndex`；新增 `has_embedding()`。
  - statement 改变、Supersede 关闭前驱、Tombstone 退役时丢弃该记录的向量。
- `CMakeLists.txt`：新增 Core 源文件。
- `tests/m4/m4_memory_embedding_test.cpp`：新增六个用例；`tests/CMakeLists.txt` 注册。
- 文档：设计文档 §0/§14.3/§14.4/§16.2、API 文档、`DEC-060`、本计划。

### 2.2 非目标

- 不在 Core 中实现任何推理（不放确定性 hash embedding，不放 HTTP embedding 客户端）。
- 不把 embedding 方法并入 `IMemory`；不改动 `IMemory` 的任何公开方法签名。
- 不在写入热路径上自动嵌入（`MemoryConsolidator`、学习路径、procedure 索引均不自动
  调用生产者）。
- 不给 `memory_embeddings` 增加列，不改 Memory schema，不做迁移。
- 不实现向量 profile / 模型身份比较（留给后续）。
- 不改变向量的存储格式、维度上限（4096）或 query 侧的 cosine 腿实现。
- 不引入新的 Executor 能力使用方式；索引与修复由宿主在自己的 worker 上驱动。
- 不登记 Executor 反馈台账：本轮改动不涉及 Executor 能力边界。

## 3. 设计与决策依据

- 决策记录：[DEC-060](../decisions/DEC-060-memory-embedding-provider.md)。
- 设计基线：`docs/design/context_and_memory_design.md` §6.3（per-kind 策略表）、
  §14.1（检索阶段）、§14.3（首期后端，点名 `IEmbeddingProvider`）、§16.2（参考 schema
  与投影）、§23（拒绝同步写 embedding）。
- 现状核查：仓库内无 `IEmbeddingProvider`；`SqliteMemoryStore::index_embedding` 是存储
  专属方法；无 `has_embedding`；向量腿无生产者。`IModelProvider` 与
  `IContextEmbedder` 同样只有契约、无实现——宿主边界是既有分工，不是本轮新设。

## 4. 工作项

- [x] `MNT-202610-31` 生产者与投影契约：`IEmbeddingProvider`（含批量默认实现）与
      `IMemoryEmbeddingIndex` 落成 Core 公开契约；`SqliteMemoryStore` 实现投影面并
      新增 `has_embedding()`。
- [x] `MNT-202610-32` 驱动组件：`MemoryEmbeddingIndexer` 提供单条索引、按 scope 的
      有界修复与查询向量产出；修复报告区分 repaired / failed / ineligible。
- [x] `MNT-202610-33` 投影正确性：statement 改变或记录离开活跃面时丢弃旧向量，记录
      重新计入 index lag；kind 准入按 §6.3 策略表执行（结构化 kind 拒绝并跳过）。
- [x] `MNT-202610-34` 测试与文档：六个新用例覆盖契约、向量腿命中、修复、生产者失败
      降级、陈旧向量丢弃、退役丢向量；设计 §14.4、API 文档、`DEC-060` 与本计划同步。

## 5. 风险与阻塞

- **向量腿仍需宿主提供生产者**：Core 不做推理，这与 `IModelProvider` 同一分工。本轮
  交付的是缺失的那道缝，不是推理实现；宿主提供实现后，向量腿才非空。
- **新增可观察失败**：`index_record()` 对结构化 kind（Episode / Procedure /
  RecoveryLesson / SkillHint）返回 `InvalidRecord`。宿主需按
  `memory_kind_policy(kind)` 判断是否值得嵌入。
- **记录更新后语义召回变少**：statement 改变即丢弃向量，直到修复补齐。刻意如此：宁可
  少一条语义候选，也不用旧文本作答。
- **修复 pass 的覆盖有界**：scope 内 Active 记录超过 `max_records`（默认 512）时，宿主
  需用 `index_record` 逐条补齐剩余记录（`MemoryQuery` 没有游标分页）。
- **无 profile 身份**：向量不记录 embedding 模型身份，换模型后宿主需显式
  `clear_embeddings()` 再重建；维度失配按既有路径降级。
- 无 Executor 阻塞，无平台条件阻塞，无凭据或设备依赖。

## 6. 测试与退出条件

- [x] `provider_and_index_contract`：attach / probe / lag / clear；结构化 kind 被拒且
      不花模型调用；清空后记录仍可读。
- [x] `semantic_leg_answers_from_the_projection`：带查询向量的查询由向量腿命中记录。
- [x] `repair_embeds_records_without_vectors`：修复补齐、跳过结构化记录、第二轮无事
      可做。
- [x] `producer_failure_degrades_the_leg`：生产者失败 → failed 计数、向量腿缺席、
      lexical 腿照常回答。
- [x] `stale_vector_is_dropped_when_the_statement_changes`：向量被丢弃、重新计入 lag、
      修复重新补齐。
- [x] `tombstone_drops_the_vector`：退役丢向量，记录仍可按 id 读取。
- [x] 全量 `ctest --preset debug` 通过。
- [x] `architecture-check`、`check_docs`、平台边界与格式检查通过。

## 7. 验证记录

### 2026-10-23：实现、测试与文档同步完成

- **范围**：`IEmbeddingProvider` 与 `IMemoryEmbeddingIndex` 契约、`MemoryEmbeddingIndexer`
  驱动、`SqliteMemoryStore` 的投影实现与 `has_embedding()`、陈旧向量丢弃、六个测试
  函数；设计文档 §0/§14.4/§16.2、API 文档、`DEC-060` 与本计划同步。
- **依据**：[DEC-060](../decisions/DEC-060-memory-embedding-provider.md)；设计 §6.3、
  §14.1、§14.3、§16.2、§23。
- **环境**：Linux 7.0.0-38-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；14 核；
  固定 SQLite amalgamation 3.53.4。基线提交 `f11e0ce`（PR #102 合并后的 master）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警（改动文件 0 条） |
  | `ctest --preset debug -j 14` | **110/110 通过** |
  | `ctest --test-dir build/debug -R "m4\|m13\|m23" -j 8` | 18/18 通过 |
  | `cmake --build --preset static-analysis -j 14` | 通过；改动文件 0 条告警（余下全部来自 `third_party/kairo` 与 libstdc++ 头文件实例化） |
  | `cmake --build --preset asan -j 14` + `ctest --test-dir build/asan -R "m4\|m13\|m23" -j 8` | **18/18 通过**，无 ASan 报告 |
  | `cmake --build --preset ubsan -j 14` + `ctest --test-dir build/ubsan -R "m4\|m13\|m23" -j 8` | **18/18 通过**，无 UBSan 报告 |
  | `cmake --build --preset tsan -j 14` + `setarch x86_64 -R ctest --test-dir build/tsan -R "m4\|m13\|m23" -j 4` | **18/18 通过**，0 条 ThreadSanitizer 告警 |
  | `cmake --build build/debug --target architecture-check` | `architecture policy is clean`（8 模块，17 条既有 baseline 抑制项，0 条 stale） |
  | `python3 tools/check_docs.py .` | `Markdown links and fences: OK` |
  | `python3 tools/check_platform_boundary.py` | `Mira Core platform boundary is clean` |
  | `cmake --build build/debug --target format-check` | 通过（267 个文件） |
  | `mira_stateful_consumer` / `mira_working_context_host_consumer` / `mira_minimal_consumer` | 三个可执行消费端均正常退出（0） |

- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 仅覆盖记忆相关测试组（m4 / m13 / m23，18 项）；全量 110 项仅在
    debug 配置下执行。
  - TSAN 直接运行（未关闭 ASLR）时按本仓库既有取证模式关闭 ASLR 后执行。
  - 未在 Windows 与 Android 目标上本地构建验证：本轮改动不含平台相关代码路径。
  - 未做「中和守卫后确认新测试失败」的变异验证：移除投影或丢弃逻辑的操作会被执行环境
    的权限策略拒绝；改以断言本身论证判别力——没有生产者契约时宿主无法产出向量，
    没有投影面时 `has_embedding` 与 `index_lag` 无从查询，没有丢弃逻辑时 statement
    改变后旧向量仍在（语义腿会用旧文本作答），没有 kind 准入时结构化记录会白花一次
    模型调用。
  - 修复 pass 的分页覆盖未做大 scope 压测：`MemoryQuery` 没有游标，超过
    `max_records` 的 scope 由宿主逐条补齐，本轮未实现该路径的自动化覆盖。
- **同步**：`docs/design/context_and_memory_design.md`（§0、§14.3、§14.4、§16.2）、
  `docs/api/context-memory.md`、
  `docs/decisions/DEC-060-memory-embedding-provider.md`（新增）、本计划（新增）。
