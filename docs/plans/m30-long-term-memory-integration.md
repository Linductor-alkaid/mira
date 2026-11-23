# M30：长期记忆接入——提取、检索、纠正、删除与回忆反馈

> 状态：In Progress（召回分组、人格、事件、社交记忆、世界事实与一次性目标已验收 Linux 增量）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)
> 的记忆面与
> [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
> 的价值层；来源需求为
> [Issue #111](https://github.com/Linductor-alkaid/mira/issues/111) §4）
> 前置：M28 关闭（**已满足**）；M29 关闭（**已满足**，PR #124 CI 24 项通过）；
> M23 Memory Promotion 关闭（已满足，[DEC-035](../decisions/DEC-035-context-curator-working-context.md)）；
> DEC-070 契约冻结（已满足，2026-10-28 随 M28 登记一并冻结）
> 建议发布点：非发布物；M31 价值驱动淘汰的输入侧前提
> 更新日期：2026-11-23

## 1. 目标

把长期记忆从「已有的存储与检索组件」接到「统一交互」的生命周期上：

1. **写入**：从交互与已验证结果提出长期偏好、事件经历、事实、经验等候选，
   经既有 consolidation / promotion 管线校验后写入。
2. **选择**：按当前任务选择**实际需要**的记忆进入上下文；全局偏好与任务局部要求
   的生效范围显式区分。
3. **纠正与删除**：用户纠正、「以后记住……」与「忘掉……」能关联到正确记录/范围，
   经既有权限与 policy 处理；冲突、来源、有效期与删除状态**可解释**。
4. **召回排序（可选增量）**：可验证回忆反馈、importance/使用信号与查询相关性分离的排序。
   **是否新增 API、分几步做由立项评审决定**——不得先冻结激活公式而遗漏完整交互闭环。
5. **分类读写**：按 [DEC-119](../decisions/DEC-119-six-memory-systems.md) 的六类契约接入
   人格、事件记忆、社交对象画像、世界模型记忆、目标记忆与技能记忆。

## 2. 范围与非目标

### 2.1 范围

- 协调层与 `IMemory` 之间的**记忆选择**接线：按当前工作召回、投影、装配。
- 记忆**候选**的提出与经既有 `MemoryConsolidator` 管线的写入路径。
- 「以后记住……」「忘掉……」的**意图路由**与作用范围判定。
- 主体身份（`PrincipalContext.user_id`）与记忆 ACL 域（`MemoryScope.subject_id`）
  之间的**映射规则**落地（DEC-046 登记但未提供）。
- 可选的回忆反馈与使用信号增量。

### 2.2 非目标

- 向量召回复用 DEC-111/113 的供给与混合检索，ANN 与规模优化在专项验证后立项。
- **不做** 容量准入、归档与遗忘（归 M31）。
- **不做** 记忆中的授权描述替代当前有效授权：记忆里的 grant 描述**不能**当作授权来源。
- **不做** 跨 session/tenant 泄漏；跨会话召回继续服从**精确 scope/tenant ACL**。
- **不做** UI 的记忆管理界面（Mirage 负责）。
- **不新增**第二套记忆写入路径：写入**必须**走既有 consolidation / promotion 管线。

## 3. 设计与决策依据

- [DEC-123](../decisions/DEC-123-social-identity-directory.md)：社交对象身份与别名索引。
- [DEC-124](../decisions/DEC-124-social-attribute-assertions.md)：画像字段断言与时态检索。
- [DEC-125](../decisions/DEC-125-social-relationship-assertions.md)：关系断言与双端点清理。
- [DEC-126](../decisions/DEC-126-social-erasure-recovery.md)：目录与记忆库的擦除恢复。
- [DEC-127](../decisions/DEC-127-world-fact-address-and-freshness.md)：世界事实地址与新鲜度。
- [DEC-128](../decisions/DEC-128-navigation-world-fact-refresh.md)：导航重新观察和 AppModel 世代。
- [DEC-129](../decisions/DEC-129-durable-goals-and-commitments.md)：一次性目标与承诺持久状态。
- [DEC-122](../decisions/DEC-122-durable-conversation-index.md)：持久片段和联合擦除。
- [DEC-121](../decisions/DEC-121-indexed-event-slices.md)：有界事件切片、索引重建与 Record 水位。
- [DEC-120](../decisions/DEC-120-agent-persona-admission.md)：人格目录、宿主任务绑定与 Gateway 版本准入。
- [DEC-119](../decisions/DEC-119-six-memory-systems.md) 与
  [六类记忆设计](../design/m30-memory-systems.md)：分类、事实源、写入、索引和读取契约。
- [记忆存储研究](../design/memory-storage-reference-study.md)与
  [维护计划](maintenance-2026-11-memory-storage.md)：SQLite 宿主接入、检索额度隔离和
  跨进程示例；持久会话资产索引及旧 JSON 导入仍待后续冻结，不代替 M30 生命周期验收。

- [DEC-109](../decisions/DEC-109-memory-preference-approval.md)：持久偏好提案、预览、
  显式确认/拒绝、冻结版本与删除失效；本轮不实现承诺与新世代。

- [DEC-107](../decisions/DEC-107-interaction-memory-lifecycle.md)：首批生命周期、
  确定性 scope 白名单与有界结果；不替代本计划其余实现细则的冻结。

- [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
  §2（计入范围）、§3（分层遗忘）、§4（保护集合）——本阶段提供**价值信号**，
  M31 提供**淘汰裁决**。
- [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)
  §6（上下文组织）——本阶段提供「哪些记忆进入该工作的上下文」。
- [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)（学习闭环）、
  [DEC-035](../decisions/DEC-035-context-curator-working-context.md)（Working Context 与晋升）、
  [DEC-057](../decisions/DEC-057-scoped-memory-read-and-apply-check-order.md)（scope 读取与 apply 顺序）、
  [DEC-058](../decisions/DEC-058-mutation-ledger-scope-binding.md)（台账 scope 绑定）、
  [DEC-059](../decisions/DEC-059-memory-kind-policy.md)（per-kind 策略）、
  [DEC-061](../decisions/DEC-061-memory-claim-key.md)（claim key 与冲突检测）、
  [DEC-064](../decisions/DEC-064-memory-event-surface.md)（记忆事件面）。
- [Context 与 Memory 设计](../design/context_and_memory_design.md)
  §5（记忆层级）、§6.1（MemoryScope）、§13（长期 Memory 写入）、§14（混合检索）。
- [Context Curator 设计](../design/context_curator_design.md) §13（Stage W4 晋升纪律）。
- [M36](m36-memory-agent-context-integration.md)交付可复用的受管记忆服务与请求准入；
  不关闭本里程碑的承诺、完整纠正/删除交互、主体映射授权与恢复依赖。
  同 scope 删除后重新接纳需另行冻结世代/来源水位协议，不能移除删除屏障后回灌旧事件。
- [记忆代理与长期上下文参考研究](../design/memory-agent-reference-study.md)
  （2026-11-09，`MNT-202611-MA-01`；供 `M30-01/03/04/08` 评审：宿主记忆服务、
  有界记忆代理、既有写入管线、请求装配与删除失效；仅研究输入，未批准新 API 或实现范围）。

## 4. 冻结的契约语义（2026-10-28 由 DEC-070 冻结）

§4 的**行为契约**已由
[DEC-070](../decisions/DEC-070-long-term-memory-behavior-contract.md) 冻结：
写入（§1）、检索与选择（§2）、主体 → `MemoryScope` 映射（§3）、
承诺实体（§4）、纠正与删除（§5）、排序增量的延后（§6）。
门禁矩阵 `M30-G1`–`M30-G8` 同步定义于 DEC-070「验证方式」。

本里程碑立项时**仍须**回答并冻结的实现细则：

1. 承诺实体的**具体存储形态**（DEC-070 §4/P3 要求：可查询、可纠正、可删除、
   计入 DEC-069 预算、属保护集合；具体类型与表结构由本里程碑冻结）。
2. 主体 → `MemoryScope` 映射的**稳定内部表示**的具体生成方式
   （DEC-070 §3 已冻结语义：稳定、不可逆、不含外部账号信息）。
3. 候选提出的**具体触发点**（哪些既有管线节点提出候选、提出什么 kind/scope）。
4. 与 DEC-069 §3 四层遗忘的**接口实现**：本阶段只产出**价值信号**，
   淘汰裁决归 M31；**不得**在本阶段实现物理删除。
5. 召回排序增量的**分步方案**：使用信号与查询相关性**分离**，
   可验证回忆反馈的可选增量——按 DEC-070 §6/O4，在 §4 闭环交付**之后**另行评审，
   **不并入**本里程碑验收。

## 5. 工作项

- [ ] `M30-01` 立项与跑前冻结（DEC-107/109 增量及 DEC-119 六类契约已冻结；承诺存储、
      完整记忆意图、价值信号与删除世代的实施细则待冻结）。
- [x] `M30-02` 主体 → MemoryScope 映射规则与租户隔离落地（User/Session/Application 内部摘要，自动 User + 当前 Session 白名单，当前授权及归属校验；Linux 离线验收）。
- [ ] `M30-03` 记忆候选提出与经既有管线的写入路径（终态 Session Record 及显式 User 偏好提案/持久审批已接通；其他候选报告的统一持久收集与触发仍待完成）。
- [x] `M30-04` 按当前工作的记忆选择与上下文装配接线（目标/最新约束、队列接纳边界、Gateway P4 候选、Layer 0 准入与失效探针；Linux 离线验收）。
- [ ] `M30-05` 「记住 / 忘掉 / 纠正」的路由、范围判定与可解释输出（偏好提案、版本冻结的纠正、确认/拒绝/过期/未知结果已交付；完整意图与遗忘交互仍待完成）。
- [ ] `M30-06` 价值信号（importance / 使用信号）产出（供 M31 使用）。
- [ ] `M30-07` IVA 测试矩阵（生命周期与偏好协议共八组通过；承诺/完整纠正删除/价值信号/新世代矩阵待补）。
- [ ] `M30-08` 文档同步（DEC-107/108/109 设计/API/术语/计划已同步，完整里程碑收口仍待后续增量）。
- [ ] `M30-09` 冻结并实现删除后的同 scope 新知识接纳：区分新世代与已删除来源，
      旧事件、摘要和在途提案不得回灌；有新的明确用户输入时可重新记忆。与 M31
      的屏障保留/回收协同验收，不能直接删除 M36 的屏障来重新开放 scope。
- [x] `M30-10` 冻结六类契约并交付宿主可配置的独立召回组，验证类别配额、ACL、
      追加查询、去重和 Gateway 候选失效（Linux 离线首增量）。
- [x] `M30-11` 提供按租户与 Agent ID 读取生效人格的宿主 API 及 Gateway 接线，
      验证重载、回滚、授权、角色边界、预算和在途失效（Linux 离线）。
- [x] `M30-12` 接通事件取证水位与 Session/Task/type 查询，以及持久会话片段召回，
      验证时间过滤、重建、重开和联合擦除（Linux 离线）。
- [ ] `M30-13` 提供社交对象与关系的稳定身份、属性级证据和环境内索引，验证同名歧义和跨域隔离。
- [x] `M30-14` 接通环境/应用事实的实体、属性与时态查询，验证环境隔离及过期重新观察（Linux / Simulator）。
- [ ] `M30-15` 持久化目标和承诺的状态、授权、到期索引及重复计划身份，验证兑现、取消、
      重启和幂等恢复；主动派发与打断预算归 M33。
- [ ] `M30-16` 接通技能目录和带版本的经验检索，验证撤回、版本迁移及当前权限。
- [x] `M30-17` 交付 M30-12 的证据源增量，提供有界事件切片与 Record 水位，验证空页、
      文件重开、时间边界、错误传播及擦除屏障（Linux 离线）。
- [x] `M30-18` 交付 M30-13 的社交身份目录，验收同名、跨域、版本覆盖与擦除后重开。
- [x] `M30-19` 交付 M30-13 的画像字段断言，验收来源、有效区间、记录时间、字段索引与跨域隔离。
- [x] `M30-20` 交付有向关系断言，验收有序端点、谓词与时态索引、重开和对象双端擦除。
- [x] `M30-21` 交付目录与记忆库的联合擦除恢复，验收重开、数据库绑定及环境范围隔离。
- [x] `M30-22` 交付结构化世界事实写入及双时态读取，验收隔离、过期、刷新与重开。
- [x] `M30-23` 交付导航重新观察及 AppModel 版本联动，验收过期、换版、取消和撤权。
- [x] `M30-24` 交付一次性任务与承诺的持久状态和到期索引，验收归属、结算、重开和擦除。

## 6. 测试与退出条件

- [ ] 工作项与门禁全部勾选，验证记录落档。
- [ ] 覆盖 issue #111 的验收场景 5（跨时记忆：偏好被认可、被纠正、被删除）。
- [ ] 覆盖跨 session/tenant/scope 的**不泄漏**用例。
- [ ] 「频繁提到 ≠ 更可信」的负向用例。
- [ ] CI 全矩阵绿；受影响的决策/设计/API 手册/术语表已同步。
- [ ] 未执行的真实模型与平台验证保持未勾选并记录原因与补跑条件。

## 7. 风险与阻塞

- **依赖 M28/M29**：本阶段依赖上下文归属与并发归属已冻结。
- **价值公式风险**：issue #111 §4 明确「不应先冻结激活公式而遗漏完整交互闭环」。
  本阶段**先交付闭环**，排序增量按可选项推进；**未经验证的模型评分不得**成为
  全部规则（DEC-069 §3/F1）。
- **范围界定风险**：`MemoryScope.subject_id`（记忆 ACL 域）与
  `PrincipalContext.user_id`（安全主体）是**两个不同语义**（DEC-046），
  映射规则必须显式，不得隐式复用同一字符串。

## 8. 验证记录

### 2026-11-10：首批实现开始

基线 `a2c33a2`，分支 `feat/memory-interaction-lifecycle`。本轮承接 M30-02/03/04/07/08：
三类内部 scope 映射、协调层显式绑定、投递成功边界的 Recall、结算后的 Record、
当前主体可查询的异步结果与错误。复用 M36 Executor worker、写入管线及 Layer 0，
不新增自动重试、授权来源或物理删除路径。承诺、完整记忆意图/审批/纠正/删除、
价值信号及同 scope 新世代接纳保持未完成。最终验收结果在实现结束后追加。

### 2026-11-10：召回原生角色化增量（DEC-108）

部署反馈：召回上下文的说话人仅靠文本前缀标注，模型无法在输入层面分辨身份。
按 DEC-108 交付：`ConversationEntry::Kind` 贯穿投影（segment entry/索引资产/
候选 turns），`ContextItem::wire_role`（仅 RetrievedMemory，只允许
User/Assistant）与 `context_items_from_candidate` 逐轮原生角色展开；事实召回
合并为单个 `[recalled memory v1|count=N]` 带框块；`conversation_log` 去除
`assistant said:` 前缀烘焙（`entry_line` 成为唯一前缀渲染器）；compress 提示
要求摘要归因 speaker。M30-04/M30-08 的范围随之更新。

验证（Independent-Verification-Agent 独立执行，主循环复核）：更新
m17/m3/m30_lifecycle/m36_context_admission/m36_memory_agent/
m36_memory_agent_model 六个测试文件并新增 per-run 原生角色展开、turns JSON
round-trip、索引 entries 校验、wire_role 准入 fail-closed、召回带框块去重等
测试点；`cmake --build build/debug -j 8` 退出 0；`ctest --output-on-failure
-j 8` 176/176 通过、0 失败；`tools/check_architecture.py` clean。剩余补跑
条件：真实长会话触顶 `max_input_items` 的逐轮回放、supervisor 组合路由、
真实模型质量评估，保持未验证并记录于此。

### 2026-11-10：首批生命周期增量验收

基线 `a2c33a2` 上的未提交工作树。交付 DEC-107、三类内部 scope 映射、可选
set_memory_service、stage/ack 输入帧、Recall/terminal Record 接线及主体过滤的
memory_status。所有作业复用 M36 worker/句柄/future；没有修改 third_party、
架构策略或 baseline，没有新增授权或写入旁路。

四组测试与范围：

| 测试 | 验证结果 |
| --- | --- |
| m30_scope_binding_test | 映射可复现、tenant/domain 分离、约束容量、旧 revision 拒绝、主体/Session/scope 不可改、退休不可复活、探针 false/异常保守封闭 |
| m30_lifecycle_test | 队列接纳前无 Recall，当前用户跨 Session 复用偏好、其他用户/租户/Session 不泄漏、重复输入不重派、真实 Gateway 请求及 selection digest、纠正与 Runtime 接管失效、删除后不再发放旧 future |
| m30_recording_test | 首次结算后才 Record，TaskFactVerified 经 consolidation 成为 Verified，模型 Preference 保持 Unverified 且待审批，RecoveryLesson 等级不足拒绝，重复结算不重写 |
| m30_lifecycle_faults_test | 拒绝/异常投递封闭候选、纠正取消迟到结果、旧作业未退出显式背压、最新约束传入语义工作、取消/超时/模型异常/close/Executor 拒绝均可消费且不回滚输入 |

环境：Linux 7.0.0-42-generic / x86_64，GCC 13.3.0，C++20 Debug、
WarningsAsErrors；SQLite 3.53.4、kairo pin `78c6f5575331`。离线 Provider、
FileEventStore 与 SQLite，无真实模型网络或设备输入。准确复验命令：

```bash
cmake --preset debug
cmake --build build/debug -j 4
ctest --test-dir build/debug --output-on-failure -j 4
# 176/176 通过，无 skip。

mira_m30_targets=(
  mira_m30_scope_binding_test mira_m30_lifecycle_test
  mira_m30_recording_test mira_m30_lifecycle_faults_test
  mira_m36_context_admission_test mira_m36_memory_agent_test
  mira_m36_memory_agent_lifecycle_test mira_m36_memory_agent_model_test
  mira_m29_arbitration_test mira_m29_ledger_atomicity_test mira_m29_delivery_test
  mira_m29_lifecycle_test mira_m29_loop_interruption_test
  mira_m29_clarification_recovery_test mira_m29_restart_replay_test
  mira_m29_recovery_verification_test mira_m29_recovery_faults_test
  mira_m29_recovery_lifecycle_test mira_m28_lifecycle_test
  mira_m3_assistant_speech_test mira_m4_recovery_test
  mira_memory_agent_host_consumer mira_interaction_recovery_host_consumer
)
# 此 regex 对应以上 23 个测试；旧宿主验证未启用该功能时行为保持。
mira_m30_regex='mira_m30_|mira_m36_|mira_m29_|mira_m28_lifecycle_test|mira_m3_assistant_speech_test|mira_m4_recovery_test|mira_memory_agent_host_consumer_test|mira_interaction_recovery_host_consumer_test'
cmake --preset asan -DMIRA_ENABLE_UBSAN=ON
cmake --build build/asan --target "${mira_m30_targets[@]}" -j 2
ctest --test-dir build/asan -R "$mira_m30_regex" --output-on-failure -j 2
# ASAN+UBSAN 23/23 通过。
cmake --preset tsan
cmake --build build/tsan --target "${mira_m30_targets[@]}" -j 2
setarch x86_64 -R ctest --test-dir build/tsan -R "$mira_m30_regex" --output-on-failure -j 2
# TSan 23/23 通过；与 CI 一样禁用 ASLR。

for mira_unit in src/context/{memory_agent,memory_agent_binding,memory_agent_jobs,memory_agent_model,memory_agent_seams}.cpp src/interaction/{interaction_memory,interaction_coordinator,interaction_delivery}.cpp; do
  clang-tidy -p build/debug "$mira_unit" --quiet || exit 1
done
# 8 个生产编译单元严格检查通过，无 warning/error。
cmake --build build/debug --target format-check docs-check sbom-check platform-boundary-check architecture-check
git diff --check
# 全部通过；架构 baseline 保持 16，无新增豁免。
```

首轮测试编译修正了 fixture 对字段名的误用（proposed、requested_intent 和 provenance
非 optional），没有改变生产契约。新增擦除断言在首次旧构建产物上失败（175/176），
修正回执再校验并重编后全绿。短 deadline 测试允许在取得回执时已显式失败，避免用
“必须仍在途”的调度假设冒充超时语义。最终复查还将新绑定与 input_pending 的发布
放入同一提交锁，并让 bind/stage 共用结构及容量校验；上述全量、Sanitizer 与
8 单元静态检查均已在这次调整后重跑通过。最终验证日志：
`/tmp/mira-m30-{full,targeted}-{build,test}.log`、
`/tmp/mira-m30-{asan,tsan}-{configure,build,test}.log`、
`/tmp/mira-m30-tidy-*.log`、`/tmp/mira-m30-quality.log`；首轮回归另存
`/tmp/mira-m30-initial-full-test.log`。

本轮只关闭 M30-02/04 的 Linux 离线机制验收；其余工作项和完整 M30 退出条件继续
未完成。固定排序公式、承诺实体与自动纠正/删除状态机没有实现或被本轮隐式批准。

| 未执行/待完成项 | 原因与声明边界 | 负责人及补跑条件 |
| --- | --- | --- |
| M30 当前增量的远端 CI、Windows/MSVC、Android/NDK | PR #124 的 CI 只能证明旧基线；本分支尚未推送 | Mira Maintainers；提交后运行仓库 CI 全矩阵并保存 head/run 链接 |
| 真实模型召回/提取质量与真机长期运行 | 本轮使用脚本 Provider；不宣称语义准确率、长期资源增长或稳定性保证 | Mira Maintainers；配置受控端点、设备与标注工作负载，记录版本、样本与指标 |
| 持久化的记忆意图/审批/纠正/删除、承诺及价值信号 | 尚未交付完整用户交互及存储形态，M30-01/03/05/06/07/08 保持未完成 | Mira Maintainers；按 DEC-070 冻结剩余细则，补充跨重启与联合幂等测试后验收 |
| 同 scope 删除后的新知识接纳 | M36 屏障仍保守封闭；本轮没有移除屏障或自动重放旧事件 | Mira Maintainers；按 M30-09 冻结新世代及来源水位，与 M31 协同验收后开放 |
| 与宿主恢复 P3/Working Context 的组合 | supplier 的其他 owner 仍由宿主显式合并；不会自动覆盖已有回调 | Mira Maintainers/宿主集成者；合并候选及各自 current 探针，按实际宿主链路补端到端验证 |

### 2026-11-10：长期偏好审批增量开始

已同步 master `23f1291`（PR #125，含 DEC-108 原生角色召回），在
`feat/memory-preference-approval` 推进 M30-03/05/07/08。交付目标为：模型只提出
User Preference 候选，宿主持当前 grant 预览并按摘要确认/拒绝，经原 consolidation
与 apply_pending 写入；持久 Artifact/事件、scope 擦除和重开幂等闭合。
完整 M30 继续 In Progress。

### 2026-11-10：长期偏好协议验收

基线 `23f1291`，分支 `feat/memory-preference-approval` 的未提交工作树。
交付 DEC-109、强制待审批 policy 开关、User Preference 提案/预览/决定 API、
只可提案的模型工具，以及事件/Artifact 恢复与删除联动。没有修改 third_party、
没有新建线程/定时设施或第二套 IMemory 写入路径；到期测试用 kairo one-shot timer
并消费 future。DEC-108 的原生角色和带框事实召回继续保留。

验证对象：

| 测试 | 核心证据 |
| --- | --- |
| m30_preference_approval_test | 提案不写 MemoryRecord；当前用户按 digest 确认；纠正 HumanCorrection 仍先待审批；Supersede 版本链；重复不提升 Unverified；拒绝清理；禁止内容/注入过滤；模型无批准工具；事件不含正文 |
| m30_preference_security_test | user/tenant/identity_version/缺 grant/过期 grant/非精确 resource/Anonymous 拒绝；摘要不匹配拒绝；同 claim 两提案只允许当前版本；批准使旧请求快照失效；scope 外部失效和擦除阻止旧提案回写 |
| m30_preference_recovery_test | 提案、审批意图、完成回执三个窗口，写入前失败及写入后丢 ack 均重开 EventStore/SQLite/ArtifactStore 验证；弱 ack 封锁；重复确认只留一条记录；拒绝/擦除重建不可恢复正文 |
| m30_preference_lifecycle_test | 缺 deadline、取消、额度、到期（仅元数据）、关闭；取消发生在耐久 intent 后不写入，显式续办；Artifact commit 未知与中断后清理失败都封锁，最多一份未知 Artifact/封锁窗口 |

环境：Linux 7.0.0-42-generic / x86_64、GCC 13.3.0、C++20 Debug、
WarningsAsErrors；SQLite 3.53.4、kairo pin `78c6f5575331`。Provider 为离线 fixture，
没有真实模型端点、用户记忆数据或设备输入。可复验命令与最终结果：

```bash
cmake --preset debug
cmake --build build/debug -j 4
ctest --test-dir build/debug --output-on-failure -j 4
# 180/180，通过且无 skip。

mira_preference_targets=(
  mira_m30_scope_binding_test mira_m30_lifecycle_test
  mira_m30_recording_test mira_m30_lifecycle_faults_test
  mira_m30_preference_approval_test mira_m30_preference_security_test
  mira_m30_preference_recovery_test mira_m30_preference_lifecycle_test
  mira_m36_context_admission_test mira_m36_memory_agent_test
  mira_m36_memory_agent_lifecycle_test mira_m36_memory_agent_model_test
  mira_m29_arbitration_test mira_m29_ledger_atomicity_test mira_m29_delivery_test
  mira_m29_lifecycle_test mira_m29_loop_interruption_test
  mira_m29_clarification_recovery_test mira_m29_restart_replay_test
  mira_m29_recovery_verification_test mira_m29_recovery_faults_test
  mira_m29_recovery_lifecycle_test mira_m28_lifecycle_test
  mira_m3_assistant_speech_test mira_m4_recovery_test mira_m4_consolidation_test
  mira_m23_promotion_test mira_memory_agent_host_consumer mira_interaction_recovery_host_consumer
)
mira_preference_regex='mira_m30_|mira_m36_|mira_m29_|mira_m28_lifecycle_test|mira_m3_assistant_speech_test|mira_m4_(recovery|consolidation)_test|mira_m23_promotion_test|mira_memory_agent_host_consumer_test|mira_interaction_recovery_host_consumer_test'
cmake --preset asan -DMIRA_ENABLE_UBSAN=ON
cmake --build build/asan --target "${mira_preference_targets[@]}" -j 2
ctest --test-dir build/asan -R "$mira_preference_regex" --output-on-failure -j 2
# ASAN+UBSAN 29/29 通过，包含旧 consolidation/promotion 默认行为。
cmake --preset tsan
cmake --build build/tsan --target "${mira_preference_targets[@]}" -j 2
setarch x86_64 -R ctest --test-dir build/tsan -R "$mira_preference_regex" --output-on-failure -j 2
# TSan 29/29 通过，按 CI 方式禁用 ASLR。

for mira_unit in src/context/{memory_preference,memory_preference_storage,memory_preference_tool,memory_consolidation,memory_agent,memory_agent_seams}.cpp; do
  clang-tidy -p build/debug "$mira_unit" --quiet || exit 1
done
# 六个生产编译单元严格检查通过，无 warning/error。
cmake --build build/debug --target format-check docs-check sbom-check platform-boundary-check architecture-check
git diff --check
```

开发时修正了 JsonValue 的 const 对象访问/boolean API 名称与测试头引用；没有放宽
生产校验或测试断言。最后追加的审批前 TTL 重验已重新构建并全量通过。日志：
`/tmp/mira-m30-preference-{configure,core-build,targeted-build,targeted-test}.log`、
`/tmp/mira-m30-preference-full-{build,test}.log`、
`/tmp/mira-m30-preference-{asan,tsan}-{configure,build,test}.log`、
`/tmp/mira-m30-preference-tidy-*.log`、`/tmp/mira-m30-preference-quality.log`。

工作项仍按增量记录：M30-03 已有显式偏好持久路径，其他临时报表的统一收集不冒充
完成；M30-05 尚未交付完整自然语言意图/逐记录遗忘与前端 UI 集成，M30-01/06/09
的承诺、价值信号与删除后新世代仍未完成。没有改动已通过的 DEC-108 召回契约。

| 未执行/待完成 | 原因及负责人 | 补跑条件 |
| --- | --- | --- |
| Windows/Android 真实设备与模型运行 | CI 只覆盖 Windows 离线测试、Android 构建与交叉链接；Mira Maintainers/宿主集成者 | 在目标设备、固定模型与真实授权环境执行交互/取消/恢复矩阵，记录模型和设备版本 |
| 真实用户界面与模型选择偏好工具的语义质量 | Mirage UI/模型凭据不在本轮；宿主集成者/Mira Maintainers | 接入真实认证与确认 UI，验证“只授权保存”的文案、跨会话回执与操作取消，记录模型版本和标注场景 |
| OS 强杀/断电、长期容量与物理 GC | 本轮为关闭/重开及故障注入，未知注册 Artifact 的回收归 M31；Mira Maintainers | 目标平台故障矩阵、固定负载/时长与 Artifact/回执预算；不能将超时或到期当作已回收字节 |
| 完整记忆纠正/忘掉/承诺/新世代 | 首批仅覆盖 Preference 与 scope 屏障联动；Mira Maintainers | 后续 M30 增量冻结剩余契约，补精确范围、前端反馈、联合幂等与已删除来源不回灌测试 |

2026-11-10 CI 复核：原 master 的 GCC Release optional 警告与 Windows scope-budget
夹具清理已由 [PR #126](https://github.com/Linductor-alkaid/mira/pull/126) 修复。代码 head
`8585e49` 的两组 CI 合计 **24/24 job 通过**：Linux GCC/Clang Debug/Release、三套
sanitizer、Windows Debug/Release（各 180/180 离线测试）、Android 双架构构建与安装
消费交叉链接、quality 全部通过。具体 run、复现及历史分支核对见
[存储维护记录](maintenance-2026-11-memory-storage.md)。原“远端 CI/Windows/NDK 待补跑”
已由该证据补齐，真实设备/模型、强杀/断电及本里程碑其他工作项保持未完成。

### 2026-11-20：六类契约与独立召回组

基线为 `a70f10a`，分支 `feat/m30-memory-systems`。按维护者决定冻结人格、事件记忆、
社交对象画像、世界模型记忆、目标记忆和技能记忆的来源、写入、索引、纠正与失效边界。
依据为 DEC-119 与六类记忆设计。M30-10 通过 MemoryAgentConfig 的宿主配置将现有类型
分组查询，独立消耗记录和文本额度，在同一次受管工作内去重并按组发布 P4 候选。
SQLite 继续使用现有格式，实体和专用索引由 M30-11～16 各自验收。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake --build build/release -j 10
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 6 --target mira_m30_recall_groups_test \
  mira_m30_lifecycle_faults_test mira_m36_memory_agent_lifecycle_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_(recall_groups|lifecycle_faults)|m36_memory_agent_lifecycle)_test'
clang-tidy -p build/release --warnings-as-errors='*' \
  src/context/memory_agent.cpp src/context/memory_agent_jobs.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
```

Linux Release 全量 **196/196**，ASAN 定向 **3/3**。定向集成测试经
InteractionCoordinator 自动触发已授权工作的 Recall，以真实 SQLite 验证按类独立配额、
追加查询去重、跨租户及未配置类型排除、空结果、无效配置和快照失效。
宿主示例经 Gateway 使用分组召回。既有生命周期用例覆盖提交拒绝、异常、取消、超时、
关停与接管。格式、文档、SBOM、平台边界、架构检查及生产变更单元严格 clang-tidy 通过。

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 远端 CI、Windows、Android 和 Linux 其他 sanitizer | Mira Maintainers，分支提交后运行仓库完整矩阵，按对应 head 回填 |
| 真实模型召回质量和规模延迟 | Mira Maintainers 与宿主集成者，固定数据集、部署硬件、授权与模型配置后执行分类召回与配额矩阵 |
| 六类实体和专用索引 | Mira Maintainers，按 M30-11～16 实施并验证各自的读取、重启与失效路径 |

### 2026-11-20：人格目录与 Gateway 准入

在同一分支的 M30-10 基础上执行 M30-11。DEC-120 冻结宿主配置加载、按租户与 Agent
ID 解析、具体激活版本失效、完整请求预算及角色边界。宿主以现有授权和持久配置作为
事实源。Core 目录只保留有界的当前版本，不创建新任务、定时器或持久库。

模型请求的宿主指令前缀内加入 Internal System 人格项，原历史及工具选择保持原权限。
调用内的重试沿用已捕获人格，发送、预览和完成均复核版本与任务授权。失效完成结算用量
并丢弃正文、工具提议及续接。宿主示例展示生效配置从 v1 到 v2 后在正常请求中切换。
测试覆盖回滚不会复活旧快照、撤销、身份隔离、容量、预算和三种模型协议映射。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release
cmake --build build/release -j 10
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_persona_test \
  mira_m36_context_admission_test mira_m3_conversation_system_prompt_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_persona|m36_context_admission|m3_conversation_system_prompt)_test'
clang-tidy -p build/release --warnings-as-errors='*' \
  src/model/agent_persona.cpp src/model/model_gateway.cpp \
  examples/memory_agent_host_consumer.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
```

Linux Release 全量 **197/197**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、
SBOM、平台边界及架构门禁通过。首轮静态检查发现宿主示例的异常退出路径，已在示例
入口显式处理并复跑，异常仍按原 owner 顺序关闭 Executor。

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| Mirage 持久配置与已认证任务映射接线 | Mirage Maintainers，在商定产品配置格式及授权来源后接线，验证配置保存、重启恢复与回滚 |
| Windows/Android、远端 CI、其他 sanitizer | Mira Maintainers，提交分支后运行 CI 矩阵，并以相应 head 回填 |
| 在线模型人格效果 | Mira Maintainers 与宿主集成者，使用实际模型与固定场景验证配置合成和更新效果 |

### 2026-11-20：事件切片与 Record 取证水位

沿用 `feat/m30-memory-systems`。DEC-121 / M30-17 接通 M30-12 的证据源层。MemoryEventStore
按 Session、Session/Task、Session/type 构建有界位置索引，FileEventStore 从原日志校验
并重建。切片按会话序号分页，返回固定上界与已扫描水位，按墙钟过滤且跳过未知时间
的旧事件。每页的返回数、扫描位置和载荷字节独立设限。原日志格式与幂等追加语义延续。

MemoryAgent 的 Record 直接读取已绑定任务的证据，排除自己的审计记录。多页处理在
进程内的工作绑定上保留原上界，即使本轮有空页也推进扫描水位。旧适配器显式返回 UnsupportedCapability
时使用原有会话分页。读失败或后端水位不一致时作业失败，后续重试使用原水位。
擦除屏障继续阻止已删除 scope 在重开后重新绑定与写入。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release
cmake --build build/release -j 10
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_event_slices_test \
  mira_file_event_time_test mira_m36_memory_agent_lifecycle_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_event_slices|file_event_time|m36_memory_agent_lifecycle)_test'
clang-tidy -p build/release --warnings-as-errors='*' \
  src/storage/event_store.cpp src/context/memory_agent_jobs.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
```

Linux Release 全量 **198/198**、最终重开场景定向 **2/2**，ASAN 定向 **3/3**。
严格 clang-tidy、格式、文档、SBOM、平台边界和架构门禁通过。
定向覆盖跨 Session/Task、类型与时间条件、固定上界、空页、字节额度、重开和损坏
拒绝。记录测试在每页一条的条件下穿过同会话的其他任务事件和自身审计事件，并验证
后续追加、旧适配器、有错误的切片后端以及擦除后重开日志并重启服务。

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 持久会话片段索引和派生物联合擦除 | Mira Maintainers，按 M30-12 和 MS-04 冻结派生物存储、时间索引与水位恢复后补测 |
| Windows/Android、远端 CI、其他 sanitizer | Mira Maintainers，分支提交后运行 CI，按被测 head 回填 |
| 规模、驻留、分段归档与真实崩溃 | Mira Maintainers，在 M31 分段及回收契约与目标资料集就绪后补测 |

### 2026-11-23：持久会话片段与联合擦除

在 `feat/m30-memory-systems` 上按 DEC-122 实施 M30-12。SqliteConversationIndex 复用
会话投影、窗口分段和 StoreChannel。宿主以授权 Session 及稳定租户 scope 从原日志
重建固定水位，读取前及提交时核对会话归属与擦除屏障。SQLite 事务同时替换片段、
全文索引及水位。按会话与精确 scope 过滤，保留来源事件和原生对话角色。

MemoryAgent 把会话片段纳入已提交的 scope 擦除。索引失败时本实例封闭召回和重建，
既有长期记忆清理仍执行。定向用例覆盖文件重开、全文/字面匹配、租户隔离、降级、超时、
同水位冲突、容量回滚、内容门禁、已擦除源禁止重建，以及索引侧容量故障后的擦除重放。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release
cmake --build build/release -j 10
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 5 --target mira_m30_conversation_index_test \
  mira_m30_event_slices_test mira_m36_memory_agent_lifecycle_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_(conversation_index|event_slices)|m36_memory_agent_lifecycle)_test'
clang-tidy -p build/release --warnings-as-errors='*' \
  src/storage/sqlite_conversation_index.cpp src/storage/sqlite_support.cpp \
  src/storage/sqlite_memory_store.cpp src/context/memory_agent.cpp \
  src/model/conversation_log.cpp src/support/content_markers.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
```

Linux Release 全量 **199/199**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、
SBOM、平台边界和架构门禁通过。架构检查的私有头引用已改为公开内容标记检查接口。
一次构建期间更新源码使 Release 留下旧索引实现，强制重编后擦除前置检查与全量复跑均通过。

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 真实 Mirage 宿主通过认证绑定挂载 | Mirage Maintainers，在产品采用本库版本后验证启动重放顺序、索引刷新与授权所有权 |
| Windows/Android、远端 CI、其他 sanitizer | Mira Maintainers，分支提交后运行完整矩阵，按被测 head 回填 |
| 其他上下文资产、规模与联合物理回收 | Mira Maintainers，续按 MS-04/MS-06、M31 的负载与回收契约验证 |

### 2026-11-23：社交身份与别名索引

基线 `b7a958d`，分支 `feat/m30-memory-systems`。按 DEC-123 验收 M30-18。
`SqliteSocialDirectory` 按租户和社交环境保存稳定对象 ID、版本与别名。测试验证同名结果
在候选截断后仍标为歧义，跨租户和环境的同 ID 互不影响，同版本重试幂等，较高版本
原子替换别名。对象和环境屏障在重开后仍拒绝旧写；容量不足的擦除会停用当前目录，
宿主增容重开并补做擦除后恢复服务。M30-13 的属性级证据、时态与关系断言继续开放。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 10
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 5 --target mira_m30_social_directory_test \
  mira_m30_conversation_index_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_m30_(social_directory|conversation_index)_test'
clang-tidy -p build/release --warnings-as-errors='*' src/storage/sqlite_social_directory.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

Linux Release 全量 **200/200**，ASAN 定向 **2/2**。生产编译单元 clang-tidy 和格式、
文档、SBOM、平台边界、架构门禁通过。

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| Mirage 宿主认证绑定和接入 | Mirage Maintainers，采用目录 API 后验证授权、同名确认与启动时的擦除补做 |
| Windows/Android、远端 CI、其他 sanitizer | Mira Maintainers，提交并触发对应矩阵后按被测 head 回填 |
| 属性级证据、时态、关系断言及规模验证 | Mira Maintainers，M30-13 后续增量接入既有记忆提交管线后验收，并结合 M31 的容量和回收契约补测 |

### 2026-11-23：社交画像的字段断言与有效区间

在 `feat/m30-memory-systems` 上承接 M30-18，按 DEC-124 实施 M30-19。自述、用户确认和
推断分别保留来源和验证级别。对象、字段、来源与事件共同确定断言身份，重复提交保持
幂等，冲突正文或区间被拒绝。SQLite 迁移在同一事务中安装字段索引并更新 schema 版本。
宿主在授权对象范围内读取，可独立选择有效时点和记录时点。首轮回归发现通用文本去重
会合并异源断言，画像字段读取已保留这些断言。测试覆盖边界时点、停用和物理擦除、跨
租户与环境隔离、模型提案拒绝、文件重开，以及迁移失败回滚。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_social_profile_test \
  mira_m30_social_directory_test mira_m4_store_open_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_(social_profile|social_directory)|m4_store_open)_test'
for unit in src/context/{social_profile,memory_contracts,memory_consolidation,memory_agent_binding}.cpp \
  src/storage/sqlite_memory_store.cpp src/workflow/workflow_learning.cpp; do
  clang-tidy -p build/release --warnings-as-errors='*' "$unit" || exit 1
done
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

Linux Release 全量 **201/201**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、
SBOM、平台边界和架构门禁通过。日志见 `/tmp/mira-m30-profile-{full-test,asan-test,tidy,quality}.log`。

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 画像关系、目录与知识的联合擦除恢复、实时撤权 | Mira Maintainers，M30-13 接通关系端点和持久清理序列后，验证部分失败、重开重试及撤权在途结果 |
| Mirage 产品调用链 | Mirage Maintainers，在产品层绑定授权、来源事件和目录版本后，补写读与删除端到端测试 |
| Windows/Android、远端 CI、其他 sanitizer 与规模 | Mira Maintainers，提交并触发对应矩阵后按被测 head 回填，按 M31 目标工作负载验证限额和回收 |

### 2026-11-23：社交关系断言与双端点擦除

在 `feat/m30-memory-systems` 上承接 M30-19，按 DEC-125 实施 M30-20。有向关系 scope
绑定同一社交环境内的两个对象，读取由谓词与双时态筛选。共用断言的内容门禁、来源标注、
不可变提交和版本读取。SQLite schema 1.5 原子安装关系及端点索引，保留 1.4 的画像记录。
对象 scope 擦除同时清理该对象的出入关系。注入删除失败并重开，验证待擦除端点封闭关系
查询、按 ID 读取与提交。补做成功后，精确范围内的画像与关系历史消失，其他租户和对象
对可读。持久屏障写入失败显式返回错误，由拥有目录屏障的宿主收拢。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_social_relationship_test \
  mira_m30_social_profile_test mira_m30_social_directory_test mira_m4_store_open_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_social_(relationship|profile|directory)|m4_store_open)_test'
