# 记忆存储与检索方案核查

> 状态：Completed（静态核查与方案研究；实现进度另计）
> 日期：2026-11-10
> 负责人：Mira Maintainers
> 基线：`1bc162e`；关联工作：[记忆存储维护](../plans/maintenance-2026-11-memory-storage.md)

## 1. 结论

Mira 已有 SQLite 长期记忆和任务 checkpoint，缺口主要在宿主装配、会话资产索引的
持久实现和长期容量治理。用户所见 `event.json` 来自仓库外的 QQ 临时宿主；其代码、
schema 与运行配置未提供，无法据文件名判断是否执行过记忆提取或 SQLite 写入。
本报告不代表已检查或迁移该部署。

优先复用 `SqliteMemoryStore`，把事件、任务状态、长期知识和召回投影分别接线。
SQLite 内保存部分 JSON 快照是合理做法：关键是 ACL、身份、水位与检索字段可索引，
不能把整份聊天 JSON 读入后做无界扫描。换文件格式不会自动获得记忆能力。

## 2. Mira 当前实现

| 数据职责 | 当前能力与代码 | 限制 / 后续工作 |
| --- | --- | --- |
| Cold 事件事实源 | [FileEventStore](../../src/storage/event_store.cpp)，`events.log`；有序、幂等、摘要、恢复 | 恢复载入 MemoryEventStore，默认 10万条；没有 SqliteEventStore；分段/索引与容量归 M31 |
| 长期知识 | [SqliteMemoryStore](../../src/storage/sqlite_memory_store.cpp)，WAL、records/versions/provenance/mutations/holds/erasure | 宿主必须实例化并注入；文件存在不代表有调用 consolidation |
| 关键词与向量 | 同库 FTS5、embedding BLOB、外部供给与 repair | 余弦是有界扫描，无生产 embedding 实现或 ANN；中文分词质量未验证 |
| 任务恢复 | [SqliteCheckpointStore](../../src/storage/sqlite_checkpoint_store.cpp)，水位与投影摘要 | 任务 checkpoint 不等于 Conversation checkpoint 或 Working Context 的持久后端 |
| 会话段/流程回合/恢复教训资产 | [InMemoryContextIndex](../../src/context/context_retrieval.cpp)，来源事件与确定性 ID | 仅进程内；重启要重建，没有对应 SQLite 资产索引 |
| 会话 checkpoint / Working Context | [consolidation](../../include/mira/context_consolidation.hpp)、[snapshot](../../include/mira/context_working_context.hpp) 契约及内存参考 store | 不可因为有任务 SQLite checkpoint 就声称所有 Warm 状态已持久 |
| 记忆服务与压缩 | [MemoryAgentService](../../include/mira/memory_agent.hpp)，IMemory 注入、Artifact 引用、删除屏障 | 宿主仍负责持久 artifact/event/memory 的装配、稳定身份和关闭；没有通用 Layer 4 压缩器 |

两个应分别处理的查询问题：

1. 本轮修复：FTS 的全库 `LIMIT 512` 与向量全库 `max_vector_scan` 先于候选求交，
   其他 scope 或已排除的 claim/敏感内容会占用额度。改为先对已过滤候选 ID 做成员过滤，
   再消耗各腿额度。没有扩大 ACL，也没有把未命中降级成整库上下文。
2. 仍有边界：当前先取 scope 内最新 4096 条，再处理后续过滤和召回，旧但相关的知识
   可能不在候选池；as-of 记录解析后，FTS/vector 仍不是按历史版本构建的文本/向量索引。
   这需要后续重构候选生成与历史索引契约，不能把本轮修复称为全历史精确召回。

## 3. 外部项目的一手证据

全部来源于项目自己的文档或源码，访问日期为 2026-11-10；`main` 和网站内容可变化。
未安装这些服务、运行其模型或复现其基准。下列“采用”是 Mira 侧建议，不代表原项目
提供相同权限、可靠性或删除保证。

### Mem0：修改历史与召回存储各有职责

