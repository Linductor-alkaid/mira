# DEC-087：文件事件 wall time 持久化与旧日志时间缺失

> 状态：Accepted
> 日期：2026-11-06
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-12 / BUG-20261106-001

## 决定

FileEventStore 新写入使用物理日志 v2：保存一次采集的 wall 纳秒整数，追加完整行
后将同一时间放入内存投影。恢复逐纳秒还原 wall，不持久化/复用跨进程的 steady
clock；恢复后的 monotonic 为默认值，不可用于 elapsed/deadline 计算。

EventEnvelope 尾部新增 EventTimestampOrigin：AppendProcess 表示本次进程 append
采集的双时钟，PersistedWall 表示恢复的 wall（无可用 monotonic），UnavailableLegacy
表示 v1 从未存下原时间。旧事件 timestamp 置默认值并明确标识，不用恢复时间或文件
mtime 伪造历史。公开布局增加，C++ 消费者必须与库一同重编译；EventEnvelope 的
逻辑 schema 不变，物理文件版本独立管理。

v2 使用两行防降级标记 `MIRA_EVENT_LOG 2` / `MIRA_EVENT_RECORDS`，之后为 `E2 `
前缀的严格 JSON 行。它存完整身份、session/task 序号、cause、分类、payload、wall
纳秒、原请求幂等摘要和覆盖整条元数据的 record 摘要。uint64 序号/cause 使用十进制
字符串避免 JSON 整数范围损失。请求摘要保持原含义，record 摘要额外覆盖时间/序号，
不是密码学认证或磁盘 commit 时钟。重复 ID 除原请求摘要外还比较实际请求字段，
避免旧分隔符拼接摘要的边界歧义；不改变已存请求摘要的字节协议。

旧数值行仍可读；首个新 append 在旧前缀后追加一次两行标记，再写 v2 行，不原地
重写历史、不猜时间。标记之后不接受旧行或第二次格式切换。双标记确保既有 v1
reader 在新段遇到“中间的非数值行”而只读拒绝，而非把唯一的新行当坏尾截断。
已有加载的旧 writer 与新 writer 仍不能并发占用同一路径；文件 store 维持单 owner。

未知版本、语义错误、checksum/序号错误和非法降级不可自动修复。只有最后未换行的
语法残缺记录/未完成格式标记可按 repair_uncommitted_tail 截断；v2 已换行的损坏行、
中间残缺与禁用修复时
均只读保留。完整但未换行的最后记录可保留，下一次 append 先补分隔符，不能把两条
记录拼接。v2 JSON 正确转义 payload 换行/NUL，旧前缀继续沿用 v1 原有能力边界。

2026-11-06 CI 修正：新写入/flush 统一 binary stream，格式分隔符固定单字节 LF；
旧 CRLF 行仍按原始字节计截尾偏移，解析只移除行末 CR，追加不改旧前缀。此修正
落实物理行格式，避免 Windows 文本流自动转换，无 schema 或持久化等级变化。

## 下游与验证

时序以持久化 sequence 为准，wall 可回拨，不排序重发。时间敏感消费者须检查
origin；MemoryConsolidator 对缺失原时间的 legacy flat fact/Completed episode
明确拒绝整批自动固化，不能将零时间回填为 now。已有带 valid_from 的 v1 新事实
协议仍用 payload 时间，DEC-083 路径不回退。Conversation history 仍按 sequence
组织，旧时间不可渲染为真实发生时间。已生成的旧缓存不自动纠正或重写。

验证新旧/混合日志、重复 ID、跨 session/task/cause 大整数、双重重开、纳秒稳定、
缺失时间、未知版本、非法字段/分类、时间篡改、尾部修复、只读、旧 reader 不改文件，
并回归 journal crash matrix、Checkpoint/Memory/Replay 与安装消费。没有增加线程、
worker、timer 或 Executor owner，不修改第三方；真实 OS 强杀/断电和平台门禁仍
按 02/03 保持开放。

关联：[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)、
[事件设计](../design/event_artifact_crash_consistency.md)、
[既有事实迁移](../compatibility/verified-fact-v1-and-checkpoint-v2.md)。