for unit in src/context/{social_profile,memory_contracts,memory_consolidation,memory_agent_binding}.cpp \
  src/storage/sqlite_memory_store.cpp src/workflow/workflow_learning.cpp; do
  clang-tidy -p build/release --warnings-as-errors='*' "$unit" || exit 1
done
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 目录与知识库之间的擦除恢复、实时撤权 | Mira Maintainers，在 M30-13 接通持久擦除序列后，验证跨库中断及撤权时的在途结果 |
| Mirage 产品调用链 | Mirage Maintainers，完成双端点授权、目录版本复核和来源映射后补测 |
| Windows/Android、远端 CI、其他 sanitizer 与规模 | Mira Maintainers，提交后执行目标矩阵，按被测 head 回填；M31 负载就绪后验证容量和回收 |

Linux Release 全量 **202/202**，ASAN 定向 **4/4**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-rel-{full-test,asan-test,tidy,quality}.log`。

### 2026-11-23：社交目录与记忆库的擦除恢复

在 `feat/m30-memory-systems` 上承接 M30-20，按 DEC-126 实施 M30-21。目录 schema 1.1
在同一事务登记擦除意图并持久封闭目标；记忆库 schema 1.6 增加精确环境擦除索引。
启动通过持久库 ID 验证配对，恢复未结算擦除后才开放目录。对象擦除沿已有端点索引，
环境擦除覆盖目录中没有当前版本的画像与关系，保留其他环境和租户的内容。
故障注入覆盖记忆清理失败、最终回执失败和超时后已接纳的清理继续执行。升级测试保留
旧目录的别名和对象身份。错误配对和恢复失败均返回错误。本轮验证使用正常关闭与重开。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_social_erasure_recovery_test \
  mira_m30_social_relationship_test mira_m30_social_profile_test \
  mira_m30_social_directory_test mira_m4_store_open_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_social_(erasure_recovery|relationship|profile|directory)|m4_store_open)_test'
for unit in src/context/{social_profile,memory_contracts}.cpp \
  src/storage/{sqlite_memory_store,sqlite_social_directory}.cpp; do
  clang-tidy -p build/release --warnings-as-errors='*' "$unit" || exit 1
done
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| Mirage 产品授权和实时撤权接线 | Mirage Maintainers，在产品接通稳定对象 ID、当前目录版本和在途访问收拢后补端到端测试 |
| 强杀、断电、备份恢复及大环境清理 | Mira Maintainers，目标故障环境与 M31 负载及回收契约就绪后验证 |
| Windows/Android、远端 CI、其他 sanitizer | Mira Maintainers，提交后执行目标矩阵并按被测 head 回填 |

Linux Release 全量 **203/203**，ASAN 定向 **5/5**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-cross-erase-{full-test,asan-test,tidy,quality}.log`。

