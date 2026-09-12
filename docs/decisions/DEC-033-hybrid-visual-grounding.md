# DEC-033：Android 混合视觉 Grounding 管线（本地感知重入的目标架构）

> 状态：Accepted（目标契约与调度策略方向冻结；实现里程碑仍受 DEC-011 证据门禁约束，见第 9 条）
> 日期：2026-09-12
> 负责人：Mira Maintainers
> 冻结里程碑：M7 重定义（`MNT-202609-30`，暂定）
> 替代/被替代：无（承接 [DEC-011](DEC-011-demo-first-external-validation.md) 留白的
> "由 demo 证据重新定义"入口与 [DEC-006](DEC-006-local-perception-task-models.md) 的方向
> 记录；不重开 M5/M6，不改变 [DEC-005](DEC-005-observation-coordinate-host-boundary.md)
> 已冻结的坐标与 Host 边界）

## 背景与问题

miracle 真机运行依赖 VLM 直接预测像素坐标做 GUI 定位，在无障碍信息贫乏场景（游戏、
Canvas、Unity、Flutter 自绘 UI）与图标密集场景成功率不稳定。M4 交付的 Observation
契约已为本地感知预留占位（`PerceptionEvidence`、`ElementSource{UiTree, Ocr, Detector,
Fused}`），但 M5（本地 ONNX 感知）已随 DEC-011 终止，`ObservationPipeline` 没有
perception 挂钩，仓库内无任何推理依赖。

