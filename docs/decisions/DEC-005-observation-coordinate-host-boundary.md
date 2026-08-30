# DEC-005：Observation 坐标与 Android Host 边界

> 状态：Accepted  
> 日期：2026-08-30  
> 负责人：Mira Maintainers  
> 冻结里程碑：M2  
> 替代/被替代：无

## 背景与问题

截图、UI Tree、本地检测和平台输入可能使用不同原点、缩放、旋转和时间。Android 权限、生命周期
与线程亲和又只能由宿主掌握。若 Core 猜测平台坐标或 JNI 生命周期，会产生错误点击和悬空回调。

## 决策

- Observation 是带 capture interval 和 `environment_epoch` 的不可变聚合；每个 component 声明
  自己的时间、质量和到 canonical viewport 的变换。
- Core 的 canonical 坐标是方向归一后的 logical viewport，范围 `[0,1] x [0,1]`。原始 pixels、
  window、content、UI node 坐标均携带显式 `CoordinateSpaceId` 和变换链。
- 无法证明同一快照的 screenshot/tree/detection 标记为非原子组件，Planner 按 freshness policy
  使用，不能伪装成同步事实。
- Android Host 拥有 Activity/Service、JNI、MediaProjection、Accessibility 和主线程 dispatcher；
  NDK library 不保存未经宿主生命周期保护的 JNI local reference。
- Host 以版本化、句柄式 ABI 向 Adapter 提供 frame/tree/input/interrupt 和 capability；权限撤销、
  surface 变化和宿主销毁必须递增 epoch、解除阻塞并使旧动作失效。

## 备选方案

- 所有坐标都使用屏幕像素：无法稳定跨旋转、缩放和 ROI，不采用。
- Core 直接调用 JNI/Android SDK：破坏平台独立性，不采用。
- 假设 screenshot 与 Accessibility Tree 原子同步：多数平台无法保证，不采用。

## 影响与风险

每个 Adapter 必须实现 transform contract tests 和 epoch 失效测试。视觉检测输出保留来源 frame 和
transform provenance，不能只返回裸 `x/y`。

## 验证方式

- Simulator 覆盖旋转、裁剪、letterbox、inset、多 display、非原子组件与 epoch 失效：M2 已由
  `tests/m2/m2_simulator_scenario_test.cpp` 与 `tests/m2/m2_observation_coordinate_test.cpp` 验证。
- Android Host 边界：M2 冻结 `include/mira/adapters/android/host_abi.h` 并以 fake host 契约测试
  （`tests/m2/m2_android_host_abi_test.cpp`）验证 callback exactly-once、lease 恰好一次释放与
  epoch 递增失效；证据等级见 [android-host-abi.md](../compatibility/android-host-abi.md)。
- 真实 Android 前后台、权限撤销、Host 销毁、JNI 线程 attach/detach 和输入安全释放：待 JNI
  Host bridge 交付后在目标环境补跑。

## 关联文档和工作项

- [Observation、坐标与 Android Host ABI 设计](../design/observation_coordinate_android_host.md)
- [Mira 实施总计划](../plans/mira-implementation-plan.md)：M2

