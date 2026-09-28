# World State 投影

> 头文件：`mira/world_state.hpp`

会话级「Runtime 当前 believed 环境认知」的确定性投影核
（[DEC-041](../decisions/DEC-041-session-world-state-projection.md) 首阶段，
[M27 里程碑](../plans/m27-world-state-projection-core.md) §4 为正式契约冻结面）：
前台应用信念、页面假设（`Believed`/`Stale`/`Unknown` 三态）、有界存活实体表与
有界最近变化环，实体引用一律来自[实体词表](../design/entity_vocabulary_design.md)
（[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)）。纯 Core、无平台、
无感知、无模型依赖；不读系统时钟、不创建线程/定时器/Executor 注册面（§4.6），
全部入口为调用方驱动的有界同步函数。投影只是数据、不构成动作授权（RULE-09）。

## 数据契约

- `WorldStateOptions`：策略而非状态——实体容量/变化环容量/摘要字节上界/陈旧界；
  值类型不内嵌 options、wire 无 options 键，八个算子全部显式携带
  `const WorldStateOptions &` 并各自维护容量与摘要不变量；任一容量 0 →
  `OptionsInvalid`。缺省值（128/64/2048/30s）为 RULE-08 已记录的暂定默认，
  最迟冻结点为消费接线里程碑。
- `WorldState`：`schema_version{1,0}`、前台信念
  `WorldForegroundApp`（`status == Unknown` ⇔ `package_name` 为空）、页面假设
  `WorldPageAssumption`（`(app_id, state_id)` 二元组，Unknown 时同为空且
  confidence 为 0）、实体表 `WorldEntity`（`ElementRef` 七字段证据引用——
  证据非永久身份、confidence、stale、`source_event_digest`）与变化环
  `WorldChange`（实例内严格单调 `change_sequence`）。
- `validate()` 只查结构不变量（嵌套 validate、枚举域、序列严格推进）；容量与
  摘要上界是算子不变量而非值不变量。嵌套失败一律折叠 `StateInvalid`。

## 输入记录（闭集，唯一写入口）

六类输入 = 五类更新 + 一类显式过期指令：`WorldObservationInput`（观察，
含前台 `AppContext` 与 `ElementRef` 列表）、`WorldScreenStateInput`（宿主
识别）、`WorldNavigationInput`（导航）、`WorldToolSettledInput`（工具结算）、
`WorldVerificationInput`（验证结算，首阶段范围 = 页面假设）、
`WorldExpiryInput`（过期指令，`expire_stale` 的可重放形态）。
词表违规（nil `observation_id`、非法 space、空 `state_id`/`app_id`、空或
超 256 字节 `tool_ref` 等）→ `VocabularyViolation`（仅算子输入路径产生；
`validate()`/`from_json()` 永不产生该码）。

## 更新算子（冻结八个，纯函数）

`apply_world_observation` / `apply_screen_state` / `apply_navigation` /
`apply_tool_settled` / `apply_verification` / `apply_expiry` /
`prune_entities` / `rebuild_world_state`。要点：

- 强一致失败语义：算子失败时投影原值不动、只返回错误；时间只来自输入，
  永不读时钟。
- 前台观察无条件逐字段覆盖（同内容后到观察仍刷新 `observed_at`；空
  `package_name` 复位 `Unknown`）；实体按七字段查找刷新或插入，插入溢出
  先执行冻结淘汰序。
- `ConflictMarked` 唯一触发：导航 `from_state` 对账（投影现值二元组 ≠
  声称出发二元组 → 一条冲突条目，转移语义照常）；屏幕识别与成功导航是
  正常信念更新（`PageAssumed`/`NavigationObserved` 留痕），不记冲突。
- 淘汰序冻结：stale 优先 → 最早 `last_seen_at` → 最低 confidence →
  `ElementRef` canonical JSON 字典序；`prune_entities` 为审计便利入口，
  不在重建等价配方内。
- 陈旧判定以 `Timestamp::wall` 差值对 `options.stale_after` 比较（1 ns
  粒度）；已 `Stale` 条目不重复衰减，`Unknown` 页面无可陈旧信念。
- 验证结算：`Unknown` 目标页 → `StaleTargetMissing`（显式失败，不静默
  插入）；confirmed/refuted 以输入 confidence 与 `verified_at` 落新信念
  （新结算的假设不被既有陈旧界立即淘汰）；inconclusive 零变化。
- 每条目 `source_event_digest` = 产生该条目的输入记录 canonical JSON 摘要。

## wire schema 与错误域

- `mira.worldstate.v1`：`to_json()` / `from_json()` canonical JSON 字节一致
  往返；`digest()` 为 wire 无 options 形式的 canonical 摘要，跨进程一致
  （`WS-G3`）。DEC-002 版本策略：`{1,x}` 读取，更老/更新 major →
  `SchemaUnsupported`；解析失败/未知或缺失成员/成员类型错 →
  `PayloadMalformed`；成员值非法（含嵌套折叠）→ `StateInvalid`。
- `mira.world_state` 八码错误域（DEC-002 稳定公共值，`int32` 1..8 + 稳定
  名 + 确定性 `ErrorCode` 映射）：`OptionsInvalid` / `StateInvalid` /
  `RecordInvalid` / `VocabularyViolation` / `PayloadTooLarge`（摘要超界
  拒绝不截断）/ `SchemaUnsupported` / `PayloadMalformed` /
  `StaleTargetMissing`。折叠规则：`validate()`/`from_json()` 嵌套失败
  （含词表类值非法与 `change_sequence` 回归）一律折叠 `StateInvalid`。

## 重建与确定性

`rebuild_world_state`：校验 options 后按序折叠输入序列（五类更新经各自
`apply_*`、`WorldExpiryInput` 经 `apply_expiry`；插入溢出淘汰发生在观察
算子内）。同输入序列（六类）+ 同 options 的重放结果与等价增量推进
`digest()` 字节一致；空输入产出合法默认投影。

## 关联

- 里程碑：[M27](../plans/m27-world-state-projection-core.md)（§4 冻结面、
  §9 验证记录）
- 词表承载：[实体词表设计](../design/entity_vocabulary_design.md)
  （[DEC-046](../decisions/DEC-046-entity-vocabulary-alignment.md)）
- 消费锚点：DEC-038 首阶段（行为轨迹，引用同一词表）、Temporal Policy T2+
  世界视图映射
- 测试矩阵：`tests/m27/`（词表 fail-closed + 投影契约/重建/淘汰/wire/
  错误域，35 用例，label `integration;m27`）
