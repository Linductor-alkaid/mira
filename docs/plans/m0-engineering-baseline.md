# M0：仓库、构建与 Executor 工程基线

> 状态：Planned  
> 负责人：Mira Maintainers  
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)  
> 前置：无  
> 建议发布点：内部工程基线  
> 更新日期：2026-08-30

## 1. 目标

建立可重复构建、测试和诊断的 C++20 空骨架，验证 Mira 对 Executor 的所有权、串行控制、
取消和关闭假设。M0 不交付真实模型或平台自动化。

## 2. 范围与非目标

范围包括顶层构建、目标分层、质量工具、依赖锁定、Simulator/Fake 基础、Executor integration
spike 和 CI 入口。非目标包括完整 Task 闭环、生产 EventStore、Android Adapter 和 ONNX 推理。

## 3. 设计与决策依据

- [Mira Runtime 设计](../design/mira_runtime_design.md)
- [DEC-001：Runtime 的 Executor 所有权与串行控制面](../decisions/DEC-001-runtime-executor-ownership.md)
- [项目管理与文档规范](../project/project_management_and_documentation.md)

## 4. 工作项

### 4.1 仓库与构建

- [ ] `M0-01` 建立顶层 CMake、C++20 编译基线以及 Debug、Release、ASAN、UBSAN、TSAN 预设。
- [ ] `M0-02` 建立 `include/mira/`、`src/`、`adapters/simulator/`、`tests/`、`cmake/` 和 `tools/`
  骨架，目录依赖与 Runtime 设计一致。
- [ ] `M0-03` 建立 `mira_core`、`mira_simulator_adapter` 和最小 consumer targets；公开头不包含平台
  SDK 或具体 HTTP/ONNX 类型。
- [ ] `M0-04` 配置 warning、format、静态检查和测试标签，CI 不以“没有测试”冒充成功。

### 4.2 Executor 集成原型

- [ ] `M0-05` 显式创建实例化 `executor::Executor`，在首次提交前初始化，并由非 worker owner
  完成最终 `shutdown(true)`。
- [ ] `M0-06` 原型验证每个 Runtime 一个 `executor::SerialExecutionContext`：多 Task 命令和
  completion 按接纳顺序提交，回调只做有界状态提交且不等待 future。
- [ ] `M0-07` 使用 `submit_on_with_handle()` 验证排队取消、异常传播、上下文关闭拒绝和迟到
  completion 隔离；保存并消费全部 future。
- [ ] `M0-08` 验证关闭顺序：停止 producer、取消 operation、回收并结算 blocking/realtime/store
  路径、进入 `Quiesced`、关闭串行上下文并消费其 future，最后由外部 owner 关闭 Executor。
- [ ] `M0-09` 用小线程池和满队列验证 admission rejection 不丢失控制命令；若当前 API 无法满足
  必需语义，登记 `EXE-*` 反馈而不是增加私有线程或无界队列。

### 4.3 测试与供应链

- [ ] `M0-10` 建立 Fake Clock、Fake Environment、可控 Provider 和确定性 ID 工具；Fake 不创建
  自有异步生命周期。
- [ ] `M0-11` 建立单元、contract、integration、replay、stress 和 benchmark 稳定入口。
- [ ] `M0-12` 锁定 Executor 版本或 commit，生成直接依赖许可证清单和初始 SBOM。
- [ ] `M0-13` 建立 Linux GCC/Clang 构建测试；其他目标的未运行项明确记录补跑环境。
- [ ] `M0-14` 建立文档链接、Markdown 围栏和公共头独立包含检查。

## 5. 风险与阻塞

- `RISK-2026-001`：`SerialExecutionContext` 拥有内部串行线程，且 `submit_on` facade wrapper 在
  默认池中等待回调完成。M0 必须用最小线程数和突发 completion 验证不会因长控制回调造成饥饿；
  控制回调不得执行模型、I/O、阻塞等待或用户回调。
- `RISK-2026-002`：Executor 的 wait API 不覆盖 blocking/realtime 路径。Mira 必须分别保存并
  回收这些 handle，不能只以默认池 idle 判定 Runtime 已停止。

## 6. 测试与退出条件

- [ ] Linux Debug/Release 和适用 sanitizer 可 configure、build、install、test。
- [ ] 最小 consumer 只依赖安装后的公共 API 和 Executor 公共依赖即可编译链接。
- [ ] 10,000 次混合 Task 命令/completion 压测保持单写者顺序，无终态复活和未观察 future。
- [ ] 排队取消、任务异常、串行上下文关闭、Executor submission rejection 均形成结构化结果。
- [ ] shutdown 后没有 Mira producer、Controller、blocking worker、timer 或 Observer 回调。
- [ ] 所有 `M0-01` 至 `M0-14` 完成，且无未登记的 Executor 绕行。

## 7. 验证记录

尚未执行。当前仓库仅有设计文档和 Executor 依赖，不能将文档完成视为 M0 工程验收。
