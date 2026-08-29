# Mira 实时控制层设计

> 状态：Active  
> 版本：1.0  
> 更新日期：2026-08-30  
> 适用范围：Joystick、Drag、TouchTrajectory、在线状态反馈、watchdog 和平台输入  
> 上位设计：[Mira Runtime 设计](mira_runtime_design.md)

## 1. 目标与实时定义

实时控制层把低频 `ActionIntent` 转换为有时间约束的输入轨迹，并在取消、Human Takeover、环境
变化或反馈丢失时安全停止。Mira 首期追求 bounded-latency soft realtime，不宣称硬实时。

三个频率域严格分离：

- 决策域：LLM/VLM，通常百毫秒到秒级。
- 感知/状态域：screen diff、tracker、任务 ONNX，通常数十毫秒级并允许降频。
- 控制域：轨迹采样、watchdog、输入 chunk，固定周期且不能等待前两者。

控制层只执行已验证、有最大时长的 intent，不制定任务目标，不调用模型，不授予权限。

## 2. 控制链

```text
ActionIntent
 -> ActionValidator
 -> ControllerPlan (immutable, bounded)
 -> acquire ActionLease
 -> durable ActionDispatchStarted
 -> RealtimeController + latest feedback
 -> InputChunk stream
 -> IInputProvider / Host Adapter
 -> ControlReceipt
 -> Verify
```

`ControllerPlan` 固定本次动作的 Task/Environment epoch、coordinate transform、duration、rate、pointer
ownership、safety envelope、stop conditions 和平台 capability snapshot。运行中策略更新生成新 version，
不能修改正在被 realtime callback 读取的对象。

## 3. 时间与控制数据

```cpp
struct ControllerPlan {
    ControllerId controller_id;
    ActionId action_id;
    TaskEpoch task_epoch;
    EnvironmentEpoch environment_epoch;
    ActionLease lease;
    std::chrono::microseconds period;
    std::chrono::milliseconds max_duration;
    TrajectoryProfile trajectory;
    SafetyEnvelope safety;
    StopPredicateSet stop;
    InputCapabilitySnapshot input;
};

struct ControlTarget {
    ControlSequence sequence;
    MonotonicTime valid_from;
    MonotonicTime expires_at;
    Vec2 direction;
    float magnitude;
    TargetFlags flags;
};

struct FeedbackSnapshot {
    FeedbackSequence sequence;
    ObservationId source_observation;
    EnvironmentEpoch environment_epoch;
    MonotonicTime captured_at;
    StateEstimate state;
    FeedbackQuality quality;
};
```

控制周期只使用 monotonic clock。来自设备/帧的其他 clock domain 必须在普通路径转换，并携带误差；
误差过大时 feedback 不可用于安全 stop predicate。

## 4. Executor 路由

- Controller 使用 Executor realtime executor；创建、start、stop、status 和 handle 由
  ExecutionSupervisor 管理。
- 控制命令使用有界 `RealtimeChannel` 或等价 realtime-safe channel；只保留最新目标时使用
  `LatestMailbox` 语义。
- Feedback 通过预分配的 `LatestMailbox`/DoubleBuffer 读取完整 snapshot，不读取正在写入的对象。
- 结果/统计通过有界普通控制 completion 回传；realtime callback 不写 EventStore 或调用 Observer。
- 每个 Controller 有唯一名称/handle 和 owner；Runtime shutdown 不以 default pool idle 代替 realtime
  stop/status。

正式实现前按 Executor realtime capability card 复核具体 API、drop status、cycle budget 和 stop 语义。

## 5. Realtime callback 约束

callback 每周期只允许：

1. 读取 immutable plan 和最新 target/feedback snapshot。
2. 检查 deadline、heartbeat、epoch shadow、lease shadow 和 stop flags。
3. 计算固定上限个采样点或一个 bounded InputChunk。
4. 尝试投递平台 realtime-safe input endpoint。
5. 更新 lock-free/bounded counters 和最终 stop reason。

