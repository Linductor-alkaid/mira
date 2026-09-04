# 环境与 Observation

> 头文件：`mira/environment.hpp`、`mira/observation.hpp`、`mira/coordinates.hpp`、
> `mira/observation_pipeline.hpp`、`mira/adapters/`

## environment.hpp：IEnvironment

聚合环境观察与最终输入派发的平台中立契约，是 Core 与宿主之间最重要的边界：

```cpp
class IEnvironment {
  public:
    [[nodiscard]] virtual EnvironmentCapabilities capabilities() const = 0;
    virtual Result<Observation> observe(const ObservationRequest &request,
                                        const OperationContext &context) = 0;
    virtual Result<ExecutionReceipt> execute(const InputSequence &input,
                                             const OperationContext &context) = 0;
    virtual Result<void> interrupt(const OperationContext &context) = 0;
};
```

### 输入契约

- `InputEvent{kind, payload}`：一个离散平台输入事件（`"tap"`、`"long_press"`、
  `"swipe"`、`"type"`、`"back"` 等）；payload 是已脱敏的规范坐标/文本，环境将其视为
  不可信文本，不得原样入日志。
- `InputSequence`：面向单 display 的已编译序列；nil display 选择主 display，动作不假设
  全局共享坐标空间。
- `ExecutionStatus`：`Dispatched`（平台已接受，完成未知）/ `Completed` / `Rejected`
  （副作用前拒绝）/ `Unknown`（无法确认，副作用可能已发生）。
- `ExecutionReceipt.side_effect_may_have_occurred`：平台不能排除输入已达设备时为真；
  不确定结果必须重新 Observe/Verify，不得重发。
- `interrupt()`：尽力释放 in-flight 输入与平台等待；幂等，迟到完成不得复活已结算操作。

### 能力声明

`EnvironmentCapabilities` 逐字段声明环境能诚实交付的能力（`screen_capture`、`ui_tree`、
`atomic_observation`、`max_component_skew`、`discrete_input`、`input_release`、
`epoch_invalidation` 等）。Adapter 不得声明无法兑现的能力；Core 不从平台名猜测能力。
`unsupported_required_components(capabilities, request)` 列出请求中能力无法满足的组件，
环境据此 fail closed 而不是返回静默不完整的观察。

### OperationContext

携带一次有界操作的身份与协作取消状态：`session/task/step/operation` ID、`task_epoch`、
`started_at`、steady clock 绝对 `deadline`、`cancellation_requested` 探针。长捕获在
阻塞步骤间轮询 `cancelled_or_expired()`。控制面调用可用 `make_control_context()`。

## observation.hpp：Observation

可扩展环境快照。`Observation` 聚合：

- `screen`（`ScreenFrameDescriptor`）、`structure`（`UiTreeSnapshot`）、
  `perception`（若干 `PerceptionEvidence`）、`foreground`（`AppContext`）、`device`
  （`DeviceState`），每个组件是 `ObservationComponent<T>`，独立携带 provenance、捕获
  时间窗与质量。
- `topology`（display 拓扑）、`atomicity`（组件是否单事务捕获）、`aggregate_span`、
  `quality`（`ClockSyncQuality`、`ComponentQuality` 分项）。
- `environment_epoch`：快照对应的坐标纪元。

`ObservationRequest` 以 `required`（必备组件集合）与 `max_age` 表达需求；
`validate_observation_request()` 预校验请求，`evaluate_observation()`（返回
`ObservationEvaluation`）在 Verify 时判定观察是否满足请求、足够新鲜、组件偏差是否在
声明范围内。`observation_component_skew()` 计算组件起始时间的最大成对差。

大体积截图等载荷以 `ArtifactRef` 引用 + 摘要进入事件流，不在事件间复制。

## coordinates.hpp：坐标

- `PointF`/`RectF`：规范坐标空间中的归一化值（`[0, 1]`），动作编译输出必须落在此域。
- `CoordinateSpace`（`CoordinateSpaceKind` + display/纪元锚定）、`CoordinateTransform`、
  `Matrix3x3`、`TransformChain`、`Rotation`、`TransformQuality`：跨空间变换与质量衰减
  （`worse_transform_quality()`）；`validate_coordinate_transform()` 拒绝非法变换。
- 环境任何不连续（旋转、拓扑、权限）通过 `EnvironmentEpoch` 递增使旧变换失效。

## observation_pipeline.hpp：ObservationPipeline

在 Executor 上组装多源 Observation 的管线：注入 `TopologySource` 与各类
`ObservationComponentSource<T>`（screen/structure/foreground 等），按请求聚合组件并
产出质量与偏差元数据；配置（`ObservationPipelineConfig`）与统计
（`ObservationPipelineStats`）可观测。

## Adapters

### Simulator（`mira/adapters/simulator/simulator_environment.hpp`）

参考环境 `SimulatorEnvironment`：`SimulatorSetup::single_display()` 等脚本化场景，
生成确定性帧与 UI 树，用于离线测试与契约验证（`Mira::simulator_adapter`）。

### Android Host（`mira/adapters/android/`）

- `host_abi.h`：稳定 C ABI，Android 宿主进程（APK/JNI 侧）以此桥接截屏、结构、输入
  与生命周期回调；追加字段/新版本演进，旧宿主 fail closed
  （[Android Host ABI](../compatibility/android-host-abi.md)）。
- `android_host_adapter.hpp`：`AndroidHostAdapter`（`IEnvironment` 实现），经桥接转发
  观察与输入。
- `host_dispatcher.hpp`：`HostDispatcherBridge`、`HostLeaseGuard` 与 `HostFrameOutcome`/
  `HostTreeOutcome`/`HostInputOutcome`——有界等待、租约与取消语义的宿主侧结算
  （`Mira::android_adapter`）。真机运行证据不在当前声明范围（见平台矩阵）。

### Replay（`mira/replay.hpp`）

`OfflineReplayEnvironment`：以录制观察与回执逐条回放，`observe()` 返回下一条记录原文，
不执行任何真实捕获或输入副作用。

## 相关文档

- [Observation、坐标与 Android Host ABI 设计](../design/observation_coordinate_android_host.md)
- [DEC-005 Observation 坐标与宿主边界](../decisions/DEC-005-observation-coordinate-host-boundary.md)
