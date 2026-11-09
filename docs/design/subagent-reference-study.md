# Subagent 执行与协作参考研究

> 状态：Proposed（研究输入，不构成决策或实现承诺）
> 版本：0.1
> 更新日期：2026-11-09
> 负责人：Mira Maintainers
> 工作项：[MNT-202611-SA-01](../plans/maintenance-2026-11-subagent-study.md)
> 适用范围：Agent Harness 中的子会话委派与结果回流

## 1. 结论与证据边界

Mira 已有子代理的上下文隔离基础，尚缺模型可调用、由明确 owner 管理的子代理执行
与协作闭环。不能把 M24 的完成理解为 spawn/消息/后台结果回收已经交付，也不宜另建
一套与 Task、Session、Executor 无关的运行时。

本报告基于 2026-11-09 的只读源码核查。所有「支持」均指源码路径存在；测试只阅读，
未在外部仓库执行，不声明运行互操作、性能、模型服从率或跨进程恢复保证。

| 对象 | 固定版本 | 核查入口 |
| --- | --- | --- |
| Mira | `edef84713a505d678d515f47f74ee297eeb478c5` | DEC-044、M24、M25、ConversationLoop、Runtime、ModelGateway、BudgetLedger |
| DeepSeek Harness（DSH） | `fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159` | subagent manager、activation、fork provider、delegation/control tools、parent-result 测试 |
| ZCode | `a9fe401eede6e6e5e01be9048679c206fe6f02d9` | SubagentPort、runner、child runtime、tool policy、background notification/command queue、相关测试 |

