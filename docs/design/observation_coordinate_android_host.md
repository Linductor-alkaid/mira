# Mira Observation、坐标与 Android Host ABI 设计

> 状态：Active  
> 版本：1.0  
> 更新日期：2026-08-30  
> 适用范围：Observation Pipeline、屏幕/UI 结构、本地感知证据、输入坐标和 Android Host/NDK 边界  
> 上位决策：[DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)

## 1. 目标

本设计解决三个容易被错误合并的问题：

1. Screenshot、UI Tree、OCR/检测和设备状态可能来自不同时间，如何组成诚实的 Observation。
2. 模型/检测/平台输入使用不同坐标空间时，如何可靠映射并使陈旧坐标失效。
3. Android 权限、生命周期和线程亲和属于 Host，如何通过稳定 ABI 让 C++ Core 使用能力而不直接
   依赖 JNI/Android SDK。

核心原则：不把“尽量同时采集”描述为原子快照，不返回没有空间来源的裸坐标，不让 JNI 对象或
Android 类型进入 Mira Core。

## 2. Observation 聚合模型

### 2.1 CaptureSpan 与 Component

```cpp
struct CaptureSpan {
    ClockDomainId clock_domain;
    std::int64_t begin_ticks;
    std::int64_t end_ticks;
    Timestamp normalized_begin;
    Timestamp normalized_end;
    ClockSyncQuality sync_quality;
};

template <typename T>
struct ObservationComponent {
    T value;
    CaptureSpan capture;
    ComponentQuality quality;
    Provenance provenance;
    EnvironmentEpoch environment_epoch;
};

struct Observation {
    ObservationId id;
    SessionId session_id;
    EnvironmentEpoch environment_epoch;
    CaptureSpan aggregate_span;
    ObservationAtomicity atomicity;
    DisplayTopology topology;
    std::optional<ObservationComponent<ScreenFrameRef>> screen;
    std::optional<ObservationComponent<UiTreeSnapshot>> structure;
    std::vector<ObservationComponent<PerceptionEvidence>> perception;
    std::optional<ObservationComponent<AppContext>> foreground;
    std::optional<ObservationComponent<DeviceState>> device;
    ObservationQuality quality;
};
```

`ObservationAtomicity`：

- `Atomic`：平台以同一事务/帧 ID 保证组件属于同一快照。
- `BoundedSkew`：组件不原子，但最大 capture skew 在 capability 声明范围内。
- `NonAtomic`：无法给出可靠 skew，Planner 必须按组件新鲜度使用。

Mira 不因组件在同一次 `observe()` 返回就标记 `Atomic`。

### 2.2 ObservationRequest

```cpp
struct ObservationRequest {
    ObservationMode mode;              // Full | Verification | Diff | Roi
    RequiredComponents required;
    OptionalComponents optional;
    std::vector<RegionRef> regions;
    std::optional<ObservationId> baseline;
    std::chrono::milliseconds max_age;
    std::chrono::milliseconds max_component_skew;
    ArtifactPolicy artifact_policy;
};
```

- required component 缺失使 request 失败或明确 partial，不得静默当完整 Observation。
- Verification request 只采集能够证明 ExpectedOutcome 的最小证据。
- ROI 必须引用 source Observation/space/transform；不能拿旧 ROI 直接应用到新 epoch。
- Adapter 不能满足 skew 时返回实际值和质量，由 Core 决定重新采集或降级。

### 2.3 聚合顺序

建议 Pipeline：

```text
read environment epoch/topology
 -> start capture span
 -> request screen/tree/device in platform-supported order
 -> validate all returned epochs/topology
 -> publish immutable artifacts
 -> optional screen diff/local perception
 -> calculate skew/quality
 -> construct immutable Observation
```

采集中 epoch 改变则丢弃组件并返回 `StaleObservation`。部分 Provider 超时不应无限拖住其他组件；
结果按 required/optional 和 deadline 结算。

## 3. ScreenFrame 契约

