# World State 投影设计——会话级环境认知的共享表示

> 状态：Active（v0.1；首阶段已按 [M27](../plans/m27-world-state-projection-core.md)
> §4 冻结面交付（2026-09-28，[PR #71](https://github.com/Linductor-alkaid/mira/pull/71)，
> `Completed`）——草案与冻结面不一致处以 M27 §4 为准，实现取舍与偏差清单见
> M27 §9 第五/六次验证记录与本文 §12 实现注记）
> 版本：0.1
> 更新日期：2026-09-28
> 负责人：Mira Maintainers
> 上位决策：[DEC-041](../decisions/DEC-041-session-world-state-projection.md)
> （方向与边界）、[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)
> （实体词表）、[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)
> （双平面与 W 条款）
> 关联计划：[M27](../plans/m27-world-state-projection-core.md)（首阶段）

## 1. 背景与目标

[Issue #55](https://github.com/Linductor-alkaid/mira/issues/55) 指出的中间语义层缺口
之一：Observation 是单次快照（[DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)）、
App Model 是跨任务长期图知识（[DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)）、
Working Context 是面向模型调用的内容投影（[DEC-035](../decisions/DEC-035-context-curator-working-context.md)），
中间缺一档「Runtime 当前 believed 处于哪个状态、哪些实体存活」的会话级投影。
[DEC-041](../decisions/DEC-041-session-world-state-projection.md) 已冻结其方向：
投影而非事实源、纯函数更新、消费者只读、有界可淘汰。

本文冻结首阶段的实现形态与阶段切分；正式 schema、更新算子清单与缺省淘汰策略在
[M27](../plans/m27-world-state-projection-core.md) §4 冻结（DEC-041 §非目标条款的
指定承载面）。

## 2. 系统上下文与模块职责

```
L0 事件（EventStore，事实源，RULE-07）
        │  重放（重建配方 = 更新算子序列）
        ▼
WorldState 投影（纯函数核：值类型 + 更新算子 + rebuild）      ← 本设计范围
        │ 只读
        ├─ Context（DEC-032/035 输入源之一）      （消费接线随其立项）
        ├─ Recovery（DEC-031 续跑上下文）          （消费接线随其立项）
        ├─ Workflow 导航（DEC-028 会话侧对应物）   （消费接线随其立项）
        └─ Temporal Policy（DEC-037 T2+，经词表映射，词表设计 §3）
```

- **投影核**（首阶段交付）：`WorldState` 值类型 + 更新算子（纯函数）+
  `rebuild_world_state` 只读重建投影。无独立执行面、无后台任务、无全局可变状态。
- **消费接线**（后续立项）：把投影核接入 Runtime/Session 生命周期、观察热路径与
  各消费者，属新的公开契约面与集成形态，随各自里程碑冻结。
- **事实源**：EventStore 不变（`W-03`：投影可重建，[agent_harness 设计](agent_harness_and_workflow_architecture.md) :154）；
  Runtime 对投影的认可与修正以事件留痕的形态随消费接线立项冻结。

## 3. 数据模型（草案）

投影维护三块会话内状态，全部从 L0 事件确定性重建：

1. **页面假设**：当前 believed 的 `(app_id, state_id)` 二元组（[实体词表](entity_vocabulary_design.md)
   §2.2）+ 置信 + 陈旧标记 + 显式 `Unknown`（无供给或不可判定时，不以猜测冒充，
   RULE-09/RULE-10 精神）。
2. **存活实体表**：`ElementRef` 证据引用（词表 §2.1）+ 置信 + 陈旧标记 + 最近
   见证时间；容量有界（RULE-08），淘汰按显式确定性策略。
3. **最近状态变化环**：有界变化条目（种类闭集 + 源事件摘要 digest + 有界摘要），
   大内容以稳定引用承载、不在条目内复制。

字段命名与 wire 形态以 M27 §4 冻结面为准（本节为草案，沿
[Temporal Policy 设计](temporal_policy_design.md) §4 前注「字段命名以届时契约为准」
同一安排）。

## 4. 更新语义

- **纯函数**：当前投影 + 输入记录 + 显式时间 → 新投影；不读时钟
  （[DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md) 置信度
  纯函数纪律）。同输入序列（含显式过期指令）+ 同选项 → 同投影、跨进程
  digest 一致是首阶段验收目标；更新算子显式携带 `WorldStateOptions`（容量/
  摘要上界/陈旧界的唯一取值来源，投影值类型不内嵌选项）。
- **输入记录（六类）**：观察记录（app context + epoch + `ElementRef` 集）、宿主
  识别（`ScreenStateSnapshot` 同型，DEC-027 §3）、导航观测
  （`WorkflowNavigationObserved` 同型 + 应用域）、工具结算（toolref 引用 +
  处置闭集；core 侧登记级检查，语法解析权威在 workflow 模块）、验证结算
  （页面假设的确认/反驳/不确定）、显式过期指令（陈旧判定的重放形态）。
  全部输入引用 [实体词表](entity_vocabulary_design.md) v1，词表外引用
  fail closed。
- **显式表达**：观测冲突以冲突标记留痕（不静默覆盖）；证据过期经显式过期算子
  以调用方时间判定；无供给进入 `Unknown`。
- **淘汰**：实体表超容量按显式确定性淘汰序逐出；淘汰过程可从事件重放验证
  （DEC-041 §决策 8）。缺省容量与淘汰序为暂定默认值，随 M27 §4 冻结并标注
  负责人与最迟冻结里程碑（项目管理规范 §8）。

## 5. 与相邻面的边界

- **Observation**：契约不变（DEC-005/DEC-041 §决策 4）；投影消费观察产生的记录
  元数据与结构化证据引用，不修改 Observation schema（加法版本化除外）。
- **App Model**：当下假设（会话投影）vs 长期知识（版本化资产，`W-03`）；投影引用
  `state_id` 表达页面假设，不写 App Model。
- **Behavior Trace（DEC-038）**：并列投影、互不隶属，都从 L0 重建（DEC-038 §决策 5）；
  两者经 [实体词表](entity_vocabulary_design.md) 对齐，Trace 不维护 believed 状态、
  World State 不承载行为历史。DEC-038 首阶段随后立项并引用同一词表。
- **Temporal Policy**：T1 的 `PolicyWorldView` 是局部契约（[M26](../plans/m26-temporal-policy-stage-t1.md)
  §4.2）；T2+ 消费投影时经词表 §3 显式映射，本设计不预承诺映射契约。
- **授权与安全**：World State 只是数据，不提升权限、不构成动作授权
  （authority 在 [DEC-004](../decisions/DEC-004-security-authority-confirmation.md)
  门禁与 `W-05`/`W-06` 路径）；`believed`/`Stale`/`Unknown` 的假设性在类型与
  命名上显式表达。

## 6. 所有权、并发与 Executor 路由

- **首阶段零 Executor 注册面**（沿 [M26](../plans/m26-temporal-policy-stage-t1.md)
  §4.5 形态）：更新算子与重建全部是有界同步纯函数，由调用方/harness 同步驱动；
  不创建线程、定时器、周期任务或 realtime lane。评估 harness 编排任务经
  `submit_auto()` 提交且 future 必消费（AGENTS.md Executor 纪律第 3 条）。
- **生产形态注记**：投影更新贴近观察热路径（DEC-041 §影响与风险）；增量更新、
  水位与预算控制在观察路径上的集成形态随消费接线立项冻结（全量重建仅用于恢复
  与审计）。本阶段不声明任何更新延迟或实时性（RULE-10），只冻结容量上限缺省值。
- **生命周期**：投影值类型无自有资源、无关闭面；会话级 owner 与 checkpoint/恢复
  集成随消费接线立项（DEC-041 §决策 7 的落地形态）。

## 7. 错误处理与 fail-closed

- 词表外引用（未知事件类型、toolref 语法违规、空 `state_id`、关键字段缺失的
  `ElementRef`）→ 显式词表违规错误，拒绝进入投影（不猜测、不丢字段）。
- 输入记录结构非法、载荷超限 → 显式拒绝，不截断冒充。
- wire 反序列化：DEC-002 reader 纪律——当前/上一 major 读取、未知更新 major 拒绝、
  未知字段 fail closed。
- 错误域沿 `mira.*` 点分惯例与显式 int32 枚举 + 稳定名映射先例
  （`ContextDomainCode`/`mira.temporal_policy` 同构）；逐码触发条件随 M27 §4 冻结。

## 8. 安全、隐私与可观测性

- 投影载荷只含 ID、digest、枚举名、有界摘要与数值；截图等大内容以稳定引用 +
  摘要承载，不在多个条目复制（DEC-022 §5 / DEC-029 §2 同源边界，AGENTS.md
  事件纪律）。
- 敏感前台应用（`AppContext.sensitive`）进入投影时保持标记，消费侧按既有脱敏
  规则处理；投影不新增日志面。
- 投影 `digest()` 提供跨进程一致性核对锚点（M16–M20 口径），重建路径以
  digest 相等证明可重建（`W-03`）。

## 9. 备选方案与已知限制

- 方向级备选（内嵌 Observation、扩展 App Model、可写事实源、Context Manager
  兼职、各子系统自持假设）已在 [DEC-041](../decisions/DEC-041-session-world-state-projection.md)
  §备选方案逐一否决，本文不重复。
- 已知限制：跨帧实体同一性归感知侧，投影内实体条目以证据引用为身份，跨 Observation
  的「同一元素」表达力有限（[实体词表](entity_vocabulary_design.md) §6）；`Fused`
  来源无生产者；页面级验证结算为首阶段冻结范围，实体级验证语义不进首阶段。

## 10. 分阶段落地与测试策略

- **首阶段（[M27](../plans/m27-world-state-projection-core.md)，已立项冻结）**：
  投影核——值类型 + 更新算子 + 重建 + 淘汰 + 错误域 + wire schema；门禁
  `WS-G1`–`WS-G7`（`tests/m27/`），全部确定性断言，无平台、无感知、无模型依赖。
- **消费接线（后续立项）**：Runtime/Session 生命周期集成、观察热路径增量更新、
  事件留痕、消费者（Context/Recovery/导航/Temporal Policy T2+）接线、checkpoint/
  恢复矩阵——各自里程碑文件冻结，本设计不预分配编号。
- 测试策略：词表目录一致性 + fail-closed 负向矩阵、更新算子正负矩阵、重建与
  增量终态 digest 一致、淘汰可重放、wire 往返与 DEC-002 版本策略、错误域逐码
  断言；测试的编写与取证由 Independent-Verification-Agent 独立完成（AGENTS.md
  用户级指令）。

## 11. 关联决策、计划与文档

- [DEC-041](../decisions/DEC-041-session-world-state-projection.md)（方向）、
  [DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md) +
  [实体词表设计](entity_vocabulary_design.md)（词表）、
  [DEC-002](../decisions/DEC-002-public-contract-versioning.md)（版本化）、
  [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)/
  [DEC-027](../decisions/DEC-027-app-model-contract-and-confidence.md)/
  [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)（观察与识别边界）、
  [DEC-038](../decisions/DEC-038-unified-behavior-trace.md)（并列投影）
- [M27](../plans/m27-world-state-projection-core.md)（首阶段立项与正式契约冻结面）
- [Temporal Policy 设计](temporal_policy_design.md) §16（`PolicyWorldView` 局部
  契约与词表对齐注记）、[术语表](../project/glossary.md)（World State 词条）

## 12. 实现注记（首阶段交付后回填，2026-09-28）

首阶段（[M27](../plans/m27-world-state-projection-core.md) `Completed`）按 §4
冻结面实现并经 IVA 矩阵复验；草案 v0.1 与冻结面/实现的关键差异以 M27 §4 与
§9 为准，要点留痕：

- options 归属：值类型不内嵌、wire 无 options 键、八算子显式携带
  `const WorldStateOptions &`（§4 B-A 修订；本草案第 3 节早期形态未定此点）。
- 过期指令显式入序列：`WorldExpiryInput{now}` 为第六类输入，`expire_stale`
  冻结为 `apply_expiry`，含过期推进可重放（B-B 修订）；`prune_entities` 为
  非重建语义的审计便利入口。
- toolref 校验归调用方：core 只做登记级检查（非空 + ≤ 256 字节），语法权威
  在 workflow 模块 `parse_tool_reference`（B-C 修订，依赖方向约束）。
- 导航 `from_state` 对账为 `ConflictMarked` 唯一触发；识别/成功导航正常留痕
  不记冲突（B1 修订）；错误码折叠规则见 M27 §4.5（B2 修订）。
- 前台观察无条件逐字段覆盖，`same_foreground_content` 仅门控 `PageAssumed`
  环条目追加（IVA 复验修正，M27 §9 第六次）。
- `Timestamp::wall` 为 `WallTimePoint`（system_clock 上显式纳秒精度），wire
  wall 纳秒整数逐位精确往返、陈旧判定 1 ns 粒度（跨平台修正，M27 §9 第十次）。
