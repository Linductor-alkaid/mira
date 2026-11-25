# 维护计划：词法分词、查询组合与会话索引排序（2026-11）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M30 期间维护轮）
> 前置：[混合检索维护记录](maintenance-2026-11-hybrid-retrieval.md)（其纯词法基线受本计划修正）
> 建议发布点：Host contract alpha 修订（查询语义与两个 store 的 schema 小版本变更，见 DEC-147）
> 更新日期：2026-11-25

## 1. 目标

让词法检索对中文可用，并修复它依赖的 UTF-8 字节边界处理。交付后，中文的记忆与会话
片段可以按字和词组召回，自然语言问句不再因为"必须包含全部词"而落空。

## 2. 范围与非目标

### 2.1 范围

- 新增 `include/mira/text_segmentation.hpp` 与 `src/support/text_segmentation.cpp`：
  无词典的确定性分词与按码点边界截断。
- `src/context/context_support.hpp`、`context_retrieval.cpp`、`context_rerank.cpp`：
  进程内词法腿与 `TokenOverlapContextReranker` 改用共享分词。
- `src/storage/sqlite_support.cpp`：查询表达式改为 OR 与短语，带单元数上限。
- `src/storage/sqlite_memory_store.cpp`：索引文本写入、schema 1.9 迁移、bm25 方向
  修正、`exact_terms` 强制过滤。
- `src/storage/sqlite_conversation_index.cpp`：索引文本列、schema 1.1 原地迁移、
  bm25 排序。
- 按字节截断的 5 处修复；`tests/benchmark/hybrid-retrieval/eval_metrics.py` 的崩溃。
- 测试、`CMakeLists.txt`、`tests/CMakeLists.txt`、`tests/headers/CMakeLists.txt`。
- 文档：DEC-147（新增）、DEC-113 与 DEC-122（修订说明）、设计、API、术语表、基准
  注记、存储核查报告、README、总计划、本计划。

### 2.2 非目标（DEC-147 第 5 节）

词典分词、LLM token 计数、身份键归一化、内容标记扫描前的归一化、繁简与拼音、英文
词干还原、`WeightedSum` 融合模式的权重、混合检索基准的重测（需要真实供应商凭据）。

## 3. 设计与决策依据

- 决策记录：[DEC-147](../decisions/DEC-147-lexical-segmentation-and-query-composition.md)。
- 缺陷编号：
  - `BUG-20261124-001`：按字节截断产生非法 UTF-8，超长中文消息使会话索引重建失败。
  - `BUG-20261124-002`：`SqliteMemoryStore` 的词法腿把 bm25 方向归一化反了，多命中时
    从弱到强返回。
  - `BUG-20261124-003`：`SqliteMemoryStore` 的 `exact_terms` 在没有记录命中时被跳过。
- 复用：`src/support/ascii_text.hpp` 的 ASCII 折叠口径、store 打开时的链式迁移模板
  （`migrate_memory_schema_1_x_to_1_y`）、FTS5 外部内容表加触发器的同步方式。

### 决策依据的测量条件

- **环境**：Linux 7.0.0-43-generic x86_64，gcc 13.3.0，Python `sqlite3` 模块（SQLite
  3.45.1，内存库）。44 篇语料下现状方案与所选方案在 Mira 捆绑的 SQLite 3.53.4
  静态库上重放，20 条查询的排名逐条一致。
- **语料**：[基准数据集](../../tests/benchmark/hybrid-retrieval/eval_dataset.json)的
  44 条记忆与 20 条查询；干扰文档取自 `docs/**/*.md`（不含混合检索基准文档）的中文
  段落，共 40,076 段、约 134 万字，取前 440、4,000、40,076 段形成四个规模。
- **方法**：每个方案建一张 FTS5 表（`unicode61`），查询用 OR 连接、`ORDER BY rank`。
  现状方案按交付代码的做法：空白切分、引号短语、AND。指标是 20 条查询宏平均的
  R@10，多金标查询取前 10 名中金标占比，严格口径（不扩展近重孪生）。
- **探针脚本**：`build/tokenizer-probe/`（本地目录，不入库；jieba 对照依赖
  genie-tts-native 的移植）。
- **局限**：20 条查询，单一数据集；只按 bm25 排序，未经过交付 store 的先验与 RRF
  融合；各真实分词方案之间约 10 个百分点以内的差异不具有分辨力。

