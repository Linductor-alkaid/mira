# Journaled Input Dispatch 设计

> 状态：Active
> 版本：1.0
> 更新日期：2026-11-05
> 负责人：Mira Maintainers
> 依据：[DEC-075](../decisions/DEC-075-agent-loop-journaled-input-dispatch.md)

## 1. 目标与模块

将 AgentLoop 编译后输入接入 DEC-003 副作用日志。公开头
`mira/action_dispatcher.hpp` 属 Core，依赖公共 environment/action_journal/event_store，
实现位于 `src/model/action_dispatcher.cpp`；不依赖 Adapter、Workflow 或 SQLite。
模型原文不能越过 compile/validate 边界。工具副作用和连续控制的完整覆盖仍属后续范围。

## 2. 接口与所有权

宿主持有外部 Executor、Environment、EventStore 与资源仲裁；Dispatcher shared ownership
延长 store 生命周期。每次派发的 ActionJournal 只在同步调用栈中保存一个动作，
结束后释放本地状态。Loop 持有 Dispatcher。
ActionLeaseAcquirer 接收带 ActionId、任务/环境 epoch 与输入摘要的 ActionIntent，返回
unique_ptr lease；lease 存活覆盖 start、最终校验、真实调用与结果提交，析构释放资源。
setters 必须在 run 之前完成，不允许执行中重新装配。

宿主需要将自身资源仲裁封装为 IActionDispatchLease，validate 检查任务仍可行动、
epoch 一致、许可未撤回。资源撤回同时取消 context 并 interrupt Environment，
Adapter 最终输入仍检查取消；不在 Runtime records_mutex 内持锁等待平台 I/O。
测试的 ScriptedActionLease 为无真实授权含义的替身，不是生产样板。

## 3. 时序、恢复和事件

prepare → acquire → start(critical, ProcessCrash) → validate → execute →
receipt/uncertain → release → Observe/Verify。输入摘要包含 display、完整事件顺序与
duration，只持久化 digest。目标字段固定安全词汇，结果摘要固定，不记录 type 文本。

任何 gate 失败都返回 Result 错误。start 写入后不知道平台是否执行时恢复先 Verify；
execute 异常/错误同样 uncertain，不重试。receipt 写入失败仍保留 start 并尝试补 uncertain。
未知/仍在派发及执行异常/错误还会在释放 lease 前调用 interrupt 收敛输入，失败显式返回。
Unknown/Dispatched/side_effect flag 一律 uncertain；Loop Verify 未证实即结束本轮。
终态幂等仍由 Runtime/Checkpoint/RecoveryPlanner 保证，不在派发器重开任务。

新派发路径只由 journal 的 start/receipt 表达动作，避免重复计入 recent_actions。旧 ActionDispatched 的 reducer 读回保持兼容。JSON 与 pipe 的旧 reducer 输入仍
保持有效。普通 Loop 诊断可能丢失时 journal 的 start 仍能令恢复 verification_pending。
Reducer 对 Loop 的 task_id/task_epoch/detail envelope 校验身份与 epoch 后展开，
同时接受旧 flat JSON。未知副作用且目标未证实时不发清除 pin 的 VerificationResult。
保留/删除与 Journal 内存上限由 M31 承接；本设计不放宽未决动作保护。

## 4. 并发、取消与关闭

派发函数为同步协议，不产生任务、timer 或 worker。宿主把 Loop 放在 kairo 有限任务中，
持有并消费 future，阻塞 I/O 由 Adapter 的 Executor 管理能力承载。
取消与 deadline 在 prepare 前、lease 后、durable start 后、最终执行前检查；
执行中由宿主取消桥与 interrupt 解阻塞。deadline 不是强制终止。

关闭顺序沿用外部 owner：停止生产、取消/interrupt、回收控制与 I/O 路径、等待有限
任务，最后非 worker shutdown(true)。lease 和 Dispatcher 不隐藏 Executor 生命周期。

## 5. 验证与限制

验证以输入次数、事件顺序、租约释放和 RecoveryPlanner 结果为依据，覆盖 DEC-075 的
故障矩阵。文件 reopen 只证明耐久读回，强杀/断电、Windows/Android 与真实 Adapter
Takeover 要由目标环境补跑，不从 fixture 推导平台保证。

2026-11-05 第三轮：通用 journal 身份/pipe 校验与恢复分页由
[DEC-076](../decisions/DEC-076-journal-identity-and-bounded-recovery.md) 补齐：缓存完整
intent 绑定、journal/checkpoint 共用严格 pipe 解析器、按所有会话事件计量的有界扫描、
分页进展与身份校验、坏回执/存储异常显式失败。合法旧八字段保留，尾部含 `|` 不再截断。
RecoveryPlanner 也修正了只统计目标任务与末页越界的漏洞。

已知后续：工具派发共用日志边界、
Controller watchdog、目标平台与所有 crash 窗口、容量/保留。记录于
[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)，本批不关闭其余范围。
