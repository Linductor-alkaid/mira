# M31：容量准入、自动归档与可恢复清理

> 状态：Planned（2026-10-28 随 [M28](m28-interaction-coordinator-first-stage.md)
> 一并登记；**跑前冻结待本里程碑立项时完成**，本文件当前只冻结范围边界与非目标）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)；
> 来源需求为
> [Issue #111](https://github.com/Linductor-alkaid/mira/issues/111) §6）
> 前置：M28 关闭（**未满足**，台账与注册表须计入预算）；M30 关闭（**未满足**，
> 价值信号是淘汰裁决的输入）；DEC-069 数值档位冻结（本立项完成）；
> [`EXE-20261028-001`](../executor_feedback/ledger.md) 的临时边界确定
> 建议发布点：非发布物；部署档位与有限容量契约的冻结锚点
> 更新日期：2026-10-28（登记）

## 1. 目标

把「持续交互」从「能一直聊」变成「**能一直聊且资源有界**」：

1. **总量有界**：所有持久化与驻留数据层纳入**一个**容量预算；按主体份额、
   软/硬水位、回收预留分层裁决。
2. **价值驱动的遗忘**：四层遗忘（召回降权 / 去重抽象巩固 / 有限归档 / 物理删除）
   各有触发与退出条件；淘汰序**冻结**且可重放。
3. **会话自动退出与归档**：完全结束的会话释放受管理名额并最终清理；
   仍有工具、未读结果、未结算更新或共享证据引用时**不能误删**。
4. **可证明回收**：分别报告逻辑删除、可重用空间与实际文件占用/峰值；
   回收失败与隐私删除**分别结算**。
5. **周期清扫宿主**：交付设计文档承诺但当前**不存在**的
   `Retention/GC | soft periodic timer` 宿主。

## 2. 范围与非目标

### 2.1 范围

- 跨层容量预算与准入裁决（总量 / 分主体份额 / 软水位 / 硬水位 / 回收预留）。
- **所有**无界 append-only 层的回收窗口：`memory_mutations`、`memory_erasure_log`、
  `memory_provenance`、`memory_scope_holds`、`FileEventStore.events.log`、
  `FileArtifactStore.manifest.log` 与 `tombstones/`、输入台账（DEC-068）、
  主体注册表投影、`ContinuationCache`、`ActionJournal` 驻留状态。
- 价值字段与**冻结淘汰序**（沿 WorldState 既有淘汰序形状，
  [`include/mira/world_state.hpp:331-340`](../../include/mira/world_state.hpp#L331)）。
- SQLite 空间回收能力：`auto_vacuum=INCREMENTAL` + 定期 `incremental_vacuum`
  + WAL checkpoint 策略——**否则「回收」在磁盘上不可证明**。
- EventStore 磁盘日志分段与归档段回收；解除「内存索引满 → store 永久 `read_only` +
  `DataLoss`」的死锁（[`src/storage/event_store.cpp:392-397`](../../src/storage/event_store.cpp#L392-L397)）。
- ArtifactStore `manifest.log` 压实、tombstone 归档、启动时惰性/分批加载
  （当前启动**全量载入内存**，`:203-287`）。
- 会话退出生命周期（`Managed → Executable → Converging → Archivable → Archived → Purged`）
  与归档资格裁决。
- 保护集合与其**自身上限**。
- 周期清扫宿主（Executor timer + `submit_delayed` 重装链，保证不重叠与可取消）。
- 部署档位数值冻结（桌面常驻 / 移动端受限 / 受限嵌入式）。

### 2.2 非目标

- **不做** UI（Mirage 负责）。
- **不做** 无限记忆或永久保存全部原始经历的承诺。
- **不做** 用归档代替「忘掉」：归档**不**释放 Erasure Pending 的 hold，
  归档**不**被计为已遗忘（DEC-069 §7/X2）。
- **不做** 绕过 [AGENTS.md](../../AGENTS.md) 的 Executor 约束实现清扫调度。
- **不改** `third_party/kairo` 以掩盖集成问题。
- **不做** 模型介导的淘汰裁决：淘汰序必须**冻结且确定性**。

## 3. 设计与决策依据

- [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
  §1–§10（本里程碑的主决策）。
- [DEC-062](../decisions/DEC-062-memory-retention-sweep.md)——保留期清扫的窄接口、
  有界 pass、`more_pending` 收敛与 savepoint 回滚语义；**复用**而非重写。
- [DEC-058](../decisions/DEC-058-mutation-ledger-scope-binding.md)——台账 scope 绑定；
  本阶段补其**生命周期**。
- [事件与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
  §5.3（引用与 GC）、§10（删除与 Replay 降级）、§11（背压、容量与故障）、
  §12（Executor 映射与关闭）、§14.2（副作用安全）。
- [Context 与 Memory 设计](../design/context_and_memory_design.md) §16.4、§17
  （保留期与 Executor 映射表——**本阶段首次让 §17 的 `soft periodic timer` 落地**）。
- [Core Curator 设计](../design/context_curator_design.md) §13（跨 store 擦除纪律）。
- Executor 反馈：[`EXE-20261028-001`](../executor_feedback/ledger.md)。

## 4. 冻结的契约语义（跑前冻结待立项）

本节**当前未冻结**。立项时必须回答并冻结：

1. 容量账本的**结构与计量单位**（按层的字节/条目口径）与**观测接口**。
2. 准入裁决的**错误语义**（新增 `CapacityExceeded` 之类的稳定错误码）与
   硬水位下的**有界背压**形态。
3. 价值字段与**冻结淘汰序**（含稳定 tiebreak）。
4. 保护集合的**成员判定**与**自身上限**（DEC-069 §4/P1–P5）。
5. 会话退出生命周期的**状态机**与**归档资格**的合取判定。
6. 跨 store 分步删除的**幂等键**、**墓碑语义**与**崩溃恢复**流程。
7. 三种度量的**分别报告**口径（逻辑删除 / 可重用空间 / 实际占用与峰值）。
8. 部署档位数值与**测量方法**（必须经目标设备验证，**未经测量不得**写入默认值）。
9. 周期清扫宿主的**调度形态**（`submit_delayed` 重装链）与**过载让步**策略。

## 5. 工作项（立项时细化）

- [ ] `M31-01` 立项与跑前冻结（含部署档位数值与测量方法）；
      前置须确认 kairo **含 PR #223 的可用版本**已发布并可被 Mira 消费
      （否则分主体份额只能走台账的过渡边界）。
- [ ] `M31-02` 容量账本、计量单位与跨层观测接口。
- [ ] `M31-03` 准入裁决、份额分配与硬水位有界背压；**分主体份额**经
      Executor 原生分桶实现（`set_subject_max_in_flight_tasks` +
      `TaskBuilder::subject()`），并把受分桶约束的提交点**迁移到带键形态**；
      上游版本可用前按台账的嵌套预过滤过渡，版本可用后**移除**过渡层。
- [ ] `M31-04` 价值字段与冻结淘汰序。
- [ ] `M31-05` 无界 append-only 层的回收窗口与配额。
- [ ] `M31-06` SQLite 空间回收、EventStore 分段、ArtifactStore 压实与惰性加载。
- [ ] `M31-07` 会话退出生命周期、归档资格与自动归档。
- [ ] `M31-08` 保护集合与其上限。
- [ ] `M31-09` 周期清扫宿主与跨 store 分步删除（幂等/可取消/可恢复）。
- [ ] `M31-10` 三种度量的分别报告与可证明回收验证。
- [ ] `M31-11` IVA 测试矩阵（`tests/m31/`）；分桶用例须在**真实提交路径**上断言
      「A 用满自身份额后 B 仍可接纳」，并区分「分桶拒绝」与「总量拒绝」
      （`subject_quota_exhausted_count` 与事件 `subject`）。
- [ ] `M31-12` 文档同步。

## 6. 测试与退出条件

- [ ] 工作项与门禁全部勾选，验证记录落档。
- [ ] 覆盖 issue #111 的验收场景 8（容量长期收敛）与 9（安全归档/遗忘）。
- [ ] 小配额持续写入不同大小的事件/附件/记忆，越过软/硬水位，
      验证驻留/受管理会话数、归档总量、**实际文件占用与峰值**始终符合冻结预算。
- [ ] 保护集合满、磁盘满、回收失败**可见、有界**，
      **不以无限归档或删除计数代替资源测量**。
- [ ] 回收失败**不**解除 Erasure Pending 的 hold；归档**不**被计为已遗忘。
- [ ] 清理中崩溃/取消、原始证据过期、隐私删除后不复活、去重台账过期后旧请求
      不重复副作用——各有可复现用例。
- [ ] CI 全矩阵绿；受影响的决策/设计/API 手册/术语表/兼容性文档已同步。
- [ ] 未执行的平台测量保持未勾选并记录原因与补跑条件。

## 7. 风险与阻塞

- **依赖 M28/M30**：台账/注册表与价值信号是本阶段的输入。
- **Executor 缺口**：`EXE-20261028-001`（按主体份额准入）**上游已交付**——
  kairo PR [#223](https://github.com/Linductor-alkaid/kairo/pull/223) 合并入 master
  并关闭 [#222](https://github.com/Linductor-alkaid/kairo/issues/222)，
  但**尚未发布含该修复的 tag**，Mira 未消费 → 台账状态 `Accepted`。
  期间按台账记录的嵌套预过滤临时边界过渡；版本可用并迁移后**移除**该过渡层，
  改为直接使用原生分桶，**不得**与原生能力叠加。
  **不得**因此引入私有线程池或第二套调度器。
- **提交形态未迁移 = 配额不生效**：上游分桶键只在
  `submit_auto(TaskBuilder)` / `submit_after(TaskBuilder)` /
  `submit_after_with_handle(TaskBuilder)` 上被消费；Mira 现有 11 处 `submit_auto`
  **全部是裸 lambda 形态**，不受分桶覆盖。只接入原生能力而不迁移提交形态，
  会得到一个**不约束任何实际提交**的配额。
  已就此在上游 #222 提出后续调整（请求补带 subject 的裸形态入口或诊断面）；
  该请求**非阻塞**——M31 按「迁移到带键形态」执行，不为上游潜在变更预留分支。
  若上游后续提供裸形态入口，M31 的迁移项可相应缩减（届时按变更记录回填）。
- **迁移风险**：现网数据可能已超出新预算。必须提供迁移与清扫路径，
  **不得**在迁移中静默删除用户数据。
- **EventStore 只读死锁**：当前「内存索引满 → store 永久 `read_only` + `DataLoss`」
  是不可恢复状态，必须先解除，否则容量问题会变成停机问题。
- **磁盘可证明性**：SQLite 无空间回收 ⇒ 「回收」不可证明；
  该项**必须**与容量准入同批交付，否则不得宣称预算达标。

## 8. 验证记录

> 尚无记录。
