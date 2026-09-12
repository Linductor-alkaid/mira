# DEC-034：离散动作与 Workflow 最小评估 Profile v1（跑前冻结口径与阈值）

> 状态：Accepted
> 日期：2026-09-12
> 负责人：Mira Maintainers
> 冻结里程碑：无独立里程碑；实现入口为 [阶段 F 后续计划](../plans/maintenance-2026-09-post-stage-f.md)
> 的 `MNT-202609-29`（本决策由 `MNT-202609-28` 产出）
> 替代/被替代：无（是[评估与基准体系设计](../design/evaluation_and_benchmark_design.md)
> 的首个 profile 实例化，不改变该设计任何层级定义）

## 背景与问题

`MNT-202609-21` 审计确认阶段 F 缺 Workflow 任务级统一评估与学习增益对照：
[DEC-031](DEC-031-agent-recovery-orchestration.md) §5 的恢复预算与 lesson 上下文上限全部
是暂定默认值（`RULE-10`），恢复收益声明被显式挂起等待对照证据；
[DEC-032](DEC-032-context-intelligence-layered-context.md) 的 Stage A long-session 基线
依赖一个先行的评估 profile。`MNT-202609-28` 的验收要求：固定 case/fixture/baseline
digest、失败分类、重复样本方法与预算，覆盖直接 Agent、Strict Workflow、恢复编排、
启用/禁用 lesson 四种对照，指标含成功率、恢复率、模型调用/token/cost、尾延迟、人工
介入、重复副作用、内存/句柄与 shutdown 时间；阈值在跑结果前冻结，且不得沿用已取消的
ONNX/realtime 准入要求。

需要一个决策把口径与阈值冻结为 `MNT-202609-29` 的实现规范，防止「先跑后定标准」。

## 决策

1. **采纳[离散动作与 Workflow 最小评估 Profile](../design/discrete_workflow_eval_profile.md)
   v1** 作为 `MNT-202609-29` 实现、运行与报告的唯一规范：四臂对照（`agent-direct`、
   `workflow-strict`、`recovery-no-memory`、`recovery-with-memory`）、17 个 EvalCase、
   七类失败闭集、seed {0,1,2} 加 seed-0 重复的确定性方法、串行执行、指标来源映射与
   manifest 纪律，均以该文件为准。
2. **阈值跑前冻结，首轮只建基线**：硬门禁（G1–G6 零容忍）与绝对预算护栏（调用/token/
   墙钟/成本/shutdown 上限）在本决策生效即冻结；首轮正式运行固化基线 digest，不回填
   修改阈值，不设首轮成功率/恢复率通过线。后续轮次的回归规则（确定性差异须归因、时序
   >1.5× 归因/>2× 失败、RSS 增长 >10% 归因）随本决策同时冻结。
3. **学习增益只报告不判定**：D/C 臂对照报告 paired delta 与检索命中机制证据，v1 不设
   增益阈值、不宣称收益（延续 DEC-031「无对照证据前不宣称恢复率改善」）。
4. **对照臂以现有公开 API 装配，不新增契约**：C 臂的「禁用 lesson」语义冻结为不调用
   `set_learning_context`（记录与检索同关的 no-memory 对照）；A 臂不含 lesson 组合
   （DEC-031 既定分层）。细粒度 lesson 开关、内存/句柄与 shutdown 计量如需进入运行时
   公开面，须凭 29 的数据另立决策，本决策不预承诺。
5. **显式排除已取消范围的准入要求**：本 profile 不含 M5（ONNX/感知模型级指标）与
   M6（实时控制 jitter/deadline）的任何门禁维度；连续动作与视觉感知管线不在 case 集。
   `MNT-202609-30` 重定义 M7 时不得把本 profile 的基线外推为这些维度的证据。
6. **平台边界如实**：recorded 轮仅声明 Simulator 环境结论；内存/句柄仅 Linux 报告；
   live canary 单独 manifest 单独报告，不进回归门禁；真实平台结论归 `MNT-202609-27`。

## 备选方案

- **不裁剪，直接实例化评估设计全部分层**：L0–L5 全层级与全场景集一次性落地，超出当前
  已交付能力（无感知/实时/真实平台面），交付周期与 27 的外部证据耦合，无法独立开工。
  不采用；v1 只覆盖已有能力（离散动作 + Workflow + 恢复编排）。