### 2026-11-23：世界事实的结构化索引与新鲜度

基线 `20d1579`，分支 `feat/m30-memory-systems`。按 DEC-127 实施 M30-22。宿主将
已提交的观察转换为独立 `EnvironmentFact` 或 `ApplicationFact` 记录。SQLite schema
1.7 按精确 scope、实体与属性索引。查询可独立选择有效时点及记录时点。最新观察过期
时省略正文并要求重新观察，即使旧观察有效期更长。相同值的新观察保有自己的时态和证据。
验证覆盖延迟写入、重复与冲突提交、歧义、重开、租户/环境/应用隔离、模型写入拒绝和
作用域擦除。迁移测试覆盖新索引和 schema 标记的联合回滚。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_world_fact_test \
  mira_m30_social_erasure_recovery_test mira_m4_store_open_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_(world_fact|social_erasure_recovery)|m4_store_open)_test'
for unit in src/context/{world_fact,memory_contracts,memory_agent_binding,memory_consolidation}.cpp \
  src/storage/sqlite_memory_store.cpp; do
  clang-tidy -p build/release --warnings-as-errors='*' "$unit" || exit 1
done
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 运行时重新观察及 AppModel 版本联动 | Mira Maintainers，M30-14 接通真实观察入口后验证过期、应用更新和权限撤销时不会依旧事实推进动作 |
| 生产宿主、目标平台与规模验证 | Mira Maintainers 和宿主集成者，产品接通稳定环境/实体键后按被测 head 执行 Windows/Android、远端 CI 和 M31 负载验证 |