禁止：动态内存分配、锁/condition、阻塞等待、磁盘/网络、模型/ONNX、JSON、日志格式化、Observer、
公共命令递归、无界 channel drain 或容器增长。Debug/CI 可使用 Executor realtime allocation guard
检测违反；其结果是诊断，不替代代码审计。

## 6. 轨迹与 Pointer 所有权

### 6.1 PointerSession

每段触摸具有唯一 `PointerSessionId` 和状态：

```text
Idle -> DownPending -> Active -> ReleasePending -> Released
                     |             |
                     +-> CancelPending -> Released
```

- 只有 Controller owner 能生成该 session 的 Move/Up/Cancel。
- Down 未确认前不能假定 pointer active；receipt 不确定时仍按可能 active 做 release。
- 任意终止路径必须尝试 exactly one logical release；Adapter 可将重复 release 幂等处理。
- 新 Controller 不能复用旧 pointer ID，直到平台 receipt/watchdog 表明旧 session 收敛。

### 6.2 轨迹编译

- Intent 坐标先在普通路径绑定 Observation transform 和 safety envelope。
- easing、dead zone、acceleration、max velocity/jerk 和 boundary clamp 由 profile 定义。
- sample count、duration、pointer 数和 chunk 大小不超过 Adapter capability。
- 平台只支持 whole gesture 时预编译有界 trajectory；支持 streaming 时按 look-ahead window 分块。
- 分块 overlap/continuity 和平台 timestamp semantics 由 Adapter contract test 验证。

## 7. Joystick、Drag 与自定义轨迹

### 7.1 Joystick

`JoystickIntent` 定义 center、方向/幅度、duration、response curve 和 expected state。Controller 维持
Down，按 target 更新 Move，结束时 Up。新 target overwrite 旧 target 是允许的，但统计 overwrite；
过期 target 不延续最后方向，而进入 neutral/stop policy。

### 7.2 Drag

Drag 是预定起终点或带 waypoint 的有限 trajectory。目标元素/起点必须在执行前重新验证；长 drag
可使用 tracker feedback，但 feedback 丢失时按 profile 停止而不是继续推测。

### 7.3 TouchTrajectory

自定义轨迹只能来自通过 schema/Policy 的结构化数据，限制：最大 duration、sample、pointer、pressure
range、coordinate region 和 monotonic offsets。模型不能提交平台原生事件或绕过 compiler。

## 8. Feedback 与停止条件

Stop predicate 分为：

- 无条件：duration/deadline、cancel、Takeover、shutdown、lease/epoch change。
- 平台：input rejection、Host lifecycle/permission、pointer callback、watchdog。
- 本地状态：目标到达、危险区域、元素丢失、状态矛盾。
- 质量：feedback stale、OOD、tracking lost、transform invalid。

安全停止只依赖确定性、本地、有界读取的 predicate。VLM 结果不能作为 realtime callback 的即时
刹车信号；它可在普通控制面发 stop command。不同条件同时发生时使用固定优先级并保留首因和全部
flags。

Feedback freshness：

```text
age = controller_now - feedback.capture_monotonic
usable iff epoch matches
        and quality >= plan minimum
        and age <= max_feedback_age
        and clock uncertainty <= bound
```

不满足时采用计划的 `Stop`、`HoldNeutral` 或有限 open-loop fallback；高风险/不明 profile 默认 Stop。

## 9. Watchdog 与安全停止

分层 watchdog：

1. Target heartbeat：Planner/普通控制面不再更新。
2. Controller cycle：周期 callback 超预算或未运行。
3. Input receipt：平台 endpoint 不确认 chunk/gesture。
4. Host watchdog：native 进程/回调失联时释放或让 gesture 在最大 duration 自行结束。

安全停止流程：设置 stop flag -> 停止新 Move -> 生成 Up/Cancel -> 请求 Adapter release_all（需要时）
-> 等待有界 receipt -> 产生 `ControlReceipt`。未确认释放必须标 `unsafe_release_unconfirmed=true`，
Task/Shutdown 不能报告 clean。

## 10. 平台能力与降级

