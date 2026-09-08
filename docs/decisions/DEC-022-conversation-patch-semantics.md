# DEC-022：对话 patch 语义与 Conversation 工件

> 状态：Accepted
> 日期：2026-09-08
> 负责人：Mira Maintainers
> 冻结里程碑：[M8](../plans/m8-workflow-contracts.md)（`M8-04`/`M8-10`）
> 替代/被替代：无（承接 [DEC-016](DEC-016-conversation-events-and-user-messages.md) 预留的
  完整语义冻结点）

## 背景与问题

架构设计 §7.4 要求对话驱动的 Workflow 介入是 Harness 原生能力：patch 幂等、可审计、可
撤销到版本边界；「以后默认都不要加附言」类指令必须区分「本次运行修改 / Workflow 定义
修改 / 用户偏好记忆」三个目标，歧义时向用户确认（`W-04`/`W-05`）。DEC-016 已交付最小
机制（步边界消息队列、对话事件、会话投影），完整 patch 语义留待本决策。
`RISK-2026-036` 提醒对话 patch 语义范围过大、难以一次冻结；本决策冻结语义与 schema，
交互实现与策略全集留给阶段 C，无法收敛的子项显式列为开放问题。

## 决策

### 1. 三类 patch 目标的判定与歧义确认

对话介入的解释结果是三选一（封闭枚举 `PatchTarget`）：

| 目标 | 效果 | 风险与可逆性 |
| --- | --- | --- |
| `run_parameters` / Run 修改 | 修改本次 Run 的有效参数或步骤实参（含跳过/替换步骤、切换策略，形态见 [DEC-021](DEC-021-workflow-tool-channel.md) §2） | 影响范围单次执行，可回退到 patch 前边界 |
| Workflow 定义修改 | 产生新版本（不可变追加，[DEC-020](DEC-020-workflow-run-lifecycle.md) §4），必须通过验证后入库 | 长期资产变更，回退 = 回滚到旧版本 |
| 用户偏好记忆（User Model） | 经既有 `update_memory` 规则更新偏好（DEC-004：User-scope 默认人工审批） | 跨任务长期生效，受授权范围约束 |

判定规则（按序）：

1. **显式标记优先**：介入文本含明确指向（如「以后」「默认」指向定义/偏好；「这次」
   「本次」指向 Run）时按标记归类。
2. **缺省归 Run 修改**：无法判定时默认只改本次 Run——影响最小、可逆、不需要新版本。
3. **歧义升级 `WaitingUser`**：当文本同时指向多个目标（如「以后发日报都改成李四，这次
   先别发」混合两类），或指向定义/偏好但后果不可逆且未获显式确认时，Run 进入
   `WaitingUser` 决策点向用户确认；确认前不应用任何 patch（`W-05`：不借「流式体验」
   绕过确认）。
4. 判定本身是模型参与的语义解释（阶段 C 实现交互闭环）；本决策冻结判定规则、目标枚举
   与确认义务，不冻结提示词或解释器实现。

### 2. Patch 的幂等、审计与版本边界回退

- **幂等键**：每个 patch 携带 `patch_id`（稳定 ID）与 `patch_digest`（patch 条目的
  canonical digest）。同一 `patch_id` 重复提交：digest 相同 → NoOp 成功（幂等）；
  digest 不同 → `AlreadyExists` 拒绝（不允许复用 ID 改写内容）。比按到达序号匹配更强
  （参考研究 §5.2 对 LangGraph 索引匹配的改进）。
- **审计**：patch 生命周期是三事件：`WorkflowPatchProposed`（含目标、条目摘要与
  digest，不含参数明文）→ `WorkflowPatchApplied` 或 `WorkflowPatchRejected`（含原因
  码）。事件属 `EventClass::State`，随 EventStore 持久化（DEC-003）。
- **版本边界回退**：Run 参数 patch 形成可回退边界（`run_patch_epoch` 单调递增）；回退
  = 显式提交一条回退 patch（指向前一边界的 digest），而不是隐式恢复快照。Workflow 定义
  patch 永不修改历史：回退是创建一个内容等于旧版本的新版本（`W-03`）。
- **生效点**：patch 在步边界生效（复用 DEC-016 的步边界 drain 机制：排队消息在步边界
  被消费并解释）；当前正在执行的步骤不受影响，完成后按新参数继续。跳过/替换步骤的
  patch 不得打断已派发动作的验证（`RULE-05`：已派发动作必须重新观察验证）。

### 3. `WaitingUser` 决策点的解决语义

- 决策点携带稳定 ID 与载荷 digest（`pending_decision`）；用户答复（经对话或宿主 UI）
  按 ID + digest 匹配解决：digest 不匹配（决策点已被替代/撤销）→ 拒绝并按当前决策点
  重新询问，不误配。
- 解决结果是封闭枚举：`accept`（按提议应用）、`reject`（保持现状并继续）、`cancel_run`。
  答复不直接成为任意 Workflow 变更：提议内容仍走 §2 的校验与事件管线（`W-04`：模型
  输出与用户答复都不是免验通道）。
