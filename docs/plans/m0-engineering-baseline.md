# M0：仓库、构建与 Executor 工程基线

> 状态：Completed
> 负责人：Mira Maintainers  
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)  
> 前置：无  
> 建议发布点：内部工程基线  
> 更新日期：2026-08-30

## 1. 目标

建立可重复构建、测试和诊断的 C++20 空骨架，验证 Mira 对 Executor 的所有权、串行控制、
取消和关闭假设，并从构建组合上锁定 Linux、Windows、Android 三个目标族。M0 不交付真实模型
或平台输入自动化。

## 2. 范围与非目标

范围包括顶层构建、目标分层、质量工具、依赖锁定、Simulator/Fake 基础、Executor integration
spike、Linux/Windows/Android 构建入口和 CI 门禁。非目标包括完整 Task 闭环、生产 EventStore、
真实 Android/Linux/Windows Adapter 和 ONNX 推理。

## 3. 设计与决策依据

- [Mira Runtime 设计](../design/mira_runtime_design.md)
- [DEC-001：Runtime 的 Executor 所有权与串行控制面](../decisions/DEC-001-runtime-executor-ownership.md)
- [项目管理与文档规范](../project/project_management_and_documentation.md)

## 4. 工作项

### 4.1 仓库与构建

- [x] `M0-01` 建立顶层 CMake、C++20 编译基线以及 Debug、Release、ASAN、UBSAN、TSAN 预设。
- [x] `M0-02` 建立 `include/mira/`、`src/`、`adapters/simulator/`、`tests/`、`cmake/` 和 `tools/`
  骨架，目录依赖与 Runtime 设计一致。
- [x] `M0-03` 建立 `mira_core`、`mira_simulator_adapter` 和最小 consumer targets；公开头不包含平台
  SDK 或具体 HTTP/ONNX 类型。
- [x] `M0-04` 配置 warning、format、静态检查和测试标签，CI 不以“没有测试”冒充成功。

### 4.2 Executor 集成原型

- [x] `M0-05` 显式创建实例化 `executor::Executor`，在首次提交前初始化，并由非 worker owner
  完成最终 `shutdown(true)`。
- [x] `M0-06` 原型验证每个 Runtime 一个 `executor::SerialExecutionContext`：多 Task 命令和
  completion 按接纳顺序提交，回调只做有界状态提交且不等待 future。
- [x] `M0-07` 验证 `submit_on_with_handle()` 的排队取消、异常传播、上下文关闭拒绝和迟到
  completion 隔离；活动路径因 `EXE-20260830-002`、`EXE-20260830-003` 改由非阻塞 tracked
  dispatch compatibility boundary 承载，保存并消费全部 dispatch/business future。
- [x] `M0-08` 验证关闭顺序：停止 producer、取消 operation、回收并结算 blocking/realtime/store
  路径、进入 `Quiesced`、关闭串行上下文并消费其 future，最后由外部 owner 关闭 Executor。
- [x] `M0-09` 用小线程池和满队列验证 admission rejection 不丢失控制命令；若当前 API 无法满足
  必需语义，登记 `EXE-*` 反馈而不是增加私有线程或无界队列。

### 4.3 测试与供应链

- [x] `M0-10` 建立 Fake Clock、Fake Environment、可控 Provider 和确定性 ID 工具；Fake 不创建
  自有异步生命周期。
- [x] `M0-11` 建立单元、contract、integration、replay、stress 和 benchmark 稳定入口。
- [x] `M0-12` 锁定 Executor 版本或 commit，生成直接依赖许可证清单和初始 SBOM。
- [x] `M0-13` 建立 Linux GCC/Clang 构建测试；其他目标的未运行项明确记录补跑环境。
- [x] `M0-14` 建立文档链接、Markdown 围栏和公共头独立包含检查。
- [x] `M0-15` 建立 Linux、Windows 和 Android arm64-v8a 的 CMake 组合入口；Core 平台边界检查
  拒绝 SDK 头文件和平台宏倒灌，并在 CI 中保留三平台构建门禁。

## 5. 风险与阻塞

- `BUG-20260830-001`：Android NDK 26.3 libc++ 不为 `std::array<uint8_t, 16>` 提供可用的三路比较，
  导致 `Id128` 的默认 `operator<=>` 在 `-Werror` 下被删除。修复为 `Id128` 内部按字节显式返回
  `std::strong_ordering`，并以契约测试覆盖排序语义；待 Android CI 重跑确认。
- `RISK-2026-001`：`SerialExecutionContext` 拥有内部串行线程，且 `submit_on` facade wrapper 在
  默认池中等待回调完成。M0 必须用最小线程数和突发 completion 验证不会因长控制回调造成饥饿；
  控制回调不得执行模型、I/O、阻塞等待或用户回调。
- `RISK-2026-002`：Executor 的 wait API 不覆盖 blocking/realtime 路径。Mira 必须分别保存并
  回收这些 handle，不能只以默认池 idle 判定 Runtime 已停止。

## 6. 测试与退出条件

- [x] Linux Debug/Release 和适用 sanitizer 可 configure、build、install、test。
- [x] 最小 consumer 只依赖安装后的公共 API 和 Executor 公共依赖即可编译链接。
- [x] 10,000 次混合 Task 命令/completion 压测保持单写者顺序，无终态复活和未观察 future。
- [x] 排队取消、任务异常、串行上下文关闭、Executor submission rejection 均形成结构化结果。
- [x] shutdown 后没有 Mira producer、Controller、blocking worker、timer 或 Observer 回调。
- [x] 所有 `M0-01` 至 `M0-15` 完成，且无未登记的 Executor 绕行。
- [x] Linux、Windows、Android 的目标组合、工具链入口和未运行条件记录在[平台矩阵](../compatibility/platform-matrix.md)；
  M0 不将未运行的 Windows/Android 目标宣称为 Runtime 支持。

