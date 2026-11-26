# M30 验收与验证记录

> 状态：In Progress
> 更新日期：2026-11-26
> 负责人：Mira Maintainers
> 所属计划：[M30 长期记忆接入](m30-long-term-memory-integration.md)
> 验收依据：[DEC-070](../decisions/DEC-070-long-term-memory-behavior-contract.md) 与
> [DEC-156](../decisions/DEC-156-m30-implementation-freeze.md)

## 1. 当前验收矩阵

验收按已验证的行为和环境登记。以下证据覆盖 Linux 离线范围。完整工作项状态以
[M30 工作项](m30-long-term-memory-integration.md#5-工作项)为准，各增量的命令、结果
与适用边界见[历史验收记录](#4-历史验收记录)。

| 门禁 | 当前证据 | 未完成的验收及归属 |
| --- | --- | --- |
| `M30-G1` 候选经既有写入管线 | Record、Episode、社交属性与有向关系、偏好审批、世界观察、技能提示均有合并或类型化写入证据。见[写入验收索引](#evidence-recording) | `M30-03` 补余下类型化入口的候选与处置报告，并验证直写绕过的拒绝或标记。`M30-38` 验收统一自动触发及六类路由 |
| `M30-G2` 重复提及不抬高可信度，验证等级具备证据链 | Linux 离线已验收。`mira_m30_preference_approval_test` 同一主张重复提出仍为 Unverified。`mira_m4_write_evolution_test` 验证独立证据合并、严格晋升与防降级。见[偏好记录](#history-preference)和[写入演进](maintenance-2026-11-write-evolution.md) | — |
| `M30-G3` 主体映射与范围隔离 | Linux 离线已验收。`mira_m30_scope_binding_test` 验证 User、Session、Application 的稳定映射及主体摘要。`mira_m30_lifecycle_test` 验证跨 Session 仅复用授权用户偏好，并隔离其他用户、租户和 Session 范围。见[生命周期记录](#history-lifecycle) | — |
| `M30-G4` 承诺、到期与状态解释 | 存储创建、纠正、取消、擦除、到期索引和任务完成证据对账已有覆盖，见[目标验收索引](#evidence-goals) | `M30-37` 验收到期承诺的生产调度和恢复闭环，`M30-05` 验收面向用户的状态与证据解释 |
| `M30-G5` 纠正、删除与发起者区分 | 已覆盖显式偏好批准、版本化纠正、范围擦除及恢复，见[偏好记录](#history-preference)。社交与会话已分别验收联合擦除 | `M30-05` 补逐记录遗忘、明确意图及四类发起者各自结算的回执。与 M31 联验容量回收、归档和 Erasure hold |
| `M30-G6` 删除解释与失败正文 | 已覆盖擦除后拒绝旧结果及旧证据。失败轮次的正文和结果分别保存，见[会话结算维护验收](maintenance-2026-11-conversation-outcomes.md) | `M30-05` 验收完整删除说明，`M30-09` 验收同 scope 新世代和对旧来源、摘要及在途结果的阻断 |
| `M30-G7` 记忆内容无授权效力 | `mira_m30_lifecycle_test` 覆盖当前 grant 检查，`mira_m30_preference_security_test` 覆盖越域及撤权拒绝，`mira_m36_context_admission_test` 覆盖候选准入的低权限边界 | `M30-07` 保留带授权字样的记忆与缺失当前授权组合场景的端到端验收 |
| `M30-G8` 架构与契约同步 | 已实施增量持续通过本地文档、架构及平台边界检查 | `M30-08` 对照最终实现收口决策、设计、API、术语与总计划，按同一最终 Git head 收集目标矩阵 |

[Issue #111 的偏好场景](https://github.com/Linductor-alkaid/mira/issues/111)由
`M30-07` 汇总验收。Linux 离线已有批准、纠正与范围擦除的分段证据，逐记录遗忘与
统一意图闭环仍归 `M30-05`。范围隔离和重复提及的结果分别归入 G3 和 G2。

## 2. 按交付能力查证

表中子项的完成状态由计划记录。汇总项引用这些证据。各项曾记录的延期和已补跑结果保留在
历史段落，当前统一补跑条件见第 3 节。

| 能力 | 工作项 | 已执行证据 |
| --- | --- | --- |
| 实施细则、主体绑定、低权限候选准入 | M30-01 / M30-02 / M30-04 | [生命周期](#history-lifecycle)、[实施细则与 scoped Record](#history-record-cursors)，以及 [M36 机制验收](m36-memory-agent-context-integration.md) |
| 偏好批准与版本化纠正 | M30-05 已交付部分 | [偏好审批、恢复与擦除测试](#history-preference) |
| 六类契约、人格目录、事件切片及会话资产索引 | M30-10 / M30-11 / M30-12 / M30-17 | [六类与人格](#history-systems)、[事件切片](#history-event-slices)、[持久会话片段](#history-conversation-index) |
| 社交目录、属性、关系及联合擦除 | M30-13 汇总 M30-18～21 | [身份、属性、双端关系与配对擦除](#history-social-foundation) |
| 世界事实及重新观察 | M30-14 汇总 M30-22 / M30-23，M30-03 接通验证事件写入 | [结构化事实、时间和导航重新观察](#history-world)、[验证事实写入](#history-verified-world-recording) |
| 目标持久层 | M30-15 汇总 M30-24～28，兑现证据来自 M30-46 | [目标和计划存储、授权、纠正及提醒回执](#history-goals)、[派发与兑现](#history-goal-dispatch) |
| 技能目录、带版本的恢复经验与提示 | M30-16 汇总 M30-29～34，M30-03 接通技能提示候选报告 | [技能发布、撤回、版本、授予范围及故障续办](#history-skills)、[技能提示候选报告](#history-skill-hint-candidate-report) |
| 社交来源账号与消息归因、自动提及、候选写入 | M30-35 / M30-40～42，M30-03 与 M30-36 的已交付部分 | [来源账号、消息证据和正文提及](#history-social-recording)、[属性](#history-social-attributes)、[有向关系](#history-social-relations)、[前序指代双证据](#history-social-references) |
| 到期派发与已确认事实恢复 | M30-43～52，M30-37 的已交付部分 | [待派发、重复计划、兑现、只读对账、准入和完成证据](#history-goal-dispatch) |
| Record 候选与持续处理 | M30-39，M30-03 与 M30-38 的已交付部分 | [多页提取](#history-social-recording)、[进度持久化](#history-record-cursors)、[Episode 与分类处置](#history-loop-episodes) |

<a id="evidence-recording"></a>

写入门禁的主要证据来自 [Record 与循环事件](#history-loop-episodes)、[偏好](#history-preference)、
[社交候选](#history-social-attributes)、[验证事实写入](#history-verified-world-recording)及
[技能提示](#history-skill-hint-candidate-report)。各自沿对应的范围与证据规则验收。

<a id="evidence-goals"></a>

目标门禁的存储与运行时证据分别见[目标存储](#history-goals)和[派发及兑现](#history-goal-dispatch)。

## 3. 里程碑关闭与补跑

`M30-07` 管理验收覆盖和环境证据，`M30-08` 管理最终契约同步。所有实施项、门禁、
目标宿主场景和对应矩阵的复跑证据齐备后才关闭 M30。

| 未执行或待完成 | 负责人 | 补跑条件 |
| --- | --- | --- |
| 当前分支的完整远端 CI，包括 Windows/MSVC、Android/NDK 及仓库指定 sanitizer 组合 | Mira Maintainers | 推送被测提交并触发完整平台矩阵，保存同一 head 的 CI run 与平台结果。早期偏好主干 CI 只覆盖其被测版本 |
| 社交账号推断、多方指代和生产事件触发 | Mira Maintainers、Mirage 宿主集成者 | 使用 QQ/B 站合成双来源和生产 Adapter 场景验收 M30-36，覆盖多方群聊、同人多账号、同名不同人、反证纠正、跨环境隔离与擦除 |
| 技能、世界事实和目标的真实宿主联动 | Mira Maintainers、宿主集成者 | 接通生产授权及技能目录后补 M30-16 的宿主和真实环境验证。验证世界事实过期重观，并在 M30-37 检查漏触发、重复、取消、纠正、兑现和进程退出恢复 |
| 价值信号进入容量策略 | Mira Maintainers | M30-06 提供确认重要性及 Gateway 实际准入信号后，复跑正常准入、幂等重放、失败与擦除，再由 M31 验证策略读取 |
| 真实模型的提取、巩固、召回质量及长会话稳定性 | Mira Maintainers、宿主集成者 | 固定端点、模型、测试语料与真实工作负载，提交可复跑的准确率、延迟及稳定性结果 |
| 删除生命周期与后续里程碑接缝 | Mira Maintainers | M30-05 接通逐记录遗忘、M30-09 接通新世代后复跑。容量回收不解除擦除屏障由 M31 联验。M33 消费到期派发；其主动预算与新的主动行为准入独立验收 |

### 2026-11-25 计划与证据复核

以 `c0d572a` 为代码基线，在 Linux Debug 运行
`ctest --test-dir build/debug -j 10 --output-on-failure`，通过 214/214。按子项证据核对
M30-13～16 的实现范围，按门禁条件核对 G2 和 G3 的现有离线覆盖。
`cmake --build build/debug --target docs-check` 通过。逐项校对了 52 个工作项编号、8 个门禁、
45 节搬移的历史记录以及计划与验收之间的链接。Windows、Android、真实宿主及模型的当前
分支结果仍待上表条件满足后取证。

## 4. 历史验收记录

以下保留各增量当时的基线、测试命令、结果和未执行说明。记录中的待办与平台覆盖只对应
当时的被测范围。当前验收状态见第 1 至 3 节。

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

<a id="history-lifecycle"></a>

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

<a id="history-preference"></a>

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

<a id="history-systems"></a>

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

<a id="history-event-slices"></a>

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

<a id="history-conversation-index"></a>

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

<a id="history-social-foundation"></a>

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

<a id="history-world"></a>

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

<a id="history-goals"></a>

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

### 2026-11-23：重复计划与持久触发回执

基线 `fde5a4f`，分支 `feat/m30-memory-systems`。按 DEC-130 完成 M30-25。目标库
schema 1.1 接入固定 UTC 间隔计划，按归属和下一次触发时间查询。回执由计划 ID、版本
和预定时间确定，和下次到期时间同事务提交。停机后可按原预定顺序逐次补登。

测试覆盖旧库迁移并保留一次性目标、租户隔离、到期分页、并发重试、故障回滚、重开后
回执恢复、停机积压、换版与取消隔离、擦除和额度。测试用 SQLite 触发器拒绝时间更新，
验证回执写入一并回滚。回执额度达到上限时保持原到期时间，擦除释放回执额度。

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

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 一次性目标纠正、提醒计数及日历计划 | Mira Maintainers，后续冻结时区、计划修改与提醒证据后验收 |
| 实际派发、产品授权和回执回收 | Mira Maintainers 与宿主集成者，M33 接入当前授权和动作准入，M31 接入容量与回收后补测 |
| Windows/Android、远端 CI 与其他 sanitizer | Mira Maintainers，提交后运行对应矩阵并按被测 head 回填 |

Linux Release 全量 **206/206**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-recurring-{full-test,asan-test,tidy,quality}.log`。

### 2026-11-24：一次性目标纠正与提醒回执

基线 `8e68713`，分支 `feat/m30-memory-systems`。按 DEC-131 实施 M30-26。目标库
schema 1.2 保存此前的目标定义及提醒回执。纠正以期望版本和新来源事件提交，旧定义、
新期限和版本在同一事务落盘。提醒计数从送达证据计算，重试、纠正和结算均不重复计数。
擦除同时清理正文、定义历史与提醒回执并保留 ID 屏障。

测试覆盖从 1.0 和 1.1 迁移、旧修订上限放宽、迁移标记失败后重试、纠正历史回滚、
变更后的到期页、竞争登记、提醒与纠正额度、终态、擦除失败回滚、重开、租户隔离及
同 ID 的重复计划不受一次性目标擦除影响。

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

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 日历及时区计划 | Mira Maintainers，冻结日历规则与夏令时行为后验证 |
| 提醒送达、主动派发与生产授权 | Mira Maintainers 和宿主集成者，宿主及 M33 接入当前授权和送达回执后验证 |
| Windows/Android、远端 CI 与其他 sanitizer | Mira Maintainers，提交后运行对应矩阵并按被测 head 回填 |

Linux Release 全量 **206/206**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-goal-history-{full-test,asan-test,tidy,quality}.log`。

### 2026-11-24：日历计划与持久时区规则

基线 `adb1d1c`，分支 `feat/m30-memory-systems`。按 DEC-132 实施 M30-27。目标库
schema 1.3 支持以本地时分和星期计算 UTC 触发，将宿主提供的时区快照和规则版本与计划
一同保存。夏令时缺口跳过，回拨选较早解释。规则耗尽后计划保留历史回执，通过换版恢复。

测试使用合成偏移规则，覆盖首次下界、星期选择、缺口、回拨重复时刻、快照不足、旧库
迁移、归属隔离、重开、规则耗尽、版本失效与擦除。已有固定间隔计划和其他目标记录保留。

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
for unit in src/context/goal_calendar.cpp src/storage/{sqlite_goal_store,sqlite_goal_calendar}.cpp; do
  clang-tidy -p build/release --warnings-as-errors='*' "$unit" || exit 1
done
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 宿主时区数据、送达及当前授权 | Mira Maintainers 与宿主集成者，产品接通实际时区数据源、提醒送达及 M33 派发入口后验证 |
| Windows/Android、远端 CI 与其他 sanitizer | Mira Maintainers，提交后运行对应矩阵并按被测 head 回填 |

Linux Release 全量 **206/206**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-calendar-{full-test,asan-test,tidy,quality}.log`。

### 2026-11-24 目标存储的当前授权

基线 `93f4768`，分支 `feat/m30-memory-systems`。按 DEC-133 实施 M30-28。目标库新增
`open_authorized`。授权查询与实际存储访问在同一个现有 Executor StoreChannel 内相继执行。
按归属、资源、操作及单条记录 ID 授权，列表与到期筛选分别核验。空探针在开库之前拒绝。

`mira_m30_goal_store_test` 使用可撤回的宿主策略验证租户、用户、记录和操作的隔离，覆盖
读取、旧记录和到期分页、写入和回执重试的撤权、探针异常及闭库。用已有回执的原输入重试
仍须通过授权；重新获准后回执可幂等读取或登记。存储 schema 沿用 1.3。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令如下。

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

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 产品级身份与权限、时区数据、提醒送达和 M33 派发 | Mira Maintainers 与宿主集成者，接通真实会话身份、当前授权数据源、时区数据及派发入口后，使用真实主体验证撤权与送达 |
| Windows/Android、远端 CI 与其他 sanitizer | Mira Maintainers，提交后运行对应矩阵并按被测 head 回填 |

Linux Release 全量 **206/206**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-goal-auth-{test,asan-test,tidy,quality}.log`。

<a id="history-skills"></a>

### 2026-11-24 当前技能发布的记忆读取

基线 `907d3fa`，分支 `feat/m30-memory-systems`。按 DEC-134 实施 M30-29。新增
`WorkflowRuntime::recall_skill_procedure`。宿主提供当前授权，目录确定当前版本；
`MemoryQuery::record_id` 在既有 SQLite 记忆查询中按主键过滤，沿用 exact scope 和擦除屏障。
复用现有投影写入与持久记录身份。升级后等新版同步再返回，旧版可单独查证。

M7 技能运行接线用 SQLite 验证目录与记忆同步、精确租户隔离、重开、升级、旧版本拒绝、
失效记录、内容不一致、墓碑、读取期间撤回与撤权、学习绑定重载，以及运行时关闭。既有
M7 技能执行测试继续验证已撤回的技能注册无法执行。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 8
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m7_tool_runtime_skill_test mira_m4_memory_retrieval_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_(m7_tool_runtime_skill|m4_memory_retrieval)_test$'
clang-tidy -p build/release --warnings-as-errors='*' src/workflow/tool_runtime_wiring.cpp \
  src/context/memory_contracts.cpp src/storage/sqlite_memory_store.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| `SkillHint` / `RecoveryLesson` 的版本及环境检索、同一发布写入多个 scope | Mira Maintainers，冻结索引键与现有写入身份的兼容方式后验收 |
| 生产授权、目录重载、Windows/Android 与远端 CI | Mira Maintainers 与宿主集成者，接入真实策略/发布目录后验证，提交后运行对应平台矩阵 |

Linux Release 全量 **206/206**，ASAN 定向 **2/2**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-skill-{test,asan-test,tidy,quality}.log`。

### 2026-11-24 技能发布的范围化写入

基线 `c1d8f5a`，分支 `feat/m30-memory-systems`。按 DEC-135 实施 M30-30。新技能发布投影
按规范化学习 scope 和发布版本派生记录与 mutation ID。SQLite 使用原有 Add 事务保留旧
回执语义，读取先查新身份，再在同一授权范围查旧身份。既有自定义后端继续用旧身份。

M7 技能运行接线使用两个 SQLite 写入连接验证同一发布在不同 scope 的并发同步、旧投影
保留、跨租户隔离、两类身份擦除后重开与继续同步，以及旧写入方遇到保留回执。M4 SQLite
测试验证新写入失败时旧身份预留随事务回滚，并覆盖旧回执归属缺失时拒绝写入。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m7_tool_runtime_skill_test mira_m4_sqlite_memory_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_(m7_tool_runtime_skill|m4_sqlite_memory)_test$'
clang-tidy -p build/release --warnings-as-errors='*' src/workflow/tool_runtime_wiring.cpp \
  src/context/memory_legacy_add.cpp src/storage/sqlite_memory_store.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| `SkillHint` / `RecoveryLesson` 的版本和环境检索 | Mira Maintainers，冻结经验索引键并接通学习和读取入口后验收 |
| 生产宿主及 Windows/Android、远端 CI | Mira Maintainers 与宿主集成者，接入真实策略和目录后验证，提交后运行对应平台矩阵 |

Linux Release 全量 **206/206**，ASAN 定向 **2/2**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-skill-scope-{test,asan-test,tidy,legacy-tidy,quality}.log`。

### 2026-11-24 按版本和失败键读取恢复经验

基线 `de2ae8a`，分支 `feat/m30-memory-systems`。按 DEC-136 实施 M30-31。SQLite 1.8
对现有恢复经验投影建立部分表达式索引，按学习 scope、工作流版本和完整失败键匹配。
宿主入口复用学习检索额度，读取前后复核授权，拒绝跨绑定结果。

`mira_m13_learning_persistence_test` 用带数据的 1.7 文件验证迁移、记录 ID 和来源、
同一工作流的多个版本和不同失败原因、步骤 ID、步骤类型及空步骤，覆盖纯文本旧记录、
跨租户授权、读取中撤权、学习绑定变化和擦除。`mira_m4_store_open_test` 覆盖索引创建
与版本戳写入的回滚及正常迁移。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m13_learning_persistence_test mira_m4_store_open_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_(m13_learning_persistence|m4_store_open)_test$'
clang-tidy -p build/release --warnings-as-errors='*' src/workflow/workflow_recovery_memory.cpp \
  src/context/memory_recovery_key.cpp src/context/memory_contracts.cpp src/storage/sqlite_memory_store.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 自动失败升级接入、`SkillHint` 和独立环境索引 | Mira Maintainers，冻结版本与环境迁移语义后验证真实回忆场景 |
| 生产宿主、Windows/Android 与远端 CI | Mira Maintainers 与宿主集成者，接入真实权限后验证，提交后运行各平台矩阵 |

Linux Release 全量 **206/206**，ASAN 定向 **2/2**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-recovery-{test,asan-test,tidy,quality}.log`。

### 2026-11-24 自动失败升级接入精确恢复经验

基线 `d58c315`，分支 `feat/m30-memory-systems`。按 DEC-137 实施 M30-32。失败驱动的
WaitingAgent 先读当前版本和失败键的恢复经验，再使用剩余条数、文本和软截止读 Episode。
两次读取间复核载体和学习绑定，返回前检查等待世代，绑定或世代变化使装配结果失效。

`mira_m13_learning_persistence_test` 使用 SQLite 验证旧版本和其他失败原因无法挤占额度，
覆盖条数、共享文本预算、擦除后的 Episode 回补、学习绑定换版和零时间预算。
`mira_m13_learning_test` 覆盖精确查询失败后的回补、读取中替换绑定、撤回载体和检查点
跳过读取。`mira_m14_recovery_test` 复验续跑、队列与模型上下文装配。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m13_learning_test \
  mira_m13_learning_persistence_test mira_m14_recovery_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_(m13_(learning|learning_persistence)|m14_recovery)_test$'
clang-tidy -p build/release --warnings-as-errors='*' src/workflow/workflow_runtime.cpp \
  src/workflow/workflow_recovery_memory.cpp src/storage/sqlite_memory_store.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| `SkillHint` 和独立环境索引 | Mira Maintainers，冻结技能经验写入证据和环境归属后验证 |
| 生产宿主、真实模型和 Windows/Android | Mira Maintainers 与宿主集成者，接入实际运行设置后复跑故障、模型与平台矩阵 |

Linux Release 全量 **206/206**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-escalation-{test,asan-test,tidy,quality}.log`。

### 2026-11-24 记录和读取带技能调用证据的提示

基线 `904d8de`，分支 `feat/m30-memory-systems`。按 DEC-138 实施 M30-33。只接受当前
学习绑定中成功返回的技能子 run 作为来源。记录通过 consolidation 校验，使用结算事件
及按 scope、发布摘要和 run 派生的稳定身份。SQLite 沿现有 scope、类型和标签检索，
读取前后复核授权和发布版本。

`mira_m7_tool_runtime_skill_test` 使用 SQLite 验证普通 Workflow 运行不能作为技能调用
证据、读写授权分离、指令形态拒绝、首次提示与持久回执、擦除后重试、重开、跨租户范围、
查询中撤权和发布换版后的失效。`mira_m4_consolidation_test` 复验既有候选处理。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m7_tool_runtime_skill_test mira_m4_consolidation_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_(m7_tool_runtime_skill|m4_consolidation)_test$'
clang-tidy -p build/release --warnings-as-errors='*' src/workflow/workflow_skill_hint.cpp \
  src/workflow/tool_runtime_wiring.cpp src/workflow/workflow_runtime.cpp src/context/memory_consolidation.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 独立环境索引与自动提示提取 | Mira Maintainers，冻结环境地址和候选来源规则后验证 |
| 生产宿主、真实模型和 Windows/Android | Mira Maintainers 与宿主集成者，接入实际策略后复跑模型和平台矩阵 |

Linux Release 全量 **206/206**，ASAN 定向 **2/2**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-hint-{test,asan-test,tidy,quality}.log`。

### 2026-11-24 技能记忆按工作和社交环境隔离

基线 `4d17d2d`，分支 `feat/m30-memory-systems`。按 DEC-139 实施 M30-34。宿主可配置
独立 `SkillEnvironment` scope，隔离发布投影、提示和恢复经验。事件范围继续独立配置。
现有 SQLite 范围及失败键索引承载查询。Memory 记录 schema 1.6 支持环境范围，旧记录
保留原身份。环境、事件范围或后端改换后，旧运行及其技能子运行的经验不会进入新绑定。

`mira_m13_learning_persistence_test` 覆盖工作与社交共用内部 ID、租户不符拒绝、恢复经验
实际写入、原事件范围读取、自动升级分范围查询、旧记录不回填、运行换绑后丢弃及单范围
擦除。`mira_m13_learning_test` 覆盖换绑前后嵌套技能子运行继承父运行绑定。
`mira_m7_tool_runtime_skill_test` 覆盖环境范围的发布投影、提示、跨租户和重开。
`mira_m4_contracts_test` 复验现有记录与查询契约。

构建环境为 Linux x86_64、GCC 13.3.0、CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m13_learning_test mira_m13_learning_persistence_test \
  mira_m7_tool_runtime_skill_test mira_m4_contracts_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_(m13_(learning|learning_persistence)|m7_tool_runtime_skill|m4_contracts)_test$'
printf '%s\n' src/context/{skill_environment,memory_query_contract,memory_contracts,memory_agent_binding}.cpp \
  src/workflow/{workflow_runtime,workflow_recovery_memory,workflow_skill_hint,tool_runtime_wiring}.cpp \
  src/storage/sqlite_memory_store.cpp | xargs -P 2 -n 1 clang-tidy -p build/release --warnings-as-errors='*'
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行/待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产工作与社交环境绑定、自动技能提示提取 | Mira Maintainers 与宿主集成者，接入真实环境授权与候选入口后复跑检索、换版、撤权和擦除 |
| Windows/Android、远端 CI 和真实模型 | Mira Maintainers，提交后运行目标矩阵并用当前环境场景回填 |

Linux Release 全量 **206/206**，ASAN 定向 **4/4**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-environment-{final-test,asan-test,tidy,nested-tidy,quality}.log`。

<a id="history-social-recording"></a>

### 2026-11-24 来源账号及结算后多页提取

基线 `2d29886`，分支 `feat/m30-memory-systems`。DEC-140 交付从已认证来源取得当前发言者
的持久目录 API、已有可靠证据时绑定新账号、按账号精确读取画像断言和擦除后的重放屏障。
DEC-141 增加协调层终态 Record 的有界多页提取，复用已有作业、截止时间和提交管线。

- `mira_m30_social_directory_test` 覆盖同名不同人、QQ 与 B 站合成来源、并发首次观察、
  重开、来源回执、绑定冲突、跨租户和环境、账号容量、对象及环境擦除。
- `mira_m30_social_erasure_recovery_test` 覆盖旧目录迁移及配对画像召回，验证联合擦除
  未结算时读写封闭，恢复后旧账号消息被拒。
- `mira_m30_recording_test` 覆盖结算后自动多页写入、页上限、同一快照排除后到事件、
  部分提交后再次读取及无效页上限拒绝。`mira_m30_lifecycle_faults_test` 和
  `mira_m36_memory_agent_test` 复验现有生命周期及服务契约。

Linux x86_64，GCC 13.3.0，CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_recording_test mira_m30_lifecycle_faults_test \
  mira_m30_social_directory_test mira_m30_social_profile_test mira_m30_social_relationship_test \
  mira_m30_social_erasure_recovery_test mira_m36_memory_agent_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_(m30_(recording|lifecycle_faults|social_(directory|profile|relationship|erasure_recovery))|m36_memory_agent)_test$'
printf '%s\n' src/storage/sqlite_social_directory.cpp src/context/memory_agent_jobs.cpp \
  src/interaction/interaction_memory.cpp | xargs -P 2 -n 1 clang-tidy -p build/release --warnings-as-errors='*'
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 真实 QQ/B 站和模型推断 | 建立真实 Adapter 与证据提取后由 Mira Maintainers 和宿主集成者复跑多人对话、同人归并与反证纠正 |
| 承诺自动派发及跨进程提取水位 | Mira Maintainers 按 M30-37/38 交付，在持久实现中补跑重启、漏触发、执行回执以及证据重放 |
| Windows、Android 与远端 CI | Mira Maintainers 提交后运行目标平台矩阵 |

Linux Release 全量 **206/206**，ASAN 定向 **7/7**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-auto-record-{release-test,asan-test,tidy,quality}.log`
和 `/tmp/mira-m30-accounts-tidy.log`。

### 2026-11-24 社交消息发言证据

基线 `684d1e1`，分支 `feat/m30-memory-systems`。DEC-142 交付 M30-40 的社交消息索引。
复用事件切片核对会话、位置、事件身份及完整性引用，原子接纳发言对象与消息证据。
按精确环境和会话区间分页读取，擦除按原目录和记忆库流程执行。

- `mira_m30_social_directory_test` 覆盖同会话多个发言者及来源、精确事件不符、
  来源和正文冲突、位置冲突回滚、限额回滚、作用域、重开和擦除后重放。
- `mira_m30_social_erasure_recovery_test` 覆盖 1.0 旧目录迁移、配对记忆库擦除失败
  时封闭证据读取、恢复后旧消息拒绝重新接纳。

Linux x86_64，GCC 13.3.0，CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_social_directory_test \
  mira_m30_social_erasure_recovery_test mira_m30_social_profile_test \
  mira_m30_social_relationship_test mira_m30_event_slices_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_m30_(social_(directory|erasure_recovery|profile|relationship)|event_slices)_test$'
clang-tidy -p build/release --warnings-as-errors='*' src/storage/sqlite_social_directory.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 真实平台事件触发、正文提及及跨来源推断 | Mira Maintainers 和宿主集成者在真实消息适配器与模型接线后补跑 M30-36 |
| Windows、Android 与远端 CI | Mira Maintainers 提交后运行目标平台矩阵 |

Linux Release 全量 **206/206**，ASAN 定向 **5/5**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-message-{test,asan-test,tidy,quality}.log`。

### 2026-11-24 社交消息参与者查询

基线 `18d8f7a`，分支 `feat/m30-memory-systems`。DEC-143 交付 M30-41。在同一目录工作单元
解析发言者和已知引用，按原输入顺序返回账号证据、消息证据、别名候选或未解析状态。
继续使用目录的环境、擦除和版本规则。

`mira_m30_social_directory_test` 覆盖同一群聊内发言者与多来源提及、引用消息、别名的
唯一和同名歧义、未知引用、跨会话限制、引用数量上限，以及擦除后引用和发言证据失效。
社交画像、关系和联合擦除用例复验现有读写行为。

Linux x86_64，GCC 13.3.0，CMake 3.28.3。复现命令如下。

```bash
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake --build build/asan -j 4 --target mira_m30_social_directory_test \
  mira_m30_social_erasure_recovery_test mira_m30_social_profile_test mira_m30_social_relationship_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_m30_social_(directory|erasure_recovery|profile|relationship)_test$'
clang-tidy -p build/release --warnings-as-errors='*' src/storage/sqlite_social_directory.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 真实平台引用、正文指代提取与跨来源推断 | Mira Maintainers 与宿主集成者在真实消息适配器和模型接线后补跑 M30-36 |
| Windows、Android、远端 CI | Mira Maintainers 提交后运行目标平台矩阵 |

Linux Release 全量 **206/206**，ASAN 定向 **4/4**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-mentions-{test,asan-test,tidy,quality}.log`。

### 2026-11-24 社交正文引用提取

基线 `7195aae`，分支 `feat/m30-memory-systems`。DEC-144 交付 M30-42。宿主可在消息索引
后启动提取，模型只看到当前正文和同会话近期消息及发言者代号。原文和前序行号经确定性
校验后用现有目录解析，模型输出继续作为候选。

`mira_m30_social_inference_test` 通过文件事件日志、SQLite 目录及离线模型供应替身验收
前序发言指代、同名歧义、未知名称、模型协议、账号隔离、原文与行号校验、会话检查、
来源完整性、禁用远端存储、禁止内容、取消与推断期间证据擦除。

Linux x86_64，GCC 13.3.0，CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_social_inference_test \
  mira_m30_social_directory_test mira_m30_event_slices_test
ctest --test-dir build/asan --output-on-failure \
  -R '^mira_m30_(social_(inference|directory)|event_slices)_test$'
printf '%s\n' src/context/social_mention_extraction.cpp src/storage/social_message_inference.cpp \
  | xargs -P 2 -n 1 clang-tidy -p build/release --warnings-as-errors='*'
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 真实平台、模型表现与跨平台身份推断 | Mira Maintainers 与宿主集成者在真实适配器与授权模型接线后补跑 M30-36 |
| Windows、Android、远端 CI | Mira Maintainers 提交后运行目标平台矩阵 |

Linux Release 全量 **207/207**，ASAN 定向 **3/3**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-inference-{test,asan-test,tidy,quality}.log`。

<a id="history-goal-dispatch"></a>

### 2026-11-24 一次性目标派发准备与提交回执

基线 `b93b338`，分支 `feat/m30-memory-systems`。DEC-145 交付 M30-43。在到期索引上
发现待派发目标，SQLite 按当前修订和期限原子准备派发身份。登记提交回执可以对账，
并从待派发列表移除已提交目标。兑现仍使用独立结算事件。

`mira_m30_goal_store_test` 验收从 1.3 迁移、到期和过期筛选、归属、分页、并发准备、
跨重开重放、提交冲突、纠正、取消、擦除、授权分离。`mira_m30_lifecycle_test` 复验
现有交互生命周期。

Linux x86_64，GCC 13.3.0，CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_goal_store_test mira_m30_lifecycle_test
ctest --test-dir build/asan --output-on-failure -R '^mira_m30_(goal_store|lifecycle)_test$'
printf '%s\n' src/storage/sqlite_goal_store.cpp src/storage/sqlite_goal_dispatch.cpp \
  | xargs -P 2 -n 1 clang-tidy -p build/release --warnings-as-errors='*'
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 周期发现、到期任务提交及重复计划派发 | Mira Maintainers 接入协调层自主入口与 Executor 检查后，按 M30-37 复验实际任务去重、补跑和兑现 |
| Windows、Android、远端 CI | Mira Maintainers 提交后运行目标平台矩阵 |

Linux Release 全量 **207/207**，ASAN 定向 **2/2**。严格 clang-tidy、格式、文档、SBOM、
平台边界和架构门禁通过。日志见 `/tmp/mira-m30-dispatch-{test,asan-test,quality}.log`。
clang-tidy 初轮检查见 `/tmp/mira-m30-dispatch-tidy.log`，派发实现的修正复验见
`/tmp/mira-m30-dispatch-recheck-tidy.log`。

### 2026-11-24 一次性目标的周期发现与宿主派发

基线 `ccb222c`，分支 `feat/m30-memory-systems`。DEC-146 交付 M30-44。派发器通过 kairo
可取消延时任务检查一个已授权归属，按到期索引分页，使用稳定操作 ID 调用宿主提交
接口。已确认提交写入原派发回执，关闭时回收下一次检查并等待在途结果。

`mira_m30_goal_dispatch_poller_test` 覆盖较早目标的提交结果不明、后续页继续发现、
模拟重开后按原 ID 对账、未到期目标、在途取消、确认回执保留、关闭与 Executor 停止。
`mira_m30_goal_store_test` 复验准备、持久回执和现有目标行为。

Linux x86_64，GCC 13.3.0，CMake 3.28.3。复现命令如下。

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DMIRA_BUILD_TESTS=ON
cmake --build build/release -j 9
ctest --test-dir build/release -j 6 --output-on-failure
ctest --test-dir build/release --output-on-failure -R '^mira_m30_goal_dispatch_poller_test$' \
  --repeat until-fail:30
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_goal_dispatch_poller_test mira_m30_goal_store_test
ctest --test-dir build/asan --output-on-failure -R '^mira_m30_goal_(dispatch_poller|store)_test$'
clang-tidy -p build/release --warnings-as-errors='*' src/storage/goal_dispatch_poller.cpp
cmake --build build/release --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产协调层提交和重复计划派发 | Mira Maintainers 与宿主集成者接入自主提交后，按 M30-37 补跑真实任务、重启对账、兑现与重复触发 |
| Windows、Android、远端 CI | Mira Maintainers 提交后运行目标平台矩阵 |

Linux Release 全量 **208/208**，周期派发器连续复跑 **30/30**，ASAN 定向 **2/2**。
严格 clang-tidy、格式、文档、SBOM、平台边界和架构门禁通过。日志见
`/tmp/mira-m30-poller-{test,repeat-test,asan-test,tidy,quality}.log`。

### 2026-11-25 重复计划的周期派发与持久回执

基线 `5fda1f9`，分支 `feat/m30-memory-systems`。DEC-148 交付 M30-45。待提交的触发
占用一条原有的触发记录额度，数据库重开后用原操作 ID 继续对账。宿主确认任务准入后，
目标库在一个事务中保存提交回执并推进原计划版本。一次性目标和重复计划沿独立游标
在同一 Executor 定时任务中轮换检查。

`mira_m30_goal_store_test` 覆盖 1.4 迁移、已有手动回执、日历规则耗尽、并发准备、
提交写入失败的事务回滚、重放、额度、撤权、换版、取消及擦除。
`mira_m30_goal_dispatch_poller_test` 覆盖混合检查、分页绕过结果未知的触发、跨重开
对账及按序补跑。使用离线宿主提交回调。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 10
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure -R '^mira_m30_goal_dispatch_poller_test$' \
  --repeat until-fail:30
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 5 --target mira_m30_goal_store_test mira_m30_goal_dispatch_poller_test
ctest --test-dir build/asan --output-on-failure -j 2 -R '^mira_m30_goal_(store|dispatch_poller)_test$'
cmake -S . -B build/ubsan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_UBSAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/ubsan -j 4 --target mira_m30_goal_store_test mira_m30_goal_dispatch_poller_test
ctest --test-dir build/ubsan --output-on-failure -j 2 -R '^mira_m30_goal_(store|dispatch_poller)_test$'
printf '%s\n' src/storage/sqlite_goal_store.cpp src/storage/sqlite_goal_occurrence.cpp \
  src/storage/goal_dispatch_poller.cpp | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 真实协调层任务提交、持久对账和实际兑现 | Mira Maintainers 与宿主集成者接入真实提交端后，按 M30-37 验收停机补跑和任务结算 |
| Windows、Android、远端 CI | Mira Maintainers 提交 PR 后运行平台矩阵 |

Linux Debug 全量 **213/213**，派发器连续复跑 **30/30**，ASan 和 UBSan 各 **2/2**。
三个实现文件通过严格 clang-tidy，格式、文档、SBOM、平台边界和架构检查通过。
日志见 `/tmp/mira-m30-recurring-final-*.log`、`/tmp/mira-m30-recurring-tidy*.log` 和
`/tmp/mira-m30-recurring-quality.log`。

### 2026-11-25 以持久任务完成证据兑现一次性目标

基线 `be218ad`，分支 `feat/m30-memory-systems`。DEC-149 交付 M30-46：协调层校验
当前主体、会话归属及 Runtime 无错误完成终态后写入任务事件。目标库通过原派发回执
读取同一个受信任务事件库，复核证据归属、持久化和时间，并复用原版本的原子结算。

`mira_m30_goal_fulfillment_test` 使用真实 Runtime 命令、FileEventStore 与 SQLite，
覆盖独立于交互台账的证据、仅缓冲事件拒绝、撤权和主体隔离、并存冲突事件、过期、失败
任务、旧修订、擦除和重开后的幂等兑现。录制和兑现的宿主入口由离线测试直接调用。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 10
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure -R '^mira_m30_goal_fulfillment_test$' \
  --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_goal_fulfillment_test \
  mira_m30_goal_store_test mira_m30_goal_dispatch_poller_test
ctest --test-dir build/asan -j 3 --output-on-failure \
  -R '^mira_m30_goal_(fulfillment|store|dispatch_poller)_test$'
printf '%s\n' src/interaction/task_completion.cpp src/storage/sqlite_goal_dispatch.cpp \
  | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 自主准入、实际宿主跨重启对账与结算自动关联 | Mira Maintainers 与宿主集成者完成 M30-37 的协调层接线后，以自主派发任务验收 |
| Windows、Android、远端 CI | Mira Maintainers 提交 PR 后运行平台矩阵 |

Linux Debug 全量 **214/214**，兑现测试连续复跑 **25/25**，ASan 定向 **3/3**。
严格 clang-tidy、格式、文档、SBOM、平台边界及架构检查通过。日志见
`/tmp/mira-m30-fulfillment-recheck-*.log`、`/tmp/mira-m30-fulfillment-tidy*.log` 和
`/tmp/mira-m30-fulfillment-quality.log`。

### 2026-11-25 周期核对一次性目标完成证据

基线 `bd95f29`，分支 `feat/m30-memory-systems`。DEC-150 交付 M30-47。按归属和目标 ID
有界分页查询已提交的当前版本目标。派发器复用同一 Executor 定时任务核对持久完成
证据，自动兑现。重开后重新遍历待结算集合，实际完成时间继续约束过期规则。

`mira_m30_goal_fulfillment_test` 覆盖分页、跨主体隔离、准备阶段与旧版本筛选、取消和
截止时间、当前写授权、仅结算配置、与新目标派发并存、重开及过期后的合法补结算。
`mira_m30_goal_dispatch_poller_test` 复验一次性与重复派发的生命周期。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 10
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure \
  -R '^mira_m30_goal_(fulfillment|dispatch_poller)_test$' --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_goal_fulfillment_test \
  mira_m30_goal_store_test mira_m30_goal_dispatch_poller_test
ctest --test-dir build/asan -j 3 --output-on-failure \
  -R '^mira_m30_goal_(fulfillment|store|dispatch_poller)_test$'
printf '%s\n' src/storage/goal_dispatch_poller.cpp src/storage/sqlite_goal_dispatch.cpp \
  | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产自主准入、持久提交对账及完成证据自动发布 | Mira Maintainers 与宿主集成者接入 M30-37 的生产任务后，按真实端到端派发、停机和恢复场景验收 |
| Windows、Android、远端 CI | Mira Maintainers 提交 PR 后运行平台矩阵 |

Linux Debug 全量 **214/214**，兑现与派发两项测试各连续复跑 **25/25**，ASan 定向
**3/3**。严格 clang-tidy、格式、文档、SBOM、平台边界和架构检查通过。日志见
`/tmp/mira-m30-auto-fulfill-recheck-*.log`、`/tmp/mira-m30-auto-fulfill-tidy.log` 和
`/tmp/mira-m30-auto-fulfill-quality.log`。

### 2026-11-25 自动发布待结算目标的 live 完成证据

基线 `e6a46a0`，分支 `feat/m30-memory-systems`。DEC-151 交付 M30-48。派发器先核对已有
持久证据。需要补齐时，从原提交回执确定任务和会话，经宿主当前主体解析调用协调层。
协调层只发布实际成功完成的证据。持久确认后由派发器重新读取证据并结算。

`mira_m30_goal_fulfillment_test` 使用 Runtime、FileEventStore 与 SQLite，覆盖运行中与
失败任务、目标归属和会话不匹配、发布授权恢复、承载库缺失或虚假确认、协调层准入锁
竞争、取消、幂等及重开后的持久证据优先。现有目标存储和派发器测试复验基础行为。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 10
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure -R '^mira_m30_goal_fulfillment_test$' \
  --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_goal_fulfillment_test \
  mira_m30_goal_store_test mira_m30_goal_dispatch_poller_test
ctest --test-dir build/asan -j 3 --output-on-failure \
  -R '^mira_m30_goal_(fulfillment|store|dispatch_poller)_test$'
printf '%s\n' src/interaction/task_completion.cpp src/storage/goal_dispatch_poller.cpp \
  | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产自主准入、提交对账及证据写入前丢失 Runtime 的恢复 | Mira Maintainers 与宿主集成者接入持久任务提交与快照后，按 M30-37 复验跨进程任务恢复和兑现 |
| Windows、Android、远端 CI | Mira Maintainers 提交 PR 后运行平台矩阵 |

Linux Debug 全量 **214/214**，兑现测试连续复跑 **25/25**，ASan 定向 **3/3**。
严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。日志见
`/tmp/mira-m30-live-proof-recheck-*.log`、`/tmp/mira-m30-live-proof-tidy.log` 和
`/tmp/mira-m30-live-proof-quality-final.log`。

### 2026-11-25 区分目标首次提交与持久只读对账

基线 `53db56e`，分支 `feat/m30-memory-systems`。DEC-152 交付 M30-49。复用已有派发
准备事务，只在新插入并确认持久化后返回首次准备资格。配置对账回调后，已有准备记录的
目标沿原操作 ID 查询宿主回执，查不到时保留准备身份并继续分页。

`mira_m30_goal_store_test` 覆盖一次性和重复触发的并发准备，验证二者都只产生一次
首次准备资格。`mira_m30_goal_dispatch_poller_test` 模拟宿主准入成功而确认丢失，覆盖
一次性和重复计划重开后的只读对账、既有未确认准备记录、分页继续推进和原回执回填。
兑现测试复验已有任务完成证据和结算接线。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 10
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure -R '^mira_m30_goal_dispatch_poller_test$' \
  --repeat until-fail:30
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_goal_fulfillment_test \
  mira_m30_goal_store_test mira_m30_goal_dispatch_poller_test
ctest --test-dir build/asan -j 3 --output-on-failure \
  -R '^mira_m30_goal_(fulfillment|store|dispatch_poller)_test$'
printf '%s\n' src/storage/sqlite_goal_dispatch.cpp src/storage/sqlite_goal_occurrence.cpp \
  src/storage/goal_dispatch_poller.cpp | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产自主准入、宿主持久准入结果和不明准备身份的确认 | Mira Maintainers 与宿主集成者接入 M30-37 的真实任务提交台账后，验收停机中断和跨进程对账 |
| Windows、Android、远端 CI | Mira Maintainers 提交 PR 后运行平台矩阵 |

Linux Debug 全量 **214/214**，派发器测试连续复跑 **30/30**，ASan 定向 **3/3**。
严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。日志见
`/tmp/mira-m30-submit-reconcile-recheck-*.log`、`/tmp/mira-m30-submit-reconcile-tidy*.log`
和 `/tmp/mira-m30-submit-reconcile-quality-final.log`。

### 2026-11-25 持久记录并回查任务准入

基线 `4becbb1`，分支 `feat/m30-memory-systems`。DEC-153 交付 M30-50。稳定操作 ID 通过
现有 `TaskSpec` 与 Runtime 命令结果传递。协调层从已生效的提交命令取原任务、会话和
接纳时间，按当前主体归属写入受信任务事件库。重开后可以只凭归属和操作 ID 读取回执。

`mira_m30_goal_fulfillment_test` 使用真实 Runtime、FileEventStore 和 SQLite，覆盖操作
和任务关联、撤权和跨主体隔离、重试、冲突、仅缓冲事件拒绝、原 Runtime 停止后的回查，
以及通过派发器对账回填已确认任务。相关 Runtime 和派发器原用例由全量测试复验。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 8
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure -R '^mira_m30_goal_fulfillment_test$' \
  --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 6 --target mira_m30_goal_fulfillment_test mira_m30_goal_dispatch_poller_test
ctest --test-dir build/asan -j 2 --output-on-failure \
  -R '^mira_m30_goal_(fulfillment|dispatch_poller)_test$'
printf '%s\n' src/runtime/runtime.cpp src/interaction/task_admission.cpp \
  | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产自主准入和登记前的任务恢复 | Mira Maintainers 与宿主集成者接通 M30-37 的自主准入和可恢复执行边界后，验收进程在命令生效与台账写入之间丢失的情形 |
| Windows、Android、远端 CI | Mira Maintainers 提交 PR 后执行目标平台矩阵 |

Linux Debug 全量 **214/214**，兑现及准入测试连续复跑 **25/25**，ASan 定向 **2/2**。
严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。日志见
`/tmp/mira-m30-admission-final-*.log`、`/tmp/mira-m30-admission-repeat.log`、
`/tmp/mira-m30-admission-asan-*.log`、`/tmp/mira-m30-admission-tidy.log` 和
`/tmp/mira-m30-admission-quality.log`。

### 2026-11-25 在发布任务前确认持久准入回执

基线 `e4d7984`，分支 `feat/m30-memory-systems`。DEC-154 交付 M30-51。宿主可把当前
授权下绑定的记录器随 `SubmitTask` 提交。Runtime 先检查会话，记录器持久确认后才对外
发布任务。持久记录阶段释放 Runtime 记录锁，记录失败通过命令拒绝反馈。

`mira_runtime_lifecycle_test` 检查记录期间候选任务不可见、命令未结算及失败结果。
`mira_m30_goal_fulfillment_test` 使用实际 Runtime、FileEventStore 与 SQLite，覆盖发布前
已能读取同一回执、跨会话拒绝、绑定后过期、仅缓冲写入、重复提交冲突以及新 Runtime
下补记目标派发，覆盖过程中省略原提交后的手动登记。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 9
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure \
  -R '^mira_(m30_goal_fulfillment|runtime_lifecycle)_test$' --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 5 --target mira_m30_goal_fulfillment_test mira_runtime_lifecycle_test
ctest --test-dir build/asan -j 2 --output-on-failure \
  -R '^mira_(m30_goal_fulfillment|runtime_lifecycle)_test$'
printf '%s\n' src/runtime/runtime.cpp src/interaction/task_admission.cpp \
  | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产自主准入与任务完成证据的恢复 | Mira Maintainers 与宿主集成者接入 M30-37 的自主准入和完成证据持久化后，验收生产任务的跨进程闭环 |
| Windows、Android、远端 CI | Mira Maintainers 提交 PR 后执行目标平台矩阵 |

Linux Debug 全量 **214/214**，Runtime 生命周期和目标兑现测试各连续复跑 **25/25**，
ASan 定向 **2/2**。严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。
日志见 `/tmp/mira-m30-admission-before-recheck-*.log`、`/tmp/mira-m30-admission-before-tidy.log`
和 `/tmp/mira-m30-admission-before-quality-final.log`。

### 2026-11-25 发布成功终态前确认持久完成证据

基线 `c176ef5`，分支 `feat/m30-memory-systems`。DEC-155 为现有 Runtime `CompleteTask`
命令接入任务绑定的记录器。第一次验证请求固定证据时间和因果序号。持久确认成功后才
发布 Runtime 终态。重试使用同一份证据，最终快照保留生效命令的序号。

`mira_runtime_lifecycle_test` 检查记录期间的快照可见性、新增操作拒绝、迟到结果、
确认不明、接管、取消、异常和稳定重试。`mira_m30_goal_fulfillment_test` 使用实际的
Runtime、FileEventStore 和 SQLite 验证写入成功而回执丢失。原 Runtime 退出后，派发器
从持久事件恢复准入并兑现目标。验收流程省略了手动完成证据发布。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 9
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug -j 2 --output-on-failure \
  -R '^mira_(m30_goal_fulfillment|runtime_lifecycle)_test$' --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_goal_fulfillment_test mira_runtime_lifecycle_test
ctest --test-dir build/asan -j 2 --output-on-failure \
  -R '^mira_(m30_goal_fulfillment|runtime_lifecycle)_test$'
printf '%s\n' src/runtime/runtime.cpp src/interaction/task_completion.cpp \
  | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产自主准入和写入前丢失 Runtime 的恢复 | Mira Maintainers 与宿主集成者接入 M30-37 的自主准入和可恢复执行后，验收提交、执行、重新验证、兑现和重启闭环 |
| Windows、Android、远端 CI | Mira Maintainers 提交 PR 后执行目标平台矩阵 |

Linux Debug 全量 **214/214**，Runtime 生命周期与目标兑现测试各连续通过 **25/25**，
ASan 定向 **2/2**。严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。
日志见 `/tmp/mira-m30-completion-debug-*.log`、`/tmp/mira-m30-completion-asan-*.log`、
`/tmp/mira-m30-completion-tidy-final.log` 和 `/tmp/mira-m30-completion-quality.log`。

<a id="history-record-cursors"></a>

### 2026-11-25 按工作项顺序补齐实施细则和 Record 页进度

基线 `cd8f8b0`，分支 `feat/m30-memory-systems`。M30-01 冻结承诺存储、候选触发、
记忆意图、价值信号和删除后同 scope 新世代的实现边界。验收任务、所有者和依赖见
[计划 §4.1](m30-long-term-memory-integration.md#41-实施细则验收)。执行契约由 DEC-156 承载。

M30-03 的本轮增量修复同一任务向不同获准 scope 写入时共用水位而漏提取的问题。
DEC-157 增加可选的持久页回执。`mira_m30_recording_test` 在真实 FileEventStore
和 SQLite 中验证范围隔离、固定快照续跑、存储重开、擦除屏障以及待审批页保留。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 9
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure -R '^mira_m30_recording_test$' \
  --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_recording_test
ctest --test-dir build/asan --output-on-failure -R '^mira_m30_recording_test$'
printf '%s\n' src/context/memory_agent_jobs.cpp src/context/memory_agent_record.cpp \
  | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 其他类型候选的统一收集和触发 | Mira Maintainers 在 M30-03 补齐其他写入入口的证据回执和候选触发。M30-38 验收默认自动恢复及六类路由 |
| 真实模型、Windows、Android 与远端 CI | Mira Maintainers 提供模型验收环境并提交 PR 后执行目标平台矩阵 |

Linux Debug 全量 **214/214**，`mira_m30_recording_test` 连续复跑 **25/25**，
ASan 定向 **1/1**。严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。
日志见 `/tmp/mira-m30-early-debug-*.log`、`/tmp/mira-m30-early-record-repeat.log`、
`/tmp/mira-m30-early-asan-*.log`、`/tmp/mira-m30-early-tidy.log` 和
`/tmp/mira-m30-early-quality-final.log`。

<a id="history-loop-episodes"></a>

### 2026-11-25 记录实际循环结算及按类型核对候选

基线 `c32ffbd`，分支 `feat/m30-memory-systems`。DEC-158 在 M30-03 交付设备循环
和会话循环结算的 Episode 提取。旧平面设备事件继续兼容，新信封按任务和原事件 ID
校验并去重。持久 Record 页回执同步保存按 `MemoryKind` 归类的处置计数。

`mira_m30_recording_test` 通过真实 ConversationLoop、协调层、FileEventStore 和 SQLite
验证普通问答后无需提取模型即可写入 Episode，并校验证据及分类回执。
`mira_m4_consolidation_test` 验证旧格式、归属不符、非状态事件、失败结果、同序号隔离、
重放及可靠时间缺失。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 9
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug -j 2 --output-on-failure \
  -R '^mira_(m30_recording|m4_consolidation)_test$' --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_recording_test mira_m4_consolidation_test
ctest --test-dir build/asan -j 2 --output-on-failure -R '^mira_(m30_recording|m4_consolidation)_test$'
printf '%s\n' src/context/memory_consolidation.cpp src/context/memory_agent_record.cpp \
  src/context/memory_agent_seams.cpp | xargs -P 2 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 专用 owner 的候选汇集和触发 | Mira Maintainers 完成 M30-03 的各写入入口接线后，核对来源、范围、分类和持久处置回执。M30-38 验收六类自动路由 |
| 真实模型、Windows、Android 与远端 CI | Mira Maintainers 提供模型验收环境并提交 PR 后执行目标平台矩阵 |

Linux Debug 全量 **214/214**，提取与合并测试各连续通过 **25/25**，ASan 定向 **2/2**。
严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。
日志见 `/tmp/mira-m30-loop-debug-*.log`、`/tmp/mira-m30-loop-repeat.log`、
`/tmp/mira-m30-loop-asan-*.log`、`/tmp/mira-m30-loop-tidy.log` 和 `/tmp/mira-m30-loop-quality.log`。

<a id="history-social-attributes"></a>

### 2026-11-25 已归因消息的社交画像候选

基线 `fd51735`，分支 `feat/m30-memory-systems`。DEC-159 在 M30-03 交付从已索引
消息自动推断并提交发言者属性的宿主入口，另提供接纳结构化候选的专用写入方法。两条
路径均复核发言对象、正文摘要和逐字摘录，并返回原 `ConsolidationReport`。

`mira_m30_social_inference_test` 使用真实 FileEventStore、配对社交目录和 SQLite 记忆库，
验证伪造或越界候选、绑定 QQ/B 站账号后的同人召回、重开幂等、同证据冲突、独立来源、
在途取消、擦除及关闭。模型为离线脚本。原目录、画像和擦除恢复测试继续复验。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 9
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug -j 4 --output-on-failure \
  -R '^mira_m30_social_(inference|directory|profile|erasure_recovery)_test$' --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_social_inference_test mira_m30_social_profile_test \
  mira_m30_social_erasure_recovery_test
ctest --test-dir build/asan -j 3 --output-on-failure \
  -R '^mira_m30_social_(inference|profile|erasure_recovery)_test$'
printf '%s\n' src/context/social_mention_extraction.cpp src/storage/social_message_inference.cpp \
  src/storage/sqlite_social_directory.cpp \
  | xargs -P 3 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产宿主统一触发和其他专用候选汇集 | Mira Maintainers 和宿主集成者在 M30-03 接好生产消息触发及其他候选入口后验收全链路。跨账号同人推断及关系候选继续由 M30-36 验收 |
| 真实模型、Windows、Android 与远端 CI | Mira Maintainers 提供模型验收环境并提交 PR 后执行目标平台矩阵 |

Linux Debug 全量 **214/214**，四项社交测试各连续通过 **25/25**，ASan 定向 **3/3**。
严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。日志见
`/tmp/mira-m30-social-candidate-final-debug-*.log`、`/tmp/mira-m30-social-candidate-final-repeat.log`、
`/tmp/mira-m30-social-candidate-final-asan-*.log`、`/tmp/mira-m30-social-candidate-final-tidy.log` 和
`/tmp/mira-m30-social-candidate-final-quality.log`。

<a id="history-social-relations"></a>

### 2026-11-25 已归因消息的有向社交关系

基线 `f31d407`，分支 `feat/m30-memory-systems`。DEC-160 在 M30-03 交付从当前发言、
逐字引文和同环境唯一别名提出有向关系的宿主入口。属性与关系共用写入前复核、擦除
串行与合并管线。报告保留歧义与缺失候选，并将已解析候选交给原管线。

`mira_m30_social_inference_test` 使用离线模型、真实 FileEventStore 和配对 SQLite
存储，验证两种方向、伪造原文、合成群聊中的同名及跨环境别名、重复提交、写入前
别名变更与关系另一端擦除。原目录、关系和联合擦除测试继续复验。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 9
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug -j 4 --output-on-failure \
  -R '^mira_m30_social_(inference|directory|relationship|erasure_recovery)_test$' --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_social_inference_test mira_m30_social_relationship_test \
  mira_m30_social_erasure_recovery_test
ctest --test-dir build/asan -j 3 --output-on-failure \
  -R '^mira_m30_social_(inference|relationship|erasure_recovery)_test$'
printf '%s\n' src/context/social_mention_extraction.cpp src/storage/social_message_inference.cpp \
  src/storage/sqlite_social_directory.cpp \
  | xargs -P 3 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产统一触发及其他专用候选入口 | Mira Maintainers 和宿主集成者在 M30-03 接通生产消息、世界和技能候选后验收。代词或前序引用关系及跨来源同人推断由 M30-36 验收 |
| 真实模型、Windows、Android 与远端 CI | Mira Maintainers 提供模型验收环境并提交 PR 后执行目标平台矩阵 |

Linux Debug 全量 **214/214**，四项社交测试各连续通过 **25/25**，ASan 定向 **3/3**。
严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。日志见
`/tmp/mira-m30-relations-final-debug-*.log`、`/tmp/mira-m30-relations-final-repeat.log`、
`/tmp/mira-m30-relations-final-asan-*.log`、`/tmp/mira-m30-relations-final-tidy.log` 和
`/tmp/mira-m30-relations-final-quality.log`。

<a id="history-social-references"></a>

### 2026-11-25 前序发言与关系双证据

基线 `f1228b3`，分支 `feat/m30-memory-systems`。DEC-161 为 M30-03 的关系候选入口
增加同会话前序发言归因，供 M30-36 复用。自动提取使用原 `Executor` 作业与有界语境，
目录复核原发言、前序来源和锁内证据快照。记忆文档 1.7 保存主张和归因事件，SQLite
复用已有 provenance 索引。

`mira_m30_social_inference_test` 使用离线模型、FileEventStore 和配对 SQLite 验证代词归因、
当前行和跨会话引用拒绝、两条来源证据、证据变更冲突、在途擦除、幂等重试和双端擦除。
`mira_m30_social_relationship_test` 复验旧文档读取、证据形态及原有有向关系契约。

Linux x86_64，GCC 13.3.0，CMake 3.28.3，捆绑 SQLite 3.53.4。复现命令如下。

```bash
cmake --preset debug
cmake --build build/debug -j 9
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug -j 4 --output-on-failure \
  -R '^mira_m30_social_(inference|directory|relationship|erasure_recovery)_test$' --repeat until-fail:25
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 4 --target mira_m30_social_inference_test mira_m30_social_relationship_test \
  mira_m30_social_erasure_recovery_test
ctest --test-dir build/asan -j 3 --output-on-failure \
  -R '^mira_m30_social_(inference|relationship|erasure_recovery)_test$'
printf '%s\n' src/context/social_profile.cpp src/context/social_mention_extraction.cpp \
  src/storage/social_message_inference.cpp \
  | xargs -P 3 -n 1 clang-tidy -p build/debug --warnings-as-errors='*'
cmake --build build/debug --target format-check docs-check sbom-check \
  platform-boundary-check architecture-check
git diff --check
```

| 未执行或待完成 | 负责人及补跑条件 |
| --- | --- |
| 生产消息触发、其他候选入口、跨来源同人推断 | Mira Maintainers 与宿主集成者在 M30-03 / M30-36 接好生产接线后，以合成多来源和线上授权场景验收 |
| 真实模型、Windows、Android 和远端 CI | Mira Maintainers 提供模型验收环境并提交 PR 后执行目标平台矩阵 |

Linux Debug 全量 **214/214**，四项社交测试各连续通过 **25/25**，ASan 定向 **3/3**。
严格 clang-tidy、格式、文档、SBOM、平台边界与架构检查通过。日志见
`/tmp/mira-m30-reference-final-debug-*.log`、`/tmp/mira-m30-reference-final-repeat.log`、
`/tmp/mira-m30-reference-asan-*.log`、`/tmp/mira-m30-reference-final-tidy.log` 和
`/tmp/mira-m30-reference-final-quality.log`。

<a id="history-verified-world-recording"></a>

### 2026-11-26 从验证事件记录世界观察

基线 `27c0b1e`，分支 `feat/m30-memory-systems`。M30-03 按
[DEC-162](../decisions/DEC-162-verified-world-fact-recording.md) 接通类型化世界事实写入。
测试通过生产者 `VerifiedFactPublisher` 生成验证事件，再由宿主入口从 FileEventStore
取证并写入 SQLite。覆盖候选处置、同租户错误 scope、跨租户拒绝、错误事实键及事件定位、
世代不符、截止与提交前取消、重开幂等、擦除后拒绝失效来源，以及租约释放。

```bash
cmake --build build/debug -j 8
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure -R '^mira_m30_world_fact_test$' --repeat until-fail:15
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 3 --target mira_m30_world_fact_test
ctest --test-dir build/asan --output-on-failure -R '^mira_m30_world_fact_test$'
clang-tidy -p build/debug --warnings-as-errors='*' src/context/world_fact_recording.cpp
cmake --build build/debug --target format-check docs-check sbom-check platform-boundary-check architecture-check
git diff --check
```

Linux Debug 全量 **214/214**，世界事实定向重复通过 **15/15**，ASan 定向 **1/1**。
严格 clang-tidy、格式、文档、SBOM、平台边界和架构检查通过。日志见
`/tmp/mira-m30-world-record-*.log`。

| 待验证 | 负责人及补跑条件 |
| --- | --- |
| 生产宿主的事件选择、租约和自动消费 | Mira Maintainers 与宿主集成者在 M30-38 接通生产事件和当前授权后，以重启、撤权和擦除并发场景验收 |
| Windows、Android、远端 CI 和真实宿主 | Mira Maintainers 在推送被测版本并具备对应环境后，按第 3 节矩阵补跑 |

<a id="history-skill-hint-candidate-report"></a>

### 2026-11-26 技能提示候选处置报告

基线 `2e753aa`，分支 `feat/m30-memory-systems`。M30-03 按
[DEC-163](../decisions/DEC-163-skill-hint-candidate-report.md) 把成功技能调用的
`SkillHint` 写入接入标准候选报告。测试通过实际技能调用和 SQLite 验证候选分类、首次写入、
策略拒绝、截止后跳过授权、提交前取消、幂等重放、擦除后无记录回灌及旧回执兼容。
为满足文件规模预算，将测试的 `--report` 生成逻辑移到独立编译单元，拆分前后输出逐字节一致。

```bash
cmake --build build/debug -j 8
ctest --test-dir build/debug -j 10 --output-on-failure
ctest --test-dir build/debug --output-on-failure -R '^mira_m7_tool_runtime_skill_test$' --repeat until-fail:15
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_BUILD_TESTS=ON
cmake --build build/asan -j 3 --target mira_m7_tool_runtime_skill_test
ctest --test-dir build/asan --output-on-failure -R '^mira_m7_tool_runtime_skill_test$'
clang-tidy -p build/debug --warnings-as-errors='*' src/workflow/workflow_skill_hint.cpp
cmake --build build/debug --target format-check docs-check sbom-check platform-boundary-check architecture-check
git diff --check
```

Linux Debug 全量通过 **214/214**，定向重复 **15/15**，ASan 定向 **1/1**。严格
clang-tidy、格式、文档、SBOM、平台边界和架构检查通过。日志见
`/tmp/mira-m30-skill-candidate-*.log`。

| 待验证 | 负责人及补跑条件 |
| --- | --- |
| 从生产技能结算触发候选处理及跨重启消费 | Mira Maintainers 与宿主集成者在 M30-38 接通持久技能来源消费后，运行重开、换版、撤权和擦除重放用例 |
| Windows、Android、完整远端 CI 和真实宿主 | Mira Maintainers 在推送被测版本并具备对应环境后，按第 3 节矩阵补跑 |
