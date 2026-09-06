# DEC-012：Host Adapter 第一轮上游反馈的契约修订

> 状态：Accepted
> 日期：2026-09-06
> 负责人：Mira Maintainers
> 冻结里程碑：M4 后维护轮（[维护计划](../plans/maintenance-2026-09-host-abi-feedback.md)）
> 替代/被替代：无（补充 [DEC-005](DEC-005-observation-coordinate-host-boundary.md) 的 Host 边界细节）

## 背景与问题

[miracle](https://github.com/Linductor-alkaid/miracle)（DEC-011 独立验证载体）在真机
（OnePlus Ace 3 / PJE110，Android 16 / API 36）验证 M2 Host ABI 骨架后，提交了第一批上游
反馈（GitHub #7、#8、#9、#10、#11、#12、#13）。其中四项涉及公开契约或产品默认值，按
项目管理规范需要决策记录：

1. **Artifact 存储不可注入且容量硬编码 8 MiB**（#10）：真机单帧 852×1876 RGBA ≈ 6.4 MiB，
   多步闭环第二步即耗尽，宿主被迫降采样截图规避。
2. **`InputEvent` 不携带手势时长**（#11）：ABI 字段 `duration_ms` 已存在但恒填 0，决策层
   无法表达"慢速滑动/超长短按"，取消契约测试被迫绕开生产路径。
3. **UI 树观察缺失**（#7）：宿主声明 `accessibility_completeness >= 1` 且 ABI 已定义
   `MIRA_HOST_OP_GET_UI_TREE`，但 adapter 不消费，v1 任务只有截图 grounding。
4. **`leases_released` 统计口径**（#12）：bridge 只计内部释放路径，observe 尾部的
   lease 释放不计入，统计与宿主实际释放不一致。

## 决策

1. **Artifact 存储**：`AndroidHostAdapter::create` 增加
   `AndroidHostAdapterOptions{artifact_store, memory_artifact_capacity_bytes}`；注入
   `IArtifactStore`（含落盘后端 `FileArtifactStore`）或声明内存容量均可；未注入时默认
   `MemoryArtifactStore` 容量 **64 MiB**（原 8 MiB）。帧写入的 `max_bytes` 按实际 lease
   大小声明。容量耗尽保持结构化 `ResourceExhausted`（可重试）错误，不静默驱逐旧帧。
2. **输入时长**：`mira::InputEvent` 增加 `std::uint32_t duration_ms = 0`（默认 0 = 宿主
   默认时长，向后兼容的聚合初始化不变）；adapter 将非零值映射到
   `MiraHostInputEventV1.duration_ms`，并对照宿主 `max_gesture_duration_ms` fail closed
   （超限 `InvalidArgument`，不静默截断）。
3. **UI 树观察**：定义版本化 JSON 负载 schema **`mira.host.tree.v1`**（规范见
   [android-host-abi.md §1.2](../compatibility/android-host-abi.md)）；宿主声明
   `accessibility_completeness >= 1` 时 `capabilities().ui_tree = true`，`observe()` 聚合
   structure 组件（含 structure-only 观察），`= 0` 维持 fail closed；解析 fail closed，
   未识别 role/state/action token 降级为 `Unknown`/忽略以保持向前兼容；帧与树的组件
   聚合声明 `BoundedSkew`（两次独立异步提交，不声称原子）。
4. **Lease 统计**：`HostBridgeStats.leases_released` 口径定义为"对宿主执行的全部 lease
   释放"，覆盖 bridge 内部释放与消费方经 `HostLeaseGuard` 的延迟释放；guard 通过共享
   计数器上报，guard 生命周期超过 bridge 时计数仍安全。
5. **x86_64 预设**（#9，工程矩阵项，随本决策记录）：新增 `android-x86_64` 工具链与预设并
   纳入 CI android matrix；运行时声明仍按证据等级管理。

## 备选方案

- **容量只上调默认值不做注入**：实现最小，但落盘后端、共享存储与宿主侧预算控制仍需注入点，
  问题会以另一种形式复发。否决。
- **时长放入 payload 字符串**（如 `"0.5,0.5,3000"`）：破坏 canonical payload 的稳定语义并
  使脱敏/校验复杂化。否决。
- **UI 树用不透明字节直存 Artifact、不定义 schema**：消费方无法做元素级 grounding，等于
  没有解决 #7；且绕过 `validate_ui_tree_snapshot` 的安全校验。否决。
- **UI 树定义二进制线格式**：体积更小，但不可读、难跨宿主调试，且 mira 已有严格 JSON
  解析与 4 MiB 文档上限，树规模下无性能必要性。否决（如未来真机证明 JSON 是瓶颈再评估）。

## 影响与风险

- 公开 API 变更：`InputEvent`（追加带默认值字段）、`AndroidHostAdapter::create`（带默认
  参数的 options）、`HostLeaseGuard`（新增 observer 设置）；均为向后兼容的追加式变更。
- 默认容量 8→64 MiB 提高宿主常驻内存下限；注入点让内存受限宿主自行权衡（含落盘）。
- `mira.host.tree.v1` 成为跨仓库契约：miracle 需按规范序列化；schema 经 `schema` 字符串
  版本化，与 ABI 布局冻结解耦。
- 未识别 token 降级是显式的向前兼容让步（枚举为建议性元数据，不参与坐标/安全判定）；
  password 明文、parent 完整性、坐标范围仍强校验。
- 风险：JSON 解析成本在超大树上高于二进制；以 4 MiB 上限 + fail closed 控制在可接受范围，
  无真机证据前不做性能声明。

## 验证方式

fake host 契约测试（`tests/m2/m2_android_host_abi_test.cpp`）：
`check_leases_released_counts_every_release_path`、`check_structure_observation_aggregation`、
`check_structure_epoch_and_capability_degradation`、`check_artifact_store_capacity_and_injection`、
`check_input_duration_semantics`，及既有测试回归。真机 UI 树互操作与 x86_64 模拟器冒烟为
补跑项，登记于 [android-host-abi.md §3](../compatibility/android-host-abi.md) 与维护计划。

## 关联文档和工作项

- GitHub issues #7、#9、#10、#11、#12（#8、#13 为方向登记与文档回填，无契约变更）
- [维护计划 maintenance-2026-09-host-abi-feedback.md](../plans/maintenance-2026-09-host-abi-feedback.md)
- [android-host-abi.md](../compatibility/android-host-abi.md)（§1.1 调用约定澄清、§1.2 线格式、§2 证据）
- [DEC-005](DEC-005-observation-coordinate-host-boundary.md)、[DEC-011](DEC-011-demo-first-external-validation.md)
