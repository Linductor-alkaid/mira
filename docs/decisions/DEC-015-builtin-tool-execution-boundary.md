# DEC-015：BuiltIn 工具执行边界与 AgentLoop 工具闭环

> 状态：Accepted
> 日期：2026-09-07
> 负责人：Mira Maintainers
> 冻结里程碑：M4 后维护轮（[maintenance-2026-09-agent-harness-closure.md](../plans/maintenance-2026-09-agent-harness-closure.md)）
> 替代/被替代：无（不改变 [DEC-009](DEC-009-tool-module-boundary.md) 的模组体系方向）

## 背景与问题

GitHub [#8](https://github.com/Linductor-alkaid/mira/issues/8)（`MIR-20260905-002`）：模型输出
ToolProposals 时 `AgentLoop` 将其视为不可执行并直接终态 `Failed`（"tool proposals are not
executable in the M3 loop"）。Agent Harness 因此缺少工具调用闭环：解析桥
（`resolve_tool_calls`）与结果回填（`build_tool_result_input`）两端存在，中间的执行派发层
缺失。原排期为 M7 重定义后按 POST-01 证据处理；2026-09-07 维护者依据
[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md) 的 Harness 审计结论（控制平面组件
清单中 Tool Calling 未闭合）指令提前落地最小闭环。

## 决策

1. 新增**最小 BuiltIn 工具执行边界**：`BuiltinToolRegistry` 以
   `BuiltinToolSpec`（ToolId、版本、wire_name、描述、参数 JSON Schema、副作用声明）+
   处理函数注册进程内工具；不含 manifest、签名、attestation、OutOfProcess 隔离和模组
   生命周期——它们仍属 DEC-009 与 M7 重定义范围。
2. 暴露经既有 `ModelRequest.tools`（`ExposedToolSpec`）：注册表快照按 wire_name 排序、
   计算 spec digest，不新增绕过 `resolve_tool_calls` 校验的模型旁路（对齐 DEC-009 的
   “Workflow/工具操作经 Tool 通道”立场与 `W-04` 精神）。
3. 执行语义 fail closed：
   - 注册时 schema 过 `gate_schema_subset`，wire_name/ToolId 冲突拒绝；
   - 执行前校验 ToolId、wire_name、版本与副作用声明与暴露快照一致（注册表被运行中篡改
     即拒绝），参数经 `validate_instance_against_schema` 本地校验；
   - **至多一次派发**：同一 `OperationId` 的重复执行直接拒绝（`W-02`）；
   - 参数不合规与处理函数失败产出 `failed` 的 `ToolExecutionRecord` 回填模型（可修复），
     注册表级拒绝（未知工具、身份不一致、重复派发、超预算）为循环终态失败；
   - 取消经 `OperationContext` 协作传播，处理函数不得抛出（边界捕获转为失败记录）。
4. AgentLoop 工具分支：存在注册表时，ToolProposals 逐步执行、结果以
   `mira.agent-loop.tool-result.v1` 来源的输入项回填下一轮请求；无注册表时保留现状
   （终态失败），消息不再声称 M3 限制。单次运行工具执行数有预算上限（`RULE-08`）。
5. Core 附带一个参考 BuiltIn 工具 `wait`（有界时长、切片睡眠、可取消、无副作用），由
   宿主显式注册；其余工具由宿主经同一边界注入。
6. 事件：每次执行发 `ToolExecuted`（wire_name、operation_id、failed、参数摘要 digest），
   不携带原始参数或结果载荷（脱敏约束）。

## 备选方案

- **等 M7 重定义落地完整 DEC-009 模组后再闭合**：闭环继续缺失，DEC-014 审计确认的
  Harness 缺口无法验证；维护者指令提前，不采用。
- **把工具执行塞进 `IEnvironment`**：环境契约是平台观察/输入边界，工具是 Harness 能力，
  混淆两层职责，不采用。
- **本轮直接实现 ToolModule/签名/隔离**：范围失控且 M7 重定义未决，不采用。

## 影响与风险

- 公开 API 新增 `tool_executor.hpp`；`AgentLoop` 新增 `set_tool_registry` 与配置字段。
- BuiltIn 边界与未来 DEC-009 模组的关系：模组体系落地后，BuiltIn 注册表应能投影为模组
  的一种来源（BuiltIn 模组）；本轮不承诺该映射的公开契约。
- 处理函数在循环占用的 Executor worker 上内联执行；有界长阻塞工具由宿主自行拆分或经
  后续能力承载（Executor 能力缺口走台账流程）。
- 工具结果是外部不可信数据（`RULE-09`）：回填时以 provenance 来源标记，不提升权限，
  不进入 System/Developer 权威。

## 验证方式

- 注册表单测：注册冲突、schema 子集拒绝、身份不一致拒绝、重复 OperationId 拒绝、
  参数校验失败转失败记录、`wait` 取消与上限。
- 循环单测：脚本化 Provider 先后返回工具调用与终态决策，断言工具执行、结果回填、
  事件序列与终态；无注册表时保留终态失败。
- 集成测试：Runtime 会话内托管循环 + 工具 + 对话消息的单一系统验证（见维护计划）。

## 关联文档和工作项

- [维护计划：Agent Harness 闭合（2026-09）](../plans/maintenance-2026-09-agent-harness-closure.md)
- [DEC-009](DEC-009-tool-module-boundary.md)、[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)、
  [DEC-016](DEC-016-conversation-events-and-user-messages.md)
- GitHub #8（本决策落地后关闭）
- [Agent Harness 参考研究](../design/harness_reference_study.md) §5.1
