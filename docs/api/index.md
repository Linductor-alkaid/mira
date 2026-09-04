# Mira API 手册

> 状态：Active
> 版本：0.1.0（对应 `Mira 0.1` 安装包）
> 更新日期：2026-09-05
> 适用范围：`include/mira/` 全部公共头文件与随包安装的 CMake 目标

本手册按模块描述 Mira 的公共 API。实现细节、设计动机和状态机推导见
[设计文档](../design/mira_runtime_design.md)；能力声明与证据见
[发布说明](../releases/stateful-agent-beta.md)。

## 模块地图

| 页面 | 头文件 | 职责 |
| --- | --- | --- |
| [核心契约与 Runtime](core-runtime.md) | `core_contracts.hpp`、`runtime.hpp`、`runtime_baseline.hpp`、`action_journal.hpp` | ID、时间戳、错误模型、三层状态机、命令/操作生命周期、动作日志 |
| [环境与 Observation](environment-observation.md) | `environment.hpp`、`observation.hpp`、`coordinates.hpp`、`observation_pipeline.hpp`、`adapters/` | `IEnvironment` 契约、Observation 快照、坐标变换、Simulator/Android/Replay 适配器 |
| [模型层与 Agent Loop](model-agent-loop.md) | `model_*.hpp`、`agent_loop.hpp` | OpenAI-compatible Provider、路由/重试/熔断/预算网关、决策解析、离散闭环 |
| [事件、Checkpoint 与恢复](events-recovery.md) | `event_store.hpp`、`artifact_store.hpp`、`json.hpp`、`replay.hpp`、`task_checkpoint.hpp`、`task_recovery.hpp`、`state_store.hpp` | EventStore 事实源、Artifact 存储、崩溃恢复规划、SQLite 参考后端 |
| [Context 与 Memory](context-memory.md) | `context_contracts.hpp`、`context_manager.hpp`、`memory_contracts.hpp`、`sqlite_memory_store.hpp`、`memory_consolidation.hpp`、`provider_continuation.hpp`、`context_memory_supervisor.hpp`、`stateful_replay.hpp` | 上下文预算与装配、长期记忆、consolidation、Provider continuation、supervisor 与分析回放 |
| [安全与权限](security.md) | `security.hpp` | Principal、能力授权、风险分级、PolicyEngine、Human Confirmation、脱敏 |

## 通用约定

### 错误模型

所有可失败操作返回 `Result<T>`（`core_contracts.hpp`）：

- `Result<T>` 持有值或 `Error`，二选一；`has_value()` / `operator bool` 判别，
  `value()` / `error()` 取用。
- `Result<void>` 只表达成功或错误。
- `Error` 携带 `ErrorCode`（稳定枚举，如 `Cancelled`、`DeadlineExceeded`、
  `SafetyRejected`、`ExecutionUncertain`）、`domain`/`domain_code`（分层定位）、
  `retryable`、`safe_message`（已脱敏、可直接入日志）以及可选
  `diagnostic_artifact` / `operation_id`。
- API 不抛异常来表达失败；错误必须被检查，不允许静默吞掉。

### 标识与时间

- 实体 ID 是强类型 128 位值（`RuntimeId`、`SessionId`、`TaskId`、`StepId`、
  `OperationId`、`EventId` 等），经 `MIRA_DEFINE_ID` 生成，支持 `generate()` /
  `parse()` / `to_string()`，nil 值可显式判别。
- `Timestamp` 同时携带 wall clock 与 monotonic clock 读数；时效判断用 monotonic。
- `EnvironmentEpoch` 由环境侧维护的单调计数：拓扑、权限或宿主会话任何不连续都会
  递增它并使旧坐标失效。

### Schema 与兼容性

- 可版本化数据结构携带 `SchemaVersion{major, minor}`；`validate_schema_version()`
  实现"current/previous 读取、拒绝更老 major 与未知更新 major"策略
  （[DEC-002](../decisions/DEC-002-public-contract-versioning.md)）。
- 未知字段解码为"未声明"而非错误；公共头文件不暴露 SQLite、TLS 库或平台类型。

### 并发与生命周期

- Mira 内部所有异步、阻塞 I/O、定时与维护任务由 Executor 承载；宿主是 Executor 的
  外部 owner（[DEC-001](../decisions/DEC-001-runtime-executor-ownership.md)）：由非
  worker 线程 `initialize()` 与 `shutdown(true)`。
- 取消是协作式的：`OperationContext.cancellation_requested` 与绝对 `deadline` 驱动；
  长操作在阻塞步骤之间轮询，deadline 不是抢占保证。
- 终态幂等：`TaskState`/`SessionState`/`RuntimeState` 的终态不可复活，迟到的完成
  按 `CompletionDisposition`（`Stale`/`Duplicate`/`NotFound`）丢弃。

### CMake 消费

```cmake
find_package(Mira 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE Mira::core Mira::state_store)
```

导出目标：`Mira::core`、`Mira::state_store`、`Mira::simulator_adapter`、
`Mira::android_adapter`、`Mira::net_transport`、`Mira::openssl_transport`、
`Mira::mbedtls_transport`。`MiraConfig` 自动解析 `executor 0.4` 与 `Threads` 依赖。
当前为 0.x（`SameMajorVersion`），无 API 稳定性承诺；消费方应钉住安装版本。

## 使用示例

仓库内提供两个编译进 CI 的示例：

- [`examples/minimal_consumer.cpp`](../../examples/minimal_consumer.cpp)：Simulator 观察
  与离散输入、`RuntimeBaseline` 生命周期。
- [`examples/stateful_agent_consumer.cpp`](../../examples/stateful_agent_consumer.cpp)：
  durable Checkpoint 与 Memory、确定性 consolidation、supervised 调度与关闭、
  分析回放。
- [`tests/consumer/`](../../tests/consumer/)：安装包最小外部消费者。
