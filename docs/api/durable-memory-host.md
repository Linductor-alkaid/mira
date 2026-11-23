# 持久记忆宿主接入

> 状态：Active
> 更新日期：2026-11-10
> 关联：[存储研究](../design/memory-storage-reference-study.md)、
> [记忆代理 API](memory-agent.md)、[存储维护计划](../plans/maintenance-2026-11-memory-storage.md)

## 数据分别存在哪里

Mira 不会因为宿主保存了聊天事件就自动启用长期记忆。需要显式装配以下 owner：

| 能力 | 实现与宿主责任 |
| --- | --- |
| 事件事实源 | `FileEventStore(data_root / "events")`，文件是 `events/events.log`；先持久事件，再推进投影 |
| 长期记忆 | `SqliteMemoryStore::open(executor, options)`，设置稳定的 `options.path`；通过 consolidation/promotion 写入 |
| 任务恢复 | `SqliteCheckpointStore::open(executor, options)`，独立数据库；从 EventStore 构建 checkpoint |
| 压缩/偏好提案正文 | `FileArtifactStore`；配置给 MemoryAgentService 的 artifact owner，不能只用进程内 store |
| 交互与请求 | `InteractionCoordinator::set_memory_service()`；Gateway `set_context_supplier()`；ConversationLoop 的 history/input seams 见记忆代理 API |

`SqliteMemoryStore::open` 与 `SqliteCheckpointStore::open` 均返回 `Result`。失败应向宿主
返回明确错误；不可悄悄换成 MemoryEventStore/InMemoryContextIndex 后继续宣称持久化。
应用构建需链接 `Mira::core`、`Mira::state_store` 与宿主使用的 kairo 目标。

### 有界 SQLite 文件

见 [DEC-116](../decisions/DEC-116-bounded-sqlite-memory-files.md)。

SqliteMemoryStoreOptions.max_storage_bytes 默认为 0（保持旧行为）；设为 1 MiB～1 TiB
开启独占文件接纳，capacity_reserve_bytes 必须小于限额。范围是数据库、WAL、SHM、
rollback journal 四个文件的长度总和，不包含 Artifact/EventStore、目录元数据或块分配。
不能让其他 SQLite 连接/进程修改这些文件；独占冲突明确拒绝。应用不要用自身计数替代
实际存储接纳，也不要自动删除超额旧库来绕过打开失败。

新 mutation 受限额减 reserve 约束，已有幂等重放优先读取原账本；erase/compact/sweep
可使用预留，但不保证一定成功。真实写入前超过限额返回 ResourceExhausted，事务与
mutation-id/WAL 恢复沿既有实现。capacity_snapshot() 在 StoreChannel 上返回逐文件
长度、合计和进程内 peak_admitted_file_bytes；这个峰值含已接纳但底层失败的预留，
不是文件系统物理占用或持久跨重启峰值。未配置容量/已关闭时查询明确拒绝。

先停止生产/排空调用，再 close 并销毁实际 Store，最后关闭外部 Executor。这里只是
M31-13 的元数据首增量，跨层清理、自动归档、价值遗忘、工件共享回收仍按 M31 推进。

### 有界工件目录

给独占的 `FileArtifactStore` 设置 `FileArtifactDirectoryBudget`，可按目录内文件
逻辑长度总和接纳正文、manifest、临时文件和墓碑。常规提交保留
`erase_reserve_bytes`；写墓碑可使用预留。目录原有文件超预算时返回
ResourceExhausted。`directory_capacity()` 返回当前总量和扫描计数，有界盘点拒绝
符号链接和异常目录项。

提交前可保留 `planned_descriptor(writer)`，失败后或重开通过 `inspect_commit`
区分 Missing、Committed、UnindexedObject、Erased。获准重试的相同 writer 可以
为字节一致的未索引对象追加账本记录；异常账本以 DataLoss 暴露。先发布墓碑再删除
正文，重试 erase 可以清理残留正文。预算覆盖逻辑长度，跨存储引用、回收策略和
断电物理持久性仍由 M31 及宿主部署验收。

### 事件日志准入

