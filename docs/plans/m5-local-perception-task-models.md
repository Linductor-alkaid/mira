# M5：本地视觉、任务模型注册与 ONNX 推理

> 状态：Planned
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M3
> 建议发布点：Local perception beta
> 更新日期：2026-09-01

## 1. 目标

交付受治理的本地感知路径：用版本化、签名且有资源上限的 ModelPackage 在端侧完成 OCR、检测、
元素定位和任务状态识别，为 Planner/Verifier 提供绑定 frame、坐标、epoch、模型 digest 和质量的
evidence；同时建立默认关闭的数据导出、蒸馏、ONNX 发布、设备评估与回滚契约。

M5 不授予本地模型动作权限。低置信度、OOD、失效 transform、模型撤销或能力不匹配必须 abstain、
重新观察或回退 VLM/Human，所有候选 Intent 仍经 Decision/Planner/Policy/ActionLease/Verify。

## 2. 范围与非目标

### 2.1 范围

- `IPerceptionProvider`、OCR/Detector/State/Embedding facade 和 evidence schema。
- ModelPackage manifest、签名/digest、Registry 状态机、资源 profile、模型卡和撤销。
- ONNX Runtime CPU 参考 Provider、session cache、pre/postprocess、golden vectors 和有界推理。
- OCR、检测、元素融合、状态模型、OOD/abstain、VLM fallback 与 Verification 集成。
- opt-in 数据导出、label provenance、group split、蒸馏、ONNX 导出/量化/签名工具链。
- shadow/canary/rollback、删除传播、设备/Agent 评估与兼容性矩阵。

### 2.2 非目标

- Core 依赖 ONNX Runtime、CUDA、DirectML、NNAPI 或训练框架类型。
- Runtime 自动上传 Event、Artifact 或 Memory，或在线自训练/自替换模型。
- 把任意 Execution Provider 可加载等同于输出、性能或安全等价。
- 在 M5 交付连续控制；本地状态 feedback 只提供 M6 可消费的契约。
- 交付完整 ToolModule Registry；M5 只冻结 policy candidate 的模组绑定 manifest 与离线校验，
  运行时协商和执行由 M7 完成。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M3 全部工作项关闭，ModelGateway、Artifact、Replay、费用和真实互操作证据可作为 VLM fallback 基线。
- ONNX Runtime、OCR/detector 模型及辅助图像库在引入前完成版本、许可证、线程模型、目标平台、
  模型/数据许可和 SBOM 审计。
- 用于训练、评估或 benchmark 的数据具有明确授权、不可变 digest、分组切分和删除传播标识。

### 3.2 设计与决策依据

- [本地感知与任务 ONNX 模型设计](../design/local_perception_and_task_models.md)
- [Observation、坐标与 Android Host ABI 设计](../design/observation_coordinate_android_host.md)
- [评估与基准体系设计](../design/evaluation_and_benchmark_design.md)
- [Context 与 Memory 设计](../design/context_and_memory_design.md)
- [工具模组设计](../design/tool_module_design.md)
- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
- [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)、
  [DEC-006](../decisions/DEC-006-local-perception-task-models.md)、
  [DEC-009](../decisions/DEC-009-tool-module-boundary.md)

## 4. 工作项

### 4.1 感知契约与模型包

- [ ] `M5-01` 实现版本化 `IPerceptionProvider`、request/result、OCR/Detector/State/Embedding facade、
  stable error 和 capability；公共边界不暴露推理后端类型。
- [ ] `M5-02` 实现 Evidence schema 与校验：每项绑定 Observation/frame/model/pre-postprocess/space/epoch，
  confidence 绑定 calibration，NaN、非法 shape/label、过期 transform 和不可信 OCR 文本 fail closed。
- [ ] `M5-03` 实现 ModelPackage manifest/schema、包 digest、签名、license/SBOM、资源/applicability 与
  current/previous reader；未知关键字段、超高 ABI/opset 和自定义 op 默认拒绝。
- [ ] `M5-04` 实现 ModelRegistry 状态机和不可变 snapshot：Discovered/Verified/Staged/Active/Deprecated/
  Revoked/Quarantined，支持 primary/fallback/shadow，撤销阻止新推理且迟到结果不得进入 Plan。

