# 维护计划：决策编译失败的可恢复语义（2026-09 第三轮）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M4 后维护轮，依据 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)）
> 前置：M2、M3、M4（Completed）；前两轮维护（[host ABI](maintenance-2026-09-host-abi-feedback.md)、
> [传输与图像媒体](maintenance-2026-09-transport-and-image-media.md)）
> 建议发布点：Agent loop alpha 修订
> 更新日期：2026-09-06

## 1. 目标

落地 GitHub #21（miracle 台账 `MIR-20260906-009`，P1）：决策 schema（`required` 仅
`action`+`reason`）不约束动作参数，而 `compile_discrete_action` 对 tap/long_press/
swipe/type 要求参数——小参数量多模态模型产出"动作+理由、省略坐标"的决策通过 schema
校验后在 compile 阶段失败，AgentLoop 直接终态 `Failed`，无反馈重试机会（真机一次
即中，steps=1、recoveries=0、repairs=0）。

## 2. 方案选择

issue 给出两个期望语义（任一或组合），本轮采用**语义 2**并暂缓语义 1：

- **采用语义 2**：`compile_discrete_action` 失败归入可恢复路径——恢复预算内
  （`max_recoveries_per_step`）以 `Recovering` 阶段记录并将编译诊断（静态安全字符串，
  如 `swipe requires four canonical coordinates`）作为下一轮请求的 feedback 携带；
  预算耗尽才终态 `Failed`（summary 附编译诊断）。反馈走完整的下一轮迭代（重新观察 +
  截图 + 目标 + feedback），而不是 `build_schema_repair_request` 的无上下文修复请求——
  参数缺失是语义级错误，模型需要截图上下文才能给出正确坐标。
- **暂缓语义 1**（schema `allOf`/`if-then` 条件必填）：`output_contract.schema` 由方言层
  原样发给 provider（`json_schema` + `strict:true`，双方言两处 encode 站点）。条件关键字
  在 guided-decoding 端点（vLLM/xgrammar/outlines 系）支持度不一，本机无法对真实端点
  验证；贸然加入可能把"决策可修复"恶化为"请求级 400"。待后续以 profile 能力开关
  （advertised/enforced schema 分离）设计后再评估，不在本轮引入新公开契约概念。

两份契约（schema 与 compile）的"一致化"由本轮测试固化：参数缺失决策的接受语义以
compile 契约 + 反馈重试表达，schema 保持线格式兼容。

## 3. 范围与非目标

### 3.1 范围

- `AgentLoop::run` 决策编译失败分支：恢复预算内 feedback 重试；预算耗尽终态并附诊断。
- m3 测试新增"动作参数缺失"用例（issue 验收要求）：真机场景复现（swipe 无坐标 →
  反馈重试 → 修正 → 完成）、无预算立即终态、持续缺失耗尽预算三个分支。
- API 手册与 M3 计划维护记录同步。

### 3.2 非目标

- schema 条件必填关键字（见 §2 暂缓理由）。
- schema 与 compile 契约的机器化一致性检查（人工评审 + 测试覆盖）。
- miracle 侧真机复验（外部依赖，见 §6）。

## 4. 设计与决策依据

- [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)（终态幂等、
  可恢复错误语义）
- [API 手册：模型层](../api/model-agent-loop.md)
- issue #21 根因分析（`src/model/agent_loop.cpp` 决策 schema 与 compile 契约不一致）
- 前置 PR #20 修复了同一真机链路上一阻断点（截图 ArtifactRef digest），本轮为其下一步

## 5. 工作项

- [x] `MNT-202609-11`（#21）`AgentLoop::run` 编译失败分支改为：恢复预算内
  `++recoveries`、`StepPhase::Recovering`、`record.note` 记录编译诊断、下一轮请求
  feedback 携带诊断与参数要求（坐标 `[0,1]`、type 需 text）；预算耗尽终态
  `Failed`（`safe_summary` 附编译诊断）。新增
  `param_missing_decision_recovers_with_feedback` 测试（三分支）并双向回归验证。

## 6. 风险与阻塞

- 反馈重试依赖模型对 feedback 的遵从：不遵从时由恢复预算 + 步数预算兜底终态，
  行为不劣于修复前（终态路径保留）。
- 编译诊断均为 `compile_discrete_action` 内静态字符串，不含原始模型输出，feedback
  注入不引入注入面（测试断言反馈内容来源）。
- 真机同任务复验（Qwen3.5-4B + 截图决策，验收"不再一步终态 Failed、重试可见"）为
  外部补跑项：miracle 侧重跑并回填证据。

## 7. 测试与退出条件

- [x] 新增测试本地通过：`param_missing_decision_recovers_with_feedback`（m3，三分支：
  反馈重试完成 / 无预算终态 / 持续缺失耗尽预算）。
- [x] 双向回归验证：临时还原修复后该测试失败于 `outcome == LoopOutcome::Completed`
  （复现 issue 一步终态 Failed 故障模式），恢复修复后通过。
- [x] 本地全量 `ctest` 43/43 通过（Ubuntu 24.04 x86_64，GCC 13.3.0，CMake 3.28.3，
  `debug` preset）。
- [x] CI 全平台矩阵全绿：PR #22 push run
  [`34034403106`](https://github.com/Linductor-alkaid/mira/actions/runs/34034403106)
  与 pull_request run
  [`34034417279`](https://github.com/Linductor-alkaid/mira/actions/runs/34034417279)
  （commit `9a1dd28`）各 12 项检查全部通过——Linux GCC/Clang Debug+Release、Windows
  Debug+Release、Android arm64+x86_64、ASAN/UBSAN/TSAN、quality（含 format-check、
  docs-check、platform-boundary-check、公共头自包含）。
- [ ] miracle 真机同任务复验通过（外部依赖，issue #21 验收条件）。

## 8. 验证记录

2026-09-06：本地验证（Ubuntu 24.04.4 LTS x86_64，GCC 13.3.0，CMake 3.28.3，`debug`
preset，分支 `fix/agentloop-decision-compile-recovery`，基于 master `635e136`）。

- 构建：`cmake --build build/debug --target mira_m3_agent_loop_test` 通过；全目标构建
  通过。
- 测试：`ctest` 43/43 通过，含新增"动作参数缺失"三分支用例。
- 双向回归：`git stash` 临时移除 `agent_loop.cpp` 修复后重建，新测试失败于
  `tests/m3/m3_agent_loop_test.cpp:264: check failed: outcome == LoopOutcome::Completed`
  ——即 issue 报告的"合法决策一步终态 Failed"；恢复修复后通过。
- 本机限制同前两轮记录（无 clang/clang-tidy/sudo、无 Android NDK）；CI 覆盖补跑
  （下条）。

2026-09-06：CI 验证（PR #22，commit `9a1dd28`）。

- push run [`34034403106`](https://github.com/Linductor-alkaid/mira/actions/runs/34034403106)
  与 pull_request run
  [`34034417279`](https://github.com/Linductor-alkaid/mira/actions/runs/34034417279)
  全部 12 项检查通过：Linux GCC/Clang Debug+Release、Windows Debug+Release、Android
  arm64+x86_64、ASAN/UBSAN/TSAN、quality（含 format-check、docs-check、
  platform-boundary-check 与公共头自包含）。
- 说明：本机 conda 前缀的 pip clang-format 18.1.8 对未改动的 master 文件也报格式
  违例（与 CI runner 同版本号但行为不同，对 `src/model/model_schema.cpp` 首行即要求
  include 重排），本地 format-check 不可作为门禁；CI quality job 的 format-check
  通过即格式门禁结论。