- `request_user_input` 的 Tool 席位：Agent 可经 Tool 通道发起决策点（席位预留，
  [DEC-021](DEC-021-workflow-tool-channel.md) §4）；其 schema 在阶段 C 随交互实现冻结。

### 4. Conversation History 与 Execution Trace 分离（§6.3 落地）

- Conversation History（`User ↔ Agent`：用户消息、澄清、patch 提议与结果摘要、自然语言
  结果）与 Execution Trace（EventStore 全量：工具调用、步骤、观察、验证、恢复）职责
  不同，不得互相替代。LLM Context 从 Trace 中按相关性提取（Context Manager 既有预算
  机制，Workflow/Run 上下文作为新分区接入，架构 §6.4）。
- Conversation 视图是可重建投影（DEC-016 已交付 `build_conversation_view`）；本决策将
  Workflow 介入纳入投影：patch 三事件与 `WaitingUser` 决策点的提出/解决在视图中显示为
  对话条目（LoopOutcome 之外的第三类 `Kind::WorkflowEvent`，载荷为摘要级）。

### 5. Conversation 工件的脱敏与保留策略

- **脱敏**：事件与 patch 载荷只携带 ID、digest、枚举、原因码与受限长度（默认 2 KiB）的
  已脱敏摘要；参数明文（可能含用户内容：联系人、消息文本）一律不进事件，需要审计时以
  ArtifactRef（digest + 敏感级别）引用（沿用既有工件机制）。入队文本的脱敏责任在宿主
  （DEC-016 第 4 条不变）；Mira 不在事件中猜测敏感字段。
- **保留**：Conversation 工件的保留随 Session 数据策略（`ModelDataPolicy` 的既有保留
  字段与授权范围）；EventStore 事实记录按 DEC-003 保留，投影可按策略重建或截断。训练
  导出默认关闭且与对话工件无关（`RULE-12`）。
- **回放**：OfflineReplay 重建对话视图与 Run 视图时不产生任何副作用（`W-08`）；patch
  事件重放为「已记录结果」，不重新应用。

### 6. 开放问题（显式列出，不用含糊措辞掩盖）

- 决策点超时策略（用户长时间不答复：保持等待 vs 降级暂停）——阶段 C。
- 多个并发决策点的排序与合并——阶段 C（当前约束：同一 Run 同时至多一个
  `pending_decision`，v1 冻结该上限）。
- 对话分叉（从某事件分叉出新会话线）的投影表达——参考研究 §5.2 提出，未冻结。
- patch 条目级权限差异（`execution_policy` 切换是否需要更高确认级别）——随阶段 C 的
  策略全集冻结；M8 按「策略切换只能在 `allowed_policies` 内且经控制面」处理。

## 备选方案

- **不区分三类目标，全部当作 Run 修改**：「以后默认」类指令会丢失长期意图，违背 §7.4
  的产品语义；把定义修改也当 Run 修改则用户意图被静默降级。不采用。
- **歧义时默认应用到影响最大的目标**：方向错误时不可逆（新版本/长期偏好），违反
  `W-04`/`W-05`。不采用；缺省归最小影响（Run 修改），歧义升级确认。
- **patch 直接改写 Workflow 历史版本**：违反不可变历史（`W-03`）。不采用。
- **Conversation 单独持久化存储（第二事实源）**：违反 `RULE-07`/DEC-003。不采用；视图
  始终从 EventStore 重建。

## 影响与风险

- 公开契约新增 patch 条目/事件 schema、`pending_decision` 结构与决策点解决纯函数
  （M8-10 实现）；`ConversationEntry` 扩展第三类条目（投影层向后兼容：旧事件不受影响）。
- 语义解释（自然语言 → patch 目标判定）依赖模型，阶段 C 前不得宣称对话介入可用；M8
  冻结的是「解释结果如何被校验、应用与审计」。
- 参数明文不进事件的约束使审计必须配合 Artifact 机制，宿主需要理解该分工（写入 API
  手册）。

## 验证方式

- 契约测试：patch 幂等（同 ID 同 digest NoOp、同 ID 异 digest 拒绝）、三事件载荷形状
  与脱敏断言（参数明文不出现）、决策点 ID+digest 匹配与不匹配路径、回退 patch 语义、
  投影含 WorkflowEvent 条目、OfflineReplay 重建无副作用。
- 歧义确认与目标判定的端到端测试随阶段 C 交互实现补充。

## 关联文档和工作项

- [M8](../plans/m8-workflow-contracts.md)：`M8-04`、`M8-10`
- [DEC-003](DEC-003-event-sourced-persistence.md)、
  [DEC-004](DEC-004-security-authority-confirmation.md)、
  [DEC-016](DEC-016-conversation-events-and-user-messages.md)、
  [DEC-019](DEC-019-workflow-ir-contract.md)、
  [DEC-020](DEC-020-workflow-run-lifecycle.md)、
  [DEC-021](DEC-021-workflow-tool-channel.md)
- [Agent Harness 参考研究](../design/harness_reference_study.md) §5.2（按 ID 解决决策点）
- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
