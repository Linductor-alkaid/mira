# Workflow、Context 与 Recovery 离线宿主

> 状态：Active
> 版本：1.0
> 更新日期：2026-11-06
> 负责人：Mira Maintainers
> 依据：[DEC-086](../decisions/DEC-086-offline-composition-hosts.md)、MNT-202611-06

## 构建和验收

```bash
cmake --preset debug
cmake --build build/debug --target mira_workflow_runtime_host_consumer mira_context_assembly_host_consumer mira_recovery_planner_host_consumer -j 4
ctest --test-dir build/debug -R '^mira_(workflow_runtime_host_consumer|context_assembly_host_consumer|recovery_planner_host_consumer|installed_consumer)_test$' --output-on-failure
```

三个程序都可从 build/debug 独立运行。installed_consumer 门禁另行安装 Mira，
用 find_package(Mira/kairo) 编译同一份样例源文件并执行；不使用 Core 私有头。
Recovery 程序同时需要同目录的 journaled_simulator_host.hpp，它只是 simulator
的固定身份/资源 lease 示例，不是生产平台授权策略。

| 程序 | 实际消费链 | 持久化声明 |
| --- | --- | --- |
| [workflow_runtime_host_consumer.cpp](../../examples/workflow_runtime_host_consumer.cpp) | 创建 run→start/wait→只读屏宽工具→step_result 验证→终态事件 reader | MemoryEventStore 仅进程内；不提供 Workflow 重启恢复 |
| [context_assembly_host_consumer.cpp](../../examples/context_assembly_host_consumer.cpp) | checkpoint JSON 往返→宿主映射→ACL 检索→Layer 0 选择→provider 检查实际请求 | 事件和索引为可重建内存投影，不是生产审计存储 |
| [recovery_planner_host_consumer.cpp](../../examples/recovery_planner_host_consumer.cpp) | FileEventStore + SQLite cache 重开→RecoveryPlanner→新 Observe→保守宿主处置→更新 cache | 要求 ProcessCrash ack；本例只测试关闭/重开，未执行 OS 强杀或断电 |

## Workflow：结果和终态都被消费

宿主初始化外部四 worker Executor 和 MiraRuntime session，再装配 WorkflowRuntime。
async drive 上限为一，为 drive 等待 step future 留出执行资源。单个只读 BuiltIn
工具显式请求 simulator 的 screen，返回其原生屏宽；后续 Verify step 消费该
step_result，验证 fixture 的 240 像素宽度。不是依据模型 done 或输入 API 的成功
确认目标成立。

宿主检查 Completed、两个 step、一次读取、验证 satisfied 和零输入，再通过公开
parse_workflow_run_settled 验证 run 身份/终态与事件一致；终态 run 不能重新 start。
顺序为 WorkflowRuntime shutdown→MiraRuntime request/finish_shutdown→外部 Executor
shutdown。该例只有只读工具，不能外推为可写 Workflow 已接 durable journal。

## Context：边界由宿主显式传递

TaskGoalSet/TaskConstraintAdded 经 CheckpointBuilder 和 JSON round-trip 得到任务
框架。宿主将这些受信 frame 字段显式映射成 P0/P1 条目，不将 checkpoint 的任意
字符串整体提升为 policy。检索索引同时放入两个 session 的相同文本，带 session
grant 的 query 只能召回本会话候选。

召回的是会话片段，映射为 HistoricalPayload/UntrustedExternalData 并保留来源，
不是将它强转为 VerifiedFact 或长期 MemoryRecord。它以较高优先级参与装配，另有
4,000 字节 disposable 历史用于证明预算裁剪。ContextItemKind::RetrievedMemory
有自己的 authority 契约，不能为通过检查而乱套类型。

StandardContextManager 在 512 window、32 output、32 safety 的界限下选择输入，
报告裁剪且总估算不超过可用预算；同输入 selection_digest 稳定。跨 epoch 和最小
必要集溢出在 provider 前拒绝。宿主把 prepared input/tools/profile/epoch/digest
复制给 ModelRequest；provider fixture 实际确认目标与检索文本已收到，并检查检索
片段仍为 User/Public，不能成为 System/Developer policy。

本例显式允许 unknown price 并断言 UnknownPrice，不将它标为免费；价格装配另见
[价格宿主](model-price-host.md)。不调用 AgentLoop/ConversationLoop，也不自动
安装 IContextManager；M25 Working Context seam 和 M30 自动候选/跨会话选择不变。
生产宿主还须在提交前重验实时 epoch/authority，样板只验证固定 frame。

## Recovery：规划输出控制下一步行为

每个场景先写 ProcessCrash 日志和基线 SQLite checkpoint；journal 增量发生在基线
之后。两个 store 关闭并重开，Planner 使用耐久序号重建，而不是只读取旧 cache。

| 场景 | 规划结果 | 宿主实际消费 |
| --- | --- | --- |
| durable start 无 receipt | ResumeVerifying，verification_pending | 新只读 Observe；等待外部验证，不重发 |
| 显式 ActionExecutionUncertain | ResumeObserving，保留副作用 pin | 新只读 Observe；证据不足仍保留 pin |
| simulator 动作完成但无 Verify | ResumeVerifying | 之前的 back 确经 Dispatcher 执行一次，恢复观察不会增加输入次数 |
| 取消终态并仍有历史 pin | AlreadyTerminal | 不 observe、不执行输入、不复活 task |
| 无任务状态 | NoState | 不 observe、不执行输入 |
| 无未决动作的任务框架 | ResumeObserving | 新 Observe 后交回新规划入口，本例不自动派发 |

观察到截图只证明有视觉输入，不足以判断先前 back 的目标是否成立。未证实场景
返回宿主 AwaitingExternalVerification 处置，不发 VerificationResult；当前 reducer
会用该事件清除 pending/pin，因此不能发一个 not_evaluable 事件冒充验证闭环。
新的 ObservationRecorded 使用实际 simulator 元数据摘要；再规划并保存 cache，
检查 observation 水位已推进、未决状态保持、projection digest 落库且输入次数不增。

这里没有还原一个运行中的 MiraRuntime task，也没有把旧 operation 当成新授权。
真实宿主须将该处置接到自己的权限/接管/验证 owner。依赖的是 03 已有 Linux
journal/恢复证据，02/03 的真机、强杀/断电门禁继续开放；BUG-20261106-001 的
信封时间问题仍由 12 跟踪，本例不据此时间推导 TTL 或原始发生顺序。

## 所有权和限制

有限工作走 kairo 并消费 future；同步装配/结果检查由主线程 owner 负责。SQLite
专用 worker 由 store 管理，主线程先 close store，再 shutdown 外部 Executor。
Recovery 临时文件在 worker 收尾之后移除。没有新线程池、私有 timer、模型网络或
真实设备副作用；不从 fixture 成功推导跨平台、实时性、语义质量或崩溃耐久保证。
