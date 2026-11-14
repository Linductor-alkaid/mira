# DEC-111：SQLite Memory 文件总量边界

> 状态：Accepted（M31 的元数据首增量，非完整容量/遗忘交付）
> 日期：2026-11-14；工作项：M31-13
> 来源：Mirage MIRA-20261112-001；维护者已明确授权补齐 Mira 并升级 pin

## 决策

为 SqliteMemoryStore 增加可选 max_storage_bytes 与 capacity_reserve_bytes。0 保持旧行为；
开启时以每 Store 的 SQLite VFS shim 对数据库、WAL、SHM、rollback journal 四个已知
文件的长度总和做真实写入前接纳，拒绝未知/临时文件、符号链接和容量溢出。预分配提示
不下传；映射扩展亦受约束。启用 exclusive locking、内存临时表、禁用 mmap；只有该
Store 是文件唯一 owner 时契约成立，不能用其他 SQLite 连接或外部进程修改这些文件。

SQLite 默认 VFS 承担实际平台 I/O 与事务/恢复。shim 不自建线程、任务队列或数据库；
文件身份白名单取自该 VFS 的 xFullPathname，不用 std::filesystem 的另一种规范化
字符串匹配 SQLite xOpen；计量路径保留原最终文件以拒绝符号链接。
初始打开/迁移后所有运行期操作沿同一 Executor StoreChannel。增长超过硬限额返回
ResourceExhausted；不自动扩容、删除、重试或冒充回滚。已有 mutation-id 幂等与 WAL
恢复继续复用。超过配置的已有文件拒绝打开且不删除；正常重开重新计量，不依赖驻留计数。

普通新 mutation 在硬限额减 reserve 的水位拒绝，幂等重放仍先由原账本处理；删除与
清扫可使用预留，但可能明确失败。预留不是清理成功保证。snapshot 分别报告四文件
实际长度与进程内最大已接纳长度；不把文件长度当物理块占用/文件系统元数据，也不
声称断电、设备损坏、恶意外部写入或完整目录全局配额保证。

本增量只覆盖 SQLite 元数据，不含 ArtifactStore/EventStore、跨主体份额、价值淘汰、
自动归档、共享工件回收或完整 M31。宿主必须为其他层另行选择已验收能力。产品可
先默认记录有界 Episode/Lesson 元数据；视觉长期工件不能因此越过自身准入缺口。

## 验证

真实 SQLite：满额拒绝/幂等重放/保留已有记录、重开后容量仍有效、超限旧库拒绝、
擦除及失败不超硬文件限额、并发排队/关闭、独占冲突、坏配置、异常路径与 sanitizer。
结果记入 M31-13。真实掉电与 Windows 留待目标环境，不以软件拒绝冒充硬件故障验收。

## 依据

[SQLite VFS shim](https://www.sqlite.org/vfs.html#vfs_shims)、
[文件虚方法](https://www.sqlite.org/c3ref/io_methods.html)、
[File control](https://www.sqlite.org/c3ref/c_fcntl_begin_atomic_write.html)、
[M31](../plans/m31-capacity-admission-and-archival-recovery.md)。
