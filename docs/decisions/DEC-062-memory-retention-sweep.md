# DEC-062：跨 scope 的保留期清扫

> 状态：Accepted
> 日期：2026-10-23
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 记忆保留期维护轮
> 替代/被替代：无
> 关闭的缺口：`IMemory::compact()` 只按 scope 清扫，没有跨 scope 的保留期入口

## 背景与问题

设计 §17 的 Executor 映射表里写着「Retention/GC | soft periodic timer」，但实现里没有
对应的入口：

- `IMemory::compact(scope)` 清扫**一个** scope。宿主必须先知道要回收哪个 scope，才谈得
  上回收。
- 真正需要回收的东西恰恰是不知道发生在哪里的那些：TTL 到期的记录、越过 `purge_horizon`
  的墓碑、超过保留窗口的版本历史。它们散落在所有 scope 里。
- 结果是保留期机制在默认部署下形同虚设：没有跨 scope 的入口，宿主就只能回收它已经
  知道的 scope，而一个宿主通常根本不知道有哪些 scope。

顺带查出三个**同一条路径上的缺陷**，它们让「清扫」这件事即使存在也不可信：

1. **失败被吞掉**：按 scope 清扫里的清除失败（`ErasureStatus::Pending`）既不计数、
   不写 note，也不 hold 住 scope——清除失败在报告里完全看不见。
2. **部分删除被提交**：清除失败时，erase 路径已经删掉的 FTS、向量、版本、provenance 与
   记录行留在事务里被提交——一次失败的清除留下半条记录。
3. **清除会释放 hold**：记录级清除成功后 `erase_locked` 会删掉该 scope 的 hold。而 hold
   的含义是「这个 scope 有一笔隐私删除没做完」——清掉一条无关记录不等于隐私删除完成，
   却又把 scope 放回了 Context。

## 决策

### 1. 独立的窄接口 `IMemoryRetentionSweep`

```cpp
class IMemoryRetentionSweep {
  public:
    virtual ~IMemoryRetentionSweep() = default;
    virtual Result<MemoryCompactionResult> sweep(MemorySweepOptions options) = 0;
};
```

**不并入 `IMemory`**：`IMemory` 的每个方法都绑定一个 scope（query/get/apply/compact/erase
都带 scope），而跨 scope 的清扫没有 scope——它是**存储所有者**的操作。在一个多租户宿主
里，「谁能清扫全部租户的数据」这个问题必须由接口形状回答，而不是由调用约定回答。

这与 DEC-060 的分工一致：向量投影是可选的旁路，也是窄接口而不是 `IMemory` 的方法。区别
在于向量投影是**可选**的，保留期清扫不是——每个耐久记忆后端都要到期，所以它必须存在，
只是不必由每个 `IMemory` 实现者承担。

### 2. 一趟有界的 pass

`MemorySweepOptions{max_records, max_versions, deadline}` 给出上限。到上限就停下并报告
`more_pending`，宿主继续跑下一趟。

为什么必须有界：清扫跑在存储的单写者 worker 上，而那一条通道有操作超时。一笔覆盖全
store 的长事务会撞上超时——要么整笔回滚（清扫永远做不完），要么调用方拿到超时错误而
工作还在后台跑。有界的 pass 让每趟都短于超时，多趟收敛。

结果沿用 `MemoryCompactionResult` 并新增三个字段：`more_pending`、`scopes_swept`、
`erasures_failed`。不另立一个结果类型：清扫就是保留期清扫，计数项完全一致。

### 3. 保留期的失败语义：回滚 + 计数，不 hold

清除失败时，该条记录**整体回滚**（savepoint）：erase 路径已经删掉的 FTS、向量、版本、
provenance 与记录行全部复原，记录原样留下，下一趟 pass 重试。失败计入
`erasures_failed` 并写进 note。

**保留期失败不把 scope 挡在 Context 之外。** 被挡住的记录本来就已到期，本来就不参与
检索；scope 里的活 claim 毫发无损。把整个 scope 挡出去，是隐私删除的规则（设计 §8.3），
不是保留期的规则。混淆两者会让一次「删不掉过期附件」把一个活跃 scope 踢出 Context。

### 4. 隐私路径仍然 fail-closed，且记录级删除也要 hold

设计 §8.3 要求隐私删除的部分失败把 scope 挡在 Context 之外。原先只有 scope 级请求满足
这条：**记录级**的删除失败时，`held_scope` 是空的，scope 没被挡住——而那条记录还在库里，
也就是说「请忘掉这条记录」失败了，而 scope 仍然进入 Context。现在记录级删除在失败时
也会解析并 hold 住记录自己的 scope。

