# DEC-085：Runtime 调度度量的宿主消费边界

> 状态：Accepted
> 日期：2026-11-06
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-08

## 决定与依据

保留 MiraRuntime::scheduling_metrics 的原始镜像语义，不把宿主命令计数混进公开
字段。kairo SchedulingMetrics 统计路由决策，不是任务数、队列深度或命令结果数；
Runtime 使用 submit_on_with_handle 的串行控制路径，不经过 submit_auto 路由。
因此该路径发生容量拒绝、停止准入拒绝或业务回调异常时，routing counter 仍可能
为零。零值不能证明无过载/异常，也不是 Executor 缺口。

新增只使用公开 Runtime/CommandHandle API 的离线诊断宿主：分别展示原始 routing
累计值、宿主提交/拒绝累计值和本宿主持有 handle 的当前 pending/settled/failed 数。
后两组属于样板局部口径，不扩展公共 API、不改变 Core 计数或 M31 的容量算法。
内部 finalizer 的关闭补交不在宿主显式提交数里，不宣称该局部计数覆盖全局命令。

采样在独立、宿主拥有的 kairo Executor 有限任务中执行，消费 future；仅调用
Runtime 的无记录锁读和 bounded handle 的 outcome(0ms)。DeadlineExceeded 在这里
表示该 handle 尚未结算，不能误计为任务执行超时，也不会取消命令。各字段读取是
弱一致观测，不是一个事务快照或同步栅栏；计数分类可重叠，不相加推导任务总量。

宿主单一 owner 保留至多 16 个 handle 和有界阶段样本，样本独立于 Runtime 生命周期
存活；输出在控制面收尾后消费，不把日志或 Observer callback 安装到控制回调。
采样 Executor 与 Runtime 内部 Executor 分别标识，不把采样任务产生的路由接受数
冒充 Runtime 的非零度量。Runtime 最终关闭后仍可采样；对象销毁前必须结束采样任务。

## 验证与非目标

以有界 simulator interrupt gate 固定住一个控制回调，在 max_in_flight=4 时填满
四个 command，验证第五个容量拒绝、关闭命令容量拒绝、Stopping 中的新命令拒绝。
等待超时不终止原命令；释放 gate 后 finalizer 有界补交，pending 回落至零，任务
取消并干净关闭。另以独立 Runtime 的 interrupt 异常证明 Failed outcome 和
clean=false ShutdownReport 可见，不能凭 routing 计数为零吞掉异常。

覆盖正常、暂停/接管/恢复、过载、等待超时、取消收敛、异常和关闭后的采样。新增
CTest consumer，沿用 Runtime 生命周期契约与 sanitizer。没有新 worker、周期器、
私有调度器、线程或 Executor 能力替代；不对真实设备、强制中断或实时采样作保证。

关联：[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)、
[Runtime API](../api/core-runtime.md)、
[核心设计](../design/core_contracts_and_state_machine.md#11-可观测性)。