Linux Release 全量 **204/204**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-world-{full-test,asan-test,tidy,quality}.log`。

### 2026-11-23：导航重新观察与 AppModel 版本联动

在 `feat/m30-memory-systems` 上承接 M30-22，按 DEC-128 实施 M30-23。WorkflowRuntime
可安装导航世界事实绑定，以已安装模型摘要选择应用 scope。SQLite 时间证据须获现行读屏
佐证，否则 Runtime 用 Executor 调用 `IEnvironment::observe` 并依新观察规划；到达验证
也读取新观察。模型世代、授权、任务世代和截止在工具派发入口复核。AppModel 置信度回写
不会改变当前安装的 scope，旧世代的在途动作结果不会回写新模型。

在 SQLite 与 Simulator 组合中验证过期、当前读屏佐证、新模型隔离、识别缺失、取消、
观察期间人工接管、工作线程入口撤权和在途换版。未安装世界事实绑定的导航以既有
M12 测试复验。生产宿主的绑定和真机识别作为单独验证项保留。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_navigation_world_test \
  mira_m12_navigation_test mira_m30_world_fact_test mira_m9_control_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_(world_fact|navigation_world)|m12_navigation|m9_control)_test'
for unit in src/context/world_fact.cpp src/workflow/workflow_runtime.cpp; do
  clang-tidy -p build/release --warnings-as-errors='*' "$unit" || exit 1
done
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 产品授权及真机观察识别 | Mira Maintainers 与宿主集成者，在宿主接通授权探针及目标设备识别器后执行更新、撤权、重新观察与到达测试 |
| Windows/Android、远端 CI、其他 sanitizer | Mira Maintainers，提交后执行目标矩阵并按 head 回填 |

Linux Release 全量 **205/205**，ASAN 定向 **4/4**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-nav-{full-test,asan-final,tidy,quality}.log`。

