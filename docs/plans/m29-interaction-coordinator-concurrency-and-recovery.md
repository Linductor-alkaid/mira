# M29：交互协调层第二阶段——执行中插话、并发归属、持久化与断线回补

> 状态：In Progress（2026-11-09 按用户要求立项，分增量交付；完整恢复未完成）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（承载
> [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)
> 第二阶段；来源需求为
> [Issue #111](https://github.com/Linductor-alkaid/mira/issues/111) §2、§5）
> 前置：M28 关闭（**已满足**——2026-10-29 已合并关闭，按总计划同步修正旧说明）
> 建议发布点：非发布物；M30 长期记忆接入的接线前提
> 更新日期：2026-11-09（DEC-102 跑前冻结与首批并发/投递实现）

## 1. 目标

在 M28 的最小闭环之上，把「统一交互」从**单任务、单输入**扩展到
**长任务执行中、多任务并发、断线重连、进程重启**四种真实形态：

1. **执行中插话**：任务运行期间接收新输入；纠正只更新目标任务的后续上下文；
   在途动作安全收敛；迟到的结果不复活旧任务，也不覆盖新要求。
2. **切换与恢复**：内部会话切换、恢复旧工作时仍能找回所需状态；
   终态任务不复活，后续工作可关联旧摘要新建任务。
3. **并发归属**：两个任务未完成时，明确引用可路由；「把那个停下」在无法确定目标时
   从同一入口澄清；**已明确的紧急停止走确定性控制路径**。
4. **持久化与断线回补**：已接纳输入的处理状态、内部任务/会话映射与必要上下文可持久化；
   断线后有界回补；未决副作用**先重观测/验证**，不因恢复流程重复执行。
5. **重启恢复**：进程重启后恢复已接纳输入的处理状态与映射；历史缺失、已擦除或
   不可恢复时**明确说明**，不补造经历。

## 2. 范围与非目标

### 2.1 范围

- 协调层的**执行中输入**投递路径与在途动作收敛语义。
- **指代消解**的澄清回路：候选生成、澄清问题呈现、澄清答案回收。
- 内部会话切换与工作恢复的**状态装配**。
- 并发输入的**归属仲裁**与归属冲突的可审计记录。
- 输入台账与主体注册表的**恢复流程**（M28 只保证耐久，本阶段交付恢复执行路径）。
- 断线后的**有界回补**（按 `input_id` 窗口回补未结算结果，不重复派发）。
- 未决副作用的**重观测/验证**接入既有 `RecoveryPlanner` 与 action journal。

### 2.2 非目标

- **不做** UI / 任务列表 / 历史详情页（Mirage 负责；Mira 只提供所需状态与事件语义）。
- **不做** 长期记忆的写入/检索/纠正/删除（归 M30）。
- **不做** 容量准入、归档与遗忘（归 M31）。
- **不做** 真实模型语义质量声明（归真实模型评估轮）。
- **不做** 模型介导路由的自动执行；模型只提供建议。
- **不改** `SessionState`；会话退出生命周期归 M31。

## 3. 设计与决策依据

- [DEC-102](../decisions/DEC-102-interaction-input-arbitration-and-delivery.md) 与
  [交互输入设计](../design/interaction-input-delivery.md)：本轮跑前冻结和实现边界。

- [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)
  §5（R2/R3/R4/R6/R7）、§8（恢复语义）。
- [核心契约与状态机设计](../design/core_contracts_and_state_machine.md)
  §6.3（Task 状态机与终态幂等）、§8（取消与超时）、§7.4（关键事件的 durable ack）。
- [事件与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
  §7（崩溃窗口矩阵）、§8（Recovery）、§6（副作用提交协议）。
- [Runtime 设计](../design/mira_runtime_design.md) §3.5（副作用默认至多执行一次）。
- [DeepSeek Super-agent 参考研究](../design/superagent_reference_study.md)
  （`M29-02`/`M29-04`/`M29-05` 的机制输入：执行中输入投递的三通道分离与保序要求、
  并发槽位对人工输入的例外、"已送达/已读"区分与重启后重投、被打断工单的恢复顺序；
  **执行机制不采纳**其自建副作用队列，见该文第 7 节；采纳与否由各决策记录逐条冻结）
- M28 门禁 `M28-G1`–`M28-G8` 提供的契约面与测试基座。
- [M36](m36-memory-agent-context-integration.md)承接记忆与完整请求接线增量；其
  input_sink 提供纠正投递前的快照失效与失败暂停，不替代本里程碑的并发归属/恢复验收。
- [记忆代理与长期上下文参考研究](../design/memory-agent-reference-study.md)
  （2026-11-09，`MNT-202611-MA-01`；供 `M29-01/03/04/05` 评审：两阶段路由、
  最新纠正与异步投影的版本边界、完整请求准入；仅研究输入，不改变当前范围或冻结状态）。

## 4. 冻结的契约语义

[DEC-102](../decisions/DEC-102-interaction-input-arbitration-and-delivery.md) 冻结仲裁、
投递、Loop 安全边界及恢复方向。工作项是否实现由 §5/§6 判断，冻结不代表验收。

### 4.1 必答八问

| 问题 | 冻结答案 |
| --- | --- |
| 入口与 owner？ | Host → InteractionCoordinator → 既有 Runtime/Loop；一个账本分区单 owner。 |
| 幂等与授权？ | 分区内 input_id 唯一，绑定 tenant/user；重复回执前重验权限，不泄露他人条目。 |
| 仲裁顺序？ | 显式 task/intent > reply_to > 有效建议 > 唯一活动任务 > 澄清；未知引用拒绝。 |
| 澄清如何回收？ | 冻结候选顺序；完整回收须引用原输入与候选，原正文不丢失、不可重复投递。 |
| 插话何时生效？ | 下一轮 drain；旧模型完成不能驱动新动作；已进入的 action/tool 按原协议收敛。 |
| Executor 与关闭？ | 有限值计算用 submit_auto 并消费 future；超时不迟写；外部 owner 先关闭协调层。 |
| 回补与恢复？ | 主体过滤的有界只读流不重派发；旧任务只读，正文/映射按受控引用重建。 |
| 未决副作用？ | RecoveryPlanner + 既有 journal，先 Observe/Verify；缺历史明确不可恢复。 |

### 4.2 门禁矩阵

| 门禁 | 验收对象 | 自动化证据/状态 |
| --- | --- | --- |
| M29-G1 | 多工作显式归属、模型冲突与未知引用 | m29_arbitration_test：显式引用/未知引用/并发重复提交通过（Linux） |
| M29-G2 | 插话使旧模型结果失效、动作边界与 repair | m29_loop_interruption_test：两类 Loop、repair、工具组、已准入动作 Verify 通过（Linux） |
| M29-G3 | 候选冻结、原输入与澄清答案幂等回收 | 候选先交付，完整回收待实现 |
| M29-G4 | 原子账本、权限绑定、补投保序与背压 | m29_ledger_atomicity_test / delivery_test / lifecycle_test 通过（Linux） |
| M29-G5 | 注册表/正文引用跨进程恢复、断线只读回补 | 待实现 |
| M29-G6 | journal 未决副作用只重观测/验证 | 待实现 |
| M29-G7 | 取消/超时/拒绝/异常/关闭/接管 | M28/M36 回归 + M29 超时/拒绝/异常/关闭/取消/接管通过（Linux）；平台 CI 未执行 |
| M29-G8 | API/设计/决策/计划与质量门禁 | 本增量文档/架构/平台边界/SBOM/格式与严格 clang-tidy 通过；完整退出仍待后续工作 |

## 5. 工作项

- [x] `M29-01` 立项与跑前冻结（DEC-102、必答八问、G1–G8；其余勾选以实际验收为准）。
- [x] `M29-02` 执行中输入投递与在途动作收敛（DEC-102 的单动作/工具边界、两类 Loop、repair、取消/接管；Linux 离线 G2/G7）。
- [ ] `M29-03` 指代消解与澄清回路（候选冻结/回执已交付；原输入引用与答案回收未交付）。
- [x] `M29-04` 并发归属仲裁与审计（显式 task/intent/reply 优先、未知引用拒绝、权限绑定、实际并发幂等、引用/候选审计；Linux 离线 G1/G4）。
- [ ] `M29-05` 持久化恢复与断线回补（已补账本失败原子性与绑定重建；主体注册表/受控正文引用/只读有界回补未交付）。
- [ ] `M29-06` 未决副作用的重观测/验证接入。
- [ ] `M29-07` 完整 IVA 测试矩阵（已有 `tests/m29/` 五组测试；G3/G5/G6 尚缺）。
- [ ] `M29-08` 完整里程碑文档同步（本增量 DEC/设计/API/术语/总计划已同步，其余随 M29-03/05/06 补齐）。

## 6. 测试与退出条件

- [ ] 工作项与门禁全部勾选，验证记录落档。
- [ ] 覆盖 issue #111 的验收场景 2（穿插话题）、3（并行工作与指代）、4（插话纠正/取消）、
      6（压缩和重启）的前半段。
- [ ] CI 全矩阵绿；受影响的决策/设计/API 手册/术语表已同步。
- [ ] 未执行的平台/真实模型验证保持未勾选并记录原因与补跑条件。

## 7. 风险与阻塞

- **依赖 M28**：已满足；M29 完整恢复仍需正文受控引用和旧 Task 只读投影。
- **恢复语义与副作用安全耦合**：恢复路径的重复执行风险由
  [事件与崩溃一致性设计 §6–§8](../design/event_artifact_crash_consistency.md) 承担，
  本阶段必须复用既有 durable ack 与 journal，**不得**新建第二套副作用提交协议。
- **真实模型语义**：路由准确率与必要澄清率仍需真实模型轮取证，**本阶段不声明**。

## 8. 验证记录

### 2026-11-09：M29-01/02/04 首批并发与投递增量

基线为 HEAD `62ad918` 及本会话已验证的 M36 工作树；分支
`feat/interaction-concurrency-recovery`，未提交工作树。M36 增量继续保留，没有作为
M29 完整恢复的验收证据。M29 状态保持 In Progress。

实现与验收：

- DEC-102 冻结仲裁顺序及失败边界；M29-01 完成。
- 显式 task/intent/reply 引用优先且不调用 resolver；未知/冲突引用拒绝；候选 TaskId
  顺序固定，写入可选审计字段；主体不能复用他人输入 ID 获取回执，撤销授权后重复
  提交不能读原回执；命令必须 Applied/NoOp。
- 上下文准备改为值捕获纯计算，超时不迟写；close 消费 future 并报告迟到失败。任务
  上限前置，路由落账后才投递；追加失败不改变账本内存投影，也不发布失败纠正。
- 补投只移除成功前缀，失败后后缀不丢失，新输入不越过；汇异常转不确定投递，耐久
  诊断可 rebuild，停止盲目重投。真实 FileEventStore 关闭/重开验证绑定与幂等。
- 两类 Loop 在模型/repair/action/tool/答案边界处理新输入，不依赖 M36 服务；未开始
  的工具返回未执行失败结果以关闭原生协议组，已开始动作完成后 Verify，再处理纠正。
  Runtime 取消和 Human Takeover 的迟到完成不发布旧答案或旧动作。
- 新增五个 M29 测试可执行文件；M28 的 MemoryEventStore fixture 明确改为模拟耐久
  ack，真实耐久证据来自 FileEventStore。恢复新工作同时保留旧约束和本次新要求。

环境：Linux 7.0.0-42-generic、x86_64，GCC 13.3.0；离线脚本 Provider，C++20、
Debug、WarningsAsErrors。验证命令与结果：

```bash
cmake --preset debug
cmake --build --preset debug -j 4
ctest --preset debug --output-on-failure
# 165/165，通过；包含 M28、M29、M36 回归及宿主示例。

cmake --preset asan -DMIRA_ENABLE_UBSAN=ON
cmake --build build/asan -j 2 --target mira_m29_arbitration_test mira_m29_ledger_atomicity_test mira_m29_delivery_test mira_m29_lifecycle_test mira_m29_loop_interruption_test mira_m28_lifecycle_test mira_m36_memory_agent_model_test
ctest --test-dir build/asan -R 'mira_m29_.*|mira_m28_lifecycle_test|mira_m36_memory_agent_model_test' --output-on-failure
# ASAN+UBSAN 7/7，通过。

cmake --preset tsan
cmake --build build/tsan -j 2 --target mira_m29_arbitration_test mira_m29_ledger_atomicity_test mira_m29_delivery_test mira_m29_lifecycle_test mira_m29_loop_interruption_test mira_m28_lifecycle_test mira_m36_memory_agent_model_test
setarch x86_64 -R ctest --test-dir build/tsan -R 'mira_m29_.*|mira_m28_lifecycle_test|mira_m36_memory_agent_model_test' --output-on-failure
# TSan 7/7，通过；与 CI 一样禁用 ASLR。

for mira_unit in src/interaction/input_ledger.cpp src/interaction/interaction_coordinator.cpp src/interaction/interaction_delivery.cpp src/interaction/interaction_routing.cpp src/model/agent_loop.cpp src/model/conversation_loop.cpp; do
  clang-tidy -p build/debug "$mira_unit" --quiet || exit 1
done
# 六个生产编译单元严格检查通过。
cmake --build build/debug --target format-check docs-check sbom-check platform-boundary-check architecture-check
git diff --check
```

日志：`/tmp/mira-m29-{build,full-test}.log`、
`/tmp/mira-m29-{asan,tsan}-{build,test}.log`、`/tmp/mira-m29-tidy-*.log`、
`/tmp/mira-m29-quality.log`。调试中暴露的 fixture 配置问题（未激活 admission、工具缺少
描述）已修正后重跑；以上最终结果没有测试 skip，不使用旧分支的 M29 日志代替本轮结果。

### 未完成与补跑

| 项目 | 状态/原因 | 负责人及补跑条件 |
| --- | --- | --- |
| M29-03 完整澄清答案回收 | 未实现受控原输入引用及回收提交 | Mira Maintainers；以冻结候选、原输入和答案的联合幂等验收继续实现 |
| M29-05 主体注册表/原输入恢复、断线回补 | 当前只 rebuild 账本，不能恢复活动 Runtime Task | Mira Maintainers；补 Artifact 正文、旧工作只读投影、主体过滤的有界只读回补及故障窗口测试 |
| M29-06 未决副作用恢复接线 | 尚未接入协调层恢复流程 | Mira Maintainers；复用 RecoveryPlanner + journal，先 Observe/Verify，不重派发动作 |
| Windows/MSVC、Android/NDK 与远端 CI 全矩阵 | 本轮只执行 Linux 本地门禁，未提交/触发远端 CI | Mira Maintainers；在最终 M29 变更上运行仓库 CI 全矩阵及目标平台适用测试 |
| 真实模型、真实设备、长期运行 | 本轮使用脚本 Provider，未配置模型凭据与设备 | Mira Maintainers；用受控主体和设备接线，记录模型版本、语义指标及资源增长，不据离线测试声明长期稳定性 |
