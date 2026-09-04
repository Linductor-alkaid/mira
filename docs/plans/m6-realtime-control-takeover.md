# M6：连续控制、实时路径与 Human Takeover

> 状态：Cancelled（2026-09-05，[DEC-011](../decisions/DEC-011-demo-first-external-validation.md)：原范围终止，不再按本文件交付）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M2、M5
> 建议发布点：无（原定 Control beta 随 DEC-011 取消）
> 更新日期：2026-09-05

## 1. 目标

交付 bounded-latency soft realtime 连续控制路径：把已验证、有最大时长的 Joystick、Drag 和
TouchTrajectory Intent 编译为有界输入轨迹，通过 Executor realtime 能力执行，并在取消、pause、
Human Takeover、环境变化、反馈丢失、watchdog 或 shutdown 时安全释放所有 pointer。

Control beta 只声明在明确参考环境和已测 profile 下的能力，不作未经目标设备实测的硬实时承诺；
ControlReceipt 只证明执行/释放结果，任务成功仍由后续 Observe/Verify 判定。

## 2. 范围与非目标

### 2.1 范围

- 连续 Intent、ControllerPlan、InputCapabilitySnapshot、ControlReceipt 与稳定错误。
- 轨迹编译、PointerSession 所有权、Joystick/Drag/TouchTrajectory。
- Executor realtime handle/channel、LatestMailbox feedback、watchdog 和有界 telemetry。
- ActionLease、durable dispatch、pause/cancel/Takeover/shutdown 与不确定释放。
- Simulator/fake Adapter 契约、Android Host ABI 连续输入扩展和平台 capability 降级。
- 控制正确性、故障注入、soft realtime benchmark 与安全门禁。

### 2.2 非目标

- 在 realtime callback 中调用 LLM/VLM、ONNX、磁盘、网络、Observer 或动态分配。
- 让 Controller 制定任务目标、授予权限或跳过 ActionValidator/Policy。
- 用普通 timer 模拟 fixed-period realtime 控制。
- 在 M6 宣称 Android/Windows/Linux 生产输入支持；真实平台矩阵与发布证据在 M7 完成。
- 将无法取消的平台原子手势伪装为逐周期 streaming。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M2 的 Observation、坐标、environment epoch 与 Android Host ABI v1 契约保持兼容。
- M5 提供绑定 epoch、时间质量和 OOD 的 `StateEstimate`/feedback；M6 不同步等待感知推理。
- 实现前复核固定 Executor 版本的 realtime capability card、channel/drop/status、allocation guard、
  stop 与 shutdown API；若不能表达所需语义，先登记 Executor 反馈。
- Android Host ABI 扩展遵守追加字段/新版本和旧 host fail-closed 规则，不破坏 ABI v1 数值布局。

### 3.2 设计与决策依据

- [实时控制层设计](../design/realtime_control_design.md)
- [Mira Runtime 设计](../design/mira_runtime_design.md)
- [核心公共契约与状态机设计](../design/core_contracts_and_state_machine.md)
- [Observation、坐标与 Android Host ABI 设计](../design/observation_coordinate_android_host.md)
- [本地感知与任务模型设计](../design/local_perception_and_task_models.md)
- [评估与基准体系设计](../design/evaluation_and_benchmark_design.md)
- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
- [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md)、
  [DEC-004](../decisions/DEC-004-security-authority-confirmation.md)、
  [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)、
  [DEC-006](../decisions/DEC-006-local-perception-task-models.md)

## 4. 工作项

### 4.1 契约、计划与轨迹

- [ ] `M6-01` 冻结连续 ActionIntent、ControllerPlan、ControlTarget、FeedbackSnapshot、
  InputCapabilitySnapshot、ControlReceipt、stop reason 和 schema/version；公共 API 不暴露平台事件。
- [ ] `M6-02` 实现 ActionValidator 与 ControllerPlan builder：绑定 Task/environment epoch、Observation
  transform、ActionLease、capability snapshot、duration/rate/safety envelope/stop predicates。
- [ ] `M6-03` 实现有界 trajectory compiler 和属性测试：easing、dead zone、速度/jerk、边界 clamp、
  sample/pointer/duration 上限、monotonic offset、whole-gesture 与 streaming chunk。
