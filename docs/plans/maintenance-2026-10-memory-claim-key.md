# 维护计划：记忆冲突检测的 claim key（2026-10）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：[MNT-202610-31…34 embedding 生产者与投影](maintenance-2026-10-memory-embedding-provider.md)（Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-23

## 1. 目标

关闭记忆冲突检测的实现缺口：检测以 statement 的 `=` 前缀为键，同一事实的两种写法
（`volume=30` 与 `volume = 30`）不被判为冲突，两条记录并存、分别参与检索与排名。

本轮把「一条记录讲的是不是同一件事」变成一个可归一化、可检索的概念（claim key），
并让冲突检索按主语收窄。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/memory_contracts.hpp` / `src/context/memory_contracts.cpp`：
  - 新增 `MemoryClaimKey{key, value, keyed}`、`memory_claim_key()`、
    `memory_same_claim()`、`memory_normalize_claim_key()`。
  - `MemoryQuery` 新增 `claim_key` 过滤项（硬过滤）。
- `src/storage/sqlite_memory_store.cpp`：查询按 claim key 收窄候选集（排序与多样性
  裁剪之前）；claim key 收窄的查询跳过每 (scope, kind) 组的多样性上限。
- `src/context/memory_consolidation.cpp`：冲突检索改用 claim key；重复判定用
  「同主语同值」，冲突判定用「同主语不同值」，冲突目标取同主语最新一条。
- `tests/m4/m4_claim_key_test.cpp`：六个用例；`tests/CMakeLists.txt` 注册。
- 测试替身：`FaithfulMemory`（M23）与 `FakeLearningMemory`（M13）按 claim key 过滤。
- 文档：设计文档 §0/§13.3/§13.5/§14.1/§16.2、API 文档、公共术语表、`DEC-061`、本计划。

### 2.2 非目标

- 不做语义等价判断：Core 不含推理（设计 §23），`volume=30` 与 `volume is 30` 仍是两个
  claim。
- 不把 claim key 存成列，不改 Memory schema，不做迁移：它是 statement 的纯函数，
  查询时现算。
- 不清扫存量数据：修复前写入的多条同主语活记录仍并存（见 §5 风险）。
- 不改 `IMemory` 的任何公开方法签名，不改 `MemoryQuery` 既有字段的语义。
- 不改示例：`claim_key` 是可选的追加项，三个消费端示例走的是词法召回路径，不涉及
  冲突检索。
- 不改 `ConsolidationEntry::key` 的定义（kind + 归一化 statement，M23 §4.3 冻结面）。
- 不引入新的 Executor 能力使用方式；不登记 Executor 反馈台账：本轮不涉及 Executor
  能力边界。

## 3. 设计与决策依据

- 决策记录：[DEC-061](../decisions/DEC-061-memory-claim-key.md)。
- 设计基线：`docs/design/context_and_memory_design.md` §13.3（固化管道）、
  §13.4（MemoryMutation 语义）、§14.1（检索阶段）、§16.2（参考后端）、§23（Core 不含
  推理）。
- 现状核查：`src/context/memory_consolidation.cpp` 的冲突检索取 statement 第一个 `=`
  之前的原文当键，用 `exact_terms` 做子串检索，命中后按 `=` 的**字节偏移**与前缀
  逐字比较（只忽略大小写）。带空格的写法既进不了候选集（`volume=` 不是 `volume = 30`
  的子串），也比较不通过（偏移与内容都不同）。

## 4. 工作项

- [x] `MNT-202610-35` claim key 契约：`MemoryClaimKey` 与三个归一化函数落成 Core 公开
      契约；`MemoryQuery::claim_key` 作为硬过滤项追加到检索契约。
- [x] `MNT-202610-36` 检索面收窄：SQLite 参考后端在排序与多样性裁剪之前按 claim key
      收窄候选集；被收窄的查询跳过多样性上限。
- [x] `MNT-202610-37` 冲突与重复判定：固化管道改用 claim key；同主语同值为
      `duplicate-noop`（附验证等级不降级），同主语不同值为冲突（Supersede，目标是
      同主语最新一条）；Core 侧自行复核主语，不信任后端过滤。
- [x] `MNT-202610-38` 测试与文档：六个新用例覆盖归一化规则、存储过滤、两种措辞不
      重复、跨措辞取代、不同主语不冲突、无值 statement 的归一化重复；设计 §13.5、
      API 文档、术语表、`DEC-061` 与本计划同步。

## 5. 风险与阻塞

- **存量同主语活记录不自动清扫**：修复前写入的 `volume=30` 与 `volume = 30` 仍然并存；
  新的写入只取代其中最新的一条。彻底修复需要一次专门的清扫 pass（类似 §14.4 的向量
  修复），本轮不做，已在设计 §13.5 与 `DEC-061` 记为已知边界。
- **散文 statement 的主语是前缀**：含 `=` 的散文按第一个 `=` 切分（Episode 的
  `task settled: outcome=…` 主语是 `task settled: outcome`）。这是词法规则的必然结果，
  与修复前的行为一致，不做例外。
- **后端可忽略 claim key 过滤**：`IMemory` 有多个实现者，忽略过滤项不会造成错误结果
  （Core 侧复核主语），只会让冲突检索多看几条记录。
- **公开契约追加**：`MemoryQuery` 新增一个可选字段，既有字段语义不变。
- **兼容性**：既有调用方不改代码即不受影响（`claim_key` 缺省即不启用过滤）；忽略
  `claim_key` 的后端结果仍然正确（Core 侧复核主语），只是冲突检索要多看几条记录。
  存储 schema 未变，无需迁移。
- 无 Executor 阻塞，无平台条件阻塞，无凭据或设备依赖。

## 6. 测试与退出条件

- [x] `claim_key_normalizes_the_subject`：主语与值的归一化、同 claim 判定、幂等性。
- [x] `claim_key_filter_narrows_the_store`：存储按主语收窄；无人写过的主语返回空；不带
      过滤时 scope 照常整体回答。
- [x] `one_fact_two_phrasings_stay_one_record`：`volume = 30` 是 `duplicate-noop`，
      scope 里仍只有一条记录。
- [x] `changed_value_supersedes_across_phrasings`：`volume = 60` 取代 `volume=30`，
      活跃面只有一条，后继带 `supersedes`。
- [x] `different_subjects_do_not_conflict`：`volume=30` 与 `volume_level=high` 各自
      `added`，两条并存。
- [x] `unkeyed_statements_conflict_on_normalized_equality`：无值 statement 只有归一化后
      相同才算重复。
- [x] 全量 `ctest --preset debug` 通过。
- [x] `architecture-check`、`check_docs`、平台边界与格式检查通过。

## 7. 验证记录

### 2026-10-23：实现、测试与文档同步完成

- **范围**：claim key 契约（`MemoryClaimKey` + 三个归一化函数）、`MemoryQuery::claim_key`、
  存储侧按主语收窄与多样性上限例外、固化管道的冲突/重复判定改写、两个测试替身按
  claim key 过滤、六个新测试函数；设计文档 §0/§13.5/§14.1/§16.2、API 文档、公共术语表、
  `DEC-061` 与本计划同步。
- **依据**：[DEC-061](../decisions/DEC-061-memory-claim-key.md)；设计 §13.3、§13.4、
  §14.1、§16.2、§23。
- **环境**：Linux 7.0.0-38-generic x86_64；gcc 13.3.0；cmake/ctest 3.28.3；14 核；
  固定 SQLite amalgamation 3.53.4。基线提交 `96e3240`（PR #103 合并后的 master）；
  本轮经 [PR #104](https://github.com/Linductor-alkaid/mira/pull/104) 提交，合并提交
  `7457091`（squash）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警（改动文件 0 条） |
  | `ctest --preset debug -j 14` | **111/111 通过** |
  | `ctest --test-dir build/debug -R "m4\|m13\|m23" -j 8` | 19/19 通过 |
  | `cmake --build --preset static-analysis -j 14` | 通过；改动文件 0 条告警（余下全部来自 `third_party/kairo` 与 libstdc++ 头文件实例化） |
  | `cmake --build --preset asan -j 14` + `ctest --test-dir build/asan -R "m4\|m13\|m23" -j 8` | **19/19 通过**，无 ASan 报告 |
  | `cmake --build --preset ubsan -j 14` + `ctest --test-dir build/ubsan -R "m4\|m13\|m23" -j 8` | **19/19 通过**，无 UBSan 报告 |
  | `cmake --build --preset tsan -j 14` + `setarch x86_64 -R ctest --test-dir build/tsan -R "m4\|m13\|m23" -j 4` | **19/19 通过**，0 条 ThreadSanitizer 告警 |
  | `cmake --build build/debug --target architecture-check` | `architecture policy is clean`（8 模块，17 条既有 baseline 抑制项，0 条 stale） |
  | `python3 tools/check_docs.py .` | `Markdown links and fences: OK` |
  | `python3 tools/check_platform_boundary.py` | `Mira Core platform boundary is clean` |
  | `cmake --build build/debug --target format-check` | 通过（268 个文件） |
  | `mira_stateful_consumer` / `mira_working_context_host_consumer` / `mira_minimal_consumer` | 三个可执行消费端均正常退出（0） |
  | `gh pr checks 104` | **24/24 通过**（push run 39901286559 与 pull_request run 39901290342 各 12 项，linux gcc/clang × Debug/Release、windows ×2、android arm64/x86_64、ASAN/UBSAN/TSAN、quality） |
  | `git diff fc7668a origin/master` | 空——合并进 master 的树就是 CI 验证过的树 |
  | 合并后在 master 上复验 | `ctest --preset debug -j 14` **111/111 通过**；三个消费端正常退出；架构 / 文档 / 平台边界 / 格式检查全过 |

- **变异验证**：把冲突检索改回修复前的 `=` 前缀 + `exact_terms` 规则后重建，
  `mira_m4_claim_key_test` 出现三处断言失败（`duplicate-noop` 与 `superseded-existing`
  断言不成立），恢复后重新通过——新用例对被修的缺口有判别力。
- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 仅覆盖记忆相关测试组（m4 / m13 / m23）；全量 111 项仅在 debug
    配置下执行。
  - TSAN 直接运行（未关闭 ASLR）时按本仓库既有取证模式关闭 ASLR 后执行。
  - 未在 Windows 与 Android 目标上本地构建验证：本轮改动不含平台相关代码路径。
  - 存量同主语活记录的清扫未实现，也未做覆盖测试（见 §5）。
  - 未做大 scope 压测：claim key 过滤在候选集上逐条计算，未测万级 scope 的开销。
- **同步**：`docs/design/context_and_memory_design.md`（§0、§13.5、§14.1、§16.2）、
  `docs/api/context-memory.md`、`docs/project/glossary.md`（Claim Key）、
  `docs/decisions/DEC-061-memory-claim-key.md`（新增）、本计划（新增）。