各方案结果见 DEC-147 的备选方案表。4 万篇时的索引体积（整库含正文）：现状 8.20 MB，
单字 10.57 MB，bigram 12.11 MB，单字 + bigram 17.68 MB。同一规模下单字加 bigram 短语
的查询延迟中位数 8.4 毫秒、最慢 26.1 毫秒（`LIMIT 512`）。不设单元数上限时，最大的
合法查询（4096 字节中文，去重后 613 个单元）在同一索引上约 1 秒。

## 4. 工作项

- [x] `MNT-202611-LS-01` 立项：DEC-147 落成，本计划冻结。
- [x] `MNT-202611-LS-02` 分词契约：公开头、实现、CMake 登记、契约测试、头文件自包含
      检查、API 文档。
- [x] `MNT-202611-LS-03` 进程内词法腿与 `TokenOverlapContextReranker` 使用
      `CoarseUnits`，含中文用例。
- [x] `MNT-202611-LS-04` `SqliteMemoryStore`：FTS 行写入索引文本，schema 1.8 → 1.9
      迁移，中文召回用例与迁移用例。
- [x] `MNT-202611-LS-05` `SqliteConversationIndex`：索引文本列、schema 1.0 → 1.1 原地
      迁移、bm25 排序，含中文召回、排序与迁移用例。
- [x] `MNT-202611-LS-06` 质量门：基准数据集驱动交付 `SqliteMemoryStore` 的纯词法腿，
      断言 R@10 下限。
- [x] `BUG-20261124-001` 按字节截断改为按码点边界回退：会话分段、`truncate_text`、
      workflow 的 `bounded_text`、抽取转录载荷、模型错误摘要。
- [x] `BUG-20261124-002` 词法腿的 bm25 方向：贡献改为 `m / (1 + m)`，并以多命中用例
      锁定由强到弱的顺序。
- [x] `BUG-20261124-003` `exact_terms` 强制过滤：声明的查询只返回逐字包含全部词的
      记录，无论是否有记录满足。
- [x] `MNT-202611-LS-07` `eval_metrics.py` 崩溃修复；混合检索基准文档加入重测说明。
- [x] `MNT-202611-LS-08` 文档同步与门禁：DEC-113 与 DEC-122 修订说明、设计、API、
      术语表、总计划、README，全量测试与五项门禁。
- [ ] `MNT-202611-LS-09` 合并前的独立全量验证与目标平台 CI：见第 7 节的未执行项。

### 跑前冻结八问

1. **行为**：DEC-147 决策 1–4。
2. **所有者**：Core 拥有分词契约与进程内词法腿；`mira_state_store` 拥有两个耐久
   索引及其迁移；宿主拥有打开与重建的时机。
3. **契约面**：`text_segmentation.hpp`（新增）、memory store schema {1,8} → {1,9}、
   conversation index schema {1,0} → {1,1}、多词查询语义、会话索引排序、
   `SqliteMemoryStore` 的 `exact_terms` 强制语义。
4. **层与依赖**：新文件只 include 标准头与 `<mira/...>`；`src/storage` 经公开头使用
   分词，不引用 Core 私有头；无新外部头族，architecture-policy 不变。
5. **复用**：迁移模板、FTS5 触发器同步、`ascii_text.hpp` 口径。
6. **时序幂等**：迁移在打开事务内执行，失败回滚且不盖版本戳；分词是纯函数，
   同一输入的索引文本恒定。
7. **Executor 路由**：分词在调用线程上同步执行（纯 CPU，每条记录微秒级）；迁移
   随 store 打开执行，与既有迁移一致；不新增异步任务。
8. **验证上下文**：契约测试、store 测试、质量门、sanitizer 冒烟、全量 `ctest` 与
   五项门禁。

## 5. 风险与阻塞

- 结果集变大：OR 之后调用方拿到更多候选。`exact_terms` 是显式的强制通道。
- 移动端查询延迟未测；OR 需要给全部命中排序，语料远超 4 万篇时可能需要把 bigram
  落到索引侧。单元数上限把单次查询的规模限制在 128 个单元。
- 混合检索基准（DEC-112、DEC-113）的数字在词法腿修复后失效，重测需要真实供应商
  凭据，本轮不执行。