- **先跑一轮再定阈值**：以首轮数据设通过线。违反「阈值在跑结果前冻结」的验收与
  `RULE-10` 纪律，且给事后调整留口子。不采用；首轮定为基线轮，通过线留给后续轮次凭
  数据与 DEC 流程增设。
- **为对照新增细粒度 lesson 开关（检索开、注入关）后再评估**：新增公开契约需独立设计与
  评审，阻塞 profile 冻结；no-memory 语义（不装 learning context）已能满足增益对照的
  最小需求。不采用；细粒度开关凭 29 数据另行决策。
- **harness 走内部接口加速实现**：绕过公共 API 会把「评估的是产品能力」退化为「评估的
  是内部函数」，违反评估设计 §10。不采用。

## 影响与风险

- `MNT-202609-29` 获得可执行规范：实现范围、验收与预算均可对照本 profile 检查；
  `MNT-202609-30` 的 M7 重定义提案获得基线引用源；DEC-032 Stage A 的 token 计量与
  manifest 纪律获得先例。
- 首轮无通过线意味着首轮结果不能直接当发布门禁；这是有意的：v1 目标是可复现基线，
  避免无数据阈值。硬门禁（安全/确定性/审计）从首轮即生效，安全回归不因「基线轮」放宽。
- 风险：17 case × 四臂的首轮样本量小（每臂家族格子 3–15），分层区间宽，误把宽区间当
  「无差异证据」。缓解：第 7/9.4 节强制分层报告与「不宣称」纪律；扩样由 29 的 soak
  阶段按需追加（seed 集变更为新 digest）。
- 风险：recorded 确定性门禁（G4）对事件序列逐位一致的要求可能暴露既有非确定源（如
  时钟、迭代序）。这是预期收益而非负担；暴露项按失败分类归因，不放宽门禁。
- 本决策不改变任何运行时公开契约、事件闭集与既有 DEC 语义；评估 harness 的 Executor
  路由与关闭顺序遵循 profile 第 11 节。

## 验证方式

- `MNT-202609-28` 验收对照：case/fixture/digest 规则（profile §4/§10）、失败分类
  （§6）、重复与预算（§7/§9.2）、四组对照（§3）、指标集（§8 含内存/句柄与 shutdown
  的口径与平台边界）、跑前冻结阈值（§9）逐项落实；文档链接与结构检查通过。
- `MNT-202609-29` 验收时以本 profile 为规范核对：公共 API 驱动、recorded 回归确定、
  live canary 单独报告、fault/cancel/Takeover/rejection/shutdown 场景覆盖、学习收益
  依据对照与分布报告。
- 2026-09-12 实现期细化（不改阈值与 case 集）：profile §9.1 G4 的规范化明确覆盖
  32/64 位 hex token 与 UUID 形态（内容摘要内嵌随机标识的级联），workflow 臂因
  双并发生产者按有向多重集比较（首轮基线实证交错顺序可交换；内容/缺失/多余仍
  失败）。该细化随 profile 文件同步更新。
- 本决策自身的修订（阈值调整、case 集变更、新增对照维度）须更新 profile 文件并链接
  本记录，基线记录保留原 digest 不回溯改写。

## 关联文档和工作项

- [离散动作与 Workflow 最小评估 Profile](../design/discrete_workflow_eval_profile.md)
  （本决策的规范载体）
- [评估与基准体系设计](../design/evaluation_and_benchmark_design.md)（上位框架）
- [阶段 F 后续计划](../plans/maintenance-2026-09-post-stage-f.md)：`MNT-202609-28`
  （本决策的产出任务）、`MNT-202609-29`（实现与运行）、`MNT-202609-30`（M7 重定义消费）
- [DEC-031](DEC-031-agent-recovery-orchestration.md)（恢复编排语义；§5 暂定默认值待
  29 校准）
- [DEC-030](DEC-030-learning-loop-runtime-semantics.md)（学习契约，C/D 臂对照基础）
- [DEC-032](DEC-032-context-intelligence-layered-context.md)（Stage A 基线依赖本 profile）
