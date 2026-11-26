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
| `MIRA-20261126-006` | 待实现 | 可召回正文投影、中文排序和按匹配项解释 |
| `MIRA-20261126-007` | 待实现 | 会话索引增量续建、结果一致性和擦除屏障 |
| `MIRA-20261126-008` | Linux 离线通过 | 区分缺会话绑定与跨会话拒绝 |
| `MIRA-20261126-009` | Linux 离线通过 | Layer 0 候选序号、准入原因和 ID 契约 |
| `MIRA-20261126-010` | Linux 离线通过 | 当前人格接管循环默认提示，保留显式宿主策略顺序 |
| `MIRA-20261126-011` | 待实现 | 非协调器宿主的目标完成证据接纳与重放 |
| `MIRA-20261126-012` | Linux 离线通过 | 画像并发别名版本变化与擦除回归已覆盖。身份换绑复验随 M30-36 的纠正接线完成 |
| `MIRA-20261126-013` | 待实现 | 重启后的派发归属发现、分页和准确授权 |
| `MIRA-20261126-014` | 待实现 | 确定性已知别名发现、歧义以及模型推断的边界 |
| `MIRA-20261126-015` | 待实现 | 社交候选逐行验收、坏行归因和容量回执 |
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

## 宿主补跑

Mira Maintainers 与 mira-bridge 集成者需要原始对话、工具执行回执以及同端点和同模型配置。
复跑时分别记录已执行的发送与后续模型响应，核对历史压缩保留的动作、未完成工作和用户
新消息，确认应答送达且模型能依据结果完成会话。真实模型的记忆提议质量、跨来源身份纠正和
目标平台矩阵仍按 [M30 验收](m30-memory-acceptance.md) 补证。