### 4.2 ONNX Provider 与在线 Pipeline

- [ ] `M5-05` 锁定 ONNX Runtime 参考版本并实现隔离 target、CPU Execution Provider、显式线程配置和
  package/session lifecycle；第三方内部线程和 shutdown 限制记录在供应链与兼容性文档。
- [ ] `M5-06` 实现有界 session cache、load/warm-up/lease/eviction；热路径不懒加载，cache 有模型数、
  权重、workspace、TTL 上限，shutdown 等待/取消 inference 后再释放 session。
- [ ] `M5-07` 实现图像 preprocess/postprocess 公共管线和 golden vectors：颜色、crop/letterbox/resize、
  normalization、NMS、坐标反变换及 native/ONNX/量化容差可复现。
- [ ] `M5-08` 交付 OCR 与 detector baseline、ElementLocator 及 UI Tree/evidence fusion；来源冲突保持
  分离，低质量结果不通过简单相加变成高置信度。
- [ ] `M5-09` 交付 task state schema、state model、OOD/abstain 和 transition evidence；错误/冲突/OOD
  触发重观察、VLM 或 Human，不强行 top-1。
- [ ] `M5-10` 实现按 VerificationPlan 选择模型、ROI/change gate、applicability、deadline 和 route event
  的 Perception Pipeline；旧 ROI 可在开始前替换，运行中超时只使结果 stale。
- [ ] `M5-11` 集成 local-only、VLM-only、hybrid 与 unavailable/OOD 路由；本地 evidence 进入 Planner/
  Verifier 但不绕过 Policy，记录成本、fallback、冲突和最终使用情况。

### 4.3 数据、训练与发布治理

- [ ] `M5-12` 实现默认关闭的 ExportManifest 校验与导出工具：capability/consent/scope/ACL、Secret 与
  secure field 脱敏、Erasure Pending 拒绝、审计 receipt 和删除传播 ID。
- [ ] `M5-13` 建立 immutable dataset、label provenance 和按 user/session/environment/application/
  trajectory 分组切分；benchmark/holdout 不回流训练，teacher/Human/weak label 可区分。
- [ ] `M5-14` 建立可复现训练/蒸馏/ONNX 导出/量化/package/sign 工具链；保存代码、环境、seed、
  teacher、dataset、超参和 checkpoint digest，并验证训练与 C++ pre/postprocess 一致。
- [ ] `M5-15` 冻结 policy candidate 的 `bindings.tool_modules` manifest 与离线校验；越权 ToolIntent、
  未绑定/不可表示模组和未知版本范围拒绝。M5 不启用真实工具调用，运行时协商留给 M7。
- [ ] `M5-16` 实现 shadow/canary/rollback/revoke 部署契约、model card 和删除传播；已发布包受删除请求
  影响时记录撤回/重训/风险决定，不能以删除源文件冒充权重删除。

### 4.4 评估、平台与文档

- [ ] `M5-17` 建立模型级与 Agent 级 eval：OCR CER/WER、detector mAP/recall、state F1/ECE/OOD、
  locator clickability、任务成功/错误动作/VLM 成本/fallback/Human 率，并保留安全分层结果。
- [ ] `M5-18` 在 CPU reference 与至少一个 v1 目标设备配置执行 cold/warm P50/P95/P99、RSS/workspace、
  并发干扰和 deadline miss；未验证 EP 不标记支持。
- [ ] `M5-19` 覆盖 package/inference 的 rejection、异常、取消、model switch/revoke、资源耗尽、late
  completion 和 shutdown；新增路径全部由 Executor 跟踪，future/handle 可结算。
- [ ] `M5-20` 更新公共 API、示例、模型兼容性矩阵、模型卡、安全/供应链/数据治理文档和 Local
  perception beta 发布说明，并把验证证据回填本计划。

## 5. Executor 路由与关闭

