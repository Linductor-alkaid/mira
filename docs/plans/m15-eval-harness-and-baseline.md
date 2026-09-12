# M15：最小任务评估 Harness 与基线轮

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载阶段 F 后续计划
> `MNT-202609-29`）
> 前置：M8–M13、M14（恢复编排）；`MNT-202609-28` 已冻结
> [最小评估 Profile](../design/discrete_workflow_eval_profile.md) v1.0 与
> [DEC-034](../decisions/DEC-034-minimal-eval-profile.md)
> 建议发布点：Workflow learning alpha 验收恢复的评估基线
> 更新日期：2026-09-12

## 1. 目标

按 profile v1.0 实现评估 harness 并产出 recorded 基线轮：四组对照臂（直接 Agent、
Strict Workflow、恢复编排 ±learning context）、17 个 EvalCase、指标采集与硬门禁
（G1–G6）全部经 Mira 公共 API 驱动；首轮正式运行固化基线 digest 与分层指标。真机/
live canary 组不在本里程碑强制范围（依赖 `MNT-202609-27` 的外部条件），但 harness
必须为它们保留同一 case 集与 manifest 纪律。

## 2. 范围与非目标

范围：`tests/m15/` 评估 harness（独立可执行目标，进 ctest 与 CI 矩阵）、四臂驱动、
R/W/L/F 四家族 case 实现、事件指标采集与规范化确定性比较（G4 含随机 `Id128` 位置
映射）、预算护栏、JSON 报告与 `dataset_digest`、soak 模式（重复全套件 + RSS 稳态，
`/proc` 仅 Linux）、基线轮运行与 `docs/benchmarks/` 基线报告。

非目标：live Provider 轮（外部凭据，跑时单独 manifest 单独报告）、真实平台/设备轮
（归 27）、DEC-031 默认值校准决策（依基线数据另行立项）、profile 口径与阈值的变更
（须修订 profile 与 DEC-034）。不新增运行时公开契约；能力缺口按 profile §13 声明由
harness 侧承载。

## 3. 设计与决策依据

- [最小评估 Profile](../design/discrete_workflow_eval_profile.md)（实现规范，逐节对应）
- [DEC-034](../decisions/DEC-034-minimal-eval-profile.md)（冻结决策）
- [评估与基准体系设计](../design/evaluation_and_benchmark_design.md)（上位框架）
- DEC-015/016（A 臂工具与用户消息路径）、DEC-020/023/024（Workflow Run 生命周期与
  patch 语义）、DEC-030/031（学习契约与恢复编排）

## 4. 工作项

- [x] `M15-01` Harness 骨架：case 注册表与 canonical JSON `dataset_digest`、四臂
  装配（公共 API + Executor 管理）、seed 集 {0,1,2} 与 seed-0 三重复、JSON 报告与
  退出码（门禁失败非零）。
- [x] `M15-02` 家族实现：R（Agent 臂基础闭环 6 case）、W（Workflow 失败/恢复 4
  case）、L（学习传递 2 case，D 臂 SQLite learning context）、F（故障与生命周期
  5 case，含取消/Takeover/shutdown/模型故障/store 故障注入）。
- [x] `M15-03` 指标与门禁：事件时间戳尾延迟、`ModelUsage` token、`BudgetLedger`
  成本、重复副作用、人工介入计数；G1–G6 断言（G4 规范化确定性比较）；预算护栏
  （profile §9.2 五项）。句柄项仅 soak RSS 覆盖（fd 计数未实现，见验证记录限制）。
- [x] `M15-04` 基线轮与报告：首轮正式运行、基线登记
  （[discrete-workflow-eval-v1](../benchmarks/discrete-workflow-eval-v1.md)，含环境/
  命令/摘要/限制）、D vs C 配对差异报告（只报告不判定）。
- [x] `M15-05` soak：重复全套件 3 轮（RSS 稳态 2.0% ≤ 10% 阈值，门禁每轮复验
  通过）；live canary 与真机组的补跑条件登记（不在本里程碑执行，见验证记录）。

