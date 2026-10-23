# DEC-061：记忆冲突检测的 claim key

> 状态：Accepted
> 日期：2026-10-23
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 记忆冲突检测维护轮
> 替代/被替代：无
> 关闭的缺口：冲突检测以 statement 的 `=` 前缀为键，同一事实的两种写法不冲突，两条记录并存

## 背景与问题

设计 §13.3 的固化管道里有一步是「Retrieve Conflicting Memories」。实现里这一步是：

```cpp
const auto separator = proposed.statement.find('=');
const std::string conflict_term = proposed.statement.substr(0, separator + 1);
conflict_query.exact_terms = {conflict_term};
```

即：取 statement 里第一个 `=` 之前的原文当键，再用它做子串检索，命中后比较两边的
前缀是否逐字相同（忽略大小写）。这条规则有三处硬伤：

1. **检索层就漏**：`volume=` 不是 `volume = 30` 的子串。带空格的写法根本进不了候选集。
2. **比较层还漏**：即使检索命中，比较要求 `=` 出现在**同一个字节偏移**上，且前缀逐字
   相同（只忽略大小写）。`volume=30` 与 `volume = 30` 的前缀既不同偏移也不同内容。
3. **同义不同写不合并**：写入路径因此允许同一个事实以不同措辞并存。两条记录都能被
   检索到、都参与排名、都不退役。

后果不是「召回差一点」，而是**事实被分裂**：一次观察到的音量 30 可以以 `volume=30`、
`volume = 30`、`VOLUME=30` 三条记录并存，各自带 TTL、各自参与冲突判断。

## 决策

### 1. claim key：statement 归一化后的主语

```cpp
struct MemoryClaimKey {
    std::string key;    // 归一化主语
    std::string value;  // 归一化断言值
    bool keyed;         // statement 是否形如 subject=value
};
```

- statement 形如 `subject=value` 时，在**第一个** `=` 处切分；不含 `=` 时 statement
  自己就是主语，没有断言值。
- 主语与断言值做同样的归一化：去首尾空白、内部空白折叠为单个空格、ASCII 大小写折叠。
  归一化幂等，后端可以对自己收到的 key 再归一化一次。
- 两条 statement 是同一个 claim，当且仅当主语相同**且**断言值相同。

于是 `volume=30` / `volume = 30` / `VOLUME=30` 是同一个 claim；`volume=60` 是同主语的
另一个 claim（冲突，Supersede）；`volume_level=high` 是另一个主语（不冲突）。

规则是**词法**的：Core 不做推理（设计 §23），`volume=30` 与 `volume is 30` 仍然是两个
claim。跨过这道边界需要模型判断，而模型不是 Core 的职责——这条边界本来就划在 Core
与宿主之间，不该由一条去重规则偷偷跨过去。

### 2. `MemoryQuery::claim_key`：按主语的硬过滤

冲突检测需要「同一个 scope 内、同一个主语的全部记录」。这不是子串匹配能表达的
（大小写与空白折叠更无法用子串表达），所以 `MemoryQuery` 增加 `claim_key` 过滤项：

- 它是**硬过滤**，与 scope 白名单同级，在排序与多样性裁剪**之前**收窄候选集。
- 存储按 `memory_claim_key(statement).key` 比较；调用方用
  `memory_normalize_claim_key()` 归一化自己的主语。
- 被 claim key 收窄的查询不再套用「每 (scope, kind) 组最多 N 条」的多样性上限：调用方
  点名了一个主语，上限只会把它要找的同主语记录挡在门外。

claim key 不是存储列，而是查询时按 statement 现算的函数：它是 statement 的派生物，
不需要 schema 迁移，也不会与 statement 脱节。

### 3. Core 侧复核，后端过滤只是收窄

`MemoryConsolidator` 在拿到候选后**自己**比较主语是否相等，不信任后端的过滤。理由：
`IMemory` 有多个实现者（真实后端加测试替身），忽略过滤项的后端不应该把一条无关记录
变成「冲突」——那会误伤用户的记忆。Core 复核让过滤项退回为**性能**手段：后端照做，
冲突检测就精确；后端不做，结果仍然正确，只是要多看几条记录。

