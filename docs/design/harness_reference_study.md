# Agent Harness 参考研究：LangGraph 与 Pi

> 状态：研究输入（research note；不构成决策、设计规范或实现承诺）
> 版本：0.1
> 更新日期：2026-09-07
> 负责人：Mira Maintainers
> 适用范围：[DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 落地中
> Agent Harness 闭合与 Workflow 契约冻结（[M8](../plans/m8-workflow-contracts.md)）的机制参考

## 1. 文档目的与效力

本文响应架构设计
[Agent Harness 与 Workflow 架构设计](agent_harness_and_workflow_architecture.md)第 18 节的研究要求，
对 LangGraph 与 Pi（pi-mono）两个参考系统做机制级分析，并映射到 Mira 当前核实的三个
Harness 缺口（工具执行闭环、Conversation 一等公民、Loop 与 Runtime 集成）及 M8 契约
冻结工作项。

效力约定：

- 本文是对外部系统的转述与对 Mira 的建议，不是规范。任何采纳都必须经
  [M8](../plans/m8-workflow-contracts.md) 的决策记录或专项设计显式冻结。
- 外部系统描述基于 2026-09-07 检索的公开文档与作者文章（见第 8 节），可能随版本变化；
  引用其行为不表示 Mira 承诺兼容或对齐。
- 第 7 节的"有意分歧"与 `AGENTS.md`、`RULE-01` 至 `RULE-12`、`W-01` 至 `W-08` 冲突时，
  以后者为准。

## 2. 参考对象概览

**LangGraph**（LangChain）：以 `StateGraph`（带类型状态、节点、条件边）为中心的有状态
执行框架。同一套图运行时既承载 LLM 决策节点，也承载确定性代码节点；配合 checkpointer
（每步快照）、thread（持久游标）与 `interrupt()`/`Command(resume=...)` 提供 durable
execution 与 human-in-the-loop。

**Pi**（badlogic/pi-mono）：极简编码 Agent 工具链，分四层——`pi-ai`（统一 LLM API，仅
四种 wire 协议 + 其上的 `agent-loop.ts`）、`pi-agent-core`（`Agent` 类：状态、事件订阅、
消息队列、附件、工具执行）、`pi-tui`（终端 UI）、`pi-coding-agent`（CLI：会话、工具、
斜杠命令）。作者的设计准则是"不需要的就不建"。

两者与 Mira 的关系互补：LangGraph 展示**可恢复、可中断的有状态执行**如何统一 agent 与
workflow 两种节点；Pi 展示**Harness 核心可以多小**、工具闭环与运行中用户消息的最小机制。

## 3. LangGraph 机制提炼

以下为 `interrupt()` 相关文档（2026-09-07）的机制要点：

1. **interrupt 语义**：节点内任意位置调用 `interrupt(payload)`，以异常方式上抛、由运行时
   捕获；图状态经 checkpointer 保存，无限期等待外部输入。恢复时 `Command(resume=value)`
   以同一 thread 重新调用，resume 值成为该 `interrupt()` 调用的返回值。
2. **恢复即节点重跑**：resume 后整个节点从头重新执行（不是从 interrupt 行继续）；同一节点
   多个 interrupt 按索引严格匹配。文档明确要求：interrupt 之前的副作用必须调用方自行幂等
   （建议 upsert），或把副作用放到 interrupt 之后。
3. **thread 即持久游标**：`thread_id` 复用则从既有 checkpoint 恢复，新开则全新状态；被
   描述为 "persistent cursor"。
4. **checkpointer 节奏**：每个 super-step 保存状态快照；生产用持久后端（Sqlite/Postgres 等）。
5. **多中断并行**：fan-out 多节点同时中断时，按中断 ID 映射各自的 resume 值。
6. **静态断点**：`interrupt_before/after` 仅为调试建议，正式 HITL 用动态 `interrupt()`。
7. **约束**：payload 必须 JSON 可序列化；不得裸 try/except 包住 interrupt；不得在不确定
   循环中乱序调用 interrupt（会导致指数级重执行）。

## 4. Pi 机制提炼

以下为作者文章《What I learned building an opinionated and minimal coding agent》
（2025-11-30）的机制要点：

1. **循环极小**：`agent-loop.ts` 处理"用户消息 -> 模型 -> 工具调用 -> 结果回填 -> 模型"，
   直到响应不再含工具调用；一切过程以事件流出，供 UI/扩展消费。无步数上限（loop until
   done）。
2. **Agent 类是粘合层**：`pi-agent-core` 的 `Agent` 在循环之上提供状态管理、事件订阅、
   附件处理与**消息队列**——每轮结束后回调取排队用户消息，注入到下一条 assistant 响应
   之前。
3. **工具抽象**：`AgentTool = { name, description, parameters(TypeBox schema),
   execute(toolCallId, args) }`；参数经 schema 校验后才执行；结果拆分为面向模型的
   `output` 与面向 UI 的 `details`。
4. **会话即消息数组**：Context 序列化为纯 JSON 即会话；支持 continue/resume/分支、会话
   中途跨供应商换模型（thinking 等差异做文本化降级）。
5. **Abort 全链路协作式**：取消经整个管道（含工具调用）传播，返回部分结果。
6. **扩展即插件**：无正式 hook API；扩展来自事件流、自定义工具、斜杠命令模板与外部 CLI。
7. **作者立场**：默认 YOLO，认为权限系统"mostly security theater"；上下文工程与全过程
   可观测性高于一切。

## 5. 映射到 Mira 的三个 Harness 缺口

缺口定义为 2026-09-07 对 M0–M4 交付物的核实结论（工具执行闭环缺失即 GitHub #8；
Conversation 无实体；AgentLoop 仅被测试使用、Runtime 不驱动）。

### 5.1 工具执行闭环（GitHub #8）

- Pi 证明最小闭环很小：**schema 校验 -> execute 派发 -> 结果事件回填 -> 下一轮请求**。
  Mira 已有两端（`resolve_tool_calls` 的 fail-closed 解析、`build_tool_result_input` 的
  结果回填），缺的只是中间的执行派发层与 AgentLoop 的工具分支
  （[agent_loop.cpp](../../src/model/agent_loop.cpp) 现将 ToolProposals 直接终态 Failed）。
- Pi 的 `output`/`details` 拆分与 Mira 的 `ToolExecutionRecord`（result + 大载荷
  ArtifactRef）同构；可补强的是"面向宿主的结构化 details"作为公共契约字段。
- 建议（供未来 Harness 闭合里程碑，不改变 M7/#8 的触发条件）：在 DEC-009 模组体系之外
  先冻结一个**最小 BuiltIn 工具执行边界**——只定义执行接口与结果/错误语义，不含模组
  manifest、签名与隔离；模组化仍归 M7 重定义。M8-03 的五个 Workflow 操作 schema 应与
  该边界同构，避免两套工具语义。

### 5.2 Conversation 一等公民

- Pi 的消息队列模式给出**最小可用的运行中介入机制**：用户消息先入队，在步边界被消费进
  下一轮上下文，而不是中断语义猜测。这可直接作为 M8-04「本次运行修改」类 patch 的基础
  语义：pending 用户消息在步边界 drain，模型或 Runtime 解释为目标修改。
- LangGraph 的 `Command(resume=...)` 给出**对 pending 决策点的解决模型**：用户回复按
  ID 匹配到挂起的 interrupt 并成为其结果。Mira 的对应物是 `WaitingUser` 决策点 + 按稳定
  ID（OperationId/digest）匹配的幂等解决；比 LangGraph 的索引匹配更强，应写入 M8-04。
- Pi 的会话分支（branching）提示 Conversation 投影需保留"从某事件分叉"的表达，与
  Workflow 版本化（M8-09）共享不可变历史 + 派生新线的设计。

### 5.3 Loop 与 Runtime 集成

- LangGraph 的核心启示是**统一原语**：agent 节点与确定性节点共用同一套
  状态-检查点-中断-恢复机制。对 Mira 即：AgentLoop 步与 WorkflowRun 步应是同一种
  "步边界 = 事件提交 + 检查点 + 中断点"的可恢复有界工作单元。M8-05 应显式表达这一
  统一，避免出现两套恢复机制。
- LangGraph "resume 即节点重跑、interrupt 前副作用自负幂等"是它的弱项；Mira 已有更强的
  `RULE-05`/`W-02`（至多一次派发 + 不确定必重观察）。WorkflowRun 的恢复语义应冻结为
  **从步边界恢复并重新观察**，不复制整节点重跑模型。此差异应写入 M8-02 的映射决策。
- Pi 的 `Agent` 类正是 Mira 缺的粘合层对应物：Session 内托管循环、状态、事件与消息队列。
  Mira 的对应设计（扩展 Runtime 还是新增 Agent 实体）属未来 Harness 闭合里程碑，但
  M8-05 的分解图应为它预留位置。
- LangGraph 的 thread ≈ 单一持久游标；Mira 已有 SessionId/TaskId/epoch 且将对话（Session）
  与执行（Task）分离，更适合 Takeover 与多任务，无需引入 thread 概念。

## 6. 对 M8 契约冻结的直接输入

| 工作项 | 参考输入 |
| --- | --- |
| `M8-02` WorkflowRun 映射与策略 | 恢复 = 步边界 + 重新观察（拒绝节点重跑模型）；`WaitingUser` 解决按稳定 ID 匹配；策略对介入集合的影响参考 LangGraph 静态/动态中断的分级（声明式介入点 vs 调试断点）。 |
| `M8-03` Workflow 操作 Tool 通道 | 与最小 BuiltIn 执行边界同构；结果拆分模型面（output）与宿正面（details）。 |
| `M8-04` 对话 patch 与 Conversation 工件 | 步边界 drain 排队消息；三类目标区分中"本次运行修改"优先用步边界消息机制表达；patch 幂等按 ID + digest 匹配（强于 LangGraph 索引匹配）。 |
| `M8-05` workflow_runtime_design | 统一可恢复步原语（AgentLoop 步 = Workflow 步）；每步事件提交 + 快照节奏对照 checkpointer 的 super-step 快照；介入点/决策点必须是 IR 声明数据并经 `W-04` 校验，不允许代码内任意中断。 |

## 7. 有意分歧（不采纳清单）

| 参考立场 | Mira 决定 | 依据 |
| --- | --- | --- |
| Pi：默认 YOLO，权限系统"mostly security theater" | 不采纳。Mira 驱动 GUI 环境的外部副作用（发消息、支付类操作），DEC-004 的权限与确认协议不动摇 | `DEC-004`、`W-05` |
| Pi：loop until done，无步数上限 | 不采纳。所有循环和上下文必须有预算上限 | `RULE-08` |
| LangGraph：图即代码（Python 闭包构图） | 不采纳。Workflow IR 必须是数据，版本化、验证后入库 | `W-03`/`W-04`、DEC-002 |
| LangGraph：interrupt 前副作用由调用方自行幂等 | 不采纳。副作用门禁 + 不确定重观察由 Runtime 强制，责任不上移给步骤作者 | `RULE-05`、`W-02` |
| LangGraph：interrupt 可在节点内任意动态放置 | 收敛。Workflow 内介入点是 IR 声明的受验数据；Agent 路径的动态性由 Decision/Tool 通道承载 | `W-04`、DEC-009 |
| Pi：thinking 跨供应商文本化降级后继续 | 部分参考。方言差异处理沿用 M3 dialect 层，但不牺牲决策 schema 的本地校验 | `RULE-04` |

## 8. 参考来源

- LangGraph Interrupts（LangChain 官方文档，2026-09-07 检索）：
  <https://docs.langchain.com/oss/python/langgraph/interrupts>
- LangGraph Human-in-the-loop（LangChain 官方文档）：
  <https://docs.langchain.com/oss/python/langgraph/human-in-the-loop>
- Mario Zechner，《What I learned building an opinionated and minimal coding agent》
  （2025-11-30）：<https://mariozechner.at/posts/2025-11-30-pi-coding-agent/>
- Armin Ronacher 对 Pi 的介绍（2026-01-31）：
  <https://lucumr.pocoo.org/2026/1/31/pi/>

## 9. 后续

- 本研究的采纳情况由决策记录逐条引用并冻结；未被引用的条目保持研究输入地位。
- 2026-09-07 更新：§5.1（工具执行闭环）、§5.2（步边界消息队列与按 ID 解决决策点）与
  §5.3 的"统一可恢复步原语"方向已由
  [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md) 与
  [DEC-017](../decisions/DEC-017-complete-task-command.md) 在最小范围内采纳落地
  （[维护计划](../plans/maintenance-2026-09-agent-harness-closure.md)）；第 7 节的
  有意分歧清单未被突破。
- Codex 与 OpenAI Agents SDK 的对应研究在后续 Harness 里程碑进入 `Planned` 前补充，
  不阻塞 M8。
