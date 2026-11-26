# 维护计划：mira-bridge 宿主反馈复核

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)、[M30](m30-long-term-memory-integration.md)
> 更新日期：2026-11-26
> 来源：mira-bridge 宿主提供的 MIRA-20261126-001～016 反馈（被测基线 `27c0b1e`）

以自主、高权限的 Continuous 行动路径为前提复核反馈。Mira 的工具接纳遵守当前授权和
任务归属。已接受的工具往返继续推进工作，模型回复、已执行动作和已验证任务结果分别结算。

| 反馈 | 状态 | 当前工作与验收边界 |
| --- | --- | --- |
| `MIRA-20261126-001` | 已修订设计，待宿主复验 | Continuous 保留自主工具往返，内置提示要求复核动作和目标结果。结合 002、010 复跑重复发送的原始对话 |
| `MIRA-20261126-002` | Linux 离线通过 | 逐请求工具选择、无有效工具时拒绝、提供者不遵从选择时恢复 |
| `MIRA-20261126-003` | Linux 离线通过 | Record 的确定性范围可观测、模型输出约束及逐候选拒绝 |
| `MIRA-20261126-004` | Linux 离线通过 | 会话事实可显式启用，默认拒绝并报告原因。召回区分事实与偏好，标记未验证事实 |
| `MIRA-20261126-005` | Linux 离线通过 | 按结构化身份合并证据或保留备选值。自由文本首尾句读归一化，沿用旧记录的 `subject=value` 解析 |
| `MIRA-20261126-006` | Linux 离线通过 | 发言正文与诊断的检索边界、双字命中和命中证据已验证。真实宿主语料待复跑 |
| `MIRA-20261126-007` | Linux 离线通过 | 按水位增量续建，积压可分批追平。容量、并发发布和擦除回归已验证 |
| `MIRA-20261126-008` | Linux 离线通过 | 区分缺会话绑定与跨会话拒绝 |
| `MIRA-20261126-009` | Linux 离线通过 | Layer 0 候选序号、准入原因和 ID 契约 |
| `MIRA-20261126-010` | Linux 离线通过 | 当前人格接管循环默认提示，保留显式宿主策略顺序 |
| `MIRA-20261126-011` | Linux 离线通过 | 宿主送达证据的持久登记、重启兑现、授权与版本边界已验证 |
| `MIRA-20261126-012` | Linux 离线通过 | 画像并发别名版本变化与擦除回归已覆盖。身份换绑复验随 M30-36 的纠正接线完成 |
| `MIRA-20261126-013` | Linux 离线通过 | 重开后的归属发现、分页及对现有工作读取权限的复核已验证 |
| `MIRA-20261126-014` | Linux 离线通过 | 已知别名、歧义、原文出处及无模型路径已验证 |
| `MIRA-20261126-015` | In Progress | 提及提取已逐行报告拒因和超出数量，画像属性和关系的批次处置待实现 |
| `MIRA-20261126-016` | 待实现 | 属性字段的语义说明及真实宿主语料复验 |

## Linux 离线验收

以 `03e8792` 加本维护增量验证。会话契约见
[DEC-168](../decisions/DEC-168-continuous-host-tool-and-persona-controls.md)，记忆抽取契约见
[DEC-169](../decisions/DEC-169-memory-agent-structured-proposal-output.md)。

- `ctest --test-dir build/debug -j 10 --output-on-failure` 通过 215/215。
- Continuous 回归在 1,024 次工具往返后收到终答，核对工具记录、请求级预算、历史压缩与
  独立 System 提示。宿主工具选择测试覆盖 Required 恢复、进展后恢复 Auto、空工具集合和
  未暴露的 Named 工具。人格测试覆盖两种模式的默认替换及显式宿主提示留存。
- 面向 `mira_m3_continuous_conversation_test`、`mira_m30_persona_test`、
  `mira_m30_conversation_index_test`、`mira_m30_recording_test`、
  `mira_m30_social_erasure_recovery_test`、`mira_m36_context_admission_test`、
  `mira_m36_memory_agent_model_test` 运行 Linux ASan，通过 7/7。
- 画像回归以暂停配对存储 worker 的方式固定交错顺序。TSan 直接启动遇到
  `unexpected memory mapping`，使用仓库 CI 同款 `setarch x86_64 -R` 再跑，通过 1/1，
  无数据竞争报告。
