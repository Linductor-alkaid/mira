# Executor 能力反馈台账

本台账记录 Mira 集成随附并发/生命周期库时确认的通用能力缺口。它不是普通 bug 列表，
也不用于记录应用层、模型供应商或平台 Adapter 的需求。

> 2026-10-05 起随附库为 `third_party/kairo`（上游项目 v0.6.0 起由 Executor 更名 kairo，
> 命名空间 `kairo::`）。目录与条目编号沿用历史命名；此后新条目仍登记于本文件。

## 状态定义

- `Open`：缺口已确认，尚无上游结论。
- `Proposed`：已有明确的 Executor 改进方案或上游提案。
- `Accepted`：上游已接受，等待可用版本。
- `Resolved`：依赖版本已提供能力且 Mira 已完成迁移。
- `Rejected`：确认不属于 Executor 职责；记录结论及 Mira 的归属层。

## 台账索引

| ID | 日期 | 状态 | 能力摘要 | Mira 影响 | 临时方案 | 上游引用 |
| --- | --- | --- | --- | --- | --- | --- |
| `EXE-20260830-001` | 2026-08-30 | Resolved | 默认异步提交缺少总量有界 admission | 已迁移到 `max_in_flight_tasks` | 已删除 Mira 在途计数，使用 Executor 原生 admission | [executor#179](https://github.com/Linductor-alkaid/executor/issues/179)（`4fd8e60`） |
| `EXE-20260830-002` | 2026-08-30 | Resolved | 多 worker 串行 facade wrapper 可互相饥饿 | 已迁移到直接 `submit_on_with_handle()` | 已删除非阻塞 tracked dispatch 兼容层 | [executor#178](https://github.com/Linductor-alkaid/executor/issues/178)（`4fd8e60`） |
| `EXE-20260830-003` | 2026-08-30 | Resolved | 串行 facade wrapper 的栈同步对象存在竞争 | 已迁移到上游共享状态结算 | 已删除 `post_reserved()` 与独立 business promise 兼容层 | [executor#178](https://github.com/Linductor-alkaid/executor/issues/178)（`4fd8e60`） |
| `EXE-20260922-001` | 2026-09-22 | Resolved | `blocking_io_executor.cpp` 的 `native_handle` static_cast 阻塞 MinGW-w64 posix 构建 | 已随 pin 升级（`ef821dc`，上游 [kairo#215](https://github.com/Linductor-alkaid/kairo/pull/215)）恢复 MinGW 全树可构建 | 已删除（pin 升级后无需子集回避） | [mira#75](https://github.com/Linductor-alkaid/mira/issues/75)、[kairo#215](https://github.com/Linductor-alkaid/kairo/pull/215)（`99a55a0`） |
| `EXE-20261028-001` | 2026-10-28 | Open | 总量有界 admission 缺少按主体分桶的份额准入语义 | 阻塞 DEC-069 的分主体份额准入；Mira 侧只能用二段式预过滤近似 | 无安全临时方案完全等价者；M31 期间用 Mira 侧分桶账本做嵌套预过滤（见记录正文「临时方案」） | [mira#111](https://github.com/Linductor-alkaid/mira/issues/111) |

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

- 状态：`Resolved`
- 发现日期：`2026-08-30`
- 记录人/责任人：Mira Maintainers
- 影响组件：Mira Runtime 控制命令 admission、`ExecutionSupervisor`
- Executor 版本或提交：原始复现 `2af11a3466dd4a97a31d8784d01a892876aeeb1a`；已迁移至 `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`
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
| 2026-08-30 | Accepted → Resolved | Mira 固定 Executor `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`，`MiraRuntime`/`RuntimeBaseline` 配置 `max_in_flight_tasks`，删除应用侧 admission counter；Mira Debug 构建与 CTest 12/12 通过，直接 Executor admission 回归通过 | `cmake --preset debug && cmake --build --preset debug -j2 && ctest --preset debug --output-on-failure`；`third_party/executor` `test_bounded_admission` |

### EXE-20260830-002：多 worker 串行 facade wrapper 可互相饥饿

- 状态：`Resolved`
- 发现日期：`2026-08-30`
- 记录人/责任人：Mira Maintainers
- 影响组件：Mira Runtime 串行控制面、`SerialExecutionContext`
- Executor 版本或提交：原始复现 `2af11a3466dd4a97a31d8784d01a892876aeeb1a`；已迁移至 `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`
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
| 2026-08-30 | Accepted → Resolved | Mira 控制面和基线原型均改用直接 `submit_on_with_handle()`；删除 `reserve`/`post_reserved` tracked compatibility boundary。Mira 10,000 条混合命令压力测试通过 | `tests/stress/runtime_ordering_stress_test.cpp`；`mira_stress_test` |

### EXE-20260830-003：串行 facade wrapper 的栈同步对象存在竞争

- 状态：`Resolved`
- 发现日期：`2026-08-30`
- 记录人/责任人：Mira Maintainers
- 影响组件：Mira Runtime 串行控制面、`submit_on_with_handle()`
- Executor 版本或提交：原始复现 `2af11a3466dd4a97a31d8784d01a892876aeeb1a`；已迁移至 `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`
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
| 2026-08-30 | Accepted → Resolved | Mira 已不再创建 facade 外部同步对象或独立业务 promise；直接 facade future 由 Runtime 保存并消费。上游 serial stress 与 Mira Debug/安装/平台边界回归均通过 | `test_serial_context_stress`；`ctest --preset debug --output-on-failure` |

### EXE-20260922-001：blocking_io_executor 的 native_handle static_cast 阻塞 MinGW-w64 posix 构建

- 状态：`Resolved`
- 发现日期：`2026-09-22`（2026-10-05 登记并转交上游流程）
- 记录人/责任人：Mira Maintainers（反馈来源：Mirage 桌面 Host，编号 MIRA-20260922-001）
- 影响组件：Windows MinGW-w64 交叉构建门禁（posix 线程模型）
- Executor 版本或提交：`2ae4fc8985af8962e08e3282a9330de5445d0d10`（随附于 `dependencies.lock.json`）
- 关联代码/测试：`third_party/kairo/src/kairo/blocking_io_executor.cpp:157`（随附依赖升级 kairo v0.6.0 后路径，原 `third_party/executor/src/executor/`）；反馈详情见
  [mira#75](https://github.com/Linductor-alkaid/mira/issues/75)
- 上游 issue/PR：executor 仓库 issue 待提出（本条即转交记录）

#### 使用场景

Mirage 桌面 Host 按 DEC-017 引导方式用 MinGW-w64 x86_64 GCC 13.2.0（posix 线程模型）交叉
构建随附 Executor。Mira 的 Windows 门禁需要"同一源码树在 MSVC 与 MinGW 双工具链可构建"；
当前 MinGW 门禁只能构建不含 executor 的 platform/desktop/integration 目标子集。

#### 已核查证据

已核对 pinned 版本 `blocking_io_executor.cpp:157`（升级 kairo v0.6.0 后仍为 `src/kairo/blocking_io_executor.cpp:157`，同一表达式）：

```cpp
auto self_handle = static_cast<std::thread::native_handle_type>(GetCurrentThread());
```

MinGW-w64 posix 线程模型的 `std::thread::native_handle_type` 是整数类型（pthread 自映射），
与 `HANDLE`（`void*`）之间无 static_cast 关系；GCC 13.2.0 报
`invalid 'static_cast' from type 'HANDLE' {aka 'void*'} to type 'std::thread::native_handle_type'
{aka 'long long unsigned int'}`。编译期最小复现（仅类型引用，不创建线程）与全树复现命令见
mira#75。MSVC（win32 线程模型，`native_handle_type` 即 `void*`）不受影响。已排除 API 选型或
配置替代：公开头文件与 `docs/API.md` 无平台无关的自身句柄获取方式。

#### 期望能力与语义

executor 源码不依赖具体线程模型的 `native_handle_type` 表达：经 `std::thread::native_handle()`
取得真实句柄，或按线程模型条件编译适配 posix/Win32 两种形态。修复不得以
`reinterpret_cast` 绕过编译：需确认 Win32 伪 HANDLE 与 pthread 句柄的真实类型/亲和性语义，
保留既有生命周期与诊断行为。修复后 MinGW-w64（posix）与 MSVC 双工具链均可构建。

#### 影响与风险

- 正确性：无运行期影响（该路径在 MSVC 下行为不变）。
- 可移植性：MinGW posix 工具链全树构建被阻塞；M4-06（产品进程 Windows 化）若要求全树
  MinGW 交叉构建则被此缺陷阻塞，届时期望以 MSVC 门禁 + 本条目状态评估是否放行。
- 不阻塞当前任何工作项与 MSVC 主工具链。

#### 临时方案

无。不修改 pinned 依赖源码；Windows 门禁在 M4-06 前按 DEC-017 决策 9 只构建不含 executor
的目标子集，M4-01 CI 已实测该子集可构建。Mirage 侧不修改 pin。

#### 上游验收标准

- MinGW-w64 GCC（posix 线程模型，x86_64）下 executor 默认构建目标零诊断。
- MSVC（windows-latest，VS 18 2026）构建与既有测试不回归。
- 亲和性设置行为在两个工具链下保持既有语义（不因绕过 cast 而静默失效）；若语义无法保留，
  需以明确诊断或文档说明差异。

#### 跟进记录

| 日期 | 状态变化 | 说明 | 证据/链接 |
| --- | --- | --- | --- |
| 2026-09-22 | Open | Mirage M4-02 交叉构建预检首次复现（全树诊断） | mira#75 |
| 2026-10-05 | Open | 编译期最小复现复核通过；正式登记台账并经 mira#75 转交上游流程 | mira#75 |
| 2026-10-05 | Open（上游复核） | 上游已更名 kairo 并发布 v0.6.0（破坏性变更窗口）；对照 tag `v0.6.0` 源码复核，同一表达式仍在 `src/kairo/blocking_io_executor.cpp:157`，且上游 tracker 无 MinGW/native_handle 相关 issue——本缺口未随 0.6.0 关闭，升级 pin 不能解决 | [kairo v0.6.0](https://github.com/Linductor-alkaid/kairo/releases/tag/v0.6.0) |
| 2026-10-05 | Open（pin 迁移） | Mira 随附依赖升级为 kairo v0.6.0（`d9602ea`，上游更名 + 兼容层清理 + Scheduling Runtime）；迁移复核确认缺陷仍在，条目保持 Open，关联路径更新为 `third_party/kairo` | 维护计划 maintenance-2026-10-kairo-migration.md |
| 2026-10-05 | Open → Resolved | 上游修复合入 master：[kairo#215](https://github.com/Linductor-alkaid/kairo/pull/215)（`99a55a0`，合入 `ef821dc`）按本条期望语义解耦 `native_handle_type`——新增 `set_current_thread_priority/affinity()`，两执行器 self_handle `#ifdef` 块删除；MinGW posix/win32 双模型全树构建零诊断、Linux ctest 147/147；上游 master CI 绿。Mira pin 升级至 `ef821dc`（`v0.6.0-12-gef821dc`），Mira 全量回归见维护计划。MinGW 交叉全树验收（Mirage 门禁侧）与 MSVC 复验由本 PR CI/mirage 补跑承接 | kairo#215、mira#75 |

### EXE-20261028-001：总量有界 admission 缺少按主体分桶的份额准入语义

- 状态：`Open`
- 发现日期：`2026-10-28`
- 记录人/责任人：Mira Maintainers
- 影响组件：`kairo::Executor` 的接纳（admission）语义；Mira 侧计划中的主体级协调层
  （[DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)）
  与有限容量资源模型
  （[DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)）
- Executor 版本或提交：kairo v0.7.0，pin commit `32027c247dd4b9280d7c995454b732bac0db9095`
  （`dependencies.lock.json:8-11`）
- 关联代码/测试：`include/kairo/executor.hpp:182-184`、`include/kairo/executor.hpp:1031-1032`、
  `include/kairo/task_router.hpp:17-22`、`include/kairo/scheduler.hpp:65-66`、
  `include/kairo/scheduling.hpp:44-53`
- 上游 issue/PR：尚未提交；经 [mira#111](https://github.com/Linductor-alkaid/mira/issues/111)
  转交上游流程

#### 使用场景

Mira 计划引入**主体级（principal/tenant）协调层**：多个已认证主体共享同一个
`kairo::Executor`，每个主体有自己的会话数、并发任务数与存储份额上限。issue #111 §6 明确
要求「区分总预算、分主体份额、软水位、硬水位与回收/取消所需预留」，且
「容量预留与并发接纳一致」——即份额上限必须**作用在 Executor 的接纳点**上，
而不是在接纳之后再做一层应用层节流。

Mira 侧既有设计也已经把这条写成了契约：
[事件与崩溃一致性设计 §11](../design/event_artifact_crash_consistency.md)
「配额至少按 runtime、tenant、session 和单 artifact 配置」；
[核心契约与状态机设计 §5.3](../design/core_contracts_and_state_machine.md) 的
`OpenSession` / `SubmitTask` 接纳条件含「配额允许」。

#### 已核查证据

**1. 现有 admission 是实例级单值计数器，没有 key 维度。**

- `include/kairo/executor.hpp:1031-1032`：`max_in_flight_tasks_` / `in_flight_submissions_`
  是两个 `std::size_t`，**没有**任何分桶键。
- 公开 API 只有 `set_max_in_flight_tasks(size_t)` / `get_max_in_flight_tasks()` /
  `get_in_flight_submissions()`（`include/kairo/executor.hpp:182-184`）——
  **没有任何按主体分桶的接口**。
- 计数语义本身是正确且原子的：`try_admit_submission`
  （`include/kairo/executor.hpp:1077-1080`）+ `AdmissionReleaser`
  （`include/kairo/executor.hpp:1046-1069`）保证「恰好一次释放」，
  `record_capacity_exhausted`（`include/kairo/executor.hpp:1082-1084`）记拒绝。
  **缺的只是分桶，不是计数。**

**2. 没有配额 / 租约 / 许可原语。**

- `include/kairo/` 全量 grep `quota|lease|permit|credit` 仅命中
  `comm/bounded_queue.hpp:80,266,295` 的 `EnqueueLease` / `ConsumerLease`——
  队列内部 RAII 的消费权交接，**不是配额**。
- `include/kairo/scheduling.hpp:44-53` 的 `ResourceRequirements` 只有 `memory_bytes`
  （GPU 内存可行性）与 `gpu_device`（设备归属）。契约明写「feasibility check against
  capability snapshots」，`third_party/kairo/docs/API.md:385` 明确「**feasibility hint，
  非资源保证；存在 TOCTOU 窗口**」，违约行为是 `BackendUnavailable` / `CapacityPressure`
  ——**没有「扣减 / 释放 / 剩余额度」的配额账本语义**。

**3. 降载是按 QoS 类别，不是按主体。**

`include/kairo/adaptive_scheduler.hpp:59-94` 的 `queue_wait_p99_target_ns` 是按 `QosClass`
索引的 4 元素数组；`:141-145` 的 `load_shed_active(QosClass)` 同理；
`:27-31` 的学习粒度是 per-(backend, executor, qos)。**没有任何主体维度。**

**4. `IScheduler` 注入点无法承担主体级配额。**

- `include/kairo/task_router.hpp:17-22`：`TaskRouter::Request` 只有
  `TaskOptions options; bool cpu_gpu_task; std::optional<bool> gpu_selected;`
  ——**没有主体/租户身份字段**；`TaskOptions`
  （`include/kairo/task_options.hpp:170-181`）也没有。
- `include/kairo/scheduler.hpp:65-66`：`IScheduler::route()` 只**产出路由决策**，
  不持有配额账本；且 `set_scheduler()` **必须在首次提交前调用**
  （`include/kairo/executor.hpp:56-61`、`third_party/kairo/docs/API.md:387-391`）。
- `IScheduler` **只被 `submit_auto` 咨询**
  （`include/kairo/executor.hpp:2921,2943`）；`submit()` / `submit_delayed()` /
  `submit_periodic()` / `submit_on()` / `submit_cancellable*()` **都不经过它**。
  ⇒ 用自定义 `IScheduler` 实现配额**只覆盖 `submit_auto` 一条路径**。

**5. 已排除「API 选型错误 / 应用层职责」两种解释。**

- 不是选型错误：`EXE-20260830-001` 已把「总量有界 admission」迁移到 Executor 原生
  `max_in_flight_tasks`（其正文只谈**总量**上限，未涉及分桶/份额）；
  本条是它的自然延伸（总量 → 按主体分桶），不是重复登记。
- 不是应用层职责：AGENTS.md 排除的是「模型供应商协议适配、手势映射、业务状态机策略、
  错误使用已有 API、平台权限」。主体份额准入是**通用的并发容量治理原语**
  （多个共存主体共享一个 Executor 时的配额与背压），不是 Mira 独有的业务策略；
  而且它必须作用在接纳点才有意义——否则只能做「先拒后放」的二段式，
  与 Executor 的「恰好一次释放」「future 就绪前计数已释放」不变式
  （`third_party/kairo/docs/API.md:601-604`）产生双份计数。

**最小复现思路**：设两个主体 A/B 共享一个 Executor，`max_in_flight_tasks = 2`。
A 提交 2 个长任务后，B 的提交被**全局**拒绝，尽管 B 自己的配额为 0 未被触碰。
现有公开 API 无法表达「A 已用满 A 的份额，但 B 仍有份额」。

#### 期望能力与语义

Executor 应提供**按 key 分桶的份额准入**，语义：

- 接纳点接受一个可选的**分桶键**（泛型或字符串标签），并对每个键维护独立的在途计数与
  份额上限；总上限与分桶上限**同时**生效，任一超限即拒绝该次提交。
- 计数生命周期与现有总量计数一致：**恰好一次接纳、恰好一次释放**；
  future 就绪前计数必须已释放。
- 拒绝必须可观测：沿用现有 `CapacityExhaustedException` /
  `FailureKind::CapacityExhausted` / `capacity_exhausted_count` 三通道
  （`include/kairo/types.hpp:47-58,185,214`），并能区分「总量超限」与「某分桶份额超限」。
- 份额上限的设置与查询应有运行期接口，且设置变更需与既有
  `set_max_in_flight_tasks` 一样在**首次提交前**完成（或提供明确的热更新语义）。
- **不要求** Executor 承担业务策略：份额值从哪来、是否按主体动态调整，仍由调用方决定；
  Executor 只负责按 key 记账与拒绝。

#### 影响与风险

- **正确性**：无直接正确性缺陷——现有行为在总量维度是自洽的。
- **交付阻塞**：[DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)
  §1/B2 的「分主体份额」与「容量预留与并发接纳一致」在当前 API 下**无法在接纳点实现**；
  M31 的部署档位因此无法冻结。
- **多租户公平性**：一个主体可以吃光全局在途额度，其他主体被全局拒绝——
  这与 Mira 既有的「配额至少按 runtime、tenant、session 配置」设计契约不符。
- **对上游的规模影响小**：是在既有 `try_admit_submission` / `AdmissionReleaser`
  机制上加一个可选的 key 维度，不是新增调度模型。

#### 临时方案

**无完全等价的安全临时方案。** 在上游提供该能力前，M31 采用**嵌套预过滤**：

- Mira 侧维护一个分桶账本，在**调用 Executor 提交之前**先做主体份额检查：
  份额不足则由 Mira 直接返回 `ResourceExhausted`，**不进入** Executor 接纳点。
- 通过 Mira 账本后，仍由 Executor 的 `max_in_flight_tasks` 做**全局**兜底。
- 二者是**嵌套**关系（Mira 分桶 ⊆ Executor 全局），因此 Executor 的
  「恰好一次释放」与「future 就绪前计数已释放」不变式**不被破坏**——
  被 Mira 拒绝的请求从未进入 Executor 计数。

**已知行为差异（必须记录，不得静默）**：

1. 拒绝计数分散在两处：Mira 账本的拒绝与 Executor 的 `capacity_exhausted_count`
   分别统计；上层诊断需要合并两者才能得到「因容量拒绝」的完整视图。
2. 存在**双重记账窗口**：Mira 账本在提交前预留、Executor 接纳后释放，
   两者之间若进程崩溃，Mira 账本可能残留预留（需由 M31 的恢复流程按
   `ControlIngressId`/任务终态清理）。Executor 侧不受影响。
3. 该方案**不是** Executor 原生分桶，因此不能覆盖 `submit_delayed` /
   `submit_periodic` / realtime / blocking-I/O 等绕过 `submit_auto` 的路径——
   这与 Executor 自身的 admission 覆盖面一致（总量维度同样不覆盖这些路径，
   `third_party/kairo/docs/API.md:580-584`）。
4. 若未来上游提供原生分桶，Mira 侧账本**必须删除**，不得与原生能力叠加。

**移除条件**：上游按本条期望语义提供能力后，Mira 侧账本与预过滤路径整体删除，
`DEC-069` §1/B2 改为直接使用 Executor 原生分桶。

**测试覆盖**：M31 的容量门禁必须覆盖「A 用满自身份额后 B 仍可接纳」与
「A+B 合计超过全局上限」两个用例，并断言拒绝原因可区分。

#### 上游验收标准

- Executor 支持按分桶键的独立在途计数与份额上限；总上限与分桶上限同时生效。
- 分桶计数遵守「恰好一次接纳、恰好一次释放」；future 就绪前计数已释放。
- 分桶份额超限的拒绝**可与总量超限区分**，且沿用既有三通道可观测面。
- 正常场景：主体 A 与 B 各自未超份额时，双方提交均被接纳，互不影响。
- 拒绝/过载场景：A 超自身份额被拒且 B 仍可接纳；A+B 合计超全局上限时双方按既有
  总量语义被拒。
- 取消场景：被取消或失败的任务**释放其分桶计数**，不泄漏额度。
- 异常场景：任务体抛异常时，分桶计数同样被释放。
- 超时场景：deadline 触发的拒绝与释放路径与总量语义一致。
- shutdown 场景：shutdown 期间的分桶拒绝语义与总量语义一致，且计数全部归零。
- 既有行为零回归：`max_in_flight_tasks` 的现有语义与全部既有测试不回归。

#### 跟进记录

| 日期 | 状态变化 | 说明 | 证据/链接 |
| --- | --- | --- | --- |
| 2026-10-28 | Open | 为 [mira#111](https://github.com/Linductor-alkaid/mira/issues/111) §6「分主体份额」需求核查 kairo v0.7.0 公开头文件、API 文档与集成指南后确认缺口：admission 为实例级单值、无 key 维度、无配额原语、`IScheduler` 仅覆盖 `submit_auto` 且无主体字段；经 [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md) §1/B2 引用登记 | mira#111、DEC-069 §1 |
