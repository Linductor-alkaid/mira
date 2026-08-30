# Android Host ABI v1 兼容性记录

> 状态：Active
> 版本：1.0
> 更新日期：2026-08-30
> 适用范围：`include/mira/adapters/android/host_abi.h`、Native Android Adapter 与 Android Host 之间的版本化 C ABI
> 上位设计：[Observation、坐标与 Android Host ABI 设计](../design/observation_coordinate_android_host.md)、
> [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)

## 1. 冻结范围

ABI v1 以 `mira_android_host_abi.h`（`include/mira/adapters/android/host_abi.h`）为准，包含：

- 生命周期：`mira_android_host_create_v1/start_v1/stop_v1/destroy_v1`；create 校验
  `struct_size` 与 `abi_version`，stop/destroy 幂等，destroy 在 lease 未清或未 stop 时返回
  `MIRA_HOST_ERR_INVALID_STATE`。
- 查询：`mira_android_host_get_capabilities_v1`、`mira_android_host_get_topology_v1`。
- 异步操作：`capture_frame_v1`、`get_ui_tree_v1`、`dispatch_input_v1`，请求携带 native 侧
  `correlation`，每个 operation 恰好一次 terminal callback。
- 协作取消：`mira_android_host_cancel_operation_v1`；不可中断的平台调用以
  `MIRA_HOST_ERR_EXECUTION_UNCERTAIN` + `side_effect_may_have_occurred` 结算。
- Buffer lease：`MiraHostBufferLeaseV1`；lease 结构体只在回调期间有效，native 桥在回调内
  完成有界复制，并对每个 lease 恰好一次 `release()`。
- Status code 集合与 `mira::ErrorCode` 的映射约定（见头文件注释）。

## 2. 证据等级

| 声明 | 等级 | 证据 |
| --- | --- | --- |
| ABI 头为纯 C、无 STL/JNI/Android 类型 | 源码确认 | `platform-boundary-check` 与公共头独立包含测试（`mira_public_headers_test`） |
| struct_size/abi_version 前向兼容与 fail closed | fake host 契约测试 | `tests/m2/m2_android_host_abi_test.cpp` `check_abi_validation_and_lifecycle` |
| callback exactly-once、重复/未知回调被隔离计数 | fake host 契约测试 | 同上 `check_duplicate_and_unknown_callbacks_isolated` |
| lease 在成功、失败、越界、取消路径均恰好一次释放 | fake host 契约测试 | `check_adapter_observe_and_lease_release`、`check_oversize_lease_rejected_and_released`、`check_cancellation_and_interrupt`，并以 `FakeAndroidHost::outstanding_leases()` 断言归零 |
| epoch 递增使在途捕获判为 StaleObservation | fake host 契约测试 | `check_epoch_invalidation_on_rotation` |
| stop 幂等、destroy 后无 lease 悬挂 | fake host 契约测试 | `check_abi_validation_and_lifecycle`、`check_adapter_shutdown_releases_everything` |
| 真实 Android Host（Kotlin/JNI）互操作 | 未验证 | 无已实现 Host bridge；见下节补跑条件 |

## 3. 限制与补跑条件

- M2 只交付 ABI 与 Native Adapter 骨架（`mira_android_adapter`：`HostDispatcherBridge` 与
  `AndroidHostAdapter`），并使用 fake host 验证契约。骨架的 capability 快照诚实声明仅
  screen capture 与离散输入；UI tree、前台与设备组件属于后续里程碑。
- 真实设备/模拟器验证（MediaProjection 授权、Accessibility 生命周期、主线程 dispatcher、
  `dispatchGesture` 输入安全）保持未验证。负责人：Mira Maintainers。补跑条件：实现 JNI
  Host bridge 后，在受支持 Android 环境运行设计文档第 15.2/15.3 节的 ABI 生命周期与输入
  安全矩阵，并把结果登记到本文件与 M2/M7 计划。
- ABI 数值（枚举值、status code、struct 布局）一经发布即冻结；扩展只能通过追加 struct 尾部
  字段与新枚举值进行，并递增 `host_sequence` 语义文档。

## 4. 关联文档

- [M2 里程碑计划](../plans/m2-observation-simulator-android-host.md)：`M2-08`、`M2-09`
- [平台构建与 Adapter 兼容性矩阵](platform-matrix.md)
