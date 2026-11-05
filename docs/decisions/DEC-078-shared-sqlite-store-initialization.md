# DEC-078：SQLite 参考存储共享打开与迁移编排

> 状态：Accepted
> 日期：2026-11-05
> 负责人：Mira Maintainers
> 冻结里程碑：MNT-202611-10（复用治理维护轮）
> 替代/被替代：无

## 背景与问题

Checkpoint 与 Memory 的 open 重复 schema 判断、版本解析、诊断收集和失败清理。
两份 stoull 逻辑都可能接受数字尾随文本，Memory 还把超出 uint16 的版本截断。
原有 DDL/迁移提交与 schema_version 更新分离，版本标记失败时可能留下半完成升级。

## 决策

在 storage 内部增加 `open_reference_store`，复用现有 open_database、DatabaseHandle、
Transaction 与 meta helpers。输入为连接选项和 backend schema spec，输出 handle 与
StoreDiagnostics。kind、当前版本、DDL、迁移 callback 与策略错误工厂由各 store 提供。

- 用 from_chars 严格读取两个完整 uint16 分量，拒绝空值、符号、空白、额外分隔符、
  尾随文本和溢出。格式错误为 DataLoss，未来版本仍按只读诊断/拒绝处理。
- 初始 DDL + kind + version 在一个事务提交；迁移内容 + version bump 同样原子提交。
  backend DDL/迁移不再独立 BEGIN/COMMIT，失败由 RAII 回滚。
- 只读诊断不安装或迁移 schema；需要升级的旧库明确 UnsupportedVersion，要求可写
  迁移，而不是尝试写只读连接后返回偶然的 SQLite 错误。
- 保留 backend 的策略错误域，底层 SQLite 错误沿用现有映射；不把迁移策略下沉到
  通用 Executor，不改 StoreChannel 的 worker、容量、超时和关闭顺序。
- 工厂参数校验及最终 Impl/StoreChannel 构造仍归各 store；共用打开的失败路径不启动 worker。

## 备选方案

仅共享版本解析：无法避免打开/迁移/诊断编排继续漂移，不采用。
共享全部 store 或 SQL schema：两个数据模型与 API 不同，不采用。
继续让 DDL 自带事务：无法原子包含 metadata stamp，不采用。

## 影响与风险

合法既有文件和 schema 版本不变；畸形/溢出版本现在拒绝，不能被自动改写成当前版本。
迁移失败后不保留半完成变更。未知 schema/错误 kind 仍拒绝，不擦除数据库。
只读旧库的错误分类变得明确。错误摘要文字可统一，ErrorCode 与 domain 的约定按文档保持。
本轮不改变文件格式、公开 C++ factory 或 Executor 注册面；无新并发路径。

## 验证方式

两类 store 覆盖新库、重开、只读、错误 kind、未知/损坏库、缺失/畸形/超大/未来版本、
失败后修复重开及忙锁。Memory 旧版本升级在版本更新处注入失败，证明迁移内容与
版本均回滚；移除故障后升级成功。运行原有 schema、关闭、拒绝与并发回归。

## 关联文档和工作项

- [维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)：10。
- [Context 与 Memory 设计](../design/context_and_memory_design.md)。
