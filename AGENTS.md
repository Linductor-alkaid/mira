# Mira 项目协作约定

## 适用范围

本文件适用于 Mira 仓库中的全部自研代码、测试、文档与构建配置。`third_party/`
中的上游代码遵循其自身约定；除非任务明确要求升级或修复依赖，否则不要修改其中的
代码。

## 项目管理与文档规范

所有计划、里程碑、设计、决策、验证证据和文档变更必须遵循
[`docs/project/project_management_and_documentation.md`](docs/project/project_management_and_documentation.md)。
开始非平凡变更前，必须确认所属计划工作项、相关设计和决策依据；完成时必须同步更新任务
状态、测试结果、验收证据以及受影响文档。环境限制导致的未执行验证不得标记为完成，必须
记录原因、负责人和补跑条件。

## 产品目标

Mira 是使用现代 C++ 构建的跨平台原生 AI Agent Runtime。核心运行闭环为：

`Observe -> Reason -> Plan -> Act -> Verify`

实现必须保持 Agent Core 与具体平台解耦。Android、Windows、Linux、机器人、模拟器等
宿主能力只能通过 Platform Adapter 接入，不得渗入 Core。核心层不得直接依赖 Android
SDK、JNI、Activity、AccessibilityService、MediaProjection 或同类平台 API。

优先围绕以下接口形成稳定边界：

- `IEnvironment`：聚合环境观察与动作执行能力。
- `IScreenProvider`：提供截图或其他视觉输入。
- `IInputProvider`：执行平台最终输入事件。
- `IModelProvider`：调用外部 LLM/VLM，并返回结构化结果。
- `ITool`：提供可发现、可验证的扩展工具。
- `IMemory`：保存任务所需的短期或长期记忆。
- `IAgentObserver`：向宿主发布状态、事件和诊断信息。

## Executor 是强制基础设施

Mira 必须依赖仓库中的 `third_party/executor` 管理并发任务和运行生命周期。集成时以
`third_party/executor/include/` 的公开头文件、`third_party/executor/docs/API.md` 和
`third_party/executor/docs/skill/executor-integration/SKILL.md` 为准。

以下规则是强制要求：

1. 所有异步任务、延时任务、周期任务、依赖任务、阻塞 I/O worker、实时控制任务及其
   启动、停止、等待和诊断，必须通过 Executor 的公开能力管理。
2. 自研代码不得使用 `std::thread`、`std::jthread`、`std::async`、自建线程池、私有定时
   调度器或脱离 Executor 生命周期的 fire-and-forget 工作。平台 API 要求线程亲和时，
   将其封装在 Platform Adapter 中，并通过 Executor 支持的外部事件循环或扩展边界协调。
3. 普通有限任务优先使用 `submit_auto()`，并保留、消费其 `future`。成功入队不等于执行
   成功，影响任务结果的异常不得被丢弃。
4. 长期阻塞 I/O 使用 Executor 的 worker 生命周期能力；允许抖动的后台周期任务使用
   timer 能力；固定周期或低延迟控制使用 realtime/low-latency 能力。不得用普通周期任务
   冒充实时控制。
5. 取消必须是协作式的。长任务和循环必须定期检查 stop/cancellation 状态，并保证等待、
   网络、模型调用和平台动作具有可解除阻塞的路径。超时不是强制终止，deadline 也不能
   被当作抢占保证。
6. Executor 必须由明确的外部 owner 初始化和关闭。关闭顺序为：停止任务生产者，发出
   取消或停止请求，回收 worker/实时路径，等待需要完成的有限任务，最后由非 worker
   线程执行 `shutdown(true)`。不得从 Executor worker 内完成自等待或最终 teardown。
7. Runtime、Task、Session、Controller 和 Platform Adapter 不得各自隐藏全局 Executor
   生命周期。依赖通过构造参数或明确 context 传递；资源所有权、任务句柄和关闭顺序必须
   可见且可测试。
8. 队列容量、背压、提交拒绝、超时、任务异常和关闭中的提交必须转化为明确结果与事件，
   不得静默重试、无限排队或吞掉失败。

## Executor 能力缺口与反馈台账

不得为了绕过 Executor 的能力边界而静默引入另一套并发或生命周期设施。当确认 Executor
无法满足 Mira 的合理需求时，必须执行以下流程：

1. 先核对当前版本的公开头文件、API 文档、集成指南及相关测试，排除 API 选型错误、配置
   错误、平台限制和应用层职责。
2. 在 `docs/executor_feedback/ledger.md` 中新增一条唯一编号的反馈记录，附上可复现证据、
   影响范围、期望语义和可验收结果。只写“Executor 不支持”不构成有效记录。
