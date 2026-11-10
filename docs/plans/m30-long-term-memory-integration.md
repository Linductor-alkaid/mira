# M30：长期记忆接入——提取、检索、纠正、删除与回忆反馈

> 状态：In Progress（DEC-107 首批 scope/生命周期增量；其余工作项继续细化）
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
> 更新日期：2026-11-10（开始 DEC-107 主体 scope 与交互记忆生命周期接线）

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

## 2. 范围与非目标

### 2.1 范围

- 协调层与 `IMemory` 之间的**记忆选择**接线：按当前工作召回、投影、装配。
- 记忆**候选**的提出与经既有 `MemoryConsolidator` 管线的写入路径。
- 「以后记住……」「忘掉……」的**意图路由**与作用范围判定。
- 主体身份（`PrincipalContext.user_id`）与记忆 ACL 域（`MemoryScope.subject_id`）
  之间的**映射规则**落地（DEC-046 登记但未提供）。
- 可选的回忆反馈与使用信号增量。

### 2.2 非目标

- **不做** 向量索引生产化：`IEmbeddingProvider` 生产实现、ANN 索引
  （当前向量腿是**有界线性余弦扫描**，`max_vector_scan=1'024`，
  [`include/mira/sqlite_memory_store.hpp:26`](../../include/mira/sqlite_memory_store.hpp#L26)）。
- **不做** 容量准入、归档与遗忘（归 M31）。
- **不做** 记忆中的授权描述替代当前有效授权：记忆里的 grant 描述**不能**当作授权来源。
- **不做** 跨 session/tenant 泄漏；跨会话召回继续服从**精确 scope/tenant ACL**。
- **不做** UI 的记忆管理界面（Mirage 负责）。
- **不新增**第二套记忆写入路径：写入**必须**走既有 consolidation / promotion 管线。

## 3. 设计与决策依据

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

- [ ] `M30-01` 立项与跑前冻结（DEC-107 首批已冻结；承诺、完整记忆意图、价值信号与删除世代细则待冻结）。
- [x] `M30-02` 主体 → MemoryScope 映射规则与租户隔离落地（User/Session/Application 内部摘要，自动 User + 当前 Session 白名单，当前授权及归属校验；Linux 离线验收）。
- [ ] `M30-03` 记忆候选提出与经既有管线的写入路径（终态 Session Record 已接通；User 长期记忆意图、审批交互及其余触发仍待完成）。
- [x] `M30-04` 按当前工作的记忆选择与上下文装配接线（目标/最新约束、队列接纳边界、Gateway P4 候选、Layer 0 准入与失效探针；Linux 离线验收）。
- [ ] `M30-05` 「记住 / 忘掉 / 纠正」的路由、范围判定与可解释输出。
- [ ] `M30-06` 价值信号（importance / 使用信号）产出（供 M31 使用）。
- [ ] `M30-07` IVA 测试矩阵（首批四组通过；承诺/完整纠正删除/价值信号/新世代矩阵待补）。
- [ ] `M30-08` 文档同步（DEC-107 首批设计/API/术语/计划已同步，完整里程碑收口仍待后续增量）。
- [ ] `M30-09` 冻结并实现删除后的同 scope 新知识接纳：区分新世代与已删除来源，
      旧事件、摘要和在途提案不得回灌；有新的明确用户输入时可重新记忆。与 M31
      的屏障保留/回收协同验收，不能直接删除 M36 的屏障来重新开放 scope。

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
