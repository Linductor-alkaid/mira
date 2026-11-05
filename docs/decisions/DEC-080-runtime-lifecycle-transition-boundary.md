# DEC-080：Runtime 生命周期写入与关闭准入边界

> 状态：Accepted
> 日期：2026-11-05
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-11（Runtime 状态写入治理）
> 替代/被替代：补充 DEC-001，不替代其 Executor 所有权约束

## 问题与依据

Runtime 的原子 store/CAS 未使用 `valid_runtime_transition`，未初始化对象直接
Constructed→Stopped，失败初始化则 Failed→Stopped。核心状态机设计已有
Failed→Stopping 清理路径，但公共校验表遗漏该边。初始化、关闭和命令登记之间
没有共同准入锁，存在关闭后重新发布 Running、关闭漏收已接纳 future 的窗口。
关闭命令被容量拒绝时，旧 finalizer 可跳过业务收敛直接宣称 Stopped；回调内部
调用 finalizer 还可能等待自身。

## 决策

- 私有 Runtime 状态单元仅提供读取及受公共转换表校验的 CAS，所有写入走同一入口。
  补齐 Failed→Stopping，与既有设计一致；Failed 表示业务失败终态，仍可做资源清理。
  不放宽 Stopping→Running 或 Stopped 的出边。
- 初始化、命令准入/返回 future 登记、停止准入共用短生命周期锁；最终 join 另由
  finalizer 锁串行化，等待时不持有准入锁。Task/Session 业务状态仍由 kairo 串行
  控制面提交，锁不建立新的任务队列、调度器或 Executor owner。
- 未初始化/初始化失败的实例不提交任务，避免触发 Executor 懒初始化。外部 finalizer
  经 Stopping→Quiesced→Stopped 收尾；初始化失败原因仍由 initialize 的 Result 返回。
- 正常关闭先停止业务准入并提交串行 shutdown 命令。若提交被拒绝，原错误立即返回；
  finalizer 消费所有已登记 submission future 后，至多补交一次 shutdown barrier。
  这是有明确诊断的有界收尾，不做无限或静默重试。kairo 的串行 future 在 admission
  释放之后就绪，故消费完既有 future 后可重新申请容量。
- shutdown 命令取消任务、逐会话释放输入；interrupt 拒绝或异常使命令 Failed，
  Runtime 保留 Failed。最终 Executor 关闭完成也不能将业务收敛失败伪报为 clean。
  失败、拒绝与 submission 异常通过 CommandHandle/ShutdownReport 显式可见。
- 消费晚到的 submission 异常时补齐未结算的 receipt/outcome，不丢弃 future 异常。
  正常业务失败已通过自己的 outcome 返回，不自动等同于 shutdown 失败。
- finalizer 必须由外部 owner 调用；用仅在控制回调动态范围有效的 thread_local 标记
  拒绝自身控制回调内的 finish_shutdown，不等待、不 teardown。重复完成返回原报告。
  该拒绝报告不扫描 pending_commands；外部完成后的报告才表示最终未决数量。

## 兼容性与非目标

公开方法签名和枚举值不变。Failed 的清理出边补齐公开校验器；未初始化关闭的错误
仍明确返回，finish_shutdown 可完成清理。关闭后 begin_operation 与新命令均拒绝。
环境 interrupt 失败现在可导致 clean=false，而非被静默吞掉。不承诺强制终止阻塞的
平台调用；Adapter 仍须提供有界、可解除阻塞且幂等的 interrupt。其他 Task/Session
状态写入、Observer 全面接线及外部平台验证不属于本次范围。

## 验证

固定 Runtime 转换表与非法值，验证正常/未初始化/配置失败/重复初始化/重复关闭；
阻塞一个串行回调制造容量拒绝，确认最终取消剩余任务且报告有补交诊断；验证
interrupt 失败/异常与回调自等待拒绝；使用外部 kairo 的有界任务交错初始化和
停止请求，所有 future 均消费，再由测试主线程最终关闭 Runtime。

关联：[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)、
[核心状态机](../design/core_contracts_and_state_machine.md)、
[Runtime API](../api/core-runtime.md)。
