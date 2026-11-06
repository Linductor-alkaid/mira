# 验证事实 v1 与 Checkpoint 2.0 迁移

> 状态：Active
> 版本：1.0
> 更新日期：2026-11-06
> 负责人：Mira Maintainers
> 依据：DEC-083、MNT-202611-04

## 读写边界

新宿主使用 `VerifiedFactPublisher` 显式提交；ILoopVerifier 的 Satisfied 与模型 done
不自动生成事实。可信 lease 验证语义、主体/scope 与当前 task/environment epoch，
并与取消、暂停、接管和完成共享 owner 门。Core 对证据仅验证引用和元数据，不能
凭 hash 证明命题为真。不要把原始模型文本包装为 ObservationRecorded 或可信 lease。

TaskFactVerified 的 v1 payload 带 schema、完整身份、精确 scope、嵌套 claim、证据
EventId/载荷 SHA-256、verifier、纳秒有效区间。源 ObservationRecorded 须有 observation_id、
task_epoch、environment_epoch、observed_at，内容应为经过宿主筛选的安全元数据或
引用，禁止凭据/敏感正文。有效期不超过源 observed_at 后一天，也不得超过 claim
valid_from 后一天；期限不证明环境未来不变，宿主仍负责 fact key 的语义和作用域。

有 schema 的畸形/未知记录拒绝，不能回退 legacy。无 schema 的旧 flat key/value
继续兼容为原有受信宿主事件，缺少新协议的来源/时效保证。新 payload 嵌套 claim，
旧 writer/reader 不应混用；旧 reader 不会读出顶层 key/value。

## Checkpoint 2.0 与 C++ 消费者

`checkpoint_schema_current()` 为 2.0，读取器仍支持 1.x。VerifiedFact 在原字段后
增加可选 valid_from/valid_until 与 valid_at；新事实两期限必须成对、非空且递增，
不能放入标称 1.x 的投影。旧 1.x 无期限事实保持旧语义。Builder 在增量重建及复用
base 时清除不在 now 区间内的事实；直接持有缓存的消费者必须检查 valid_at，并检查
自己的任务/环境新鲜度，不能把缓存当成实时环境证明。

旧 reader 对新 2.0 投影明确 UnsupportedVersion，避免忽略期限后复活过期事实。
先升级全部 projection 消费者，再启用新 producer；回退旧 reader 时隔离新版 cache，
由适配的宿主重建其可消费状态，不改写 2.0 标头伪装成 1.x。SQLite 容器 schema
没有变更，变化在 checkpoint JSON 文档。C++ VerifiedFact 布局已扩展，所有消费者
需与库一起重新编译；本轮不提供新旧二进制对象混用保证。

## 固化与幂等

MemoryConsolidator 对 v1 要求 scope（含 tenant）完全匹配且未过期；验证时间、
期限和两级证据传入既有 MemoryRecord，再走 marker、kind、冲突、审批与 ACL。
User scope 仍归 Preference 并等待人工批准。新的有效证明不启用 M30 的自动候选或
跨会话选择。Verified 是验证来源标签，不是无期限或跨环境的当前真值保证。

重试复用 publication_id 及完整 claim/证据/时间；相同 ID 不同内容 DataLoss。
弱 ack 或 store 错误可能发生在 append 之后，不得换 ID 盲重发。相同记忆 duplicate-noop
不会延长 TTL；续验用新证据/ID，到期后按现有固化与检索政策处理。旧/同时间的冲突
VerifiedEvent 不覆盖新事实，无期限 legacy 不覆盖有期限事实；HumanCorrection 与
其他来源仍遵循原审批策略。

## BUG-20261106-001：历史问题与后续文件格式修复

旧实现（截至 `4884605`）的 [FileEventStore::recover](../../src/storage/event_store.cpp) 将日志行重建为
AppendRequest，再调用 MemoryEventStore::append，后者赋 Timestamp::now；日志行未
记录原 timestamp。因此仅重开日志也可能改变信封时间。新事实宿主的跨重开重试曾
因比较 source.timestamp 与原 valid_from 被拒绝，这不是 lease 或 Executor 缺口。

本协议使用 payload 的 observed_at 与验证时间，且固化 recorded_at 采用持久化的
valid_from，保证此路径重放稳定；不把恢复信封时间当作磁盘 commit 时间。其他旧
事件的时序、审计与回放仍受基础问题影响，不能以本局部方案宣称全局已修复。
2026-11-06 后续由 [DEC-087](../decisions/DEC-087-file-event-wall-time-persistence.md)
修复新写入的 wall 持久化，并将旧行明确标为时间不可用；源时间无法追溯，旧缓存
不自动改写。格式切换、origin 与 C++ 重编译要求见
[文件日志 v2](file-event-log-v2.md)。本协议继续使用 payload 验证时间，不把文件
wall 当作事实验证/磁盘 commit 时刻；新记录修复不补造旧历史时间。