```cpp
enum class PixelFormat { RGBA8888, BGRA8888, RGBX8888, NV12, YUV420P, Gray8 };
enum class ColorSpace { SRGB, DisplayP3, BT709, Unknown };

struct PlaneLayout {
    std::uint64_t offset;
    std::uint32_t row_stride;
    std::uint32_t pixel_stride;
    std::uint32_t width;
    std::uint32_t height;
};

struct ScreenFrameDescriptor {
    FrameId frame_id;
    DisplayId display_id;
    std::uint32_t width_pixels;
    std::uint32_t height_pixels;
    PixelFormat pixel_format;
    ColorSpace color_space;
    AlphaMode alpha_mode;
    Rotation native_rotation;
    std::vector<PlaneLayout> planes;
    CoordinateSpaceId pixel_space;
    CaptureSpan capture;
    ArtifactRef payload;
};
```

- stride 不能假设等于 width × bytes-per-pixel。
- YUV range/matrix 和 HDR transfer 未声明时，视觉 Provider 必须降级或先规范化。
- screenshot 是否包含 system bars、cutout、IME、secure surface 和 overlay 由 `FrameCoverage` 明确。
- 被平台遮蔽/禁止捕获的区域标为 mask/quality，不以黑色像素推测真实内容。
- Artifact 发布后 Frame immutable；buffer 生命周期和零拷贝 handle 不跨持久化契约。

首期 Core 视觉规范格式建议 sRGB RGBA8888，但 Adapter 可以返回其他格式，由受管 preprocessing
阶段转换。该建议不是要求平台总是复制整帧。

## 4. DisplayTopology 与坐标空间

### 4.1 空间定义

每个空间都有 ID、extent、axis、orientation 和 parent transform：

| 空间 | 含义 |
| --- | --- |
| `NativeDisplayPixels` | 物理 display 当前原生像素 |
| `LogicalDisplay` | OS logical/dp-like 显示空间 |
| `Window` | 目标窗口边界空间 |
| `ContentViewport` | 去除声明的 system inset 后的可交互内容区 |
| `CanonicalViewport` | 方向归一、范围 `[0,1]²` 的 Mira 动作空间 |
| `FramePixels` | 某个 screenshot artifact 的 pixels |
| `UiNodeLocal` | 某个结构节点局部坐标 |
| `RoiLocal` | 某个 ROI 图像局部坐标 |

canonical viewport 是每个 display/window context 独立的，不存在跨所有 display 的一个全局 `[0,1]²`。
Action 必须携带 display/window target。

### 4.2 Transform

```cpp
struct CoordinateTransform {
    CoordinateSpaceId from;
    CoordinateSpaceId to;
    Matrix3x3 homogeneous;
    RectF valid_source_region;
    RectF valid_target_region;
    TransformQuality quality;
    EnvironmentEpoch environment_epoch;
};
```

二维 affine/perspective 变换使用齐次矩阵。首期平台输入只接受可验证 affine chain；若 perspective
来源于远程桌面/相机投影，Controller 必须使用专门校准并限制有效区域。

坐标转换：

1. 查找同一 epoch 的完整 transform chain。
2. 校验 source point/box 在 valid region 内。
3. 变换四角/中心；box 不能只变换左上右下而忽略旋转。
4. 根据 input capability 处理 rounding，并记录误差 bound。
5. 最终坐标执行前再次核对 display topology hash 与 environment epoch。

缺少链、quality 不足、矩阵奇异或误差超过 Policy threshold 返回 `InvalidObservation/StaleObservation`，
不能猜测 scale。

### 4.3 ElementRef

```cpp
struct ElementRef {
    ObservationId observation_id;
    EnvironmentEpoch environment_epoch;
    ElementSource source;       // UiTree | OCR | Detector | Fused
    StableNodeHint stable_hint; // 非永久 identity
    RectF bounds;
    CoordinateSpaceId space;
    Sha256 evidence_digest;
};
```

ElementRef 只在其 Observation/epoch 和 freshness policy 内有效。Accessibility node ID 通常不能跨
窗口更新作为永久 ID；执行前优先重解析可验证的 node/action，失败则重新 Observe。

## 5. UI Tree 契约

```cpp
struct UiNode {
    UiNodeId id;
    std::optional<UiNodeId> parent;
    Role role;
    std::string text;              // sensitivity/redaction metadata 单独携带
    std::string content_description;
    RectF bounds;
    CoordinateSpaceId space;
    UiStateFlags state;
    UiActionFlags supported_actions;
    std::optional<StableNodeHint> stable_hint;
    Provenance provenance;
};
```

- Tree 声明是否完整、被截断、可见节点-only、最大 depth/nodes 和 capture span。
- 文本是不可信外部数据；password/secure 字段不收集明文。
- `clickable=true` 是平台 capability，不是 Policy 授权。
- 增量 Tree 必须引用 base snapshot ID 和 sequence；base 不存在时拒绝并请求 Full。
- Tree bounds 映射到 canonical viewport 时保留 transform provenance。

