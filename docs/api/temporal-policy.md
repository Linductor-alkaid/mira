# Temporal Policy

> 头文件：`mira/temporal_policy.hpp`

条件策略的确定性最小闭环（[DEC-037](../decisions/DEC-037-temporal-policy.md)，
[M26 里程碑](../plans/m26-temporal-policy-stage-t1.md) §4 为正式契约冻结面）：
Agent 在低频探索中验证过的重复模式，经宿主显式的归纳/采纳/测试/晋升生命周期固化为
本地 Runtime 可确定性执行的 `ReactiveRule`。纯 Core、无平台、无感知、无模型依赖；
runtime 不读系统时钟、不创建线程/定时器/Executor 注册面，全部工作为调用方驱动的
有界同步函数。规则触发不是授权来源（RULE-09）——任何真实副作用仍经
`PolicyEngine` 既有校验。

## 数据契约

- `TemporalHistoryOptions` / `TemporalHistory`：实体级有界观测环
  （FIFO 容量逐出、序列严格推进），`digest()` 为条目 canonical JSON 摘要，
  跨进程一致；`TemporalHistoryEntry.source_digest` 在 T1 恒为空串（感知侧载荷
  摘要随 T2+ 输入契约进入）。
- `TrackedEntity` / `PolicyFact` / `PolicyWorldView` / `PolicyTickContext`：
  当前 tick 的确定性输入快照；实体顺序即匹配扫描序，事实键不得重复。

## ReactiveRule schema v1

- 闭集谓词条件（`Motion`/`MotionPhase`/`MotionConfidence` 对锚实体求值，
  `Fact` 对世界事实求值）× `{Eq, Lt, Le, Gt, Ge}`；固定序 `transitions`；
  provenance `source_refs`（RULE-07，无裸规则）；`support_count` 重复验证
  证据（RULE-10）。
- `rule_id = "rr-" + 语义内容摘要前 16 hex`，全生命周期不变——语义等价的规则
  跨运行同 id，是幂等采纳的基础。
- wire schema `mira.policy.rule.v1`：`to_json()` / `from_json()` canonical
  JSON 往返字节一致；DEC-002 版本策略（`{1,x}` 读取，更老/更新 major 拒绝）。

## Runtime 与生命周期

- `IPolicyRuntime`：`activate` / `deactivate` / `is_active` / `step`；
  `ReactivePolicyRuntime` 为参考后端（Stage T5 的 HSM/BT 后端加入同一接口）。
- `step()`：校验 → 逐实体累计 `TemporalHistory`（无激活状态也累计）→ 对
  Runtime 且状态激活的规则按 `(priority 升序, rule_id 字典序)` 固定序匹配 →
  恰一匹配发射 `RuleTriggered` 并返回动作标签；多匹配冲突 fail-closed
  （零动作 + `RuleConflicted` + `PolicyEscalatedToAgent` + 匹配规则全体降级）。
  纯 `Fact` 条件规则无需锚实体，`RuleTriggered` 载荷 `entity_key` 为空串。
- 宿主显式生命周期（无自动晋升）：`induce_candidate_rules`（纯计算，精确签名
  重复计数，自覆盖规范形式）→ `adopt_candidate_rules`（采纳点发射
  `RuleCandidateInduced`，幂等跳过，上界整批拒绝）→ `test_candidate_rules`
  （逐规则隔离评估）→ `promote_rule`（零误触发 + `min_test_support` 证据门）；
  `demote_rule`（`Runtime → Candidate`，reason ∈ `conflict` | `host-explicit`）、
  `retire_rule`（终态，重复退役幂等 NoOp）。
- `temporal_history(entity_key)`：只读访问器，观测 per-entity 累计器
  （与 `rebuild_temporal_histories` 重放投影做逐字节 digest 对比）。
- `PolicyRuntimeOptions.history_options` 是累计器构造参数的唯一来源；
  `rebuild_temporal_histories` 须以同一值调用以保证逐字节一致。

## 事件面

九类 `mira.policy.*.v1` 版本化事件（T1 子集）经宿主供给的
`IPolicyEventSink` 同步内联发射；载荷为 canonical JSON，键集与帧字段分工
（`type`/`tick`/`rule_id`）逐一冻结于里程碑 §4.4 事件表。sink 契约：不得阻塞、
不得抛出。T1 不写 EventStore（`EventEnvelope` 需 Runtime/Session 帧，桥接归
T2+ Runtime 集成）。

## 错误域

`mira.temporal_policy`：`TemporalPolicyDomainCode` 12 码（显式 int32，DEC-002
稳定公开值）+ `temporal_policy_domain_code_name` 稳定名映射 +
`make_temporal_policy_error` 确定性 `ErrorCode` 分配；全部错误经既有
`Result`/`Error` 返回，不抛异常。

## 边界

- 规则产出不是授权来源（RULE-09 延伸）；规则触发不构成
  `PolicyEngine`（[安全与权限](security.md)）豁免，T1 亦不触达工具通道
  （DEC-015/DEC-021）。
- 术语消歧：Temporal Policy 与 `PolicyEngine`（安全授权）、`WorkflowPolicy`
  （执行策略闭包）是不同层的三个概念，无继承、替代或豁免关系
  （[术语表](../project/glossary.md)）。
- 不声明实时性：延迟/抖动/调度归 Stage T6 门禁（RULE-10）。

## 相关文档

- [M26 里程碑](../plans/m26-temporal-policy-stage-t1.md)（§4 冻结契约、§6 门禁）
- [Temporal Policy 设计](../design/temporal_policy_design.md)（§16 实现注记）
- [模型层与 Agent Loop](model-agent-loop.md)（离散闭环与决策解析）
- [安全与权限](security.md)（授权边界，规则非豁免）
