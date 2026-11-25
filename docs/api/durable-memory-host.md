# 持久记忆宿主接入

> 状态：Active
> 更新日期：2026-11-25
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

召回包含原生角色、来源事件和片段水位。全文索引以 scope 词项收窄：查询文本按
[确定性分词](context-memory.md#text_segmentationhpp确定性文本原语)切成词、汉字单字和相邻两字，逐个加引号后以 OR 匹配，
片段按相关度（bm25）从高到低返回，同分按来源序号倒序和 ID；不带文本的查询仍按
最近片段序号及 ID 排序。查询超过 128 个单元时只匹配其中一个子集，结果以
`degraded` 和 `note` 说明。`lexical_score` 是 0 到 1 之间的相关度，索引不给权重的
匹配为 0。声明的 exact_terms 都必须逐字命中，无论是否有片段包含它们。角色转换及
最终准入使用现有 `context_items_from_candidate` 和 Gateway。重建触发、授权复核与
发布候选由宿主管理。

将同一个索引赋给 `MemoryAgentConfig::conversation_index`，先初始化 MemoryAgent
重放持久擦除意图，再对外开放索引。会话擦除会持久封闭索引 scope，旧日志重建被拒。
索引擦除失败时当前实例封闭读取与重建。宿主处理失败原因，关闭服务和索引，重开后
重放擦除意图。原始事件、备份与磁盘回收由各自生命周期管理。
参见 [DEC-122](../decisions/DEC-122-durable-conversation-index.md)；分词与排序的
修订见 [DEC-147](../decisions/DEC-147-lexical-segmentation-and-query-composition.md)。片段表自 schema 1.1 起带索引文本列，1.0 的索引文件在打开时
原地重建，水位与擦除屏障保持有效，历史会话无需宿主重建即可继续检索。

## 社交对象目录

`SqliteSocialDirectory` 保存宿主核准的对象身份和别名。数据库由宿主独占并在
Executor 关闭前排空。调用前从当前授权绑定确定租户和社交环境。对象键包含
完整环境键及稳定内部 ID。宿主把认证来源账号转换为不透明 ID，目录在环境内持久解析。

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

### 按来源账号定位交谈对象

已持久化消息到达时，宿主从已认证的信封读取来源命名空间和稳定账号 ID。命名空间
使用 1 到 64 个 ASCII 小写字母、数字、点、短横线或下划线。每个来源的账号 ID 由宿主
经过稳定映射后作为非空 `Id128` 传入，账号显示名仍使用别名入口。

```cpp
const SocialAccountKey sender{message_source, opaque_sender_id};
auto participant = directory->observe_account(environment, sender, committed_message_id);
auto exact = directory->resolve_account(environment, sender);
auto team = directory->recall_account_attribute(environment, sender, "team");
```

首次观察原子创建内部对象和账号索引，重复接纳返回原绑定和首个来源事件。宿主已有
可靠绑定证据时，可将同环境内的对象作为 `observe_account` 的第四个参数，将新账号
绑定到该对象。已有其他绑定时返回 `AlreadyExists`。读取画像字段需要配对的记忆库，
按当前账号解析出的精确对象 scope 查询，返回前复核账号绑定与对象版本。

账号容量 `max_accounts` 默认为 16384，包含被擦除对象留下的账号屏障。对象擦除清除
其绑定和事件引用，旧消息重试被拒。擦除环境清除该环境全部账号索引，重放由环境屏障
拦截。消息网关负责来源验证与授权，自动身份推断与已建对象归并属于 M30-36。
见 [DEC-140](../decisions/DEC-140-source-account-identity.md)。

### 索引社交消息证据

宿主在入站消息事件持久化后，从认证信封取得环境、会话和发送账号，用事件回执构造
`SocialMessageLocator`。在 Executor 管理的有限任务中调用 `index_message` 并消费结果。
目录从指定 EventStore 的精确位置核对会话、序号、事件 ID 和摘要，再原子提交账号接纳
和发言证据。对象、来源账号摘要、事件位置和内容完整性摘要均可用于后续引用。

```cpp
const SocialMessageLocator message{authorized_session, committed.session_sequence,
                                   committed.event_id};
auto indexed = executor.submit_auto([&] {
    return directory->index_message(environment, sender, message, *events);
}).get();
auto page = directory->read_message_evidence(environment, authorized_session, 0,
                                             committed.session_sequence, 16);
```

读证据按 `(after, through]` 的会话序号顺序返回，`has_more` 为真时可以用最后一条
证据的序号继续。历史消息补录时重查相应区间。重放要使用相同来源账号和已保存事件。
事件内容、账号或会话位置发生冲突时返回 `AlreadyExists`。会话授权与来源认证由宿主
决定。消息正文保留在原 EventStore 中，证据索引只引用位置和摘要。

`max_messages` 默认 65536，含擦除屏障。对象擦除会清理发言证据并阻止已擦除事件重新
归因。环境擦除清理全部相关索引，由环境屏障拦截旧事件。见
[DEC-142](../decisions/DEC-142-social-message-evidence.md)。

### 解析当前发言者和引用对象

宿主按已授权会话调用 `resolve_message_participants`，并传入该消息的结构化提及、引用
位置和正文提取出的别名。结果中的 `speaker` 对应当前消息。`mentions` 与输入顺序一致，
每次最多 16 条。

```cpp
const std::vector<SocialMentionReference> mentions{
    authenticated_tagged_account, earlier_message, std::string{"陈工"}};
auto participants = directory->resolve_message_participants(environment, message, mentions);
```

显式提及返回 `SocialAccountBinding`，引用更早的已索引消息返回 `SocialMessageEvidence`。
两种身份均保留已记录的证据，缺失或已擦除的引用返回 `std::monostate`。引用必须在同一
会话中，且序号早于当前消息。文本别名返回 `SocialAliasResult`，即使只有一个候选，
结果类型也保留别名来源。同名候选被截断时仍返回 `Ambiguous`。见
[DEC-143](../decisions/DEC-143-social-message-references.md)。

### 自动提取正文中的对象引用

入站适配器可在完成 `index_message` 后调用 `infer_social_message_mentions`。
`kSocialMessageTextEvent` 标识正文为纯文本的事件。宿主选择获准处理该会话的模型，
并传入本次消息的 `OperationContext`。入口从同一会话取得当前消息和前六个事件位置
中已有索引的消息，每条正文最多 4096 字节。推断复用同一取消请求和截止时间。

```cpp
auto inferred = executor.submit_auto([&] {
    return infer_social_message_mentions(*directory, *events, *provider, environment,
                                         message, operation);
}).get();
```

每个候选保留当前正文原文、可选的前序发言位置和由目录返回的解析结果。同名仍返回
歧义，前序行号表示模型推断，并附对应消息证据。调用失败、未获准的会话、无效行号、
伪造原文、取消及来源被擦除均返回错误。模型请求没有工具，关闭存储和上传。
`extract_social_mentions` 也可独立用于宿主已准备好的文本和本地发言者代号。
见 [DEC-144](../decisions/DEC-144-social-mention-extraction.md)。

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

### 纠正与提醒回执

`SqliteGoalStore` schema 1.2 支持纠正 Pending 目标。宿主保留目标类型与创建时间，以
新来源事件提交标题、截止时间和过期时间。成功后版本递增，到期页反映新期限；可用
`correction_history` 读取先前的定义及其来源事件。

```cpp
auto corrected = goals->correct(key, current_revision, revised_definition);
auto previous = goals->correction_history(key);
// 检查 corrected，并在新版本的提醒送达后登记。
auto receipt = goals->record_reminder(key, corrected.value().revision,
                                      {delivery_event, delivered_at});
```

`record_reminder` 登记已经送达的提醒。调用方传入送达时目标的版本、证据和送达时间。
相同回执重试保持幂等，已存在的证据若带不同时间或版本则拒绝。`GoalRecord.reminder_count`
来自已登记的回执，纠正后继续计数。已结算目标可重试已有回执，不能登记新的回执。
擦除目标会一并清理定义历史、提醒回执和计数。见 [DEC-131](../decisions/DEC-131-goal-corrections-and-reminder-receipts.md)。

### 一次性目标的派发回执

目标库 schema 1.4 提供 `due_for_dispatch`，以同样的截止时间和 ID 规则分页。已有提交
回执、准备记录属于旧版本，以及取消、兑现或已经过期的目标从待派发查询中排除。
`prepare_dispatch` 在事务中复核目标版本和期限，并持久分配 `key.operation`。同版本
重试复用这个 ID，宿主可在提交结果不明或重启时用它对账。

```cpp
auto pending_dispatch = goals->due_for_dispatch({owner, cutoff, 32, {}});
// 从已检查的查询结果中取出 due_goal。
auto prepared = goals->prepare_dispatch(due_goal.key, due_goal.revision, cutoff);
// 提交端确认任务和会话归属后登记原回执。
auto submitted = goals->record_dispatch_submission(prepared.value().key,
                                                    {session_id, task_id, submitted_at});
```

`read_dispatch` 可查询原准备时点和已记录的任务归属。相同回执可重放，已记录的会话、
任务或时间冲突时返回 `AlreadyExists`。已取消或已纠正的目标仍可补记先前实际提交的
任务。擦除时清除准备记录和提交回执。承诺兑现由独立结算事件推进。

`open_authorized` 分别用 `ReadDispatchDue`、`PrepareDispatch`、`ReadDispatch` 和
`RecordDispatchSubmission` 核对授权。集合查询依归属授权，其他操作带目标 ID。
实际任务提交由协调层接线负责。见 [DEC-145](../decisions/DEC-145-durable-goal-dispatch-receipts.md)。

宿主直接承接任务提交时，可通过 `prepare_dispatch_attempt` 或
`prepare_occurrence_dispatch_attempt` 同时取得准备回执和 `newly_prepared`。只有首次插入
准备记录的调用得到 `true`。重开或并发重试时通过回执中的同一操作 ID 查询已提交任务。
见 [DEC-152](../decisions/DEC-152-goal-submission-reconciliation.md)。

### 按任务完成证据兑现一次性目标

任务实际进入无错误的 `Completed` 终态后，宿主通过协调层保存完成证据。该事件和目标
派发回执共同确定哪一个任务完成了原版本的目标。

```cpp
auto evidence = coordinator.record_verified_task_completion(principal, task_id);
// 确认 evidence 成功，或在重启后从同一持久事件库补跑结算。
auto fulfilled = goals->fulfill_dispatch(dispatch_key, task_events);
```

`fulfill_dispatch` 按回执中的会话和任务读取唯一完成证据，并检查证据归属、持久化及完成
时间。已授权的重试返回原结算。目标被纠正、取消、擦除，或完成发生在过期时间之后时，
原派发无法覆盖当前状态。授权型目标库分别核对 `ReadDispatch` 和 `Settle`。
见 [DEC-149](../decisions/DEC-149-verified-goal-fulfillment.md)。

### 周期检查到期目标和计划

`GoalDispatchPoller` 通过宿主的 Executor 定时检查一个已授权归属的目标。每轮最多读取
一页待派发目标、一页重复计划和一页待结算目标，各自沿索引继续，轮换处理顺序。
上一轮结束后才登记下一轮。`status()` 提供检查、提交、兑现和失败总计及最近错误。

```cpp
auto poller = GoalDispatchPoller::start(executor, shared_goals, owner, submit_goal_task,
                                        {}, submit_recurring_task);
// 关闭执行器或存储前，从外部 owner 关闭派发器并检查结果。
auto drained = poller.value()->close();
```

`submit_goal_task` 是宿主的 `GoalDispatchSubmitter`，须按 `GoalDispatchKey::operation`
核对和去重任务提交。已受理的重复调用返回原任务、会话和时间，并在常驻授权策略下
复核是否可以执行。该接口接收协作取消和截止时间。确认提交后，即使当轮正在关闭，
派发器也会登记回执。`submit_recurring_task` 接收 `RecurringPlanRecord`、
`GoalOccurrenceDispatchKey` 与同样的 `OperationContext`，沿用稳定操作 ID 的对账和
当前授权规约。宿主可只绑定其中一个提交入口。未知结果保留稳定身份供后续检查，未就绪的
触发仍占用原到期位置。生产协调层的持久对账适配由 M30-37 跟踪。见
[DEC-146](../decisions/DEC-146-goal-dispatch-polling.md) 与 [DEC-148](../decisions/DEC-148-recurring-goal-dispatch.md)。

#### 按原操作 ID 对账

宿主可按通道启用只读对账，复用现有的提交回调处理首次准备的操作。

```cpp
GoalDispatchPollerOptions options;
options.reconcile_submission = find_goal_submission;
options.reconcile_recurring_submission = find_occurrence_submission;
auto poller = GoalDispatchPoller::start(executor, shared_goals, owner, submit_goal_task,
                                        options, submit_recurring_task);
```

每个查询回调按原操作 ID 查询宿主的持久准入结果，返回已受理的原会话、任务和时间，或
返回空值继续等待。已有准备身份的派发只走查询回调。`status().unresolved_reconciliations`
累加未查到回执的次数，派发器继续检查后续目标。离开到期集合的历史回执仍可显式登记。
未配置查询回调的通道由提交回调按原操作 ID 去重。见
[DEC-152](../decisions/DEC-152-goal-submission-reconciliation.md)。

宿主可用[协调层任务准入回执](interaction-coordinator.md#持久任务准入回执)实现查询回调。
提交任务时把 `key.operation` 带入 `TaskSpec::admission_operation`。宿主可使用
[发布前持久化准入](interaction-coordinator.md#发布前持久化准入)让 Runtime 在确认任务前保存回执，
或在既有路径的命令生效后显式登记。对账取得回执后，将 `session`、`task` 和以 UTC 毫秒
表示的 `admitted_at` 映射为 `GoalDispatchSubmission`。

#### 自动核对完成证据

在 `GoalDispatchPollerOptions::completion_events` 绑定受信的任务事件库即可周期核对已派发
目标。仅启用结算时，两个提交回调均可为空。待结算查询 `pending_fulfillments` 按目标 ID
分页，只返回仍为 Pending 且当前版本有提交回执的身份。没有完成事件时该目标留待后续
检查。即使停机期间过了目标的过期时间，之前实际完成的任务也可以补结算。

```cpp
GoalDispatchPollerOptions options;
options.completion_events = shared_task_events;
auto poller = GoalDispatchPoller::start(executor, shared_goals, owner, {}, options);
```

授权型目标库以 `ReadPendingFulfillments` 控制归属内的分页发现，以 `ReadDispatch` 和
`Settle` 控制每条结算。使用 `reconcile_dispatch_completion` 手动核对时，空值表示
尚无提交回执或完成证据。事件读取和持久化的其他失败返回原错误。宿主从外部关闭派发器
后再关闭事件库、目标库和 Executor。见 [DEC-150](../decisions/DEC-150-goal-fulfillment-polling.md)。

任务还在当前 Runtime 时，宿主可用 `GoalDispatchPollerOptions::publish_completion` 自动
补齐证据。派发器先查持久证据，再在需要时调用宿主。回调使用回执的任务、会话和操作 ID，
从当前宿主认证上下文取得归属主体，与协调层共用 `completion_events`。回调须遵守截止
时间和取消，并在同一 Executor 的工作线程内有界返回。

```cpp
options.publish_completion = [&] (const GoalDispatchKey &key,
                                  const GoalDispatchSubmission &submission,
                                  const OperationContext &context) -> Result<bool> {
    auto principal = current_principal(key.goal.owner);
    if (!principal)
        return principal.error();
    return coordinator.publish_verified_task_completion_if_ready(
        principal.value(), submission.session, submission.task, context);
};
```

回调返回 `true` 后，派发器再从事件库验证证据并结算。协调层忙时返回可重试的背压，
后续分页继续核对。已经失去 live 快照的任务只能依靠先前持久化的完成证据补结算。
见 [DEC-151](../decisions/DEC-151-live-goal-completion-publication.md)。

## 重复计划与触发回执

`SqliteGoalStore` schema 1.1 为固定 UTC 间隔的重复计划保存独立命名空间。计划的
首次触发时间、间隔和来源事件由当前授权的宿主确定。间隔为 1 秒至 365 天。
`due_plans` 按计划的下一次触发时间和 ID 分页，调用方沿用同一归属与截止时点。

```cpp
const GoalKey key{owner, internal_plan_id};
auto created = goals->create_plan(key, {title, source_event, first_at, interval});
auto due = goals->due_plans({owner, cutoff, 32, {}});
// 核对授权与 due 结果，再登记其中计划的本次触发。
auto receipt = goals->record_occurrence({key, active_revision, scheduled_at}, recorded_at);
```

登记一次只推进一个预定时间。重启后可用相同 `GoalOccurrenceKey` 查询或重试，回执保留
首次登记时间。回执与下次到期时间同事务写入。若有多次积压，宿主按已登记后的新时间
决定何时处理下一次。回执额度达到上限时，登记返回错误并保留原到期时间。

使用 `replace_plan` 和 `cancel_plan` 提交期望版本。换版从新定义的首次触发时间开始，
取消移除到期候选。两种操作都会拦截旧版未登记的触发，已有回执仍可查证。`erase_plan`
清理计划定义与回执，保留 ID 屏障。同 ID 的一次性任务使用单独的命名空间。回执供宿主
判断去重；派发前仍须核对现行授权和计划版本。见 [DEC-130](../decisions/DEC-130-durable-recurring-goal-plans.md)。

### 重复计划派发回执

目标库 schema 1.5 在触发记录上保存准备身份和任务提交回执。`prepare_occurrence_dispatch`
需要当前计划版本的下一次到期触发，准备成功后占用一个触发额度，原到期位置保持待处理。
触发仍待提交时，再次准备会返回原操作 ID。`read_occurrence_dispatch` 查询准备记录及已确认的
任务归属，早期版本的手动回执仍通过 `read_occurrence` 查询。

```cpp
auto prepared = goals->prepare_occurrence_dispatch({key, active_revision, scheduled_at}, cutoff);
// 宿主按 prepared.value().key.operation 对账，确认准入后保存原提交信息。
auto saved = goals->record_occurrence_submission(prepared.value().key,
                                                {session_id, task_id, submitted_at});
```

回执和下一次时间在同一事务写入。重复提交相同回执可重放，冲突返回 `AlreadyExists`。
换版或取消会关闭对旧触发的准备，宿主仍可补记已确认的历史提交，并保留当前版本的计划
进度。擦除会清理准备身份和已登记回执。`open_authorized` 在 `RecurringPlan` 资源上
分别核对 `PrepareDispatch`、`ReadDispatch` 和 `RecordDispatchSubmission`。
见 [DEC-148](../decisions/DEC-148-recurring-goal-dispatch.md)。

### 日历及时区

目标库 schema 1.3 支持按本地时分和星期运行的计划。`RecurringPlanDefinition` 指定
`calendar` 时，`interval` 为零。`first_at` 是 UTC 下界。`GoalCalendarRule` 使用本地
午夜后的分钟数，`weekdays` 的最低位代表周一。宿主提供时区标识、规则版本、初始偏移和
按 UTC 排序的偏移转换；至少覆盖首次触发下界之前的两天以及计划需要的未来范围。
单个快照最多 1830 天和 128 个转换。

```cpp
RecurringPlanDefinition definition;
definition.title = title;
definition.source_event = source_event;
definition.first_at = first_allowed_utc;
definition.calendar = authorized_time_zone_rules;
auto created = goals->create_plan(key, std::move(definition));
```

本地时刻不存在时跳过这一日期，重复时刻选较早的 UTC 时间。重开沿用持久规则和已登记
回执。规则覆盖耗尽后，`read_plan` 返回 Active 和空的 `next_at`，到期页省略该计划。
宿主可用新的来源事件和快照换版，恢复将来的触发。见 [DEC-132](../decisions/DEC-132-calendar-goal-plans.md)。

### 目标存储授权

宿主从认证主体构造 `GoalOwner`，并可用 `SqliteGoalStore::open_authorized` 接入当前策略。
目标归属、资源类型、操作与单条记录 ID 随 `GoalStoreAccess` 传给宿主。状态列表用
`List`，到期页用 `ReadDue`。两种集合操作的 ID 为空，按相应归属与资源类型授权。

```cpp
// check_current_goal_access 由宿主提供，签名为 bool(const GoalStoreAccess &)。
auto goals = SqliteGoalStore::open_authorized(executor, options, check_current_goal_access);
```

探针在 StoreChannel worker 开始访问数据库前运行，需要有界、线程安全且不得重入目标库。
宿主让探针捕获的策略存活至目标库关闭。拒绝和探针异常均返回 `PermissionDenied`。
下一页和幂等重试均复核当前权限。已获准的单次存储操作可以结束，后续动作派发与提醒
送达仍由宿主重新授权。普通 `open` 由调用方在外层执行当前授权。
见 [DEC-133](../decisions/DEC-133-goal-store-live-authorization.md)。

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
