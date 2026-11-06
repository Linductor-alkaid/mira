# Runtime 调度度量的离线宿主

> 状态：Active
> 版本：1.0
> 更新日期：2026-11-06
> 负责人：Mira Maintainers
> 依据：[DEC-085](../decisions/DEC-085-runtime-scheduling-diagnostic-host.md)、MNT-202611-08

## 构建与使用

[runtime_scheduling_host_consumer.cpp](../../examples/runtime_scheduling_host_consumer.cpp)
通过公开 MiraRuntime/CommandHandle 消费 routing 计数和命令结果，使用 simulator
与有界 interrupt gate 复现容量拒绝；另一独立实例注入 interrupt 异常。

```bash
cmake --preset debug
cmake --build build/debug --target mira_runtime_scheduling_host_consumer -j 4
ctest --test-dir build/debug -R '^mira_runtime_scheduling_host_consumer_test$' --output-on-failure
./build/debug/mira_runtime_scheduling_host_consumer
```

没有外部网络、凭据或平台输入。等待 gate 只用于稳定复现，不是生产 Adapter 的
阻塞策略，也不证明真实平台的中断或实时性能。

## 必须分开解释的观测面

| 输出 | 口径与用途 |
| --- | --- |
| runtime.routing.* | scheduling_metrics 全部 13 个公开字段的原样快照；Executor 构造以来的路由决策累计值，不是命令数或队列深度 |
| host.submitted | 本宿主显式调用控制命令入口的累计次数，含立即拒绝 |
| host.admitted_handles | 本宿主收到 handle 并保留的数量，不等于业务已生效 |
| host.submit_rejected | 入口 Result 立即失败；capacity/stopping 按本样板的 ErrorCode 分类 |
| host.pending | 已持有 handle 中 outcome(0ms) 尚未就绪的当前数量，不是 Executor 在途数 |
| host.settled / failed | 本样板保留窗口中已就绪/失败 outcome 数；生产窗口淘汰后不能当成全生命周期累计值 |
| sampler_executor.routing.accepted | 独立采样 Executor 的路由接受数；不能混入 Runtime 的计数 |

当前 Runtime 用 submit_on_with_handle 串行命令路径，不经过 submit_auto 路由。
因此样板中的 routing 字段在正常、过载、停止和异常阶段均为零，但提交容量拒绝、
InvalidState 和 Failed outcome 确实存在。路由计数零不证明无失败；accepted 也不
等于已完成任务数。按 reason/diagnostics 分类的计数可能交叉，一次提交也可能产生
多条决策，不能把各计数相加推导任务总量。

计数从各自 Executor 构造开始累计；重建 owner 后重置，不能跨实例直接相减。
当前字段是弱一致采样，不是事务快照，不能用于同步、授权或替代容量准入。
变化率须记录相同 owner 和两个采样时间；本例只展示固定阶段，不宣称速率或延迟分布。

## 可执行背压与失败矩阵

第一实例 max_in_flight=4：先正常打开两个会话、创建任务并完成接管/恢复，再阻塞
第一个会话的 close 回调，向串行控制面排入 pause→resume→pause。第五个命令及
shutdown 命令均容量拒绝；shutdown 即使拒绝也已经关闭准入，后续 pause 返回
InvalidState。该阶段 host.pending=4、capacity_rejected=2、stopping_rejected=1；
原始 routing 仍为零。1 ms outcome 等待返回 DeadlineExceeded，原回调继续等待，
并未被强杀。此处触发的是总在途容量上限，不把它描述为物理队列已满；一般宿主
也不能只凭 ResourceExhausted 就唯一区分 queue/OOM/提交异常等原因。

释放 gate 后由外部 owner 调用 finish_shutdown，至多一次关闭补交完成资源收敛。
最终本宿主 submitted=12、admitted_handles=9、settled=9、pending=0，任务 Cancelled，
ShutdownReport clean=true。内部 finalizer 的补交没有经过本宿主 record()，不计入
这里的显式提交数。两组数量并不矛盾，不能将局部 host 数当成 Runtime 全局审计。

第二实例 interrupt 抛异常：takeover 和 shutdown 的命令 outcome 都为 Failed，
ErrorCode::Internal 可见；最终 report clean=false/Failed，host.failed=2、pending=0。
此时 routing 仍为零。样板只输出固定错误类别和计数，不回显平台异常文本。

## 采样和诊断所有权

HostView 由单一外部 owner 维护，最多保留 16 个 handle；采样时复制该有界视图到
kairo submit_auto 任务，读取 Runtime routing/state 和 outcome(0ms)，消费 future
后才继续改变本地视图。即使控制回调持有 Runtime 记录锁，采样也不访问这些记录锁。
pending 的零等待轮询不计作执行超时，显式等待超时则单独检查返回值。

样板只在固定阶段采样，数量固定；不启用每任务高频事件、私有定时器或新调度队列。
不可变样本在控制面收尾后输出，控制回调中不运行 stdout/日志/Observer 消费者。
生产宿主若扩展为周期采样须使用 kairo timer，设置有界传递、丢弃/合并与诊断输出
失败策略；本例不提供该周期服务或完整遥测后端。

Runtime 对象可在初始化前/最终关闭后采样，必须比采样 future 活得更久；最终销毁
前结束采样生产者并消费在途 future。外部 sampler 最后由主线程 shutdown(true)。
本例没有改变 Runtime 的 Executor 所有权、M31 配额/容量策略或任何公共字段布局。
