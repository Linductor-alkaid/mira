# DEC-011：Demo 优先验证与 M5/M6 终止

> 状态：Accepted  
> 日期：2026-09-05  
> 负责人：Mira Maintainers  
> 冻结里程碑：M4 后立即生效  
> 替代/被替代：无（部分修订 DEC-006、DEC-009 的落地节奏，二者保留为方向记录）

## 背景与问题

M4 完成后，Mira 已具备跨平台 Core、Observation、OpenAI-compatible Provider、可中断闭环、
Context/Memory/Checkpoint/Replay 与恢复能力。后续 M5（本地 ONNX 感知与模型治理）和 M6
（soft realtime 连续控制与 Human Takeover）均为 `Planned`，尚无任何实现。

两个里程碑的范围建立在未经真实任务验证的假设上：M5 假设端侧必须自建 OCR/检测/任务模型
及其数据治理、蒸馏和发布链；M6 假设任务需要 bounded-latency 反馈控制，且其前置必须先
交付 M5。维护者无法在缺少真实任务证据的情况下判断这些方向是否正确，也不应在方向存疑时
投入大范围实现。

## 决策

1. 终止 M5、M6 当前范围，计划状态改为 `Cancelled`；对应设计文档保留为假设记录，不删除。
   后续能力是否需要、以何种范围需要，由外部 demo 的需求证据重新定义，可能产生替代性的
   新里程碑或重定义范围。
2. 能力验证与需求发现由独立仓库中的 demo 产品承载。demo 不进入 mira 仓库；"mira 仓库
   不含产品 UI 或完整应用"的 v1 边界由此正式化。demo 的需求验证计划（假设、失败分类法、
   量化指标、时限与重入判据）由 demo 仓库自行维护，其证据必须注明所基于的 mira 版本。
3. demo 只通过安装后的公共包接口（`find_package(Mira)`）消费 mira。发现的能力缺口回流为
   mira 的 issue、计划项或 PR；不得在 demo 中 fork mira 源码或绕过公共 API。若某需求
   无法用公共 API 满足，该事实本身作为"公共 API 表面不足"的证据记录并回流。
4. M7 挂起（`Blocked`），其前置 M5/M6 已不存在；待 demo 证据产出后重新定义 M7 的范围、
   前置与发布点。v1 交付边界收缩为 M0–M4 已验证能力加重定义后的后续范围。
5. demo 作为宿主是 Executor 的外部 owner，须遵守 DEC-001 的初始化、关闭顺序和句柄结算
   契约；mira 仓库内的工程约束（AGENTS.md）不延伸约束 demo 仓库，但公共契约约束延伸。

## 备选方案

- 按原计划推进 M5/M6：方向未经验证即大规模实现，若假设错误则治理体系与 realtime 路径
  成为负资产，不采用。
- 只终止 M6、保留 M5：M5 的数据治理、蒸馏与发布链同样依赖真实任务数据，且其价值判断
  与 M6 同源，不采用。
- demo 放在 mira 仓库内（如 `examples/` 或独立目录）：会弱化"外部消费者只经公共 API 使用
  mira"这一被验证的边界，且产品迭代节奏与 Runtime 门禁互相拖累，不采用。
- 无限期搁置、不设验证载体：无法产出可回填计划的需求证据，不采用。

## 影响与风险

- v1 交付边界中依赖 M5/M6 的条目（本地感知 pipeline、连续控制、Takeover 的实现交付）
  随本决策暂缓；总计划已同步修订。
- M7 前置失效，其 Tool 模组（DEC-009）与评估体系方向保留，落地节奏待重定义。
- mira 当前为 0.x（`SameMajorVersion`），无 API 稳定性承诺；demo 必须钉死 mira 版本，
  升级走显式变更。公共 API 表面不足的风险由外部 demo 直接检验——首个实例
  `BUG-20260905-001`（安装包暴露 state_store 头文件却未导出可链接目标）在决策当天发现
  并已修复、由 installed-consumer 测试持续覆盖。
- demo 存在漂移为产品开发、长期不回流证据的风险；以决策第 2 条的时限与重入判据约束，
  mira 侧以本决策作为重开新里程碑的唯一证据入口。
- 终止的里程碑知识随时间衰减；以保留的设计文档和计划文件（`Cancelled`）缓解。

## 验证方式

- demo 仓库产出需求验证报告：按失败分类法归因的量化指标（感知/延迟/模型/任务建模），
  作为重定义后续里程碑的唯一依据。
- mira 侧公共包完整性由 `mira_installed_consumer_test` 持续验证（含 `Mira::state_store`
  及其 SQLite 闭包的外部链接与运行）。

## 关联文档和工作项

- [Mira 实施总计划](../plans/mira-implementation-plan.md)
- [M5 计划](../plans/m5-local-perception-task-models.md)（`Cancelled`）
- [M6 计划](../plans/m6-realtime-control-takeover.md)（`Cancelled`）
- [M7 计划](../plans/m7-tools-evaluation-platform-v1.md)（`Blocked`）
- [DEC-001](DEC-001-runtime-executor-ownership.md)、[DEC-006](DEC-006-local-perception-task-models.md)、
  [DEC-009](DEC-009-tool-module-boundary.md)
- `BUG-20260905-001`（[M4 验证记录](../plans/m4-context-memory-recovery.md)）