```cpp
struct InputCapabilitySnapshot {
    bool streaming_pointer;
    bool cancel_active_gesture;
    bool release_all;
    std::uint32_t max_pointers;
    std::uint32_t max_samples_per_gesture;
    std::chrono::milliseconds max_gesture_duration;
    std::chrono::microseconds min_sample_period;
    ReceiptSemantics receipt;
};
```

- Android whole-gesture API 不能被描述成逐周期 streaming；Controller 选择短有界 gesture chunk 或拒绝
  不可安全实现的 intent。
- Adapter 不支持 Cancel 时，用最大 duration 和 Host watchdog 限制残留时间，并明确 release 未确认。
- 能力变化递增 environment epoch，旧 Controller 必须停止。
- 平台 realtime priority/CPU affinity 不可用时仍可运行 soft realtime，但 capability/benchmark 标明。

## 11. 背压与过载

- target channel `KeepLatest`，overwrite 计数；stop/cancel 使用不可丢失的独立 flag/control path。
- InputChunk queue 满不能静默继续生成。按 Adapter 语义停止、降频或报告 overrun。
- feedback 可以覆盖旧 snapshot；新 snapshot 必须完整且 sequence 单调。
- metrics 可采样，最终 release/receipt/stop reason 不可丢。
- 连续 deadline miss 达阈值触发停止和 `RealtimeBudgetExceeded`，不能用积压补发过期 sample。

## 12. Receipt

```cpp
struct ControlReceipt {
    ControllerId controller_id;
    ActionId action_id;
    ControlStopReason primary_reason;
    StopReasonFlags all_reasons;
    std::uint64_t cycles;
    std::uint64_t deadline_misses;
    std::uint64_t target_overwrites;
    std::uint64_t input_drops;
    MonotonicTime started_at;
    MonotonicTime stop_requested_at;
    MonotonicTime release_completed_at;
    bool pointer_may_remain_active;
    std::optional<Error> error;
};
```

receipt 说明控制执行和释放状态，不说明任务目标成功；Task 随后进入 Verify。平台 callback 丢失或
无法确认输入时 `pointer_may_remain_active=true`，即使 duration 已过也保留诊断。

## 13. 生命周期

- `prepare`：普通路径验证、预分配、创建 plan/channel/metrics，无输入。
- `arm`：获得 lease、durable start ack、注册 realtime callback。
- `run`：固定周期，消费 target/feedback，输出 chunk。
- `stop`：任何 stop source 原子设置；幂等。
- `settle`：普通路径收集 realtime status、平台 receipt 和释放结果。
- `destroy`：只有 callback 不再运行、channel 关闭、future/handle 消费后释放资源。

pause/cancel/Takeover 只有 settle 达到安全策略要求后才可完成对应命令。Runtime shutdown 先停 target/
feedback producer，再 stop Controller，最后释放 Adapter 和 Executor。

## 14. 测试与基准

### 14.1 单元/属性测试

- trajectory endpoints、duration、monotonic offsets、boundary、velocity/jerk。
- pointer 状态所有转换和所有错误路径 exactly one logical release。
- stale target/feedback、epoch/lease change、同时 stop reason。
- 有界容器、max samples/pointers/duration 和 malformed custom trajectory。

### 14.2 集成与故障

- target overwrite/channel full/input drop/callback delay。
- cancel、pause、Takeover、shutdown 在 Down/Move/chunk/receipt 每个边界注入。
- Host/Adapter crash、permission revoke、rotation 和 release unsupported。
- realtime callback allocation/lock/block detection。

### 14.3 Benchmark

记录 target-to-input、cycle jitter、deadline miss、stop-to-release、feedback age、CPU/thermal/功耗、queue
drop 和 shutdown settle P50/P95/P99/max。按平台、设备、输入 API、Executor config、period、负载和
build 记录。产品 profile 的频率只能由目标设备数据冻结。

## 15. 关联文档

- [核心公共契约与状态机](core_contracts_and_state_machine.md)
- [Observation、坐标与 Android Host ABI](observation_coordinate_android_host.md)
- [本地感知与任务模型](local_perception_and_task_models.md)
- [评估与基准体系](evaluation_and_benchmark_design.md)