`FileEventStore(root, max_events, max_log_bytes)` 对独占日志提供可选逻辑字节上限，
0 保留原有配置。写入前同时检查索引条目和完整编码记录的容量。达到上限后读取、
既有 EventId 的幂等重放仍可用。重开时立刻拒绝超过上限的旧日志，保留源文件。
`log_bytes()` 可查询实际文件长度；分段归档和共享引用回收由 M31 跟踪。
对已授权任务取证时，可使用[事件切片](events-recovery.md#有界事件切片)分批读取任务、
事件类型和时间窗口。FileEventStore 重开时从日志重建位置索引。

## 持久会话片段索引

将 `SqliteConversationIndex` 接到宿主的授权会话和现有 Executor。使用独占 SQLite
数据库文件。宿主选定已授权 Session 和 Tenant，消费有限重建任务的 future，再按精确
Session 和其配对的 Session scope 走 IContextRetriever。重建要求支持
`read_slice` 的事件源并固定本轮来源上界。它处理本会话的完整有界前缀，超过配置的
事件或字节上限时返回 ResourceExhausted 并保留旧索引。扫描与提交之间检查协作取消。
SQLite 已接纳的写入可能在等待超时后完成；按相同身份和水位重试可核对结果。

```cpp
auto built = executor.submit_auto([&] {
    return conversation_index->rebuild(principal.tenant_id, session, *events, task_context);
}).get();
// 确认 built 成功，随后从已授权会话构造查询。
ContextQuery query;
query.session = session;
query.scopes = {memory_session_scope(principal, session)};
query.text = "project update";
auto recalled = conversation_index->retrieve(query, RetrievalBudget{});
```

召回包含原生角色、来源事件和片段水位。FTS5 使用作用域词项和 Unicode61，
按最近片段序号及 ID 排序。所有 exact_terms 都必须命中。角色转换及最终准入使用
现有 `context_items_from_candidate` 和 Gateway。重建触发、授权复核与发布候选由
宿主管理。

将同一个索引赋给 `MemoryAgentConfig::conversation_index`，先初始化 MemoryAgent
重放持久擦除意图，再对外开放索引。会话擦除会持久封闭索引 scope，旧日志重建被拒。
索引擦除失败时当前实例封闭读取与重建。宿主处理失败原因，关闭服务和索引，重开后
重放擦除意图。原始事件、备份与磁盘回收由各自生命周期管理。
参见 [DEC-122](../decisions/DEC-122-durable-conversation-index.md)。

## 社交对象目录

`SqliteSocialDirectory` 保存宿主核准的对象身份和别名。数据库由宿主独占并在
Executor 关闭前排空。调用前从当前授权绑定确定租户和社交环境。对象键包含
完整环境键及稳定内部 ID，宿主负责外部账号与内部 ID 的映射。

```cpp
const SocialEnvironmentKey environment{principal.tenant_id, authorized_environment_id};
const SocialObjectKey object{environment, internal_object_id};
auto saved = directory->activate(object, SocialObjectIdentity{3, {"陈工", "Alicia Chen"}});
auto resolved = directory->resolve(environment, "陈工");
// 检查 saved 和 resolved，Ambiguous 时交宿主澄清。
```

别名每项上限 256 字节，每个对象最多 16 项。索引折叠 ASCII 大小写与空白，
非 ASCII 字符保留。完整对象版本和别名同事务更新，旧版本拒绝，同版本重放要求
别名内容一致。同名结果包含完整对象键和读取时的版本。即使只返回一条，也可能
标为 Ambiguous 和 truncated。宿主延后使用候选时重新核对授权和当前版本。

`erase_object` 清理别名并持久封闭该对象；`erase_environment` 清理整个社交环境
并封闭重建。目录的对象及环境配额计入保留的擦除屏障。擦除失败会封闭当前实例。
宿主处理原因并关闭实例。与记忆库配对时，重开自动恢复已提交的意图。独立目录的宿主
可按原目标重试擦除。
见 [DEC-123](../decisions/DEC-123-social-identity-directory.md)。

## 社交画像字段

宿主查验当前租户和环境授权，从目录读取对象并确认歧义，再将已核准的来源事件转换为
`SocialAttribute`。字段键由 1～48 个 ASCII 小写字母、数字、下划线或连字符组成，
首字符为字母。值为非空 UTF-8 文本，上限 2048 字节。自述、用户确认和推断分别对应
`Observed`、`HumanConfirmed` 和 `Unverified`。断言按 `Sensitive` 保存。

```cpp
auto assertion = make_social_attribute(
    object, {"team", SocialAttributeSource::SelfReported}, verified_event_value,
    source_event, ValidityInterval{valid_from, valid_until}, recorded_at);
// 检查 assertion 后，在 Executor 管理的有限任务内提交，消费任务结果。
MemoryCandidate candidate;
candidate.proposed = assertion.value();
candidate.evidence = candidate.proposed.provenance;
auto report = MemoryConsolidator{}.consolidate_candidates(
    memory, {candidate}, candidate.proposed.scope, Timestamp::now());
// 检查报告中的处置。宿主再次确认授权与目录状态后读取。
MemoryQuery query;
query.scopes = {candidate.proposed.scope};
query.social_attribute_field = "team";
auto matching = memory.query(query);
```

同一来源事件的相同断言可重试，内容或区间发生变化时需使用新的来源事件。查询默认
取当前已记录且有效的断言，可用 `as_of_recorded` 与 `as_of_valid` 分别选择记忆知道的
时点和事实生效的时点。有效区间左闭右开。`Tombstone` 停用后，只有停用前的知悉时间
能查到历史断言。此类型无默认保留期限，宿主可设置 `expires_at`。物理擦除同时清空历史。

宿主负责同步授权撤销、目录状态和访问，通用 MemoryAgent 工作绑定拒绝画像 scope。
画像擦除接入下述配对目录。见 [DEC-124](../decisions/DEC-124-social-attribute-assertions.md)。

## 社交关系断言

宿主在同一租户和社交环境中核对两个对象的当前目录版本、授权和证据。关系有方向，
有序端点不同的断言分别保存。谓词采用与画像字段相同的 ASCII 标识符规则。

```cpp
const SocialRelationshipKey pair{from, to};
auto assertion = make_social_relationship(
    pair, {"mentors", SocialAttributeSource::UserConfirmed}, approved_statement,
    source_event, ValidityInterval{valid_from, valid_until}, recorded_at);
// 检查 assertion 后沿画像字段的宿主 consolidation 管线提交。
auto scope = social_relationship_scope(pair);
// 检查 scope，复核两个端点的权限和目录状态后查询。
MemoryQuery query;
query.scopes = {scope.value()};
query.social_relationship_predicate = "mentors";
query.as_of_valid = valid_at;
auto matching = memory.query(query);
```

关系保留各自证据，支持相同断言重试、按记录时点和有效时点查询以及停用。
按任一个对象的 `SocialObject` scope 擦除会清理它作为起点或终点的关系及历史。
若擦除仍处于 `Pending`，两端关系查询、记录 ID 读取和关系写入均要通过端点的擦除检查。
宿主排空在途操作，查询返回后再次确认两个端点可发布。
见 [DEC-125](../decisions/DEC-125-social-relationship-assertions.md)。

## 社交记忆擦除恢复

先打开可写的 `SqliteMemoryStore`，再将同一实例传给社交目录。只有配对目录成功打开后，
宿主才对外提供相关读写。原始记忆接口由宿主在当前授权和目录版本内调用。

```cpp
auto opened_memory = SqliteMemoryStore::open(executor, memory_options, artifact_store);
// 检查 opened_memory 后，由宿主持有同一个记忆库实例。
std::shared_ptr<SqliteMemoryStore> memory_store{std::move(opened_memory).value()};
auto opened_directory = SqliteSocialDirectory::open(executor, directory_options, memory_store);
// 检查 opened_directory，并停止和排空擦除范围内的访问。
auto erased = opened_directory.value()->erase_environment(authorized_environment);
```

配对目录在同一事务保存擦除意图、关闭目录身份，随后清理记忆库。对象擦除覆盖该对象
与两端关系。环境擦除按租户及环境清理全部社交记忆，也覆盖目录里无当前版本的对象。
记忆库尚未确认完成时，意图保留。完成后确认意图清除失败，重开会再次执行幂等清理。
启动恢复失败时目录不会开放。若初始事务未持久登记意图，宿主处理错误后应按原目标
提交擦除。以前以独立模式执行的擦除可由宿主使用原键补做配对擦除。

配对写入持久库 ID，随后要求同一个数据库。宿主先关闭目录，再关闭记忆库和 Executor。
恢复备份时以同一一致边界恢复两库。生产授权与撤权传播仍由宿主管理。
见 [DEC-126](../decisions/DEC-126-social-erasure-recovery.md)。

## 世界事实的新鲜度

宿主从授权范围构造稳定的租户、环境和应用键，从已提交的观察取来源事件。在一个环境或
应用范围内，使用实体 ID 加属性标识符定位观察。属性为以 ASCII 小写字母开头的 1～48
字节标识符，其余字符可含数字、下划线和连字符。观察需要明确的有效结束时间。

```cpp
auto scope = world_application_scope(principal.tenant_id, environment_id, application_id);
// 检查 scope，并使用宿主确认的来源事件和有效时间。
WorldProperty property{entity_id, "display_scale"};
auto observed = make_world_observation(scope.value(), property, value, source_event,
                                       {observed_at, expires_for_use}, recorded_at);
// 检查 observed 后，通过 MemoryConsolidator 提交并消费结果。
auto fact = memory_store->lookup_world_fact({scope.value(), property, {}, {}});
// 检查 fact。requires_observation() 为真时，当前调用方应收集新的环境证据。
```

`lookup_world_fact` 先选截至查询时点已知的最新观察，再判断是否有效。`Current` 附带
记录及事件引用，`Missing`、`Expired`、`Ambiguous` 需要重新观察且省略正文。最近一次
观察过期时，即便还有有效期更长的旧观察，结果仍为 `Expired`。重新观察后用新的来源事件
写入，即使新旧值相同也保留各自的有效期。调用方可独立指定 `as_of_valid` 和
`as_of_recorded`，历史读取仍受保留期限约束。环境事实和应用事实分别使用各自的精确
scope，擦除按所提交的 scope 执行。

结构化观察通过宿主专用入口读取。宿主仍负责当前授权、观察和动作验证。
见 [DEC-127](../decisions/DEC-127-world-fact-address-and-freshness.md)。

## 导航重新观察

在 `set_navigation_context` 安装 AppModel 和读屏入口后，可使用
`WorkflowRuntime::set_navigation_world_facts` 安装导航专用绑定。读和写使用同一个由
`world_application_model_scope` 构造的应用 scope。传入宿主安装的 AppModel 摘要，
运行中演化的置信度投影可通过 `app_model_snapshot()` 单独检查。

```cpp
const auto digest = app_model_digest(model);
auto scope = world_application_model_scope(tenant, environment, application, digest);
// 检查 scope，并按它提交后续的已核准观察。
NavigationWorldFacts binding;
binding.tenant = tenant;
binding.environment = environment;
binding.application = application;
binding.app_id = model.app_id;
binding.screen_state = {entity, "screen_state"};
binding.observation_request.required.structure = true;
binding.authorized = check_host_authorization;
binding.lookup = lookup_authorized_world_fact;
binding.recognize = identify_screen_from_observation;
auto installed = workflow.set_navigation_world_facts(std::move(binding));
```

只在当前读屏快照与事实一致且时间落在有效区间内时复用该快照。其他情况下由现有
`IEnvironment::observe` 请求新观察，再把观察交给识别器。识别器使用本次传入的观察，
并尊重操作截止和取消。查询回调可使用 `SqliteMemoryStore::lookup_world_fact`。
授权探针会在派发工作进入工具注册表时复核。AppModel 重新安装
会废止旧计划；已接纳动作的结果也经过世代复核后才用于导航。见
[DEC-128](../decisions/DEC-128-navigation-world-fact-refresh.md)。

## 目标与承诺

`SqliteGoalStore` 保存一次性任务和承诺，宿主传入已授权的租户、用户和稳定目标 ID。
`GoalDefinition` 保存标题、来源事件和创建时间，承诺需要 `due_at`。所有时点使用 UTC
毫秒。`expires_at` 可选，若指定则须晚于截止时间。普通任务可不设截止时间。

```cpp
const GoalOwner owner{principal.tenant_id, principal.user_id};
const GoalKey key{owner, internal_goal_id};
auto started = goals->create(key, {GoalKind::Commitment, title, source_event,
                                  created_at, due_at, expires_at});
auto pending = goals->list({owner, GoalState::Pending, 32, {}});
auto due = goals->due({owner, cutoff, 32, {}});
// 检查 started 后，以已提交的结算事件推进版本。
auto settled = goals->settle(key, started.value().revision,
                            {GoalState::Fulfilled, resolution_event, fulfilled_at});
```

`list` 按稳定 ID 分页，也能取得未指定 `due_at` 的任务和历史终态。`due` 按截止时间和
ID 分页，返回截至指定时点已到期、仍为 Pending 且未超过 `expires_at` 的目标。两者
翻页时沿用原归属和筛选条件，页之间可观察到新的写入和结算。`Expired` 需要宿主通过
证据事件显式结算。过期的定义也可在 `list` 中看到原状态与 `expires_at`。

重试以原稳定 ID 及原内容创建，以原版本与原证据结算。擦除后仍可查询 `Erased` 状态，
正文与事件引用已清理，旧 ID 不能再创建。全库配额计入删除屏障，每主体的 Pending 项
独立限额。宿主在 Executor 关闭前关闭目标库。到期查询产生待处理候选，执行授权沿
宿主既有策略。见 [DEC-129](../decisions/DEC-129-durable-goals-and-commitments.md)。

## 宿主身份

宿主必须保存稳定的内部 Principal/User/Session 身份，按当前授权构造 scope。不要在每次
启动时生成新的 user ID，也不要把 QQ 号码或群号直接当授权。真实平台的鉴权、路由和
账号映射由 Adapter/宿主处理，Core 不依赖 QQ SDK。

## 离线运行与独立进程恢复

[stateful_agent_consumer.cpp](../../examples/stateful_agent_consumer.cpp)使用公开接口，
不调用真实模型。固定 ID 仅是示例夹具，不能复制为生产用户身份。

```bash
cmake --preset debug
cmake --build build/debug --target mira_stateful_consumer -j 4
build/debug/mira_stateful_consumer seed /tmp/mira-durable-demo
build/debug/mira_stateful_consumer inspect /tmp/mira-durable-demo
ctest --test-dir build/debug -R '^mira_stateful_consumer(_restart)?_test$' --output-on-failure
```

`seed` 先向 FileEventStore 写入已核验事实，再读回真实 envelope 进行 consolidation，
最后由 ContextMemorySupervisor 调度任务 checkpoint。`inspect` 在另一进程中打开同一
目录，不提取、不重新写入事实、不调用模型，验证：

- SQLite FTS5 的 `locale` 查询命中 `locale=de-DE`，报告 `fts_leg_ran`。
- 不相关词和另一个 scope 返回空结果。
- SQLite checkpoint 保留 goal 与已核验事实，事件全量重建的 projection digest 一致。
- 数据库和 `events.log` 继续存在，重复 `seed` 不增加同 claim 的记忆。

`mira_stateful_consumer_restart_test`通过四个独立进程依次执行 seed/inspect/seed/inspect。
它证明正常关闭后的跨进程恢复与重复投影，不代表已验证强杀、断电或真实模型正确率。
无参数模式使用并清理独占临时目录；传入目录（包括单参数兼容模式）会保留数据，
由调用方管理。示例 inspect 不执行 projection repair，但数据库 open 仍可能创建文件或
执行显式 schema 迁移，不是取证用的只读工具。

## 服务接线与运行诊断

交互宿主可以把同一个 SQLite `IMemory` 和持久 EventStore 注入 `MemoryAgentService`，
再交给 Coordinator/Gateway 使用，见
[memory_agent_host_consumer.cpp](../../examples/memory_agent_host_consumer.cpp)。该例演示
任务 scope 与纠正失效；其进程内状态和临时目录并不构成部署恢复方案。

区分三种操作：

1. 原始聊天输入先写事件；机器人说过的话不是自动 Verified 的事实。
2. 记忆提取只产候选，走既有 consolidation/promotion；模型提案为 Unverified，
   Preference 走持久提案与显式审批。
3. 召回先做授权 scope 选择，再检索，作为 ContextItem 候选交 Layer 0。不要把全部
   event/record 文本拼到 system prompt，也不要让记忆服务自行决定最终准入。

启动诊断宜输出 backend 类型、schema/journal mode、是否只读、稳定配置标识和队列上限；
查询输出命中数、实际运行的检索腿、index lag、降级/deadline 和 token 估算。当前可读取
`diagnostics()`、`pending_requests()` 和 `MemoryQueryResult::quality`；不要输出正文、
账号、凭据或完整敏感路径。不提供 embedding 时，FTS/exact 可以独立工作。

## 后台维护、关闭和备份

embedding 是可重建投影；由宿主通过 Executor 驱动有界 repair，不接记忆写入热路径。
retention sweep 是有界 API，不等于已经有自动周期清扫宿主；自动清扫和跨层容量治理
由 M31 继续交付。没有命中时返回空结果，不以整库倒出作为兜底。

关闭顺序为停止交互/维护生产者，取消或收拢记忆服务任务，消费有限任务结果，关闭 SQLite
writer，最后由非 worker 的外部 owner 调用 `executor.shutdown(true)`。不能在 worker 中
析构整个生命周期，也不能只等入队成功就宣称记忆写入成功。

备份需要一致的事件、数据库和 artifact 引用边界。使用 SQLite 支持的备份能力或停止
写入并关闭 store 后复制；运行时不能只拷贝 `.db` 忽略 WAL。跨 store 没有分布式事务，
需要记录各层已处理水位并验证恢复。SQLite 的逻辑删除不等于磁盘字节已回收，隐私删除、
checkpoint/WAL、vacuum、备份保留和索引重建屏障必须分别结算。

会话资产持久索引、旧 JSON 导入、事件后端选择以及规模基准仍在维护计划中；这个示例
不提供自动迁移，更不把任务 checkpoint 等同于全部 Warm 层持久化。
