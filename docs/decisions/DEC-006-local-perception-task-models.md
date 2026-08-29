# DEC-006：本地感知和任务 ONNX 模型边界

> 状态：Accepted  
> 日期：2026-08-30  
> 负责人：Mira Maintainers  
> 冻结里程碑：M5  
> 替代/被替代：无

## 背景与问题

VLM 延迟和成本不适合高频元素定位、OCR、状态识别与连续控制。Mira 需要把通用模型能力蒸馏成
任务小模型，但训练、数据治理和设备推理不应污染 Core 或让低置信度模型直接产生副作用。

## 决策

- Core 定义平台无关的 `IPerceptionProvider`、任务模型 manifest 和证据类型，不暴露 ONNX Runtime、
  CUDA、NNAPI 或图像库类型。
- OCR、检测、元素定位、状态估计和任务策略模型输出都绑定 source frame、transform、模型 digest、
  置信度和质量；它们是 Observation evidence，不是授权。
- ONNX 是首个可移植任务模型封装格式；Execution Provider 由 Adapter/Provider capability 选择，
  CPU fallback 和资源上限必须明确。
- 训练/蒸馏属于独立离线工具链。Runtime 数据导出默认关闭，经过授权、脱敏、来源追踪、数据集
  版本化、train/test 主体隔离和删除传播后才能使用。
- 模型上线经过离线质量、安全/OOD、目标设备延迟/内存和 shadow/fallback 验证。低置信度、OOD、
  schema 不兼容或 drift 时回退结构化 UI/VLM/Human，不能强行执行。
- 首期任务模型只做 perception/state/verification；直接输出 Action 的 policy model 必须经过额外
  Policy 和 SafetyValidator，且不绕过闭环。

## 备选方案

- 把 ONNX 当作 `IModelProvider` 的另一种 LLM：输入输出和实时约束不同，不采用。
- 自动从全部 Event/Memory 持续训练：违反授权和数据治理，不采用。
- 每个任务在 Core 中写死预处理和输出解析：阻碍模型版本化与回滚，不采用。

## 影响与风险

模型包必须同时包含预/后处理契约、labels、opset、输入范围、校准信息和签名。第三方推理库内部
线程需要显式配置并记录；Mira operation 和生命周期仍由 Executor 管理。

## 验证方式

同一模型包在参考后端和目标设备对 golden corpus 输出一致性；评估任务成功率、错误动作率、
OOD 拒绝率、VLM 回退率、P50/P95/P99、峰值内存和功耗。

## 关联文档和工作项

- [本地感知与任务模型设计](../design/local_perception_and_task_models.md)
- [Mira 实施总计划](../plans/mira-implementation-plan.md)：M5

