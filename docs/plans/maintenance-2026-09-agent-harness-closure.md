# 维护计划：Agent Harness 闭合（2026-09 第四轮）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M4 后维护轮，依据
> [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)、
> [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)）
> 前置：M2、M3、M4（Completed）；前三轮维护（[host ABI](maintenance-2026-09-host-abi-feedback.md)、
> [传输与图像媒体](maintenance-2026-09-transport-and-image-media.md)、
> [决策编译修复](maintenance-2026-09-decision-compile-repair.md)）
> 建议发布点：Agent harness alpha
> 更新日期：2026-09-07

## 1. 目标

按 [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 的 Harness 审计结论，
闭合 2026-09-07 核实的三个 Agent Harness 缺口，使"Session 内托管 AgentLoop、工具调用、
运行中用户介入、事后对话回看"成为经测试验证的单一系统：

1. 工具执行闭环（GitHub #8）：最小 BuiltIn 工具执行边界 + AgentLoop 工具分支
   （[DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)）。
2. Conversation 一等公民最小机制：对话事件、会话投影、步边界用户消息队列
   （[DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)）。
3. Loop 与 Runtime 集成验证：以集成测试证明 Runtime 会话内托管完整闭环（此前
   AgentLoop 仅被 m3 测试独立驱动）。

本轮由维护者指令启动（提前于 M7 重定义落地 #8 的最小闭环）。M7 状态与范围不变：
DEC-009 模组体系、ToolProposals 的模组化执行仍随 M7 重定义处理；#8 的原始诉求
（AgentLoop 工具分支不可执行）由本轮关闭。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/tool_executor.hpp`：`BuiltinToolSpec`、`BuiltinToolRegistry`、`make_wait_tool`。
- `AgentLoop`：`set_tool_registry`、ToolProposals 执行分支、工具结果回填、
  `enqueue_user_message` 与步边界注入、相关配置与预算。
- `include/mira/conversation_log.hpp`：会话对话投影。
- 事件类型 `UserMessageInjected`、`ToolExecuted`。
- 单测（注册表、循环工具闭环、用户消息、投影）与集成测试（Runtime + 循环 + 工具 +
  对话的单一系统）。
- 文档同步：API 手册、设计文档事实修正（§2.4/§6.1/§6.6 的"ITool/ToolModule 已有"误述）、
  M7 验证记录、总计划决策索引、#8 关闭。

### 2.2 非目标

- DEC-009 ToolModule/manifest/签名/OutOfProcess 隔离（M7 重定义范围）。
- 对话 patch、三类目标区分、暂停/恢复对话语义（M8-04）。
- Workflow 操作工具（run_workflow 等，M8-03）。
- 工具结果的 wire 级 function_call_output 回填方言适配（本轮以模型输入项回填；
  方言级 `build_tool_result_input` 的接入随后续 Provider 互操作需要处理）。
- 跨任务会话合并、多任务并发介入。

## 3. 设计与决策依据

- [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)、
  [DEC-017](../decisions/DEC-017-complete-task-command.md)
- [Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
  §6（组件清单）、§18；[Agent Harness 参考研究](../design/harness_reference_study.md)
- [Model Provider 与 Tool 扩展设计](../design/model_provider_and_tool_design.md)、
  [核心公共契约与状态机](../design/core_contracts_and_state_machine.md)
- [API 手册：模型层](../api/model-agent-loop.md)、[API 手册：Core Runtime](../api/core-runtime.md)

## 4. 工作项

- [ ] `MNT-202609-12` 实现 `BuiltinToolRegistry`（注册校验、暴露快照、fail-closed 执行、
  至多一次派发、`make_wait_tool`）并有单测覆盖全部拒绝路径（DEC-015）。
- [ ] `MNT-202609-13` AgentLoop 工具分支：ToolProposals 执行、结果以
  `mira.agent-loop.tool-result.v1` 输入项回填、`ToolExecuted` 事件、执行预算；无注册表
  保留终态失败（GitHub #8 验收）。
- [ ] `MNT-202609-14` AgentLoop 用户消息：有界队列、步边界注入、`UserMessageInjected`
  事件、常驻指令语义（DEC-016）。
- [ ] `MNT-202609-15` `build_conversation_view` 会话投影与单测（UserMessage/LoopOutcome
  两类条目、空会话、可重建性）。
- [ ] `MNT-202609-16` 集成测试：MiraRuntime 会话（Simulator 环境）内经 Executor 托管
  AgentLoop，覆盖工具调用、运行中介入、任务终态与事件/投影一致性；本地全量门禁与 CI
  取证。
- [ ] `MNT-202609-17` 文档同步：API 手册（新契约与脱敏责任）、架构设计 §2.4/§6.1/§6.6
  事实修正、M7 验证记录（#8 提前落地说明）、总计划决策索引、GitHub #8 关闭。
- [ ] `MNT-202609-18` 新增 `complete_task` 命令（DEC-017）：`CommandKind::CompleteTask`、
  终态幂等（同终态 `NoOp`、冲突终态拒绝）、按 M1 转换表的合法路径判定、完成时递增
  epoch；集成测试覆盖 Completed 结算、重复与冲突路径。

## 5. Executor 路由与关闭

不新增异步路径：工具处理函数在循环已占用的 Executor 任务内联执行（有界、可取消）；
消息队列为循环自有的互斥保护状态，无独立线程。`wait` 工具以切片睡眠轮询取消，不使用
私有定时器。Runtime 关闭顺序不变（宿主仍为 Executor 外部 owner）。

## 6. 风险与阻塞

- `RISK-2026-038`：BuiltIn 边界与未来 DEC-009 模组映射未定，可能产生迁移成本。Owner：
  Mira Maintainers。缓解：本轮决策已声明"不承诺映射契约"；模组化落地时以投影方式吸纳。
- `RISK-2026-039`：用户消息文本入事件与请求的脱敏依赖宿主自律。Owner：Mira Maintainers。
  缓解：API 手册显式声明责任边界；事件载荷带 digest 便于审计。
- `RISK-2026-040`：长阻塞 BuiltIn 工具占用循环 worker。Owner：Mira Maintainers。缓解：
  `wait` 有硬上限；文档声明长阻塞工具的宿主责任；能力缺口走 Executor 反馈台账。

## 7. 测试与退出条件

- [ ] `MNT-202609-12` 至 `MNT-202609-17` 全部完成并有可复现验证记录。
- [ ] 脚本化 Provider 的循环工具闭环用例通过：工具调用 -> 执行 -> 结果回填 -> 终态决策。
- [ ] 注册表全部 fail-closed 路径（未知工具、身份不一致、重复派发、参数不合规、超预算、
  满队列）有负向测试。
- [ ] 集成测试在 Linux 基准环境通过；Windows/Android 构建组合与 sanitizer 由 CI 取证；
  未运行项保持未勾选并记录补跑条件。
- [ ] 本地与 CI 的 `check_docs.py`、`check_sbom.py`、`check_platform_boundary.py`、
  clang-format/clang-tidy 通过。
- [ ] 文档同步完成，GitHub #8 关闭并引用本轮验证记录。

## 8. 验证记录

2026-09-07：创建本轮维护计划；DEC-015/DEC-016/DEC-017 同日接受。

2026-09-07：本地实现与验证（Ubuntu 24.04，x86_64，g++ 13.3.0，CMake 3.28.3，Ninja；
本机无 clang/clang-tidy，由 PR CI quality job 补验）。

- 实现：`tool_executor.hpp/.cpp`（BuiltinToolRegistry + wait 工具）、
  `conversation_log.hpp/.cpp`（会话投影）、`agent_loop.hpp/.cpp`（工具分支、结果回填、
  `enqueue_user_message`/步边界注入、`UserMessageInjected`/`ToolExecuted` 事件）、
  `core_contracts.hpp`/`runtime.hpp`/`runtime.cpp`（`CompleteTask` 命令与
  `complete_task`）。
- 新增测试：`mira_m3_tool_executor_test`（注册拒绝、身份一致性、至多一次、参数校验、
  异常/超限、wait 行为与取消、投影重建与 fail-closed）、`mira_m3_tool_loop_test`
  （工具闭环往返、无注册表 fail closed、失败结果回填、执行预算、步边界消息注入与队列
  上限、无工具会话投影）、`mira_agent_harness_test`（Runtime 会话内 Executor 托管闭环 +
  工具 + 介入 + `complete_task` 终态幂等 + 事件/投影一致性）。
- 结果：Debug 与 Release `ctest` 各 46/46 通过（含既有 43 项回归）；ASAN、UBSAN 各
  46/46，TSAN（`setarch x86_64 -R`，mbedtls portable 按配置禁用）45/45 通过；
  `format-check`、`docs-check`、`sbom-check`、`platform-boundary-check` 通过。
- TSAN 发现并修复：集成测试初版的取消探针在 Simulator 环境锁内回调
  `MiraRuntime::task_snapshot`，与控制面 `close_session` 在 Runtime 锁内调用
  `environment->interrupt()` 构成锁序倒置。修复：探针契约明确为"轻量、不得回调
  Runtime/环境"（`environment.hpp` 注释），测试以独立监控任务轮询快照并经原子标志
  桥接。本机无 clang/clang-tidy，由 PR CI quality job 补验。
- 限制：本机无真实模型端点，工具闭环以 RecordingProvider（canonical 响应）验证，
  真机/真供应商验证随 miracle 消费侧进行；`mbedtls portable` 等平台矩阵由 CI 覆盖。
- 同步：DEC-015/016/017、API 手册（index/model-agent-loop/core-runtime）、架构设计
  §2.4/§6.1/§6.6、M7 验证记录、总计划 §5 决策索引、参考研究 §9。