- `WeightedSum` 模式把先验线性叠加，多命中时可以盖过词法差距；本计划不重新标定
  它的权重。
- 非 ASCII 字母的折叠口径在 FTS5 腿与进程内腿之间不同；泰语等文字未覆盖。

## 6. 测试与退出条件

- [x] 契约测试（`mira_text_segmentation_contract_test`）：ASCII 行为不变、中文单字
      与 bigram、假名、全角与零宽归一化、非法 UTF-8、确定性与引号安全；截断落在
      码点边界且不超过上限。
- [x] memory store（`mira_m4_memory_lexical_test`）：中文子串与自然语言问句可召回，
      多命中由强到弱，旧库迁移后可召回且重开为 `UpToDate`，超限查询标记降级。
- [x] 会话索引（`mira_m30_conversation_index_lexical_test`）：中文召回、相关度优先于
      新旧、超长中文消息可重建、70 个片段的旧库原地迁移（跨分页边界）后水位与擦除
      有效。
- [x] 进程内词法腿与重排器的中文用例（`mira_m17_lexical_cjk_test`、
      `mira_m18_rerank_cjk_test`）。
- [x] 质量门（`mira_m4_lexical_eval_test`）：交付 store 纯词法腿在基准数据集上
      R@10 = 0.95，下限 0.90。
- [x] 全量 `ctest --preset debug` 通过。
- [x] `architecture-check`、`platform-boundary-check`、`docs-check`、`sbom-check`、
      `format-check` 通过。
- [x] 改动文件 clang-tidy 零告警。
- [ ] 合并前的独立全量验证通过，且 Windows、Android 与 clang 的 CI 通过。

## 7. 验证记录

### 2026-11-24：立项与决策依据测量

- DEC-147 落成，本计划冻结。决策依据的测量条件与结果见第 3 节与 DEC-147。
- 未执行项：LS-02 及之后的全部工作项。

### 2026-11-25：实现、测试与文档同步

- **范围**：第 4 节中已勾选的全部工作项。公开契约 `text_segmentation.hpp`（分词与
  截断）、OR 查询表达式与 128 个单元的上限、两个 store 的索引文本与 schema 迁移、
  会话索引按 bm25 排序、进程内词法腿与重排器的 `CoarseUnits`，以及三处缺陷的修复。
- **依据**：[DEC-147](../decisions/DEC-147-lexical-segmentation-and-query-composition.md)，
  `BUG-20261124-001` 至 `BUG-20261124-003`。
