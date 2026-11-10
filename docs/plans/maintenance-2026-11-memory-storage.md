# 记忆存储、索引与宿主接入优化

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[总计划](mira-implementation-plan.md)
> 更新日期：2026-11-10
> 关联：M30-03/04/08、M31-02/05/06/09/10；不改变其退出条件

## 目标与边界

用户反馈来自独立部署的 QQ 临时宿主，其代码不在本仓库，也未提供部署访问。
本轮只处理 Mira：核对已有持久层，调研一手资料，修复检索额度的跨 scope 干扰，
提供能验证跨进程重启的持久化示例。不会读取或迁移实际 QQ 数据。

依据为 [Context 与 Memory 设计](../design/context_and_memory_design.md) §13–17、
[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)、
[DEC-057](../decisions/DEC-057-scoped-memory-read-and-apply-check-order.md)、
[DEC-061](../decisions/DEC-061-memory-claim-key.md)、
[DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)。
既有 scope 硬过滤应在检索额度前生效；本轮是实现纠偏，不新增记忆写入路径、后台任务、
数据库依赖或公开契约。SQLite 查询继续复用 Executor StoreChannel。

## 工作项与验收

- [x] `MNT-202611-MS-01` 交付存储核查与外部项目研究：一手来源、访问日期、
      当前实现和建议分开，覆盖数据分层、索引、更新、删除和恢复。
- [x] `MNT-202611-MS-02` FTS/vector 在已授权、已过滤的候选集内消耗额度；
      回归覆盖超过 512 个外 scope FTS 命中、向量扫描额度被其他 scope/claim 占用、
      无命中保持空、重启后仍生效；跑相关 Debug、ASAN/UBSAN、TSan 和质量检查。
- [x] `MNT-202611-MS-03` 持久化示例使用 FileEventStore + SQLite memory/checkpoint，
      从真实事件派生投影；显式目录不被删除，独立进程恢复后检查 FTS、scope 与 checkpoint。
- [ ] `MNT-202611-MS-04` 持久会话资产索引：先冻结 SQLite schema、来源水位、
      确定性身份、ACL、异步修复和删除协议，再实现 IContextRetriever 后端及重启测试。
- [ ] `MNT-202611-MS-05` 冻结事件索引/分段或 SQLite EventStore 的选择与迁移契约，
      对比启动常驻内存、分页查询与崩溃一致性；与 M31-05/06 同步，不能替代其容量治理。
- [ ] `MNT-202611-MS-06` 真实规模基准与生命周期：候选池截断、中文检索、向量 profile、
      删除重建、空间回收；固定 1万/10万数据、查询集、目标机与负载，报告扫描量/命中率/
      启动 RSS/p50/p95，不先承诺性能。

04–06 是后续待冻结工作，不因本轮交付而标为实现。03 复用已有能力，不声称实现了 04。

## 风险与验证矩阵

FTS 与 vector 的候选 ID 集合必须来自同一次已完成 ACL、生命周期、敏感度及 claim
过滤的查询，不能重新构造较宽 scope 集。SQLite JSON 参数展开用于有界 ID 成员过滤；
参数绑定不执行用户 SQL。仍保留 4096 条最新候选上限和软 deadline，完整历史召回与
历史版本文本索引不在本次缺陷修复内。

新增逻辑无并发 owner，沿用专用 Executor SQLite worker 的拒绝、等待和关闭语义。
验证包括现有 store 拒绝/关闭测试及新检索测试；不宣称完成实际进程崩溃或断电验证。
跨平台与真实模型基准未执行，由 Mira Maintainers 在 Linux 通过后于 Windows/Android
目标环境及固定模型/数据集上补跑，结果补入本计划，不用本机结果代替。

## 验证记录

2026-11-10：从 `1bc162e` 开始；已提交此前偏好审批增量。本轮分支
`fix/memory-scoped-retrieval-budgets`。发现 SQLite FTS 全库 LIMIT 512 和向量全库
扫描上限先于授权候选求交，存在其他 scope 消耗召回额度的问题。

2026-11-10：01–03 按 Linux 离线范围验收。

- [研究报告](../design/memory-storage-reference-study.md)核对 Mem0、LangGraph/LangMem、
  Letta、OpenViking、Graphiti 官方文档/源码，不采信未经复现的基准数字。
- 检索使用一个有界、绑定的候选 ID JSON 参数，经 SQLite `json_each` 在 LIMIT 前
  求交。exact-only 查询不生成该参数，不改变 schema、写入或异步生命周期。
  新增 `mira_m4_memory_scope_budget_test`，回放到修复前的 `1bc162e` 时，外 scope FTS、
  敏感内容 FTS 和外 scope 向量三组断言均失败；恢复修复后通过。
- [示例](../../examples/stateful_agent_consumer.cpp)改用 FileEventStore，从提交的
  envelope 派生记忆，独立进程 seed/inspect/seed/inspect 验证 SQLite FTS、scope、
  checkpoint 摘要与正常关闭恢复。显式目录保留数据；默认 smoke 只清理独占临时目录。

环境：Linux x86_64，GCC 13.3.0，CMake 3.28.3，仓库 SQLite 3.53.4。最终树验证：

- Debug 全量 **182/182**：`cmake --build build/debug -j 4` 后执行
  `ctest --test-dir build/debug -j 4 --output-on-failure`。
- ASAN+UBSAN 定向 **11/11**；TSan 定向 **11/11**（后者用 `setarch x86_64 -R`）。
  两套 preset 分别构建并执行 stateful consumer 两项，以及 M4 的
  `memory_scope_budget|memory_retrieval|sqlite_memory|store_open|sqlite_checkpoint|consolidation|memory_embedding|claim_key|retention_sweep`。
  新增测试含正常查询、无命中、空 ACL、重开；既有 store 测试覆盖拒绝、超时和关闭。
- 严格 clang-tidy **2 单元通过**：`clang-tidy -p build/debug --warnings-as-errors='*'`
  检查 `src/storage/sqlite_memory_store.cpp` 与 `examples/stateful_agent_consumer.cpp`。
- format/docs/SBOM/platform-boundary/architecture 五门禁及 `git diff --check` 通过，
  architecture 存量基线仍 16 项，未改策略、基线或 third_party。

本机日志：`/tmp/mira-memory-storage-{debug,asan,tsan}-{build,test}.log`、
`/tmp/mira-memory-storage-before-fix.log`、`/tmp/mira-memory-storage-tidy.log` 与
`/tmp/mira-memory-storage-quality.log`。日志路径仅为本次运行定位，复现以命令和仓库测试为准。

04–06 保持未完成；没有实现 SqliteContextIndex、SqliteEventStore、旧 QQ JSON 迁移或
ANN。远端 CI、Windows/Android、真实数据/模型质量、强杀/断电与规模基准未执行；
由 Mira Maintainers 在对应平台、提供旧 schema 或固定 corpus/模型后补跑。本轮不关闭
M30/M31，不宣称解决 4096 候选截断、完整历史版本检索或磁盘回收。