同时，记录级清除**不再**释放 hold：hold 的含义是「有一笔隐私删除没做完」，清掉一条无关
记录不构成那笔删除的完成。

### 5. 被 hold 的 scope 不参与清扫

隐私删除未完成前，那个 scope 属于那次删除：清扫既不碰它的记录，也不释放它的 hold。
这也让 `more_pending` 有意义——否则清扫会一轮轮地捡起同一条清不掉的记录，永远报
「还有活没干完」。

### 6. 读回的版本是存储的事实

顺带修掉：存储写记录时把**调用方**的版本号写进了记录文档，读回的记录因此带着一个过期
版本号。而 `MemoryRecord::version` 的契约写的是「backends set it on read」——版本是
乐观并发的令牌，读到过期令牌会让一次读改写凭空撞上 `VersionConflict`。现在写入的文档
带存储自己的版本号。

## 备选方案

- **给 `IMemory` 加 `compact_all()`**：拒绝。`IMemory` 的方法全部绑定 scope；一个没有
  scope 的方法让「谁能清扫全部租户数据」失去接口层的答案。窄接口让宿主显式持有这个
  能力。
- **把清扫做成 `compact()` 的重载（空 scope 表示全部）**：拒绝。空 scope 不是合法 scope
  （`MemoryQuery::validate()` 要求显式 scope 白名单），用「空」表达「全部」与整套契约
  的显式性相反。
- **一笔长事务扫全 store**：拒绝。存储通道有操作超时，长事务要么整笔回滚要么让调用方
  拿着超时错误而工作在后台继续。
- **失败时把 scope 挡出 Context（与隐私删除一致）**：拒绝。保留期失败与隐私失败不是同一
  件事；挡住一个活跃 scope 是一次「删不掉过期附件」的过激反应。
- **清扫自己实现清除逻辑（不复用 erase 路径）**：拒绝。清除要走 erase 路径，才能带上
  FTS、向量、版本、provenance 与附件的完整语义。
- **为清扫单独开一条存储连接**：拒绝。存储是单写者（设计 §16.2），第二条写连接会破坏
  这个保证。

## 影响与风险

- **公开契约新增**：`IMemoryRetentionSweep` 与 `MemorySweepOptions` 进入
  `mira/memory_contracts.hpp`；`MemoryCompactionResult` 新增三个字段（追加式）。
- **存储行为变更**（都是修正）：清除失败不再被吞掉；清除失败不再提交部分删除；清除不再
  释放 hold；记录级隐私删除失败会 hold 住 scope；读回的版本是存储自己的版本。
- **清扫不释放 hold**：一个被 hold 的 scope 在隐私删除重试成功前，其到期记录不会被
  回收——这是有意的，隐私删除优先。
- **`more_pending` 只表示「还有活没干完」**：清除失败不置位该标志，失败由
  `erasures_failed` 与 note 表达，宿主按周期重跑即可。
- 本轮不涉及 Executor 能力边界，不登记 Executor 反馈台账。

## 验证方式

- `tests/m4/m4_retention_sweep_test.cpp` 七个用例：
  - `sweep_reclaims_expired_records_across_scopes`：两个 scope 的到期记录都被回收，
    未到期的留下；第二趟无事可做。
  - `sweep_is_bounded_and_resumable`：`max_records = 2` 时第一趟回收两条并报告
    `more_pending`，第二趟回收最后一条。
  - `sweep_prunes_versions_beyond_the_keep_window`：版本历史裁剪到保留窗口，记录本身
    不受影响。
  - `sweep_purges_stale_tombstones_past_the_horizon`：horizon 内的墓碑保留，越过
    horizon 的清除。
  - `sweep_leaves_held_scopes_alone`：被 hold 的 scope 不参与清扫，其 hold 也不被释放。
  - `failed_retention_erasure_is_rolled_back_and_counted`：清除失败整体回滚、计入
    `erasures_failed`，scope 的活 claim 不受影响，下一趟重试。
  - `sweep_rejects_an_empty_pass`：`max_records = 0` 被拒绝。
- 变异验证：去掉 savepoint 回滚后，失败清除的用例会看到记录被部分删除；去掉 hold 排除
  后，被 hold 的 scope 的记录会被清扫掉。

## 关联文档和工作项

- 工作项：[`maintenance-2026-10-memory-retention-sweep`](../plans/maintenance-2026-10-memory-retention-sweep.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §0、§8.3、§13.4、§16.4、§17
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 相关：[DEC-058](../decisions/DEC-058-mutation-ledger-scope-binding.md)（台账 scope 绑定）、
  [DEC-060](../decisions/DEC-060-memory-embedding-provider.md)（窄接口的分工先例）