- `format-check`、`docs-check`、`sbom-check`、`platform-boundary-check`、
  `architecture-check`、改动实现文件的 `clang-tidy` 和 `git diff --check` 通过。

### 会话事实与陈述身份增量

按 [DEC-170](../decisions/DEC-170-session-fact-admission-and-claim-identity.md) 验证 004、005。
`mira_m36_memory_agent_test` 覆盖默认拒绝、同一陈述累计出处、备选值、句读归一化及按类
召回。`mira_m36_memory_agent_model_test` 验证内置模型输出的结构化身份可经 SQLite 查回，
`mira_m4_sqlite_memory_test` 覆盖写入边界和 mutation 往返。这三项 ASan 测试均通过。
Linux Debug 完整套件通过 215/215，既有 claim key 与写入策略回归通过。

### 会话发言索引增量

以 `00db8eb` 加本增量按 [DEC-171](../decisions/DEC-171-conversation-speech-search.md) 验证。
`mira_m30_conversation_index_lexical_test` 覆盖仅共享一个双字的中文问句、跨发言与单字噪声、
FTS 和精确查询的诊断排除、命中证据和分数复算，以及 schema 1.0、1.1 原地升级。
`mira_m30_conversation_index_test` 验证授权、水位和擦除。Linux Debug 通过 215/215，
上述两项 ASan 通过 2/2，格式、文档、SBOM、平台边界和架构门禁通过。
真实语料的排序与工具发声记录由 Mira Maintainers 和 mira-bridge 集成者使用原始对话及
发送回执补跑。

### 会话索引增量续建

以 `71b7697` 加本增量按 [DEC-172](../decisions/DEC-172-incremental-conversation-index.md) 验证。
`mira_m30_conversation_index_test` 以四个新增事件为一批追平 43 个事件的会话，核对从
重开后的水位续建和一次性建索引的片段 ID、来源、文本、排序及分数。每批最多读六个
事件，含两个来源核对事件。测试也覆盖事件与字节超限、一次容量诊断、并发发布冲突及
擦除中的旧批次拒绝。`mira_m30_conversation_index_lexical_test` 复核旧文件迁移。
Linux Debug 通过 215/215，两个索引测试 ASan 通过 2/2。真实宿主长会话的延迟分布由
Mira Maintainers 与宿主集成者在生产事件源和目标负载接通后补测。

### 宿主完成证据与派发归属

以 `3b4679d` 加本增量验证 [DEC-173](../decisions/DEC-173-host-verified-goal-completion.md)
和 [DEC-174](../decisions/DEC-174-dispatch-owner-discovery.md)。`mira_m30_goal_fulfillment_test`
覆盖非协调器宿主的事件位置与时间验证、撤权复核、重复和冲突证据、重启兑现、目标取消、
纠正与超期。归属测试覆盖未来待派发、已提交待核对、启用的重复计划以及逐类别授权和游标。
同一测试套件复跑协调器兑现和周期对账。Linux Debug 通过 215/215，该套件 ASan 通过 1/1。
真实渠道回执格式和宿主重启还原由 Mira Maintainers 与 mira-bridge 集成者在宿主接线后复验。

### 正文别名与模型提及

以 `e23ca39` 加本增量按 [DEC-175](../decisions/DEC-175-literal-social-mentions.md) 验证。
`mira_m30_social_inference_test` 覆盖无模型的唯一与歧义解析、ASCII 边界、空白归一化、
同环境约束、模型无效响应后的词面结果、有效行和坏行共存、超出数量、超时、取消及擦除。
`mira_m30_social_directory_test` 复核既有目录解析。Linux Debug 通过 215/215，两个专项
ASan 通过 2/2。真实群聊语料与模型质量由 Mira Maintainers 和 mira-bridge 集成者取得
原始消息和目录别名快照后复跑。

## 宿主补跑

Mira Maintainers 与 mira-bridge 集成者需要原始对话、工具执行回执以及同端点和同模型配置。
复跑时分别记录已执行的发送与后续模型响应，核对历史压缩保留的动作、未完成工作和用户
新消息，确认应答送达且模型能依据结果完成会话。真实模型的记忆提议质量、跨来源身份纠正和
目标平台矩阵仍按 [M30 验收](m30-memory-acceptance.md) 补证。