## 7. 验证记录

### 2026-08-30：M0 工程基线验收

- 工作树：基于 `a9c82bac3eab2d7f1b3375a4de40f82b6dd2dec2` 的未提交工作树。
- 环境：Ubuntu 24.04、Linux `7.0.0-30-generic`、x86_64、14 logical CPUs；CMake 3.28.3；
  GCC 13.3.0；Clang/clang-format/clang-tidy 18.1.3；Executor
  `2af11a3466dd4a97a31d8784d01a892876aeeb1a`。
- 构建测试：GCC Debug、Release、ASAN、UBSAN 各执行 `cmake --preset <preset>`、
  `cmake --build --preset <preset>`、`ctest --preset <preset> --output-on-failure`，均 10/10 通过。
  TSAN 同样构建后用 `setarch x86_64 -R ctest --test-dir build/tsan --output-on-failure` 补跑，
  10/10 通过；直接启用 ASLR 时 GCC TSAN 在该内核报告 `unexpected memory mapping`，故 CI 明确
  固定使用该补跑条件。
- 编译器矩阵：Clang 18 Debug/Release 均 configure、build、test，分别 10/10 通过。工具通过
  Ubuntu 包解压至临时目录运行，未改变系统安装；CI 使用 Ubuntu runner 的系统包。
- 压测：`mira_stress_test` 执行 10,000 条混合 command/completion，验证 control sequence 全序、
  终态后 completion 被隔离、所有业务/dispatch future 被消费；GCC Release 用时约 0.10 秒，
  Debug 约 0.32 秒，TSAN 约 1.70 秒。该数据仅为验收运行观测，不构成性能承诺。
- 生命周期：integration test 覆盖有界 admission 拒绝、排队取消、callback 异常、stopped context、
  blocking worker wakeup/join、realtime stop、timer cancel、`Quiesced` 与外部 `shutdown(true)`。
- 安装消费：各测试配置均将 Mira/Executor 安装到临时 prefix，外部 consumer 只经 `find_package(Mira)`
  和安装后公开头/targets 构建、链接并运行。
- 质量与供应链：clang-tidy build 通过；clang-format dry-run 通过；Markdown 链接/围栏、公共头独立
  包含、git diff whitespace、依赖锁和 CycloneDX SBOM 检查通过。
- Executor 反馈：登记 `EXE-20260830-001`（缺少总量 admission）、`EXE-20260830-002`（多 worker
  wrapper 进展）和 `EXE-20260830-003`（栈条件变量竞争）。临时方案只位于 Runtime compatibility
  boundary，不创建线程、队列或第二套生命周期设施；设计与 DEC-001 已同步。
- 未运行平台：Windows、Android、macOS 未在 M0 本机执行；M0 不宣称这些目标已验证。负责人为
  Mira Maintainers；补跑条件为对应 CI runner/NDK toolchain 可用时执行 configure/build/适用 test，
  其跨平台发布门禁仍由后续真实 Adapter 里程碑管理。
- 跨平台构建入口：新增 `windows-debug`/`windows-release` 和 `android-arm64-release` CMake 预设，
  并在 `ci.yml` 增加 Windows x64 与 Android arm64-v8a Core/Simulator build job。`platform-boundary-check`
  及其测试确认 `include/mira`、`src` 不包含平台 SDK 或平台宏；入口配置不等同于目标环境运行证据。

### 2026-08-30：跨平台基线增补验证

- Linux：`cmake --preset debug && cmake --build --preset debug --parallel 2 && ctest --preset debug
  --output-on-failure`，11/11 通过；新增 `mira_platform_boundary_test` 通过。
- 静态边界：`python3 tools/check_platform_boundary.py .`、`check_docs.py` 和 `check_sbom.py` 均通过。
- Windows：本机无 Visual Studio/MSVC，未运行；`windows-debug`/`windows-release` 预设与 CI runner
  已提交，补跑条件为 Windows 2022 runner。
- Android：本机无 Android NDK/Ninja，`cmake --preset android-arm64-release` 按预期以明确的 NDK
  环境错误终止；补跑条件为 NDK 26.3.11579264（或兼容版本）及 Ninja 可用。CI 只构建
  `mira_core` 和 `mira_simulator_adapter`，真机 Host/Adapter 行为验证仍属于 M2/M7。
- `format-check` 未在本轮执行：当前容器没有 `clang-format`；CI 的 quality job 安装该工具后执行。

### 2026-08-30：Android CI 回归修复

- 失败证据：[CI run 33301936164](https://github.com/Linductor-alkaid/mira/actions/runs/33301936164)，
  Android job 在 configure 后的编译阶段失败；Windows、Linux、sanitizer 和 quality jobs 通过。
- 根因：Android NDK 26.3 的 libc++ 无法为 `std::array<uint8_t, 16>` 生成默认三路比较，
  `include/mira/core_contracts.hpp:73` 及其强类型 ID 比较运算符因此在 `-Werror` 下报错。
- 修复：`Id128::operator<=>` 改为标准库无关的显式字节序比较，并在 `mira_m1_core_test` 增加
  `<`、`>`、`!=` 回归断言。Linux GCC Debug 本地 `ctest --preset debug --output-on-failure` 通过，12/12。
- Android 目标的最终 `Build verified` 结论以包含此修复的后续 CI run 为准；在此之前保留平台矩阵的
  `Configured` 状态。