定位使用 GitHub connector 的仓库搜索与文件读取；目录枚举补用 GitHub CLI tree API，
以 `git ls-remote` 核对两个外部主分支 commit。另参考 Mira [PR #120](https://github.com/Linductor-alkaid/mira/pull/120)
的 system 模板覆盖（`eb2448d`），它在本次核查的主干基线之外，不视为 subagent 交付。

## 2. Mira 已有能力和实际缺口

### 2.1 已有基础

- [M24 公共接口](../../include/mira/context_working_context_fork.hpp)提供
  `fork_working_context`、`working_context_delta_from_fork` 和
  `merge_working_context_delta`。这些都是确定性投影函数，不创建会话、不执行模型、
  不启动后台工作，也不直接写 store。fork 继承的是 Working Context 快照，不是
  供应商原始会话日志或 KV cache。
- [M25 集成测试](../../tests/integration/working_context_host_test.cpp)由宿主显式
  创建 child Session/Task，构造自己的 gateway、admission 和 AgentLoop，消费子链、
  产生 delta 并合并。这证明基础可以接线；它不是模型可调用的委派 API，也没有提供
  通用的后台子任务管理器。
- [MiraRuntime](../../include/mira/runtime.hpp)已有 Session、Task、取消、接管和关闭；
  [DEC-097](../decisions/DEC-097-per-request-tool-selection.md)已有每请求工具选择。
  PR #120 提供宿主 system 模板配置，能用于描述子角色，但角色提示不能代替执行设施。

### 2.2 缺少的闭环

尚未看到统一的模型工具和宿主契约覆盖：创建子会话、登记父子关系、启动与持有执行
句柄、查询/跟进/中断、限制并发与深度、父级预算约束、回收结果并继续父会话。

尤其是 [ConversationLoop](../../src/model/conversation_loop.cpp)：`run()` 的工具历史
是局部变量；无工具文本结算为 `Answered` 并退出。Continuous 取消累计次数上限，
仍以文本回答终止。[用户消息入队](../../include/mira/conversation_loop.hpp)仅在活跃
run 的轮边界抽取，不能自动恢复已退出 run 的私有历史，也不会唤醒结束后的父会话。

由此推断：把 `spawn_agent` 注册成工具再调用一次 `enqueue_user_message()`，不足以
交付可靠的异步委派。必须明确父 Session 的持久上下文、后续推进入口与结果消费水位；
不能通过复活已 Completed/Cancelled 的 Task 来模拟继续。

## 3. DeepSeek Harness：会话身份与一次执行分离

### 3.1 生命周期与后端

DSH 将持久 child Session 和一次活跃的 activation 分开：子会话可保留身份和历史，
一个本地 child 同时只有一次活跃执行；后续消息可继续或冷加载该 child。activation
持有自己的结果与 `dispose()`，旧句柄只能释放那次执行，不能误停同 ID 的后续执行。

manager 负责准入、创建、输入接收、冷恢复和释放；provider 提供不同后端的创建数据
或单次外部执行句柄。本地 spawn/fork 可继续，Codex/Claude Code/ACP/DSH SDK 等
外部后端不因此自动获得消息续跑能力；请求前以能力标记检查可选参数。
参见[子代理契约](https://github.com/deepseek-ai/deepseek-harness/blob/fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159/docs/subsystems/subagent.md)、
[manager](https://github.com/deepseek-ai/deepseek-harness/blob/fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159/packages/subagent/subagent/src/manager.ts)。

### 3.2 上下文和配置

spawn 是新上下文；fork 只复制父日志截至最后一个 `turn/end` 的完整前缀，不复制当前
未闭合工具轮。前缀仅在首次创建时捕获，后续恢复使用子日志，不重新 fork 父的新历史。
这与 Mira 的快照 fork 不同，应作为两个显式的上下文策略讨论，不能混用同一名称。
见[fork 源码](https://github.com/deepseek-ai/deepseek-harness/blob/fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159/packages/subagent/subagent-fork-in-process/src/index.ts)。

子角色与工具组合归 preset；spawn 可选择独立 preset，fork 继承父组合。模型路由选择
由配置允许后才暴露，执行时再次做授权路由和能力校验。权限状态在委派时捕获；组合了
approval 服务时，child 的审批策略固定为 never，需要超出范围时返回限制。不能把
「继承父上下文」误认为「继承父的一次性授权」。见
[工具配置](https://github.com/deepseek-ai/deepseek-harness/blob/fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159/packages/subagent/tool-subagent/README.md)、
[child-agent 源码](https://github.com/deepseek-ai/deepseek-harness/blob/fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159/packages/subagent/subagent/src/child-agent.ts)。

### 3.3 消息、结果与关闭

模型侧委派工具在启动接收后返回 started，不等待最终结果。manager 捕获结果、释放
资源后，向仍在线的直接父代理发完成通知；父空闲时 queue 可启动下一轮，活跃时用
steer。结果、停止原因和清理失败分别表达，不用清理异常覆盖已捕获的子结果。父离线
时这条通知路径不提供无条件投递保证。

control 工具有 `send_message`、`list_agents`、`interrupt_agent`、`wait_agent`。
发消息按同根树验证身份；中断需要祖先授权；名字解析不授予权限。interrupt、单次
activation dispose 和整棵树 drain 是不同操作，不能都映射成一个 cancel。
`wait_agent` 等的是调用者自己的下一步 inbox，并非某个子任务完成屏障。
见[control 契约](https://github.com/deepseek-ai/deepseek-harness/blob/fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159/packages/subagent/tool-subagent-control/README.md)、
[parent-result 测试](https://github.com/deepseek-ai/deepseek-harness/blob/fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159/packages/subagent/subagent/tests/parent-result.spec.ts)。

每个根树共享活跃容量池，启动超限明确拒绝；默认配置为 maxDepth=1、
maxActiveSubagents=8。容量按活跃执行回收，不按历史 child 数量永久占用。
关闭先封准入，等待或取消未发布创建，再按树释放。
见[activation 容量池](https://github.com/deepseek-ai/deepseek-harness/blob/fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159/packages/subagent/subagent/src/activation.ts)、
[默认配置](https://github.com/deepseek-ai/deepseek-harness/blob/fcc0ad462172522aaf5c93a7b2e01f6b9f0b0159/packages/subagent/subagent/src/index.ts)。

## 4. ZCode：工具入口、子 runtime 与通知队列

### 4.1 入口与配置

canonical 模型工具是 `Agent`，`Task` 为隐藏的兼容 alias；无 SubagentPort 时不注册。
Port 提供 launch/run/start、状态查询、等待、停止和发消息。前台返回 completed，后台
返回 async_launched，均带稳定 agent/task 标识。实现由 profile 驱动，并提供通用和
Explore 角色；child 具有自己的 Session 和 runtime。
见[SubagentPort](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/apps/zcode-cli/packages/contracts/src/interfaces/subagent.port.ts)、
[Agent handler](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/apps/zcode-cli/packages/core/src/tool/handlers/agent.ts)。

child 使用专门的 context builder 和角色 system，而非自动复制父对话；父已解析的
AGENTS 指令可按 profile 配置继承。child runtime 禁止再次创建 subagent，也统一
剔除没有独立审批恢复面的 plan 工具。插件权限声明只能收紧；具体工具仍各自执行
准入，不能因为 Agent 入口标为 read-only 就认为子任务没有副作用。
见[child runtime 接线](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/apps/zcode-cli/packages/core/src/runtime/methods/subagent.ts)、
[工具策略](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/apps/zcode-cli/packages/core/src/subagent/tool-policy.ts)、
[上下文构建](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/apps/zcode-cli/packages/core/src/subagent/context-builder.ts)。

### 4.2 后台结果真正回到父模型

runner 完成后写 output/metadata 等产物，更新 runtime task，再提交父通知。通知进入
运行时 command queue；idle drain 启动 model-only 后续轮，活跃轮使用对应消费路径。
批次在模型请求前持久化成 synthetic user message，带 background_task 来源，不伪装
成用户手发消息或 System 指令。通知有分支代次检查，shutdown 期间不再唤醒模型。
见[通知接收与持久化](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/apps/zcode-cli/packages/core/src/runtime/methods/background-notifications.ts)、
[command drain](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/apps/zcode-cli/packages/core/src/runtime/methods/runtime-command-queue.ts)。

当前源码注释明确：command queue 本身仍在内存，重启后未消费 admitted 通知收口为
discarded(session_resumed)，不是耐久通知重投。不可将通知 ledger 的存在说成任意
崩溃后的后台执行恢复或恰好一次副作用保证。

旧[后台唤醒计划](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/docs/cli-background-subagent-wake-plan.md)
记录过「child 已完成，但父 idle 后无人 drain」的问题；当前源码和
[测试](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/apps/zcode-cli/packages/core/tests/subagent-background.test.ts)
已有 idle wake、批次和 model-only 持久化断言。旧计划不能代表当前仍有同一缺陷。

### 4.3 取消和 owner

Agent 工具当前没有固定工具墙钟 timeout，仍支持取消；前台 setup/ready 阶段也安装
inactivity watchdog 和 abort guard。runner 有可选的 autoBackgroundMs 转后台路径，
不重启 child。后台只脱离父 turn，未脱离 owner：port.close 封准入、请求停止、广播
shutdown、等待 tracked work 并关闭自己持有的 child runtimes，清理错误显式汇总。
见[runner](https://github.com/zai-org/ZCode/blob/a9fe401eede6e6e5e01be9048679c206fe6f02d9/apps/zcode-cli/packages/core/src/subagent/runner.ts)。

## 5. 对 Mira 的具体建议

以下是研究建议，尚未形成 Accepted 决策。

| 问题 | 建议采用的边界 |
| --- | --- |
| 建模 | 子代理仍是 child Session + Task；分开持久 child 身份与每次执行 ID/epoch，句柄只作用于准确执行 |
| 模型入口 | 小型委派工具返回接收句柄；运行 owner 管理结果、取消和关闭，不让工具 handler 私藏生命周期 |
| 上下文 | 首期新上下文或 M24 快照基线；完整历史 fork 后续单独定义协议闭合与过滤，不承诺 KV cache 复用 |
| 模型/工具 | 宿主角色配置给出 profile、system、工具权限；DEC-097 只负责本轮暴露，执行权限仍独立校验 |
| 结果回流 | 带 child/run/parent/epoch/provenance 的结构化完成输入；父活跃时接收，父 idle 时经 owner 后续推进；终态 Task 不复活 |
| 预算 | 同时限制单次执行、活跃数量、深度、累计创建数、根任务预算、inbox/结果容量；并发上限不等于总费用上限 |
| 取消/接管 | 区分父轮结束、父任务取消、child interrupt、child 执行取消、Session close；Human Takeover 阻止新自主副作用 |
| 持久化 | 明确结果接收/消费水位与重复投递规则；先确定进程崩溃后的处置，不把所有 child 重启或重新执行工具 |

三个需要先解决的 Mira 特有问题：

1. **会话恢复与结果投递**：同一 run 的入队与跨 run 的后续推进不是同一 API。
   应区分父模型一轮结束、等待子结果和任务真正完成；父任务被取消后，迟到结果只能
   记录或供显式新任务读取，不应重新启动旧任务。结果通知本身也不应自动合并 M24
   delta；合并继续走已有校验/提交管线。
2. **并发所有权和共享状态**：[ModelGateway](../../include/mira/model_gateway.hpp)
   含 circuit、provider 诊断和事件 session 绑定；[BudgetLedger](../../include/mira/model_budget.hpp)
   按 TaskId 记账，未见这里给出并发共用及根子聚合预算契约。不能把一个 gateway 无
   约束地交给多个 child 并行 infer。首期可由宿主为每个 child 构造隔离依赖，或经
   明确串行 owner 访问；总预算仍须单独设计，不能因隔离而绕过父限额。
3. **真实设备共享**：DEC-044 的 W5 不包含动作，租约按 Session 隔离。父子 Session
   若指向同一设备，独立租约本身不会防止同时输入。首期建议只开放推理和明确只读
   工具，设备输入、发送消息等对外写操作另行授权并设计共享资源仲裁；不能直接把
   编程 harness 的并行 Bash/共享工作目录假设搬到 Android 连续控制。

所有执行和唤醒继续由外部注入的 kairo Executor 管理。不得同步占满同一有限 worker
池后再等待必须由该池执行的 child；具体 worker/有限任务/等待方式须在实施设计中
核对当前公开 API、取消与关闭路径，不从 TypeScript Promise 代码推定 C++ 调度保证。
目前没有确认 Executor 缺口，不登记未经复现的能力问题。

## 6. 建议落地顺序与验收

### P0：受管委派闭环

在 Agent Harness 层设明确 owner，交付单层子会话、新上下文或 M24 基线、固定宿主
角色/工具授权、有界并发和结果、启动/查询/取消、完成回流及 idle 后续推进。
建议从异步接收/完成的生命周期建模，避免先做阻塞工具、再补后台时重写所有权。
内部结果可有 future/handle，但由 owner 持有并消费，不交给模型充当持久身份。

P0 不应只验证「调用工具后回文本」；至少覆盖：

- 正常完成、child 模型/工具失败、结构化结果错误与部分输出；未知 profile/工具权限拒绝。
- 深度、并发、累计创建、预算、输入/结果容量及 Executor 提交拒绝；失败无孤儿资源。
- 创建中/执行中取消、deadline、父任务终态、Human Takeover 和 shutdown；迟到结果不复活。
- 父活跃时收结果、父 idle 后无需新用户输入继续、父结束瞬间完成的竞态；同一结果不重复消费。
- 关闭与启动、消息与关闭、重复取消竞争；收到停止请求和真实资源释放分别可观测。
- 进程重开后的 child/result 处置与 replay 无真实副作用；有稳定 parent/child/run 归属事件。
- 只有两个 worker 且并发多个 child 的进展测试；共享 gateway/预算/事件依赖的竞争检查。

### P1：持续协作和恢复

补 child 跟进/steer、结构化消息 inbox、发现/等待，以及 settled child 的显式新执行。
若需要耐久 outbox、崩溃后继续和层级预算，应作为对应的明确交付项，而非宣称已有
event store 就自然具备。可按已存在的 M29/M31 能力复用，不能假定未交付分支已可用。

### P2：扩展后端与副作用

按真实需求扩展外部进程/ACP 后端、多层委派、共享设备写操作及仲裁。不同后端以能力
描述区分「能否继续、能否 fork、结构化输出、模型选择」，不强行提供相同假语义。

实施前需要新的决策记录，并明确补充或修订
[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)第 1 条：继续保持
child Session 和双平面架构，同时处理原文明确排除「subagent 框架」与新编排层的关系。
M24 的投影契约和既有完成状态无需因新增执行层而被重写；P0/P1/P2 均尚未实施。

2026-11-09 后续：用户已授权依据本报告推进；单层 P0 由
[DEC-099](../decisions/DEC-099-managed-subagent-delegation.md)与
[M34](../plans/m34-managed-subagent-delegation.md)承载。上文保持调研时点的证据，
后续实际交付状态以 M34 为准，P1/P2 不随 P0 自动完成。
