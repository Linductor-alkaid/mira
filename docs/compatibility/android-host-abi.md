# Android Host ABI v1 兼容性记录

> 状态：Active
> 版本：1.2
> 更新日期：2026-09-06
> 适用范围：`include/mira/adapters/android/host_abi.h`、Native Android Adapter 与 Android Host 之间的版本化 C ABI
> 上位设计：[Observation、坐标与 Android Host ABI 设计](../design/observation_coordinate_android_host.md)、
> [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)、
> [DEC-012](../decisions/DEC-012-host-adapter-feedback-round1.md)

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

### 1.1 调用约定澄清（2026-09-06，源自 miracle 实测）

以下约定此前只存在于参考实现行为中，现已在头文件注释中成文（GitHub #13）：

1. **out 参数零初始化**：`get_capabilities_v1` / `get_topology_v1` 的调用方（native 侧）会
   传入零初始化的 out 结构体，`struct_size == 0`。宿主必须将其视为"填充完整 v1 结构"，回写
   结果（含 `struct_size`），不得视为参数错误，也不得读取 out 结构体的既有内容。输入结构体
   仍然要求 `struct_size` 不小于版本化前缀，两者语义不同。
2. **`out_operation` 可空**：三个异步提交函数的 `out_operation` 均可为 `NULL`。native 桥以
   请求中的 `correlation` 匹配 terminal callback；宿主必须接受空出参（取消同样以 correlation
   进行）。
3. **`deadline_ns` 语义**：请求中的 `deadline_ns` 是宿主单调钟（`CLOCK_MONOTONIC`/steady
   clock 纪元）上的**绝对时刻**（纳秒），由 native 侧从 `OperationContext.deadline` 直接传入；
   宿主自行换算剩余时长，`0` 表示不设宿主 deadline。
4. **`duration_ms` 语义**（2026-09-06 新增）：`MiraHostInputEventV1.duration_ms` 为
   LONG_PRESS/SWIPE 的请求时长（毫秒）；`0` 表示由宿主使用默认时长。native 侧提交前会对照
   `max_gesture_duration_ms` 校验非零值，超限拒绝（`InvalidArgument`），不会静默截断。

### 1.2 UI 树负载线格式 `mira.host.tree.v1`（2026-09-06 新增）

`MIRA_HOST_OP_GET_UI_TREE` 的 lease 负载是 UTF-8 JSON 文档（上限 4 MiB，与
`MiraHostTreeRequestV1.max_bytes` 一致）。宿主声明 `accessibility_completeness >= 1` 时，
`AndroidHostAdapter::observe()` 会请求并聚合 structure 组件；`= 0` 时 fail closed。文档结构：

```json
{
  "schema": "mira.host.tree.v1",
  "complete": true,
  "truncated": false,
  "visible_only": false,
  "max_depth_reached": 8,
  "capture_begin_ns": 100,
  "capture_end_ns": 200,
  "nodes": [
    {
      "id": "00000000000000000000000000000001",
      "parent": null,
      "role": "root",
      "text": "",
      "content_description": "",
      "hint": "com.example/root",
      "state": ["visible", "enabled"],
      "actions": ["click"],
      "bounds": {"left": 0.0, "top": 0.0, "right": 1.0, "bottom": 1.0}
    }
  ]
}
```

解析规则（fail closed，任何违规拒绝整个组件，绝不做猜测性修补）：

- `schema` 必需且必须等于 `mira.host.tree.v1`；`nodes` 必需且为数组。
- `id`/`parent` 为 32 位十六进制（`Id128`）；`parent` 可为 `null` 或缺省（根节点）。
- `bounds` 必需，四个分量均为 canonical `[0, 1]` 有限 double（目标 display 的归一化视口坐标），
  且 well-formed（`right >= left`、`bottom >= top`）。
- `role`/`state`/`actions` 取值集合与 `mira::UiRole`/`UiNodeState`/`UiNodeAction` 对应
  （如 `app_bar`、`list_item`、`password`、`clear_selection`）；**未识别的 token 降级为
  `Unknown`/忽略**，保证与更新版本宿主的向前兼容。
- `capture_begin_ns`/`capture_end_ns` 必需、非负且有序（单调钟纳秒）。
- 未知对象成员忽略（向前兼容）。password 节点携带明文 `text`、parent 不在树内、重复 id 等
  由 `validate_ui_tree_snapshot` 拒绝。
- 坐标空间由 adapter 按 display 铸造，节点 `space` 统一赋值；宿主不需要生成空间 ID。

## 2. 证据等级

