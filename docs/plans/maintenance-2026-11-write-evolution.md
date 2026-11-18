# 维护计划：Memory Core 写入面分层解耦（2026-11）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M36 后维护轮）
> 前置：[MNT-202611-ME 抽取供给](maintenance-2026-11-extraction-supply.md)
>（同日交付，DEC-114「模型候选永不取代」是本轮冲突语义的输入）
> 建议发布点：Host contract alpha 修订（变异类型扩展 + 策略默认值变更 +
> 管道语义修订，见 DEC-115 影响与风险）
> 更新日期：2026-11-18

## 1. 目标

把 Memory Core 写入管道的「信息留存 / 证据积累 / 权威事实更新」解耦：
冲突分类处理（纪元接续 Supersede vs PreserveAlternative 并存）、重复写入
证据追加与严格验证晋升、Preference kind 审批默认取消（DEC-109 API 不变）、
陈旧保护改按事实纪元、截断可见化。安全门、kind floors、CAS/scope/台账
语义全部不变。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/memory_contracts.hpp` / `src/context/memory_contracts.cpp`：
  `MemoryMutationType` +AppendEvidence/PreserveAlternative、wire 名表、
  `memory_mutation_from_json` 未知 type fail-closed、`MemoryMutation::validate`
  新 case、`MemoryRecord.alternative_of`（document minor {1,2}）、
  `ConsolidationEntry.incumbent`、`ConsolidationReport.candidates_truncated`、
  `ConsolidationPolicy` 默认审批清空 + `max_alternatives_per_claim`。
- `src/storage/sqlite_memory_store.cpp`：apply() 显式新 case（AppendEvidence
  版本不动、PreserveAlternative 类 Add + alternative_of 校验）+ document
  往返。
- `src/context/memory_consolidation.cpp`：冲突分类分析、纪元接续判据、
  截断计数、valid_from 回填。
- `src/context/context_working_context_promotion.cpp`：cap 跳过计数。
- 测试：新 `tests/m4/m4_write_evolution_test.cpp`；钉死用例 consciously
  更新（清单见 WE-05）。
- 示例：`examples/extraction_supply_host_consumer.cpp` 断言同步 + 并存场景。
- 文档：DEC-115（新增）、DEC-109 修订注记、设计三节、API 两处、术语表、
  m23 计划注记、README、总计划、本计划。

### 2.2 非目标（DEC-115 明示）

价值/访问统计遗忘与聚合；NLP 级冲突理解；持久 pending 队列；claim-key
列/索引；preference 存储 whitelist 扩展；DEC-109 API 语义变更；旧二进制
forward-compat 升级器。

## 3. 设计与决策依据

- 决策记录：[DEC-115](../decisions/DEC-115-write-path-decoupling.md)。
- 实现事实：同 claim-key Active 记录本可共存（无唯一约束）；台账 TEXT 列
  免迁移；Update 类型闲置；duplicate-noop 在审批闸之前。
- 钉死测试地图：m4_consolidation（duplicate-noop/审批默认/注入门）、
  contract/verified_fact（观察时间陈旧/legacy 边界）、m23（无降级矩阵——
  必须保持绿）、m30（审批/记录）、m4_claim_key（两值并存/取代）、
  m4_extraction_supply（模型 Preference）。

## 4. 工作项（跑前冻结）

- [x] `MNT-202611-WE-01` 立项与跑前冻结：DEC-115 落成、DEC-109 修订注记、
      本计划冻结。
- [x] `MNT-202611-WE-02` 契约：变异类型/validate/wire 表/JSON fail-closed/
      `alternative_of`/policy/report 字段；document minor {1,2}。
- [x] `MNT-202611-WE-03` store：apply() AppendEvidence（CAS 核对不消费、
      版本不动、provenance 合并幂等）与 PreserveAlternative（alternative_of
      校验、类 Add）；switch 全枚举覆盖。
- [x] `MNT-202611-WE-04` 管道：冲突分类分析（duplicate/alternative 族）、
      纪元接续判据、模型候选永不取代、审批默认清空、截断计数、valid_from
      回填、提升投影 overflow 计数。
- [x] `MNT-202611-WE-05` 测试：钉死用例 consciously 更新（verified_fact/
      m4_consolidation/m30_recording/m4_extraction_supply/m4_claim_key/
      m23_promotion——m23 无降级矩阵保持绿）+ 新 `m4_write_evolution_test`
      全门。
- [x] `MNT-202611-WE-06` 示例：extraction_supply 断言同步（自动写入）+
      并存场景。
- [x] `MNT-202611-WE-07` 文档同步：设计 §13.3/§13.4/§13.5、API 两处、
      术语表、m23 注记、README、总计划。
- [x] `MNT-202611-WE-08` 全量验证：构建 + `ctest --preset debug` 全过 +
      门禁四目标 + static-analysis 零告警 + clang-format；证据回填。

### 跑前冻结八问

1. **行为**：DEC-115 决策 1–4；安全门/floors/DEC-109 API 不变；设计三节
   修订。
2. **所有者**：宿主拥有 policy 配置与审批流；Core 拥有管道分类、变异
   语义与 store 路径。
3. **契约面**：`memory_contracts.hpp`（变异类型/记录字段/策略/报告）、
   `memory_consolidation.hpp`（entry/report）、store 行为语义（无 DDL）。
   document minor {1,2}。
4. **层与依赖**：改动全部在 core 内（storage+context）；无新外部头族；
   policy 不变。
5. **复用**：claim-key/same-claim 现算、memory_provenance 表 INSERT OR
   IGNORE、CAS/台账语义、既有测试固定装置；不新增平行写入路径。
6. **时序幂等**：AppendEvidence 幂等（event-id 去重合并）+ 台账重放；
   PreserveAlternative 走 Add 的 id 纪律；纪元判据是纯函数；截断计数
   确定性。
7. **Executor 路由**：写入路径不变（Record job blocking worker、supervisor
   `schedule_mutation` Critical、consolidation Deferrable 原样）。
8. **验证上下文**：钉死用例 consciously 更新清单 + 新门 + 全量 ctest +
   四门禁 + tidy；CI 平台矩阵随 PR。

## 5. 风险与阻塞

- 并存主张增加召回候选多样性：claim-key 查询已禁用多样性上限 + token
  打包纪律，冲突裁决归消费方（DEC-112 分层先例）；基线对照随验证记录。
- 默认审批取消改变三条路径的写入行为（deterministic/提升/Record 模型
  偏好）：全部有钉死测试 consciously 更新 + DEC-109 API 路径用例保持
  Pending 证明不受影响。
- 纪元判据依赖 `valid_from` 质量：agent Record 候选本轮回填（=recorded_at）；
  后续生产者应尽量声明真实纪元。
- 无 Executor、平台阻塞；无真实供应商依赖。

## 6. 测试与退出条件

- [x] 新契约测试全门过（AppendEvidence/PreserveAlternative/纪元接续/
      晋升只升不降/模型不取代/截断计数/DEC-109 仍 Pending/台账重放）。
- [x] 钉死用例 consciously 更新后全绿；m23 无降级矩阵保持绿；安全门回归
      原样全绿。
- [x] 全量 `ctest --preset debug` 通过（~195+ 项）。
- [x] `architecture-check`、`platform-boundary-check`、`docs-check`、
      `format-check`、static-analysis 改动文件零告警。
- [x] `extraction_supply_host_consumer` 更新后正常退出（0）。
- [x] 文档同步完成（设计三节/API 两处/术语表/m23 注记/README/总计划）。

## 7. 验证记录

### 2026-11-18：立项与跑前冻结

- DEC-115 落成（修订 §13.3/§13.4/§13.5 语义与审批默认；DEC-109 API 不变）；
  DEC-109 加修订注记；本计划冻结。
- 未执行项：WE-02…08。

### 2026-11-18：实现、测试与文档同步（WE-02…08）

- **范围**：契约层（`MemoryMutationType` +AppendEvidence/PreserveAlternative、
  wire 名表、from_json 未知 type fail-closed、`MemoryRecord.alternative_of`
  document minor {1,2}、`ConsolidationEntry.incumbent`、
  `ConsolidationReport.candidates_truncated`、`candidates_examined` 语义改
  截断前全量、`ConsolidationPolicy` 审批默认清空 + `max_alternatives_per_claim=4`）；
  store apply 显式 AppendEvidence（CAS 核对不消费、版本不动、provenance 幂等
  合并）与 PreserveAlternative（alternative_of Active 校验、类 Add）case，
  switch 全枚举覆盖；管道冲突分类分析重写（duplicate/alternative 族、纪元
  接续判据、模型候选永不取代、严格晋升、截断计数、valid_from 回填）；
  提升投影 `dropped_overflow` 计数；`m4_write_evolution_test` 六门；八处钉死
  用例 consciously 更新；示例并存场景；文档七处。
- **依据**：[DEC-115](../decisions/DEC-115-write-path-decoupling.md)。
- **环境**：Linux 7.0.0-43-generic x86_64；gcc 13.3.0；clang-tidy 18
  （static-analysis preset）；基线 = master `69bf4eb`（同日 DEC-113/114 交付后）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警 |
  | `ctest --preset debug -j 14` | **192/192 通过**（新增 m4_write_evolution 六门；11 个受影响钉死用例 consciously 更新后全绿，含 m23 无降级矩阵） |
  | architecture / platform-boundary / docs / format 四门禁 | 全部通过 |
  | static-analysis（clang-tidy 18） | 全部受影响目标零告警 |
  | `mira_extraction_supply_host_consumer` | 退出 0（自动写入 + 并存场景 + 降级轮） |

- **钉死用例 conscious 更新清单**（旧行为 → 新行为，全部有意）：
  duplicate-noop → evidence-appended（重复携带新事件即证据累积）；观察时间
  陈旧拒绝 → 纪元并存/纪元接续；Preference PendingApproval → 自动写入
  （审批机器以 policy 显式重臂的用例保留）；提升 changed-value Supersede →
  PreserveAlternative 并存；晋升候选永不取代在位者。
- **实现期发现（后续轮次参考）**：
  1. `to_json_string(JsonValue(text))` 产出完整带引号 token——responses wire
     body 拼接前后缀不可再补引号（DEC-114 轮已见，本轮示例复踩一次后修正）。
  2. strict 输出契约下模型输出裸数组会被 fail-closed 降级（DEC-114 轮发现，
     本轮示例沿用正确包裹）。
  3. `MemoryDataPolicy.store` 是 `optional<bool>`：`!x` 测 engaged 非 falsy。
  4. 模型候选经命名空间导入契约可达 kind 闸之外（默认 exemption=true）；
     DEC-101 agent 路径强制关闭——两条路径行为分叉是设计预期。
  5. `IConsolidationModel::propose` 无错误通道（接口冻结）——fail-closed 只能
     经异常表达；`degrade_on_error` 双态契约先行（DEC-114）。
  6. 纪元接续取代 era head 后，更旧纪元的并存主张仍 Active——检索面同 key
     可返回多纪元记录，冲突裁决归消费方（与 DEC-112 分层一致）。
- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 与 Windows/Android 未本地执行，随 PR CI；按测试分层，
    发版前全量独立验证另行委派。
  - 并存主张对检索面召回多样性的量化对照未做（结构上与 duplicate-noop 之前
    相比候选变多；消费方裁决语义归 DEC-112 分层）。
  - 超限候选持久 pending 队列、基于价值的遗忘/聚合：非目标，后续 DEC。
- **同步**：设计 §13.3/§13.4/§13.5、`docs/api/context-memory.md`（冲突处置/
  截断/提升计数）、`docs/api/memory-agent.md`（DEC-109 不变注记）、
  `docs/project/glossary.md`（纪元接续/并存主张/证据追加词条）、
  `docs/plans/m23-memory-promotion-stage-w4.md`（§4.1 修订注记）、
  `docs/decisions/DEC-114-…`（模型候选永不取代注记）、`README.md`、
  `docs/plans/mira-implementation-plan.md`、`docs/decisions/DEC-115-…`（新增）、
  `docs/decisions/DEC-109-…`（修订注记）、本计划。