## 6. 环境 Epoch 与失效

以下至少递增 `EnvironmentEpoch`：

- display 添加/移除、旋转、分辨率、density、zoom 或窗口 bounds 变化。
- 前台应用/目标窗口 identity 变化。
- MediaProjection/Accessibility session 重建。
- Host pause/resume 造成 frame/input domain 不连续。
- Human Takeover release。
- Adapter 明确无法证明旧坐标仍有效的任何事件。

IME、system bar、overlay 或 transient animation 是否递增 epoch 由 Adapter capability 声明；若不递增，
必须体现在 topology/inset/Observation freshness 并让动作前校验发现变化。

## 7. Android 分层

```text
Android App / Service (Kotlin/Java)
├── permissions and user consent
├── MediaProjection / AccessibilityService
├── lifecycle and foreground service
├── trusted confirmation UI
├── main-thread / service dispatchers
└── JNI Host Bridge
       ↓ versioned C ABI
Native Android Adapter
       ↓ platform-neutral interfaces
Mira Core
```

Core 和 `mira_core` target 不包含 `<jni.h>` 或 Android headers。JNI 只存在 Host bridge target；Native
Adapter 将 Host ABI 数据转换成 Mira 类型，不包含 Planner/Policy。

## 8. Android Host ABI

### 8.1 ABI 原则

- C ABI、显式 `struct_size` 和 `abi_version`；不跨边界传 STL、异常、RTTI 或所有权不明指针。
- 所有 async operation 有 64-bit/128-bit opaque operation handle，并且 exactly one terminal callback。
- callback 可以来自 Host 声明的线程，Native bridge 只做有界复制/引用获取和 Executor completion
  submission，不运行 Core 状态机。
- 字符串为 UTF-8 pointer+length，不依赖 NUL；所有数组有 count 和上限。
- Native 不保留 JNI local ref。需要的 Java 对象由 Host 用 global/weak global ref 管理，并在 destroy
  前终止 operation；线程 attach/detach 由 JNI bridge 明确处理。
- ABI 方法不让 C++ exception 越界，返回稳定 status code 和可选 safe diagnostic。

### 8.2 生命周期接口草案

```c
typedef struct MiraAndroidHostV1 MiraAndroidHostV1;
typedef struct MiraAndroidCallbacksV1 MiraAndroidCallbacksV1;

MiraHostStatus mira_android_host_create_v1(
    const MiraAndroidHostConfigV1* config,
    const MiraAndroidCallbacksV1* callbacks,
    MiraAndroidHostV1** out_host);

MiraHostStatus mira_android_host_start_v1(MiraAndroidHostV1* host);
MiraHostStatus mira_android_host_stop_v1(MiraAndroidHostV1* host);
MiraHostStatus mira_android_host_destroy_v1(MiraAndroidHostV1* host);
```

实际产品更常由 Java 创建能力后注册到 native；以上表达的是 Native Adapter 可依赖的稳定语义，
不是要求 Java 调用方向固定。`destroy` 仅在所有 callback lease/buffer release 后完成；重复 stop/destroy
幂等。

### 8.3 能力与事件

Host capability snapshot 至少包含：

- screenshot backend、pixel formats、max dimensions、secure surface behavior。
- Accessibility tree completeness、supported node actions。
- tap/swipe/gesture/multi-pointer、最大 duration/points、cancel/release 能力。
- main/service thread dispatcher、callback thread model。
- lifecycle state、permission state、display topology version。
- trusted confirmation UI 和 identity strength。

能力、权限或 topology 变化以单调 host sequence 发送。Native Adapter 更新 environment epoch，并使
相关 operation/ActionLease 失效。

## 9. Buffer 所有权

### 9.1 BufferLease

```c
typedef struct {
    uint64_t lease_id;
    const uint8_t* data;
    uint64_t size;
    uint32_t plane_count;
    MiraPlaneV1 planes[MIRA_MAX_PLANES];
    void (*release)(uint64_t lease_id, void* user_data);
    void* user_data;
} MiraHostBufferLeaseV1;
```