### 4. 重复判定与冲突判定分开

- **同 claim**（主语相同且值相同）且已存副本验证等级 ≥ 提案等级 → `duplicate-noop`：
  不写、不churn、不降级。归一化让这条判定比原来更严：以前 `VOLUME=30` 与 `volume=30`
  是两条记录（后者会取代前者，造成验证等级降级），现在是同一条。
- **同主语不同值** → 冲突 → Supersede：保留历史与有效时间（设计 §13.4）。
- 冲突目标取同主语活记录中**最新**的一条：它是当前生效的 claim。

## 备选方案

- **在 Core 里比较 statement 的归一化字符串（后缀空格、大小写）**：拒绝。那只修好了
  「同一个键的不同写法」，修不好「同主语不同值」——后者才是冲突检测要处理的主要情形，
  而且归一化串本身不是可检索的键。
- **把 claim key 存成列并建索引**：拒绝（首期）。它需要一次 schema 迁移与存量回填，而
  claim key 是 statement 的纯函数，现算即可。数据量到需要索引时再评估。
- **在 Core 里做语义等价判断（embedding 相似度阈值）**：拒绝。Core 不含推理；而且向量
  腿是可选投影，不能成为正确性前提。
- **用 embedding 相似度找冲突**：拒绝。语义相近不等于讲同一件事；`volume=30` 与
  `volume=40` 语义极近却是冲突，`volume=30` 与 `volume_level=high` 语义近却不是。
- **保留 `=` 前缀规则，只补一个空白容忍**：拒绝。修不好大小写，修不好偏移比较，也
  修不好检索层的子串失配。

## 影响与风险

- **公开契约新增**：`MemoryClaimKey` 与三个函数进入 `mira/memory_contracts.hpp`；
  `MemoryQuery` 新增 `claim_key` 字段（追加式，不改既有字段语义）。
- **冲突检测变严**：过去并存的两条记录，现在会被判为同一个 claim——第二条成为
  `duplicate-noop` 而不是新记录。这是修复的目的。
- **散文 statement 的主语是前缀**：含 `=` 的散文按第一个 `=` 切分（如
  `task settled: outcome=…` 的主语是 `task settled: outcome`）。这是词法规则的必然
  结果，不做例外。
- **存量数据**：修复前写入的多条同主语活记录仍然并存；新写入只取代其中最新的一条。
  清扫既有数据需要一次专门的修复 pass（类似 §14.4 的向量修复），本轮不做。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

- `tests/m4/m4_claim_key_test.cpp` 六个用例：
  - `claim_key_normalizes_the_subject`：归一化规则、同 claim 判定、幂等性。
  - `claim_key_filter_narrows_the_store`：存储按主语收窄；无人写过的主语返回空；不带
    过滤时 scope 照常整体回答。
  - `one_fact_two_phrasings_stay_one_record`：`volume=30` 之后的 `volume = 30` 是
    `duplicate-noop`，scope 里仍只有一条记录。
  - `changed_value_supersedes_across_phrasings`：`volume = 60` 取代 `volume=30`，活跃面
    只有一条，后继带 `supersedes`。
  - `different_subjects_do_not_conflict`：`volume=30` 与 `volume_level=high` 各自
    `added`，两条并存。
  - `unkeyed_statements_conflict_on_normalized_equality`：无值 statement 只有归一化后
    相同才算重复。
- 变异验证：把冲突检索改回 `=` 前缀 + `exact_terms` 的旧规则后，上述三个用例失败
  （`duplicate-noop` / `superseded-existing` 断言不成立）。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-memory-claim-key`](../plans/maintenance-2026-10-memory-claim-key.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §0、§13.3、§13.5、§14.1、§16.2、§23
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 术语：[`docs/project/glossary.md`](../project/glossary.md)（Claim Key）
- 相关：[DEC-059](../decisions/DEC-059-memory-kind-policy.md)（per-kind 策略表）、
  [DEC-060](../decisions/DEC-060-memory-embedding-provider.md)（向量腿的生产者与投影）
