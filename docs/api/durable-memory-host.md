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

### 有界 SQLite 文件（DEC-111）

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