### 2026-11-23：一次性任务与承诺的持久状态

基线 `b49f6e1`，分支 `feat/m30-memory-systems`。按 DEC-129 完成 M30-24。独立
SQLite 目标库按租户、用户与稳定 ID 接纳任务及承诺，独立提供状态列表及到期页。待完成
额度和全库额度分别控制。结算携带事件证据并使用版本校验，擦除清理正文并持久保留 ID
屏障。测试覆盖无截止任务、同截止时间分页、精确归属、并发终态、过期、重开、旧 ID
拒绝和关闭后的请求。目标库复用 Executor StoreChannel。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_goal_store_test \
  mira_m30_social_erasure_recovery_test mira_m4_store_open_test
ctest --test-dir build/asan --output-on-failure \
  -R 'mira_(m30_(goal_store|social_erasure_recovery)|m4_store_open)_test'
clang-tidy -p build/release --warnings-as-errors='*' src/storage/sqlite_goal_store.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

Linux Release 全量 **206/206**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、
SBOM、平台边界和架构门禁通过。日志见 `/tmp/mira-m30-goal-{full-test,asan-test,tidy,quality}.log`。

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 重复计划、纠正及提醒计数 | Mira Maintainers，M30-15 冻结重复计划与触发身份后，验证重启、变更及取消 |
| 主动派发、生产宿主授权及总量治理 | Mira Maintainers 和宿主集成者，按 M33 派发及 M31 预算接入后执行端到端用例 |
| Windows/Android、远端 CI 与其他 sanitizer | Mira Maintainers，提交后按目标矩阵和被测 head 回填 |