- callback 交付时 Native 获取一次 lease；完成复制/预处理/Artifact commit 后 exactly once release。
- Host stop 等待 outstanding lease 到 deadline；超时进入 degraded/unsafe report，不能释放仍被 native
  访问的内存。
- 零拷贝 AHardwareBuffer 等是可选扩展，由 capability/extension struct 表达，不进入 Core 类型。
- Native 不能跨长期 Task 保存平台 buffer；长期保存先转 ArtifactStore。
- size、plane offset/stride 和映射范围在访问前验证，防止整数溢出和越界。

## 10. Host dispatcher 与 Executor

平台 API 需要主线程/AccessibilityService 线程时：

1. Mira operation 由 ExecutionSupervisor 注册并在 Executor 路径上发起。
2. Android Adapter 通过 Host dispatcher 提交一个带 operation ID 的有界请求。
3. Host 在正确线程调用平台 API。
4. 结果 callback 只做边界校验并提交 Executor completion。
5. Task 状态最终在 Runtime 串行控制面提交。

Host dispatcher 是外部事件循环扩展边界，不是 Mira 私有调度器。Adapter 不自行创建线程。Host
调用迟迟不返回时，cancel 调用必须能使 callback 最终以 Cancelled/DeadlineExceeded 结算；平台
无法取消的原子调用在返回后标记 stale，不能改变旧 Task 状态。

## 11. 截图、Accessibility 与输入流程

### 11.1 Screenshot

- Android Host 负责 MediaProjection 用户授权、foreground service、ImageReader 和系统资源关闭。
- 每次 frame 带 display metrics/topology version、capture timestamp、format/planes 和 buffer lease。
- `onStop`、projection revoke 或 ImageReader error 立即发布 permission/capability change 并解除等待。
- secure surface/blank region 进入 FrameCoverage，不伪装成正常黑屏。

### 11.2 Accessibility Tree

- Host 在 AccessibilityService 合法线程读取 root/node，复制为有界 POD/serialized snapshot。
- 不把 `AccessibilityNodeInfo` 指针跨 JNI 或跨 callback 保存。
- 节点数量、深度、文本长度和序列化总大小有上限；超限标记 truncated。
- node action 和全局 action receipt 区分“平台接受”与“目标完成”。

### 11.3 Input

- Native Controller 输出 canonical InputSequence，Android Adapter 映射到 dispatchGesture/global action。
- Host 返回 accepted/rejected/completed/cancelled/unknown receipt 和 side-effect-may-have-occurred。
- Android API 不能保证 mid-gesture 逐 sample 更新时，Adapter 声明 chunk/whole-gesture capability，
  Controller 按能力编译，不能假装具有 streaming input。
- `release_all`/interrupt 幂等；若平台无法显式释放已派发 gesture，依赖 bounded duration/watchdog 并
  将安全释放标为未确认。

## 12. Lifecycle 状态

| Android 事件 | Host 行为 | Mira 行为 |
| --- | --- | --- |
| Activity recreate | 保留/重建 service-owned capability，更新 Host generation | epoch++，旧坐标失效 |
| App background | 按权限停止或降级 capture/input | pause/fail active Task，释放输入 |
| MediaProjection revoke | 关闭 frame producer，结算 pending capture | capability change，停止视觉动作 |
| Accessibility disabled | 结算 tree/input，释放 node refs | permission denied，request Human/fail |
| Service destroyed | stop producer、interrupt、等待 callbacks/leases | Session close/unsafe report |
| Display rotation/fold | 发布新 topology version | epoch++，丢弃旧 plan/ElementRef |
| Process recreation | 新 HostInstanceId，不恢复旧 handles | Event recovery，新 Session/Adapter |

Host generation、HostInstanceId 和 operation handle 共同防止旧 Java callback 注入新 Runtime。

## 13. 错误语义

Host status 映射到 Mira 稳定错误：permission denied、temporarily unavailable、unsupported、cancelled、
deadline、invalid buffer/topology、platform error 和 execution uncertain。平台 message 进入受保护
diagnostic 并先脱敏；Core 不按 Android exception 字符串做业务分支。

Callback 重复、未知 operation、错误 generation、buffer 越界和 callback after destroy 都记录为
contract violation。它们不能让 Task 状态改变，严重时隔离 Adapter/Session。

## 14. 安全与隐私