3. 在相关代码、测试或设计文档中引用该反馈编号。
4. 确需临时方案时，将其限制在单一 Adapter/compatibility boundary 内，说明行为差异、风险、
   移除条件和测试覆盖。临时方案不得改变“Mira 的任务与生命周期由 Executor 管理”这一
   总体约束。
5. 未经明确授权，不直接修改 `third_party/executor` 来掩盖集成问题，也不把项目特有策略
   下沉到通用 Executor。

以下情况不是 Executor 能力缺口：模型供应商协议适配、Android 手势映射、业务状态机策略、
错误使用已有 API，或平台本身不提供所需权限。它们应在 Mira 对应层解决。

## Runtime 与状态模型

- Agent 必须实现为可推进、可观测、可中断的显式状态机，而不是不可中断的单体 `run()`。
- 标准状态至少覆盖 `Idle`、`Observing`、`Reasoning`、`Planning`、`Acting`、
  `Verifying`、`Recovering`、`Completed` 和 `Failed`；暂停、恢复、取消、超时和 Human
  Takeover 必须有定义明确的转换。
- 每个推进步骤应是有界工作单元。耗时网络请求、截图、输入执行和控制循环交给 Executor
  管理，并通过结构化结果推动状态转换。
- Task 与 Session 必须有稳定 ID、取消上下文和生命周期所有者。终态必须幂等；迟到的模型
  响应或动作结果不得让已取消/已完成任务重新进入活动状态。
- Human Takeover 必须阻止新的自主动作进入环境，取消或安全收敛正在执行的连续控制，并在
  恢复前重新观察环境。

## Observation、Model 与 Action 边界

- Observation 是可扩展快照，可包含截图、UI/Accessibility Tree、前台应用、设备状态、
  时间戳和历史动作。不能假设所有平台都具有结构化 UI 信息。
- 模型层采用 API-first 设计。首阶段通过 `IModelProvider` 接入 OpenAI-compatible 服务；
  Core 不依赖具体供应商 SDK，凭据不得进入事件日志或源码。
- 模型输出必须先解析和验证为结构化 Decision/Action。任何原始模型文本都不能直接调用
  平台输入能力。
- Action 至少区分高层 Intent、中层控制动作和底层输入事件。平台 Adapter 只负责最终映射，
  不承载通用规划、轨迹生成或验证策略。
- `tap`、`long_press`、`back`、`home`、`type` 等离散动作与 `swipe`、`drag`、
  `joystick`、自定义 Touch Trajectory 等连续动作必须使用同一套可取消、可观测的结果语义。
- 低频 LLM/VLM 决策不得进入高频控制循环。诸如 `JoystickIntent` 的高层意图由 Native
  Controller 转成有时间边界的轨迹，再交给 `IInputProvider`。
- 每次动作执行后必须重新观察或使用明确的低成本检测信号进行 Verify；不能仅凭输入 API
  返回成功就判定目标完成。

## 事件、可观测性与复现

- 每个 Task 记录带顺序号和时间戳的事件：Goal、Observation 元数据、Model Request/Response
  元数据、Decision、Plan、Action、Execution Result、Verification Result、状态转换、取消、
  恢复和终态。
- 事件载荷应可版本化和序列化。大体积截图等内容使用稳定引用和摘要，不在多个事件中复制。
- 日志必须脱敏，禁止记录 API key、Authorization header、用户密码或输入法敏感内容。
- Observer 回调不能阻塞 Runtime 关键路径；回调异常必须隔离并转为诊断信息。
- Replay 必须能区分已记录的外部结果与真实副作用，默认不得在回放时重新执行输入动作或网络
  请求。

## 工程约束

- 使用 C++20 和 CMake；公开 API 避免暴露平台类型，平台相关编译单元保持可选。
- Core 依赖方向应指向抽象，Platform Adapter 依赖 Core 的接口，不得反向依赖。
- 所有跨线程共享状态必须有明确所有权或使用 Executor 提供的通信原语；不得依赖隐式全局
  可变状态。
- 为状态转换、取消竞态、关闭顺序、队列拒绝、模型响应解析、动作校验和连续控制边界编写测试。
- 新增并发路径时至少验证：正常完成、任务异常、提交拒绝、执行中取消、超时、shutdown 与
  Human Takeover。
- 变更公开契约时同步更新设计文档和示例。不得宣称未通过目标平台或基准验证的实时性、性能
  或跨平台保证。

## 完成定义

一项 Mira 变更只有在以下条件满足时才算完成：职责位于正确层；全部任务受 Executor 管理；
取消和 shutdown 路径闭合；失败对调用方和 Observer 可见；关键事件可复现；相关测试通过；
计划状态、设计、决策和验收证据已经同步；若遇到 Executor 能力缺口，反馈台账已经按要求
登记并被实现引用。
