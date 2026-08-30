# DEC-001：Runtime 的 Executor 所有权与串行控制面

> 状态：Accepted  
> 日期：2026-08-30  
> 负责人：Mira Maintainers  
> 冻结里程碑：M0  
> 替代/被替代：无

## 背景与问题

Mira 要同时处理多个 Task 的命令、模型/平台 completion、取消和终态。如果任意 worker 直接修改
Task 状态，将产生终态复活、错 epoch 提交和 shutdown 竞态。原总设计曾写 Coordinator“不占有
专用线程”，但 Executor 当前公开 `SerialExecutionContext` 明确使用一个内部串行线程，因此需要
冻结与真实依赖一致的所有权模型。

## 决策

1. 每个 `MiraRuntime` 拥有一个独立 `executor::Executor`，由外部 owner 显式初始化和最终关闭。
2. 每个 Runtime 首期拥有一个 `executor::SerialExecutionContext` 作为控制平面。所有会提交
   Runtime、Session 或 Task 业务状态的命令和 completion 均通过 `submit_on_with_handle()` 进入。
3. TaskCoordinator 是逻辑 actor，不拥有线程；多个 Task 共享 Runtime 串行上下文。模型、截图、
   I/O、持久化、推理和实时控制在 Executor 的其他路径执行，完成后只投递小型 completion。
4. 串行回调必须有界、不可阻塞，不得等待 future、调用 Provider、写磁盘、执行用户 Observer 或
   运行模型。它只做校验、状态提交、生成下一批 operation 和事件草案。
5. `ExecutionSupervisor` 保存所有 submission handle/future，并将异常、拒绝、取消和结果转换为
   completion；业务模块不缓存裸 Executor 指针。
6. 关闭顺序是：停止外部 producer；发出取消；停止 realtime/blocking/timer/store producer；将
   结算 completion 提交到控制面并进入 `Quiesced`；关闭串行上下文并消费对应 future；最后从非
   worker 线程关闭 Executor。`Stopped` 不依赖关闭后的 Executor 回调。

M0 验证发现当前 `submit_on_with_handle()` 在多 worker 下存在进展和同步对象生命周期问题，记录为
`EXE-20260830-002`、`EXE-20260830-003`。在上游修复前，决策第 2 条由单一 compatibility boundary
实现：先 reserve FIFO ticket，再由普通 Executor tracked task 非阻塞 `post_reserved()`，独立业务
promise 在串行 callback 内结算；两类 future 都必须消费，取消必须 abandon 未发布 ticket。该兼容
边界保持同一串行上下文和 Executor 所有权，不等同于允许自建 actor 线程或队列。

## 备选方案

- 每 Task 一个 `SerialExecutionContext`：线程数随 Task 增长，不接受。
- 用 mutex 允许任意 worker 修改状态：无法自然保证命令全序和可复现性，不接受。
- 自建 actor 线程或私有队列：违反 Executor 强制生命周期，不接受。
- 单纯使用 `MpscChannel`：它可以承载消息，但仍需要受 Executor 管理的唯一消费者；首期没有必要
  再建立平行调度器。

## 影响与风险

- 控制回调慢会阻塞全部 Session，因此必须监测执行时间并禁止外部工作。
- `submit_on` facade wrapper 在默认池等待串行回调完成；M0 必须在小线程池、突发 completion、排队
  取消和 shutdown 下验证容量。验证失败时先登记 Executor 反馈，不静默自建线程。
- 后续若单 Runtime 多 Session 的控制吞吐成为实测瓶颈，可通过新 DEC 按 Session 分片；同一
  Session/Task 的单写者和序号语义不得改变。

## 验证方式

- Executor 当前公开头、API 文档和 `test_serial_execution_context` 证明 FIFO、异常、排队取消和
  shutdown rejection 语义。
- M0 增加 Mira integration spike，覆盖 10,000 次混合命令/completion、终态幂等和关闭风暴。

## 关联文档和工作项

- [M0 Executor 工程基线](../plans/m0-engineering-baseline.md)：`M0-05` 至 `M0-09`
- [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)
- [Executor 反馈台账](../executor_feedback/ledger.md)