- [ ] `M6-04` 实现 PointerSession 状态机和唯一 owner；Down 不确定、chunk 失败、取消和重复回调均产生
  exactly one logical Up/Cancel/release attempt，新 Controller 不复用未收敛 pointer。

### 4.2 Realtime 执行与反馈

- [ ] `M6-05` 基于 Executor realtime executor 实现 RealtimeController owner、start/stop/status/handle；
  每个 callback 使用预分配资源，禁止锁、阻塞、动态分配和无界循环，并启用适用 allocation guard。
- [ ] `M6-06` 实现有界 control channel 与独立不可丢 stop path；target 使用 KeepLatest 并统计 overwrite，
  InputChunk queue full 明确 stop/降频/拒绝，不补发过期 sample。
- [ ] `M6-07` 实现预分配 LatestMailbox/DoubleBuffer feedback：sequence 单调、完整 snapshot、epoch/freshness/
  clock uncertainty/OOD 门禁，feedback 不可用时按 profile Stop/HoldNeutral/有限 open-loop。
- [ ] `M6-08` 实现分层 watchdog：target heartbeat、controller cycle、input receipt 和 host watchdog；
  同时 stop 条件使用固定优先级，保留首因和全部 flags。

### 4.3 动作、生命周期与平台契约

- [ ] `M6-09` 实现 Joystick Controller：center/direction/magnitude/response curve、有界 duration、target
  overwrite、过期 target neutral/stop 和结束 release。
- [ ] `M6-10` 实现 Drag 与自定义 TouchTrajectory：执行前重验元素/坐标，限制 waypoint/sample/pointer/
  pressure/region；模型不能提交原生平台事件。
- [ ] `M6-11` 将连续动作接入 ActionJournal/ExecutionSupervisor：durable start ack 后才能 arm，结果为
  terminal/uncertain receipt，随后强制重新 Observe/Verify，禁止因输入 API 成功判任务完成。
- [ ] `M6-12` 闭合 pause、cancel、deadline、Takeover 与 shutdown：停止 producer、撤销 lease、stop
  Controller、release_all（需要时）、有界等待 receipt；未确认释放必须阻止 clean settlement。
- [ ] `M6-13` 升级 Simulator/fake input Adapter，覆盖 streaming/whole gesture/cancel/release_all/receipt
  capability、旋转/epoch、permission revoke、host crash 与故障注入。
- [ ] `M6-14` 设计并实现兼容的 Android Host ABI 连续输入扩展与 Native Adapter 骨架；旧 Host 诚实
  返回 capability mismatch，fake host 验证 callback/lease/stop/destroy，不以骨架宣称真机支持。

### 4.4 测试、基准与文档

- [ ] `M6-15` 建立 realtime 安全测试：callback allocation/lock/block、cycle overrun、channel full、
  stale feedback、同时 stop、late completion 和 teardown 后回调。
- [ ] `M6-16` 在 Down/Move/chunk/receipt 每个边界注入 cancel/pause/Takeover/shutdown，并验证终态幂等、
  ActionLease 撤销、pointer release 和 `unsafe_release_unconfirmed`。
- [ ] `M6-17` 建立控制 benchmark：target-to-input、jitter、deadline miss、stop-to-release、feedback age、
  CPU/thermal/功耗、drop 和 shutdown P50/P95/P99/max；按平台、设备、API、period、负载和 Executor
  配置记录，冻结 Control beta 参考 profile 阈值。
- [ ] `M6-18` 更新 Action/Host ABI/安全/兼容性/benchmark 文档、示例和 Control beta 发布说明，回填
  全部验证证据与未运行平台补跑条件。

## 5. Executor 路由与关闭

| 工作 | Executor 能力 | Owner | 约束 |
| --- | --- | --- | --- |
| plan/trajectory 构建与校验 | `submit_auto()` | Control supervisor | 有界 CPU，future 消费 |
| fixed-period callback | realtime executor | RealtimeController | 预分配、无锁/阻塞/动态分配 |
| target/feedback | bounded realtime channel/mailbox | Controller/Perception | KeepLatest + 不可丢 stop flag |
| Host input wait | Adapter external-loop/blocking boundary | Input Adapter | 可解除等待、receipt terminal |
| state/event commit | Runtime 串行控制面 | TaskCoordinator | callback 不直接写 store/observer |