- **环境**：Linux 7.0.0-43-generic x86_64；gcc 13.3.0；cmake 与 ctest 3.28；
  clang-tidy 18.1.8（miniconda）；clang-format 18.1.8；SQLite 3.53.4（捆绑）。
  工作树基于 `feat/m30-memory-systems` 的 `c8f0bdf`，未提交。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build build/debug -j 14` | 通过，无告警 |
  | `ctest --preset debug -j 14` | **213/213 通过**（新增 6 个测试目标） |
  | `build/debug/tests/mira_m4_lexical_eval_test` | 纯词法 **R@10 = 0.95**（20 查询，解除多样性上限） |
  | ASAN 构建 + 13 个相关测试 | 通过，无 sanitizer 报告 |
  | UBSAN 构建 + 13 个相关测试 | 通过，无 sanitizer 报告 |
  | architecture、platform-boundary、docs、sbom、format 五项门禁 | 全部通过 |
  | clang-tidy，`--warnings-as-errors='*'`，8 个改动的源文件 | 零告警 |
  | `tests/headers` 自包含构建，40 个公开头 | 通过 |
  | `eval_metrics.py` 重算 2026-11-17 保存的排名 | dup-aware 数字与基准文档表格逐项一致 |

- **实现期发现**（供后续轮次参考）：
  1. bm25 方向（`BUG-20261124-002`）是在用真实多命中数据核对 OR 路径时发现的：
     查询词足够少见、bm25 拉开差距后，交付代码把最弱的命中排在最前。验证一个排序
     腿时要先让被比较的分数确实不同，全部并列时的顺序没有分辨力。
  2. 词法贡献先后试过两种映射。倒数形式 `1 / (2 − m / (1 + m))` 把 bm25 的差距压得
     很窄，乘上权重后被先验盖过；`m / (1 + m)` 让索引不给权重的匹配贡献为 0，差距
     不被压缩，最终采用后者。
  3. 70 个片段的迁移用例抓出了新迁移中分页循环的一个错误（只读第一页就退出并报
     错），修正后才通过。只有 1 个片段的用例覆盖不到翻页路径。
  4. `WeightedSum` 与 `ReciprocalRank` 在多命中时可以不同序：前者线性叠加先验。
     DEC-113 的等价锚点此前成立，只是因为 AND 下只有单命中。锚点改为"默认融合保持
     词法腿的相关度顺序"。
  5. 最大的合法查询不设上限时在 4 万篇索引上约 1 秒，且期间占住 store 的工作线程，
     所以单元数上限是必需的；超限时选择"按次序放弃并报告降级"而不是拒绝查询。
- **限制与未执行项**：
  - 合并前的独立全量验证（`MNT-202611-LS-09`，按测试分层约定委派
    Independent-Verification-Agent）未执行。负责人：Mira Maintainers。补跑条件：
    提交 PR 或合并主分支之前。
  - Windows、Android 与 clang 构建及 TSAN 未在本机执行。负责人：Mira Maintainers。
    补跑条件：随 PR 的 CI 运行。
  - 混合检索基准（DEC-112、DEC-113 的 B–E 档）重测未执行，它需要真实供应商凭据
    （`SF_EMBEDDING_KEY`）。负责人：Mira Maintainers。补跑条件：提供凭据后用
    `tests/benchmark/hybrid-retrieval/retrieval_eval` 与 `eval_metrics.py` 重跑，并更新
    基准文档；在此之前基准文档中的各配置差值不得引用。
  - 移动端的查询延迟未测。负责人：Mira Maintainers。补跑条件：Android 目标上有可用的
    基准语料后测量 OR 查询的延迟分布。
- **同步**：`docs/decisions/DEC-147-…`（新增）、`DEC-113-…` 与 `DEC-122-…`（修订
  说明）、`docs/design/context_and_memory_design.md`（§0 注记、§14.1、§14.1a、
  §14.1c）、`docs/design/memory-storage-reference-study.md`、`docs/api/context-memory.md`、
  `docs/api/durable-memory-host.md`、`docs/api/index.md`、
  `docs/benchmarks/memory-hybrid-retrieval-v1.md`、`docs/project/glossary.md`（词法单元）、
  `docs/plans/mira-implementation-plan.md`、`README.md`、公开头注释（`context_rerank.hpp`、
  `context_retrieval.hpp`、`memory_contracts.hpp`、`sqlite_memory_store.hpp`、
  `sqlite_conversation_index.hpp`、`tool_executor.hpp`）、本计划。

### 2026-11-25 提交前复核

- 在同一 Linux 工作树上复核 DEC-147，基线为 `c8f0bdf`。用真实 store 验证中文双字项时，
  两个分别加引号的字符被 FTS5 解释为 AND，分隔或倒序出现的记录也获得了双字项的分数。
  将两个字符写入同一 FTS5 短语后，相邻双字获得更高的相关度。新增回归覆盖相邻、分隔和
  倒序三种情况。
- 用 SQLite 单独复现会话 FTS5 的外部内容列不匹配。索引写入分词文本时，旧声明会让
  `integrity-check` 读取同一行的原文并报错。统一使用 `lexical` 后，独立复现通过，
  新建索引和从 1.0 迁移的 store 都通过了外部内容完整性校验。
- `cmake --build build/debug -j 10` 与 `ctest --test-dir build/debug -j 10 --output-on-failure`
  通过，测试结果为 213/213。`build/debug/tests/mira_m4_lexical_eval_test` 的 R@10 为 0.95
  （20 条查询，下限 0.90）。
- `build/asan` 与 `build/ubsan` 各通过 8 个相关测试，无 sanitizer 报告。覆盖分词契约、
  记忆检索与评估、store 打开和混合检索接线、进程内召回与重排、会话索引与迁移。
  对 8 个改动的实现文件执行 `clang-tidy --warnings-as-errors='*'` 通过。
- `format-check`、`docs-check`、`sbom-check`、`platform-boundary-check` 和
  `architecture-check` 通过。
- 合并前待验证项继续由 `MNT-202611-LS-09` 跟踪。