| 工作 | Executor 能力 | Owner | 结算要求 |
| --- | --- | --- | --- |
| package 发现/验证/load/warm-up | `submit_auto()` | ModelRegistry/Provider | future 消费，失败不重试隐藏 |
| inference/pre/postprocess | 有界普通任务 | Perception supervisor | deadline、资源 profile、stale result |
| 导出/训练工具 | 独立离线进程/工具链 | Training owner | 不属于 Core Runtime producer |
| cache eviction/registry maintenance | delayed/soft timer | Provider owner | 可取消、状态可诊断 |
| route/state commit | Runtime 串行控制面 | TaskCoordinator | 小型 descriptor/event |

Runtime shutdown 先停 perception producer 和新 cache acquire，取消 queued inference，等待/标记不可取消
的在途推理，释放 session/Execution Provider，再关闭 Executor。ONNX 自身线程是受限的第三方内部资源，
不得据此引入 Mira 私有线程池；确认 Executor 缺口时按 `EXE-*` 流程处理。

## 6. 风险与阻塞

- `RISK-2026-021`：ONNX Runtime 三平台包、算子与内部线程行为不一致。Owner：M5 inference owner。
  解除条件：锁定依赖、CPU reference golden、目标组合构建/运行和 shutdown 证据。
- `RISK-2026-022`：模型/数据许可证、删除传播或签名链不完整会使产物不可发布。Owner：M5 model
  governance owner。缓解：package gate 同时验证模型、数据摘要、license、SBOM、评估与 provenance。
- `RISK-2026-023`：平均准确率掩盖高风险区域的高置信度错误。Owner：M5 evaluation owner。
  缓解：按风险/应用版本/OOD 分层，单列 unsafe proposal 和 Verify 捕获率。
- `RISK-2026-024`：policy candidate 的 ToolModule 绑定先于 M7 Registry 实现。Owner：M5/M7 owners。
  缓解：M5 只冻结 manifest 与离线 fail-closed 契约，不创建旁路执行通道。

## 7. 测试矩阵

| 维度 | 必测场景 |
| --- | --- |
| 契约 | schema/signature/digest/ABI/opset/unknown label、frame/epoch/space/provenance |
| Pre/Post | crop/letterbox/resize/NMS/反变换、native/ONNX/量化容差 |
| Registry | 状态转换、snapshot、fallback/shadow、switch/revoke、tombstone |
| 推理 | cold/warm、并发、deadline、取消、资源耗尽、非法 tensor/output |
| 路由 | local/VLM/hybrid/unavailable/OOD、UI Tree 冲突、Verify 集成 |
| 数据 | 未授权导出、跨 tenant、Secret、group split、provenance、删除传播 |
| 安全 | 恶意 OCR、错误高置信度、越权 policy ToolIntent、签名/包篡改 |
| 生命周期 | rejection、异常、cache eviction、late completion、shutdown |
| 设备 | CPU reference、目标 EP/设备、内存/尾延迟/thermal（可用时） |

## 8. 退出条件

- [ ] `M5-01` 至 `M5-20` 全部完成并有可复现验证记录。
- [ ] Core 公共 API 不依赖 ONNX/平台类型；package、evidence、坐标、epoch 和质量契约测试通过。
- [ ] 至少 OCR、detector 和 task state 三类 reference 模型通过 package、golden、OOD 和 Agent 回归门禁。
- [ ] 本地模型不能绕过 Planner/Policy/Verify；越权 ToolIntent 和 stale/revoked evidence 均被拒绝。
- [ ] 未授权、跨 tenant、含 Secret 或 Erasure Pending 的数据不能进入导出或训练材料。
- [ ] CPU reference 与声明的目标设备/EP 有实际构建和运行证据；其他 EP 保持 Unknown/Planned。
- [ ] ASAN/UBSAN、适用 TSAN、取消、拒绝、异常、资源耗尽和 shutdown 测试通过。
- [ ] 模型卡、评估报告、数据/模型许可、SBOM、回滚和删除传播信息完整。

## 9. 验证记录

2026-09-01：依据总计划、DEC-006/DEC-009 和本地感知设计创建详细计划；状态为 `Planned`，尚未
引入 ONNX Runtime、模型资产或训练数据，未执行 M5 测试和设备 benchmark。负责人为 Mira Maintainers；
准入条件为 M3 完成以及依赖、模型、数据和许可证审计通过。