| 声明 | 等级 | 证据 |
| --- | --- | --- |
| ABI 头为纯 C、无 STL/JNI/Android 类型 | 源码确认 | `platform-boundary-check` 与公共头独立包含测试（`mira_public_headers_test`） |
| struct_size/abi_version 前向兼容与 fail closed | fake host 契约测试 | `tests/m2/m2_android_host_abi_test.cpp` `check_abi_validation_and_lifecycle` |
| callback exactly-once、重复/未知回调被隔离计数 | fake host 契约测试 | 同上 `check_duplicate_and_unknown_callbacks_isolated` |
| lease 在成功、失败、越界、取消路径均恰好一次释放 | fake host 契约测试 | `check_adapter_observe_and_lease_release`、`check_oversize_lease_rejected_and_released`、`check_cancellation_and_interrupt`，并以 `FakeAndroidHost::outstanding_leases()` 断言归零 |
| `leases_released` 统计覆盖全部释放路径（bridge 内部释放、adapter observe 尾部释放、错误结算释放） | fake host 契约测试 | `check_leases_released_counts_every_release_path`（2026-09-06，BUG-20260906-001 / GitHub #12） |
| epoch 递增使在途捕获判为 StaleObservation | fake host 契约测试 | `check_epoch_invalidation_on_rotation`、`check_structure_epoch_and_capability_degradation`（树路径同语义） |
| stop 幂等、destroy 后无 lease 悬挂 | fake host 契约测试 | `check_abi_validation_and_lifecycle`、`check_adapter_shutdown_releases_everything` |
| UI 树经 ABI 聚合进 Observation（`mira.host.tree.v1` 解析、能力门控、fail closed、垃圾负载拒绝） | fake host 契约测试 | `check_structure_observation_aggregation`、`check_adapter_observe_and_lease_release`（能力缺失路径，2026-09-06，GitHub #7） |
| 手势时长经 adapter 路径传入 `duration_ms`，超宿主上限拒绝 | fake host 契约测试 | `check_input_duration_semantics`（2026-09-06，GitHub #11） |
| Artifact store 容量可注入/可配置，耗尽转为结构化错误 | fake host 契约测试 | `check_artifact_store_capacity_and_injection`（2026-09-06，GitHub #10） |
| 真实 Android Host（Kotlin/JNI）互操作：create/start/stop/destroy、capabilities/topology、capture_frame 全路径 | Runtime verified（emulator+device，外部仓库证据） | miracle 仓库 P1：OnePlus Ace 3（Android 16/API 36，arm64）与 API 35 x86_64 模拟器（ARM 翻译）上，`AndroidHostAdapter::observe` 连续两帧成功（RGBA8888，epoch 一致，无违规回调）；宿主实现为 `libmiracle_host.so`（`find_package(Mira)` 消费 0.1.0 安装包）。证据文件：miracle `docs/plans/p1-screen-capture.md`、`docs/compatibility/oneplus-ace3.md`（GitHub #13 回填） |
| lease 释放语义在真实宿主路径闭合 | Runtime verified（外部仓库证据） | 同上：宿主侧 outstanding lease 归零、destroy 无悬挂、bridge 违规计数为 0；bridge `leases_released` 统计口径漏计 observe 尾部释放路径已确认并修复（BUG-20260906-001 / GitHub #12），修复后统计覆盖全部释放路径 |

## 3. 限制与补跑条件

- M2 交付 ABI 与 Native Adapter（`mira_android_adapter`：`HostDispatcherBridge` 与
  `AndroidHostAdapter`），并以 fake host 契约测试验证。2026-09-06 起（DEC-012）capability
  快照如实映射宿主声明：`accessibility_completeness >= 1` 时声明 `ui_tree` 并聚合
  structure 组件；`0` 时 fail closed。前台与设备组件仍属后续里程碑。
- 真实设备/模拟器的 **screen 全路径** 互操作已由 miracle P1 验证（见 §2 证据行）；
  **UI 树真实宿主互操作未验证**：miracle 侧尚未实现 `mira.host.tree.v1` 序列化。
  负责人：Mira Maintainers。补跑条件：miracle 消费 DEC-012 线格式后，在受支持 Android
  环境验证 structure 聚合与能力降级，并把结果登记到本文件与维护计划
  （[maintenance-2026-09-host-abi-feedback.md](../plans/maintenance-2026-09-host-abi-feedback.md)）。
- MediaProjection 授权生命周期、Accessibility 生命周期、主线程 dispatcher、`dispatchGesture`
  输入安全的完整矩阵保持未验证（miracle P1 仅覆盖 screen 路径）。负责人：Mira Maintainers。
  补跑条件：实现完整 JNI Host bridge 后，在设计文档第 15.2/15.3 节矩阵上补跑并回填。
- ABI 数值（枚举值、status code、struct 布局）一经发布即冻结；扩展只能通过追加 struct 尾部
  字段与新枚举值进行，并递增 `host_sequence` 语义文档。`mira.host.tree.v1` 为负载层 schema，
  版本化通过 `schema` 字符串演进，不改动 ABI 布局。

## 4. 关联文档

- [M2 里程碑计划](../plans/m2-observation-simulator-android-host.md)：`M2-08`、`M2-09`
- [维护计划：Host ABI 第一轮上游反馈](../plans/maintenance-2026-09-host-abi-feedback.md)
- [DEC-012：Host Adapter 第一轮反馈契约修订](../decisions/DEC-012-host-adapter-feedback-round1.md)
- [平台构建与 Adapter 兼容性矩阵](platform-matrix.md)
