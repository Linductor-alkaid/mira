# Executor 能力反馈台账

本台账记录 Mira 集成 `third_party/executor` 时确认的通用能力缺口。它不是普通 bug 列表，
也不用于记录应用层、模型供应商或平台 Adapter 的需求。

## 状态定义

- `Open`：缺口已确认，尚无上游结论。
- `Proposed`：已有明确的 Executor 改进方案或上游提案。
- `Accepted`：上游已接受，等待可用版本。
- `Resolved`：依赖版本已提供能力且 Mira 已完成迁移。
- `Rejected`：确认不属于 Executor 职责；记录结论及 Mira 的归属层。

## 台账索引

| ID | 日期 | 状态 | 能力摘要 | Mira 影响 | 临时方案 | 上游引用 |
| --- | --- | --- | --- | --- | --- | --- |
| `EXE-20260830-001` | 2026-08-30 | Accepted | 默认异步提交缺少总量有界 admission | M0 控制命令无法由线程池容量得到拒绝语义 | Mira Runtime 边界使用有界在途 admission | [executor#179](https://github.com/Linductor-alkaid/executor/issues/179)（master `def5200`） |
| `EXE-20260830-002` | 2026-08-30 | Accepted | 多 worker 串行 facade wrapper 可互相饥饿 | M0 突发 completion 无法结算 | 非阻塞 tracked dispatch | [executor#178](https://github.com/Linductor-alkaid/executor/issues/178)（master `def5200`） |
| `EXE-20260830-003` | 2026-08-30 | Accepted | 串行 facade wrapper 的栈同步对象存在竞争 | TSAN 报告 callback notify 与析构竞争 | 非阻塞 tracked dispatch + 独立 business promise | [executor#178](https://github.com/Linductor-alkaid/executor/issues/178)（master `def5200`） |

新增第一条记录时删除“当前暂无记录”占位行。编号格式为 `EXE-YYYYMMDD-NNN`，其中序号按
当天记录递增。

## 记录模板

复制本节并放在文档末尾，同时在“台账索引”增加对应行。不要覆盖本模板。

### EXE-YYYYMMDD-NNN：能力摘要

- 状态：`Open`
- 发现日期：`YYYY-MM-DD`
- 记录人/责任人：
- 影响组件：
- Executor 版本或提交：
- 关联代码/测试：
- 上游 issue/PR：

#### 使用场景

说明 Mira 的真实工作负载、生命周期、线程模型、平台和频率/延迟/容量约束。

#### 已核查证据

列出核查过的公开头文件、API 文档、集成指南、测试，以及最小复现或失败输出。说明为什么
现有 API 无法满足需求，而不是仅陈述不存在某个函数名。

#### 期望能力与语义

描述 Executor 应提供的最小通用能力，包括所有权、完成、取消、超时、背压、错误和 shutdown
语义。避免加入仅属于 Mira 的业务策略。

#### 影响与风险

说明该缺口对正确性、性能、可移植性、可观测性或交付的具体影响。

#### 临时方案

记录临时适配的位置、行为差异、已知风险、测试覆盖和禁用/回滚方式。若没有安全的临时方案，
明确写“无”。

#### 上游验收标准

列出可由测试验证的完成条件，并包含正常、拒绝/过载、取消、异常、超时和 shutdown 中适用的
场景。

#### 跟进记录

| 日期 | 状态变化 | 说明 | 证据/链接 |
| --- | --- | --- | --- |
| YYYY-MM-DD | Open | 首次登记 | - |

### EXE-20260830-001：默认异步提交缺少总量有界 admission

- 状态：`Accepted`
- 发现日期：`2026-08-30`
- 记录人/责任人：Mira Maintainers
- 影响组件：Mira Runtime 控制命令 admission、`ExecutionSupervisor`
- Executor 版本或提交：`2af11a3466dd4a97a31d8784d01a892876aeeb1a`
- 关联代码/测试：`src/runtime/runtime_baseline.cpp`、`tests/integration/executor_lifecycle_test.cpp`
- 上游 issue/PR：[executor#179](https://github.com/Linductor-alkaid/executor/issues/179)（实现合入 executor master：`13214c0` + `def5200`）

#### 使用场景

Mira 每个 Runtime 使用一个小型默认线程池和一个 `SerialExecutionContext`。公共控制命令与
operation completion 必须在容量耗尽时立即形成结构化拒绝，不能进入无界队列，也不能静默丢失。
串行回调保持短小且不阻塞；压力场景会突发提交数千条 completion。

#### 已核查证据

已核查 `include/executor/config.hpp`、`include/executor/executor.hpp`、`docs/API.md`、Executor
integration skill 的 scheduling/requirement 路由，以及 `ThreadPool::try_submit()`、
`TaskDispatcher::dispatch()` 和相关线程池测试。`ExecutorConfig::queue_capacity` 只构造每个 worker
的本地有界队列；当本地队列满时，dispatcher 将任务重新放回 `PriorityScheduler`。该 scheduler
使用无容量上限的 `std::vector` 队列，因此 `submit_on_with_handle()` 不会因配置的 queue capacity
耗尽而拒绝。取消 registry 容量可拒绝 tracked submission，但它同时承担取消状态保留，不是一般
异步队列 admission 契约，也不能表达普通 `submit_auto()` 的总量上限。

最小复现由 M0 测试配置单 worker、小 `queue_capacity`，持续提交工作并检查 Executor 仍接纳超过
该容量的 tracked tasks；Mira 自身的 `max_in_flight` 达到上限时则返回 `Rejected`。

#### 期望能力与语义

Executor 默认异步 facade 应可配置一个跨 scheduler 与 worker 本地队列的总在途/待执行容量，或
提供明确的 `try_submit*` admission result。达到上限时调用方应立即获得可区分的 capacity rejection，
future 必须就绪，failure event/计数同步增加；正常完成、异常、排队取消、超时和 shutdown 都应只
释放一次容量。容量不得通过无界 fallback 被绕过。

#### 影响与风险

若 Mira 直接把 `queue_capacity` 当作背压边界，突发 completion 可导致无界内存增长，并让公开命令
无法按契约报告 `ResourceExhausted`。这影响过载正确性和 shutdown 时间上界，但不影响 Executor
当前任务不丢失的设计目标。

#### 临时方案

临时方案仅位于 `RuntimeBaseline`/未来 `ExecutionSupervisor` admission boundary：提交前以原子
在途计数执行有界接纳，拒绝被转换为 Mira 结构化结果；已接纳任务仍全部通过 Executor
`submit_on_with_handle()` 执行并保存 future/handle。计数不创建线程、队列或调度器。行为差异是
容量按 Mira command lifecycle 而非 Executor 内部排队位置计算。上游提供总量 admission 后移除该
计数器，回归正常、拒绝、取消、异常、超时和 shutdown 测试。

#### 上游验收标准

- 单 worker、总容量 N 时，第 N+1 个尚未结算的 submit 明确拒绝且 future 就绪。
- 完成、任务异常、排队取消、执行前超时各释放一次容量。
- shutdown 与并发 submit 不越过容量，也不留下未就绪 future。
- failure/status 能区分 capacity rejection 与 stopping、invalid input。
- `submit_on_with_handle()` 的 context ticket 在 rejection 时被释放，后续 FIFO 不阻塞。

#### 跟进记录

| 日期 | 状态变化 | 说明 | 证据/链接 |
| --- | --- | --- | --- |
| 2026-08-30 | Open | M0 集成核查确认，采用单一 Runtime admission boundary | `tests/integration/executor_lifecycle_test.cpp` |
| 2026-08-30 | Open → Proposed | 上游提案：`ExecutorConfig::max_in_flight_tasks`（facade 总量 admission、`CapacityExhaustedException` 拒绝、恰好一次释放），设计稿 `docs/design/bounded_admission.md` 已随实现合入 executor master | [executor#179](https://github.com/Linductor-alkaid/executor/issues/179) |
| 2026-08-30 | Proposed → Accepted | 实现与文档已合入 executor master（`13214c0` 实现+测试，`def5200` 文档+网站；135 项 CTest 全绿、TSAN 0 报告）。等待可用版本后 Mira 移除 Runtime 在途计数临时方案 | [executor#179](https://github.com/Linductor-alkaid/executor/issues/179)、executor master `def5200` |

### EXE-20260830-002：多 worker 串行 facade wrapper 可互相饥饿

- 状态：`Accepted`
- 发现日期：`2026-08-30`
- 记录人/责任人：Mira Maintainers
- 影响组件：Mira Runtime 串行控制面、`SerialExecutionContext`
- Executor 版本或提交：`2af11a3466dd4a97a31d8784d01a892876aeeb1a`
- 关联代码/测试：`src/runtime/runtime_baseline.cpp`、`tests/stress/runtime_ordering_stress_test.cpp`
- 上游 issue/PR：[executor#178](https://github.com/Linductor-alkaid/executor/issues/178)（实现合入 executor master：`13214c0` + `def5200`）

#### 使用场景

Mira 按 DEC-001 将控制命令和 operation completion 通过 `submit_on_with_handle()` 投递到每 Runtime
唯一的 `SerialExecutionContext`。控制回调有界且不阻塞。压力验收从一个 producer 连续投递
10,000 条命令/completion，并消费所有 future；默认 facade 池原计划使用两个小型 worker。

#### 已核查证据

已核查 `include/executor/serial_execution_context.hpp`、`Executor::submit_on_with_handle()` 模板、
`docs/API.md`、integration skill 和 `test_serial_execution_context`。每个 facade wrapper 取得 ticket
后由任意 worker 执行；wrapper 将回调发布到串行线程后，同步等待回调完成。串行上下文只会按 ticket
顺序释放回调。当两个 worker 恰好执行较晚 ticket 的 wrapper、而更早 ticket wrapper 仍在默认池
队列时，两个 worker 都等待串行线程，串行线程等待早期 ticket，系统无可运行 worker。

Mira Debug 最小复现使用两个 worker、10,000 次快速回调：首个未就绪 future 在 30 秒超时，进程
退出时 Executor 内部等待约 30 秒，总计约 60.48 秒失败；同一测试只把 worker 数改为 1 后在约
0.51 秒通过，顺序为 1..10,000 且全部 future 被消费。回调本身不执行 I/O 或等待。

#### 期望能力与语义

`submit_on_with_handle()` 不应让等待串行 callback 的 facade wrapper 占满默认池并阻止较早 ticket
发布。可接受的通用语义包括非阻塞 continuation、由串行上下文直接结算 promise，或能保证早期
ticket wrapper 先执行的调度。仍必须保留 FIFO、排队取消、异常传播、context shutdown 拒绝和
future 完成语义。

#### 影响与风险

该问题会让完全有界的短控制回调在小型多 worker 池中永久不前进，阻塞命令结算和 Runtime
shutdown。增加 worker 只能降低复现概率，不能证明安全；Mira 不能以扩容掩盖该正确性问题。

#### 临时方案

`RuntimeBaseline` 使用与 `EXE-20260830-003` 相同的非阻塞 tracked dispatch：Executor worker 只发布
已预留 ticket，不等待串行 callback，因此小型多 worker 池不会被等待 wrapper 占满。串行上下文
仍由 Executor 公开类型拥有独立线程，所有 dispatch、取消和 teardown 仍受 Executor 管理。M1
开始实现生产 Runtime 前应优先使用上游修复后的直接 facade；不得引入第二个 Executor 或自建
线程池绕行。

#### 上游验收标准

- 两个 worker 下突发 10,000 个 `submit_on_with_handle()` 均在有界时间内按 ticket FIFO 结算。
- 1..N worker 下 later wrapper 先启动不能阻止 earlier ticket 取得执行机会。
- 排队取消会释放 ticket，异常由对应 future 重抛。
- context 并发 shutdown 使未发布 wrapper 以 `ExecutorStopping` 结算。
- Executor shutdown 不留下等待 wrapper 或未就绪 future。

#### 跟进记录

| 日期 | 状态变化 | 说明 | 证据/链接 |
| --- | --- | --- | --- |
| 2026-08-30 | Open | M0 两 worker 压测复现，改用非阻塞 tracked dispatch compatibility boundary | `tests/stress/runtime_ordering_stress_test.cpp` |
| 2026-08-30 | Open → Proposed | 上游重构为派发/结算分离（非阻塞发布 + 串行线程直接结算）；两 worker × 10,000 突发在压测中约 1s 全部 FIFO 结算，实现已随 executor master 合入 | [executor#178](https://github.com/Linductor-alkaid/executor/issues/178) |
| 2026-08-30 | Proposed → Accepted | 实现与文档已合入 executor master（`13214c0` 实现+测试，`def5200` 文档+网站；两 worker × 10,000 突发约 1s、1..4 worker 扫描、TSAN 0 报告）。等待可用版本后 Mira 恢复直接 `submit_on_with_handle()` | [executor#178](https://github.com/Linductor-alkaid/executor/issues/178)、executor master `def5200` |

### EXE-20260830-003：串行 facade wrapper 的栈同步对象存在竞争

- 状态：`Accepted`
- 发现日期：`2026-08-30`
- 记录人/责任人：Mira Maintainers
- 影响组件：Mira Runtime 串行控制面、`submit_on_with_handle()`
- Executor 版本或提交：`2af11a3466dd4a97a31d8784d01a892876aeeb1a`
- 关联代码/测试：`src/runtime/runtime_baseline.cpp`、`tests/stress/runtime_ordering_stress_test.cpp`
- 上游 issue/PR：[executor#178](https://github.com/Linductor-alkaid/executor/issues/178)（实现合入 executor master：`13214c0` + `def5200`）

#### 使用场景

同 `EXE-20260830-002`：Mira 将快速、无阻塞的控制回调投递到一个
`SerialExecutionContext`，保存并消费所有 future。M0 使用 ThreadSanitizer（TSAN）验证 callback
完成、wrapper 返回与 teardown 的跨线程同步。

#### 已核查证据

已核查 `Executor::submit_on_with_handle()` 公开模板实现。wrapper 在自己的栈上创建 `mutex`、
`condition_variable` 和 `finished`，把这些对象以引用捕获进串行 callback。callback 设置
`finished=true` 后调用 `cv.notify_one()`；wrapper 的 `cv.wait()` 观察到谓词后即可返回并析构栈对象。
通知调用本身不在 mutex 临界区内，因此 wrapper 可在串行线程仍执行 `notify_one()` 时销毁
`condition_variable`。

GCC 13.3 TSAN 在 `tests/benchmark/control_plane_benchmark.cpp` 的 1,000 次路径中报告：worker 线程
于 `executor.hpp:1095` 执行 `pthread_cond_destroy`，与串行线程于 `executor.hpp:1087` 执行
`pthread_cond_signal` 发生 data race；位置是 worker 栈。随后本机其他 TSAN 进程因 Linux
`7.0.0-30-generic`/ASLR 组合出现 `unexpected memory mapping`，这属于额外的环境限制，不使已捕获
竞争失效。

#### 期望能力与语义

串行 facade 应以拥有稳定生命周期的共享状态结算 callback，且 wrapper 返回前与 callback 的最后
一次同步对象访问建立 happens-before。修复不能丢失 FIFO、异常、取消、context shutdown 和
future ready 语义，也不能要求应用延长栈对象生命周期。

#### 影响与风险

竞争属于未定义行为，可能表现为 TSAN 失败、条件变量生命周期破坏或偶发崩溃。即使单 worker 能
回避 `EXE-20260830-002` 的饥饿，也不能消除此竞争。

#### 临时方案

`RuntimeBaseline` 不调用活动状态下的 `submit_on_with_handle()`。它先从公开
`SerialExecutionContext::reserve()` 取得 FIFO ticket，再用 Executor `submit_with_handle()` 执行一个
有界、非阻塞的 `post_reserved()`；串行 callback 通过 `shared_ptr<promise<BaselineResult>>` 结算
业务结果。Mira 同时保存/消费 dispatch future 与业务 future；排队取消时显式 `abandon(ticket)`。
该方案不创建线程、队列或调度器，所有 dispatch 仍由 Executor 管理。上游修复后移除兼容层，恢复
直接 `submit_on_with_handle()`。

#### 上游验收标准

- TSAN 下重复运行至少 10,000 次串行提交，无 condition-variable lifetime race。
- callback 正常、抛异常、排队取消与 context shutdown 均使 future 恰好结算一次。
- wrapper 先返回、callback 尾部仍运行的调度交错不得访问已析构同步对象。
- 修复同时满足 `EXE-20260830-002` 的多 worker 进展要求。

#### 跟进记录

| 日期 | 状态变化 | 说明 | 证据/链接 |
| --- | --- | --- | --- |
| 2026-08-30 | Open | M0 TSAN 捕获栈条件变量竞争，切换到非阻塞 tracked dispatch boundary | `src/runtime/runtime_baseline.cpp` |
| 2026-08-30 | Open → Proposed | 上游重构消除全部栈同步对象（共享状态 + 串行线程结算）；gcc-13 TSAN 下 4 轮 3 万+ 次串行提交 0 报告，实现已随 executor master 合入 | [executor#178](https://github.com/Linductor-alkaid/executor/issues/178) |
| 2026-08-30 | Proposed → Accepted | 实现与文档已合入 executor master（`13214c0` 实现+测试，`def5200` 文档+网站；TSAN 0 报告，condition-variable lifetime race 按构造消除）。等待可用版本后 Mira 移除非阻塞 tracked dispatch 临时方案 | [executor#178](https://github.com/Linductor-alkaid/executor/issues/178)、executor master `def5200` |
