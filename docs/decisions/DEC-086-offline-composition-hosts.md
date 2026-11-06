# DEC-086：三个离线组合宿主的交付边界

> 状态：Accepted
> 日期：2026-11-06
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-06

## 决定

分别提供三个可构建、进入 CTest consumer 的参考程序，只使用公开 API，不新增
公共接口、自动装配或产品默认值。它们验证输出被下一跳实际消费，不能只构造对象。

1. Workflow：MiraRuntime session→WorkflowRuntime create/start/wait→只读 BuiltIn
   屏宽工具→step_result 验证→终态事件 reader。外部 Executor 至少四个 worker，
   async drive 上限一，避免 drive 等待 step 时耗尽同一池。日志为进程内替身，不
   声明重启恢复；不展示尚未接 ActionJournal 的可写 Workflow 路径。
2. Context：事件投影 checkpoint→宿主显式映射 Goal/Constraint，带 session ACL 的
   InMemoryContextIndex 检索→UntrustedExternalData 候选→StandardContextManager
   有界选择→ModelGateway provider fixture 实际检查收到的输入。保留来源、epoch、
   selection digest 和预算结果；拒绝跨 epoch/最小集溢出时不调用 provider。M25
   Working Context seam、两个 Loop 的行为和 M30 自动选择不变。
3. Recovery：FileEventStore ProcessCrash ack 与 SQLite checkpoint→关闭并重开
   两个 store→RecoveryPlanner 增量重建→宿主选择新的 read-only Observe。对未决
   动作或未完成 Verify 只返回等待外部验证，保留 pin，不发成功 VerificationResult，
   不把截图存在视为目标成立，也不重发输入。AlreadyTerminal/NoState 不执行观察
   或动作。恢复宿主不伪造运行中的 Runtime task 或复用旧 operation 授权。

## 前置与证据范围

06 的恢复样板依赖 03 已完成的 Linux 通用 journal/身份/有界扫描、进程退出窗口
和保守重建证据（DEC-075/076），以这些契约形成离线消费链。03 的真实平台、OS
强杀/断电等整体门禁仍开放；提供离线样板不代表这些门禁完成，也不改变 02/03
的复选框或故障保证。06 的完成只表示三个声明边界内的离线组合已经验收。

BUG-20261106-001 仍由 12 跟踪。恢复样板只依赖耐久序号、身份与 pin，不从恢复
信封时间推导原始发生时间、TTL 或审计顺序。SQLite cache 不是第二事实源，使用
durable sequence 之后的 journal 增量验证其重建，不改写日志来掩盖不一致。

所有有限工作经外部 kairo 调度并消费 future；SQLite store 在主线程 close 后才
关闭 Executor。Workflow shutdown→MiraRuntime 收敛→外部 Executor shutdown，
停止生产者、等待必要任务的顺序可见。示例只选 read-only/fixture/simulator，不
访问网络、凭据或真实设备；不提供模型语义质量、实时性或跨平台保证。

关联：[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)、
[Workflow 设计](../design/workflow_runtime_design.md)、
[Context/Memory 设计](../design/context_and_memory_design.md)、
[恢复设计](../design/event_artifact_crash_consistency.md)。