关闭必须先停 target/feedback producer，再 stop Controller 并安全释放 pointer，回收 realtime handle 与
Host operation，结算普通 completion，最后由非 worker owner 关闭 Executor。默认池 idle 不能作为
realtime 已停止的证据。

## 6. 风险与阻塞

- `RISK-2026-025`：Executor realtime API、目标 OS 调度与平台输入 API 的周期语义可能不匹配。
  Owner：M6 control owner。解除条件：capability 复核、参考实现、过载/停止实测；通用缺口登记 `EXE-*`。
- `RISK-2026-026`：Android whole-gesture/cancel 能力不足可能无法保证快速释放。Owner：M6 Android
  owner。缓解：短有界 chunk、max duration、Host watchdog、capability fail closed 和未确认释放报告。
- `RISK-2026-027`：高频 feedback 可能诱导同步 ONNX 或跨线程可变状态进入 callback。Owner：M6/M5
  owners。缓解：只读不可变 plan 与 LatestMailbox snapshot，allocation/lock/block 测试。
- `RISK-2026-028`：Takeover UI/宿主身份在真实平台未验证。Owner：M7 platform owner。M6 只证明 Core/
  fake host 协议；真实可信 UI 与输入释放是 M7 发布门禁。

## 7. 测试矩阵

| 维度 | 必测场景 |
| --- | --- |
| 轨迹 | endpoint、duration、offset、velocity/jerk、boundary、sample/pointer 上限 |
| Pointer | 全状态转换、Down 不确定、重复 receipt、exactly one release |
| Channel | overwrite、full、drop、stop 不丢、sequence/epoch/freshness |
| Watchdog | heartbeat、cycle、receipt、host，各条件同时发生 |
| 生命周期 | pause/cancel/Takeover/shutdown × Down/Move/chunk/receipt |
| 平台降级 | whole gesture、无 Cancel、无 release_all、rotation、permission/host loss |
| Realtime | allocation/lock/block guard、jitter、deadline miss、overload stop |
| Verify | receipt 成功/不确定后均重新观察，旧 feedback 不判成功 |

## 8. 退出条件

- [ ] `M6-01` 至 `M6-18` 全部完成并有可复现验证记录。
- [ ] 所有连续动作有最大 duration、资源上限、watchdog、ActionLease 和 Verify；模型不能发原生事件。
- [ ] 每个停止/故障路径尝试唯一逻辑 release；未确认释放明确报告且不产生 clean shutdown。
- [ ] realtime callback 无动态分配、锁、阻塞、I/O、模型、Observer 或 EventStore 调用。
- [ ] rejection、异常、取消、epoch/lease 变化、channel full、overrun 和 late completion 测试通过。
- [ ] Control beta 参考 profile 有完整 benchmark manifest 和阈值；未实测平台不宣称实时能力。
- [ ] ASAN/UBSAN、适用 TSAN 与 realtime guard 通过；Executor/Adapter handle 全部结算。
- [ ] ABI、设计、安全、兼容性、基准、计划和发布材料同步。

## 9. 验证记录

2026-09-01：依据总计划和实时控制设计创建详细计划；状态为 `Planned`，尚未实现 Controller 或
Android 连续输入 ABI，未执行 realtime benchmark。负责人为 Mira Maintainers；准入条件为 M5 提供
可用的本地 feedback 契约并完成 Executor realtime capability 复核。

2026-09-05：按 [DEC-011](../decisions/DEC-011-demo-first-external-validation.md) 终止 M6 原范围，
状态改为 `Cancelled`。终止原因：本里程碑假设任务需要 bounded-latency 反馈控制且其前置必须
先交付 M5 的本地感知，该假设链未经真实任务验证；连续控制的真实需求（频率、反馈可用性、
延迟预算、是否开环有界手势即可满足）转由独立仓库的 demo 产品量化。M6 未开始实现，无已
交付内容受影响；工作项 `M6-01` 至 `M6-18` 保持未勾选，退出条件不再适用。本文件与设计文档
（[实时控制层设计](../design/realtime_control_design.md)）保留为假设记录；如 demo 证据要求
连续控制能力，以新的或重定义的里程碑重新提案，编号不复用 `M6-*`。