[Issue #25](https://github.com/Linductor-alkaid/mira/issues/25) 提出"低开销混合视觉
grounding 管线"：Accessibility 优先、OCR 与轻量 UI 检测器补充，证据融合为统一区域
模型，Set-of-Mark（SoM）渲染后由 VLM 选择离散 `region_id` 而非预测坐标；本地推理以
ncnn + PP-OCR mobile + MIT 兼容检测器为推荐生产方向。本决策冻结该方向的目标架构与
边界，专项设计见[视觉 Grounding 设计](../design/visual_grounding_design.md)。

## 决策

1. **重入路径遵守 DEC-011 证据纪律**。本决策冻结目标契约与技术方向，**不**重开
   M5/M6、不改变 M7 `Blocked` 状态、不批准任何实现。实现范围与必要性由
   `MNT-202609-27`（miracle 真机需求报告）证据重新定义，经 `MNT-202609-30`（M7
   重定义提案）立项；真机定位失败语料是该方向的直接需求证据。
2. **统一区域契约以现有 Observation 契约为基座**。`GroundingRegion` 不另起第二套区
   域模型：以 `PerceptionEvidence`（kind 如 `ocr.line`、`detector.box`）承载来源证据，
   以 `ElementRef`（`ElementSource` 已含 `Ocr/Detector/Fused`）承载融合后引用，region
   id 只在单一 Observation 与 epoch 内有效——是证据，不是永久身份。
3. **感知优先级沿用总体架构 §9.6 既有分层**：`Accessibility -> OCR -> CV/Detector ->
   VLM`。Accessibility 足够时不做神经推理；OCR 只在文本缺失/过期时执行；检测器只在
   覆盖不足或图标/自绘 UI 场景执行。VLM 不是主像素坐标预测者，其职责是选择离散
   `region_id` 与动作。
4. **推理 Provider 化，Core 无推理依赖**。OCR 与检测器分别经 `OcrBackend`/
   `UiDetectorBackend`（`IPerceptionProvider` 边界纪律，见 M5 假设记录 §6）注入；
   推理库内部线程不构成 Executor 任务，deadline 到达只置 stale，不以部分结果冒充
   完整结果。ncnn（BSD-3-Clause）为推荐生产方向、ONNX Runtime 为评估期备选，均为
   候选；运行时与模型选型不得硬编码进 Core 公共契约。
5. **事件驱动调度，屏幕未变不推理**。感知结果缓存以 Accessibility 变化、帧哈希/
   差异、超时或 Agent 显式请求为失效信号；检测器输入目标 320/416 px 并映射回原始坐
   标，小目标用有界 crop 细化而非整屏升分辨率。无变化时禁止连续帧率推理。
6. **VLM 决策 schema 演进走 DEC-002 版本化**。`region_id` 选择进入决策动作闭集
   （新 schema 版本，不静默修改 `mira-decision-v1`）；region_id 解析回 ElementRef 后
   经既有坐标变换链（`transform_box_through` 到 CanonicalViewport）与
   `compile_discrete_action` fail-closed 路径执行——不引入第二套动作执行通道
   （总体架构 `W-06`）。无可靠 region 时走有界 grid/crop-and-refine 回退，最终仍由
   canonical 坐标路径执行。
7. **SoM 渲染属请求组装层**。SoM 标注图作为 Artifact（DEC-013 线格式契约）进入模型
   请求，region 清单随请求文本提供；渲染不进入 Observation 事实记录（渲染产物是派生
   投影，RULE-07）。
8. **Host ABI 事件推送为前向兼容扩展**。屏幕变化/Accessibility 事件通知经 ABI v1 的
   `struct_size` 前向兼容机制新增操作与回调；未批准里程碑前 ABI v1 面不变。
9. **许可与供应链硬约束**。Mira 与官方捆绑模型不得引入 copyleft 运行时/模型依赖：
   不捆绑 AGPL/Ultralytics 路线的旧 `icon_detect` 与 YOLO11/YOLO26；`.pt -> ONNX ->
   ncnn` 转换不改变原许可；`icon_detect_v3`（YOLOv9 MIT 实现）与 GPA-GUI-Detector
   （Ultralytics 训练来源需审）均为待验证候选。任何权重入仓前完成 revision、训练
   来源、模型许可与通告义务的 provenance 检查并更新 `THIRD_PARTY_NOTICES`。
10. **验收以真机基准为准**。region 提案召回/精度、点击成功率、OCR 文本准确率与 bbox
    IoU、端到端 p50/p95 延迟、峰值 RSS、包体积增量、功耗/热、缓存命中率与融合消融
    （issue #25 基准计划）在立项里程碑冻结阈值；native 可访问屏幕在节点充分时必须
    无神经推理完成 grounding。

## 备选方案

- 恢复原 M5 范围（本地感知 + 任务模型 + 蒸馏链）：任务模型与训练链的价值判断仍缺真
  实证据，且超出 grounding 需求，不采用。
- 仅依赖 VLM 坐标预测：无障碍贫乏与图标场景成功率不可控，成本高，不采用。
- 仅依赖 Accessibility：游戏/Canvas/自绘 UI 场景不可用，不采用。
- ONNX Runtime 作为最终运行时：评估期可并行比较，但与 NDK/ARM/小体积目标相比 ncnn
  更契合；以基准数据定案而非预先绑定，两者均为候选。
- 在 Observation 旁另建区域存储：违反契约单一权威与 RULE-07，不采用。

## 影响与风险

- 引入本地推理后包体积、内存、功耗与热行为影响 miracle 宿主；以 320/416 px 输入、
  事件驱动调度与缓存缓解，并全部进入基准门槛。
- ABI 事件推送扩展涉及宿主实现配合（miracle 侧），需与 `MNT-202609-27` 验收同步。
- 决策 schema 版本演进带来 Provider prompt 与模型兼容成本；按 DEC-002 版本化并保留
  canonical 坐标旧路径作为回退。
- 检测器候选的许可/来源审查失败风险：以候选清单多点评估（`icon_detect_v3`、GPA 审
  记后备选）降低单点依赖。

## 验证方式

- 立项里程碑内交付 Android 基准夹具（native View/Compose、WebView/Flutter/RN、
  Canvas/游戏风格、中英文、小图标/对话框/列表/动态变化）与端到端点击成功率测试。
- 对照组至少包含：仅 Accessibility、+OCR、+检测器、VLM 直接坐标 vs SoM 选区、
  ONNX Runtime vs ncnn、FP32/FP16/INT8（按支持情况）。
- 证据等级按项目管理规范 §10 记录；未经真机验证不得宣称性能或跨平台结论。

## 关联文档和工作项

- [视觉 Grounding 设计](../design/visual_grounding_design.md)（专项设计，与本决策
  同日冻结）
- [Observation、坐标与 Android Host ABI 设计](../design/observation_coordinate_android_host.md)、
  [本地感知与任务 ONNX 模型设计](../design/local_perception_and_task_models.md)（假设
  记录，推理边界纪律沿用）
- [Issue #25](https://github.com/Linductor-alkaid/mira/issues/25)
- [DEC-005](DEC-005-observation-coordinate-host-boundary.md)、[DEC-011](DEC-011-demo-first-external-validation.md)、
  [DEC-013](DEC-013-transport-export-and-image-media.md)、[DEC-002](DEC-002-public-contract-versioning.md)
- [阶段 F 后续维护计划](../plans/maintenance-2026-09-post-stage-f.md)（`MNT-202609-27/30`）
- [直接依赖与许可证](../supply-chain/direct-dependencies.md)
- [Mira 实施总计划](../plans/mira-implementation-plan.md)