- Android permission grant 不等于 Mira capability grant；两者都必须满足。
- Screenshot/UI Tree 发送远端模型前执行 RedactionPolicy；secure/password node 不导出明文。
- Host confirmation UI 应阻止被 Agent 自己操作，并在平台支持时使用 secure surface/overlay 检测。
- JNI method/field 查找和 native registration 在初始化验证，失败不半初始化 Runtime。
- 外部 Intent、URI 和 Accessibility event 文本均是不可信输入。
- frame/tree buffer 上限、压缩比和解析深度防止内存/CPU DoS。

## 15. Contract Tests

### 15.1 坐标

- 0/90/180/270 度、density、letterbox、system inset、cutout、窗口缩放和多 display。
- FramePixels -> Canonical -> platform input 往返误差在声明 bound 内。
- ROI、UI node、OCR/detection box 使用正确 source transform；旧 epoch 一律拒绝。
- 奇异/缺失/错误方向 transform 和非原子组件不得产生错误点击。

### 15.2 ABI 与生命周期

- struct_size/abi version 前后兼容、未知 extension 安全忽略。
- callback exactly once、duplicate/late/wrong-generation 被隔离。
- buffer lease 正常、取消、超时、Host stop 和异常路径 exactly once release。
- Activity recreate、projection revoke、Accessibility disable、service/process destroy。
- 主线程 dispatcher 阻塞、取消和返回后 stale completion。

### 15.3 输入安全

- dispatchGesture accepted 后 callback 丢失、取消、Takeover 和 shutdown。
- release_all 支持/不支持两类 capability，ShutdownReport 准确区分。
- multi-pointer、max points/duration 和 chunk 编译不超过平台能力。

## 16. 性能验证

记录 capture-to-artifact、tree snapshot、JNI/ABI callback、preprocess、coordinate compile、dispatch 和
receipt 延迟 P50/P95/P99；同时记录分辨率、格式、设备、Android API、thermal state、Executor 配置
和是否复制。零拷贝收益未经实测不能作为接口复杂度依据。

## 17. 尚待冻结的 Android 实现选择

- 最低 Android API/NDK 版本与支持 ABI。
- MediaProjection 与 Accessibility 组合的产品授权流程。
- JNI 注册方案和 Host C ABI 的具体调用方向。
- AHardwareBuffer/ImageReader 零拷贝扩展。
- dispatchGesture 分块与连续摇杆可达到的稳定频率。

这些选择不能改变 Core 不依赖 Android、显式 buffer lease、callback 结算、epoch 失效和坐标
provenance 原则。

## 18. 关联文档

- [核心公共契约与状态机](core_contracts_and_state_machine.md)
- [本地感知与任务模型](local_perception_and_task_models.md)
- [威胁模型与确认协议](../security/threat_model_and_confirmation.md)
- [M2 里程碑计划](../plans/m2-observation-simulator-android-host.md)
- [Android Host ABI 兼容性记录](../compatibility/android-host-abi.md)

### 18.1 M2 落地映射（实现状态）

本设计中的结构为规范契约，字段命名以头文件为准。截至 M2 收尾：

- 坐标与 Observation 契约：`include/mira/coordinates.hpp`、`include/mira/observation.hpp`，
  实现在 `src/observation/`。
- `IEnvironment`（capabilities、request 驱动 `observe/execute/interrupt`、`OperationContext`）：
  `include/mira/environment.hpp`；Runtime、Replay、示例与测试已迁移。
- Observation Pipeline 聚合器（deadline 结算、partial 降级、skew/质量计算、发布回调）：
  `include/mira/observation_pipeline.hpp` 与 `src/observation/observation_pipeline.cpp`。
  Pipeline 永不标记 `Atomic`：它无法证明平台事务，只有平台环境自身可以。
- Simulator 参考环境（旋转、density、letterbox、inset、多 display、非原子组件、epoch 失效
  注入、契约夹具）：`include/mira/adapters/simulator/simulator_environment.hpp`。
- Android Host ABI v1：`include/mira/adapters/android/host_abi.h`（纯 C）。
- Host dispatcher 桥与 Native Android Adapter 骨架：`include/mira/adapters/android/`
  `host_dispatcher.hpp`、`android_host_adapter.hpp`；fake host 契约测试位于
  `tests/m2/m2_android_host_abi_test.cpp`。
- 第 8.2 节的生命周期接口草案已按实现调整为：create/start/stop/destroy 加 capability/topology
  查询与三类异步操作；请求侧携带 native `correlation`，lease 结构体仅在回调期间有效。