## 5. 风险与阻塞

- 风险：recorded 确定性（G4）可能暴露既有非确定源（事件顺序、迭代序）。处置：按
  失败分类归因登记，不放宽门禁（DEC-034 明确这是预期收益）。
- 风险：17 case × 四臂首轮样本量小，区间宽。处置：分层报告 + 不宣称纪律
  （profile §7/§9.4）。
- 外部：live canary 需受控凭据（27 的 Provider profile）；设备在线（OnePlus Ace 3）
  但真机组验收源在 27，不在本里程碑统计。

## 6. 测试与退出条件

- [ ] harness 进 CI 矩阵（Linux/Windows + sanitizers；Android 编译级），门禁失败
  即测试失败。
- [ ] profile §9.1 G1–G6 在全部适用 case×臂组合上通过；§9.2 预算护栏无超出。
- [ ] 基线轮报告落 `docs/benchmarks/`，含 manifest digest、`dataset_digest`、分层
  指标、D vs C 配对差异、失败分类与限制。
- [ ] soak ≥3 轮 RSS 稳态在阈值内（增长 ≤10%）且门禁复验通过。
- [ ] live canary/真机组的未执行项登记原因、负责人与补跑条件（不标完成）。

## 7. 验证记录

2026-09-12：`M15-01`～`M15-05` 本地实现与首轮基线（分支 `feat/mnt-29-eval-harness`）。

- **交付**：`tests/m15/m15_eval_profile_harness.cpp`（约 1900 行，单目标
  `mira_m15_eval_profile_harness`，链接 core/workflow/simulator_adapter/state_store/
  executor，入 ctest，label integration;m15，TIMEOUT 900）。四臂全部经公共 API
  驱动；R 6 / W 4×3 臂 / L 2×2 臂 / F 5 case 与臂适用矩阵一致；D 臂
  `SqliteMemoryStore` learning context（temp 库 + WAL/SHM 清理）；F4 经
  `KnobbedMemory` 装饰器注入 200 ms 延迟/失败旋钮；F5 Takeover 为 m14 验证过的
  请求前置形态（并发竞态形态会引入 harness 自身非确定性，不采用）。
- **首轮基线**（170 run 单元）：G1–G6 与预算护栏全绿，连续三轮复验一致；soak
  3 轮 RSS 增长 2.0%；摘要、`dataset_digest` 与发现见
  [discrete-workflow-eval-v1](../benchmarks/discrete-workflow-eval-v1.md)。
  首轮发现：跨生产者事件交错顺序可交换（G4 细化为 workflow 臂有向多重集比较，
  profile §9.1 与 DEC-034 已同步）；`Id128` 随机性经内容摘要级联（32/64 hex）
  必须纳入规范化；`create_run` 创建时校验步骤工具注册（装配顺序教训）。
- **本地门禁**（Ubuntu 24.04 x86_64、GCC 13.3.0、CMake 3.28.3）：debug 全量
  ctest 69/69；ASAN/UBSAN m15 1/1 通过；TSAN（`setarch -R`）m15 1/1 通过；
  `format-check`、`platform-boundary-check`、`tools/check_docs.py` 通过。
- **限制与未执行项**：fd 句柄计数未实现（profile §8 句柄项仅 soak RSS 覆盖）；
  live canary 未执行——补跑条件：受控 Provider profile（27 已登记 miracle 侧
  siliconflow/Qwen3.5-4B 可复用）+ 单独 manifest + 凭据不进事件；真实平台组未
  执行（归 27）；Windows/Android 运行级由 PR CI 补充取证（Android 为编译级）；
  Release/quality 门禁由 PR CI 回填后本里程碑方可关闭。
- L1 配对样本量 1 对/seed，按 DEC-034 决策 3 只报告不判定；恢复率/成本结论待
  soak 扩样与 live canary。