[配置源码](https://github.com/mem0ai/mem0/blob/main/mem0/configs/base.py)将 vector store、
embedder 和 `history_db_path` 分开；[SQLiteManager](https://github.com/mem0ai/mem0/blob/main/mem0/memory/storage.py)
保存记忆修改历史及消息窗口。由此不能得出“Mem0 把所有记忆搜索都放进 SQLite”。

采用：区分修改台账、记录和索引，更新/删除有可追踪身份。Mira 已有 mutation 与版本链，
优先补接线和一致性测试，不复制另一个记忆写入器；历史审计也必须纳入隐私删除范围。

### LangGraph / LangMem：会话恢复与跨会话记忆分开

[LangGraph Memory](https://github.com/langchain-ai/docs/blob/main/src/oss/concepts/memory.mdx)
区分 thread checkpointer 与跨 thread namespace store；
[LangMem](https://github.com/langchain-ai/langmem)提供独立于后端的提取原语、前台记忆工具和
后台管理器。框架的存储接口与一个临时内存实现不是同一部署保证。

采用：分别注入任务状态与长期知识 store，前台仅取有界结果；后台提取走既有
Executor 生命周期。namespace 是数据组织，Mira 必须另做 Principal、grant、scope 校验。

### Letta：当前产品采用可检查的分层文件记忆

旧 [letta 仓库](https://github.com/letta-ai/letta)已指向 letta-code；当前
[Memory & dreaming](https://docs.letta.com/configuration/memory)说明 MemFS 是 Git 支持的
记忆文件系统，后台 dreaming 回顾会话并更新记忆，doctor 检查重复、组织和 prompt 占用。
不能把早期 MemGPT 的部署方式当作当前 Letta 数据库架构。

采用：结构化、可检查、能回滚的记忆视图和后台整理触发。Mira 中模型只提候选，
不能仿照直接编辑记忆文件的方式绕开 consolidation、审批或 Layer 0。

### OpenViking：先定位摘要，再按需取详情

[Context Layers](https://github.com/volcengine/OpenViking/blob/main/docs/en/concepts/03-context-layers.md)
采用目录级 abstract/overview 和原始详情，默认生成预算为 256/4000 字符；
[Retrieval](https://github.com/volcengine/OpenViking/blob/main/docs/en/api/06-retrieval.md)
区分直接 find、带会话意图的 search，以及有预算的上下文装配。

采用：摘要/目录导航/详情引用分开，详情按需读取，摘要携带来源与更新水位。
它的 L0/L1/L2 是内容表示层级，不对应 Mira 的确定性 Layer 0–4；摘要不得成为授权。

### Graphiti：来源和时间是检索条件

[官方仓库](https://github.com/getzep/graphiti)将 episodes 作为来源，实体关系带有效时间，
混合语义、关键词与图遍历，并要求用户运维相应图数据库。

采用：保留来源、记录时间与有效时间，旧事实变更不能简单覆盖全部历史。Mira 已有
双时态与 claim 链；先完善查询和恢复，关系图仅作为将来可重建的可选投影，不引入
设备端必须运维的图数据库，不采用厂商性能数字作为验收结果。

## 4. 推荐的数据布局与写读协议

物理文件与语义平面是两个维度。Hot 是本轮执行集；Warm 包含 checkpoint、Working
Context 和召回候选；Cold 是事实与工件。长期知识是跨会话投影，不是整个事件文件。

| 宿主数据 | 推荐存储 | 准入与恢复 |
| --- | --- | --- |
| 原始已接纳交互、核验结果、状态与控制事实 | 当前 FileEventStore；后续评审索引/分段或 SQLite 实现 | 先提交事实，保留序列与 idempotency，按 session 分页 |
| 任务 checkpoint | 当前 SqliteCheckpointStore | 校验水位与 projection digest，损坏从事件重建 |
| 跨任务事实/偏好/经验 | 当前 SqliteMemoryStore | 只经 consolidation/promotion，User 偏好需显式审批 |
| 会话段、摘要和恢复资产索引 | 后续 SqliteContextIndex（尚未实现） | 确定性 ID、来源序列、ACL、水位、FTS；失效只影响召回 |
| 截图、压缩正文、审批提案等大载荷 | FileArtifactStore | 引用/摘要；擦除屏障覆盖派生正文 |
| 本轮请求 | 内存中的 ContextItem 候选 | Gateway / StandardContextManager 最终预算准入 |

写入必须是“事件先提交 → 受管投影工作 → 记录与本库索引事务提交”。跨 store 不宣称
原子事务：崩溃后靠幂等键与已处理水位补投影，不做应用层盲目双写。后台索引追不上时
报告 lag，允许 exact/FTS 降级；不能为等 embedding 阻塞主交互，也不能默默加载全历史。

读路径为“身份/有效 grant → scope 白名单 → 索引候选 → 细节读取 → 重排 → Layer 0”。
每次请求只带有界选集，事件库与数据库文件本身不进入 prompt；记忆命中不是事实核验。
变更、删除和权限撤回后使缓存与在途结果失效。索引重建须先应用删除屏障，避免把被删
记忆从旧事件或摘要重新提取回来。

## 5. 下一步的具体选择

按[维护计划](../plans/maintenance-2026-11-memory-storage.md)分步交付：

1. 先修召回预算，并用[持久接入示例](../api/durable-memory-host.md)验证真实 SQLite 查询
   和独立进程恢复。诊断只记录 backend、schema、队列、计数和降级原因，不记录记忆正文。
2. 再实现持久会话资产索引：SQL 列保存 owner/kind/来源水位/digest，正文/turns 作为
   有界 payload，FTS 与资产更新同库事务。记录处理水位与 index profile，使重建可幂等。
   不能只把一个大 JSON 存进 SQLite，然后启动时全量灌回 InMemoryContextIndex。
3. 事件后端选择单独冻结：SQLite 表可索引 `(session_id, sequence)`、`event_id` 和
   task 序列，文件分段可沿用现有日志协议。两者都必须保持完整性、幂等、时间来源、
   ack durability 和恢复语义；M31 的容量、归档与擦除协议仍必需。当前不做未经论证的
   全量存储替换。
4. 导入旧 JSON 前需要旧 schema 和稳定身份：提供 dry-run、版本标识、确定性 event ID、
   分批事务、水位、计数/摘要对账、重跑幂等和回退。先只读保留原件，不能因文件名相同
   猜测字段，更不能把历史机器人回答自动标记 Verified。
5. 基准后再选择中文 tokenizer、候选生成 SQL 优化和 ANN。测试 corpus 必须包含旧但
   相关的事实、其他租户大流量、无命中、相反向量、过期/删除、纠正与 as-of 查询。
   备份、WAL checkpoint、可重用空间与实际磁盘回收分开衡量。

建议继续优先使用本地可嵌入的 SQLite/Artifact 组合，保持跨平台和平台 Adapter 边界。
服务器可另接远端索引，但不能让某个 QQ 临时宿主的存储策略成为 Core 的隐式全局配置。
