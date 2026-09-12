# Mira 视觉 Grounding 设计（Android 混合定位管线）

> 状态：Active（目标契约与调度策略冻结；实现未开始，进入里程碑受 DEC-011 证据门禁约束，见 §13）  
> 版本：0.1  
> 更新日期：2026-09-12  
> 负责人：Mira Maintainers  
> 上位设计：[Agent Harness 与 Workflow 架构设计](agent_harness_and_workflow_architecture.md)（§9.6 感知优先级）、  
> [Observation、坐标与 Android Host ABI 设计](observation_coordinate_android_host.md)  
> 决策依据：[DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)  
> 需求来源：[Issue #25](https://github.com/Linductor-alkaid/mira/issues/25)  

## 1. 文档目的

本文把 issue #25 的混合视觉 grounding 管线固化为专项设计：在不要求 VLM 预测精确像素
坐标的前提下，为 Android Agent 提供可靠的图像/UI 定位能力，覆盖 native 应用、
WebView/Compose/Flutter 表面与无障碍贫乏场景（游戏、Canvas、Unity、OpenGL/Vulkan UI），
同时约束 APK 体积、内存、延迟、功耗与许可风险。

本文所有代码片段均为**契约草案或伪代码**，未实现；标注"现有"的组件以 M2/M4 交付代码
为准。本设计不重开 M5/M6（DEC-011），实现立项门槛见 §13。

## 2. 与既有决策和遗产的关系

- **DEC-005**（Observation 坐标与 Host 边界）：完全沿用。本设计消费
  `UiTreeSnapshot`、`ScreenFrameDescriptor` 与坐标变换链，不新增坐标系语义。
- **DEC-011**（M5/M6 终止）：本设计是"由 demo 证据重新定义"的目标架构侧输入；实现
  范围由 `MNT-202609-27` 真机需求报告（含 VLM 坐标定位失败语料）经 `MNT-202609-30`
  立项。
- **M5 假设记录**（[本地感知与任务 ONNX 模型设计](local_perception_and_task_models.md)）：
  其 `IPerceptionProvider`/`IOcrProvider`/`IDetectorProvider`/`IElementLocator` 契约草案、
  EvidenceHeader 绑定（observation/frame/space/epoch/model digest）、§6 推理边界纪律
  （后端隔离、session cache、库内线程不算 Executor 任务、deadline 只置 stale）与 §11
  评估指标体系直接沿用；训练/蒸馏链不在本设计范围。
- **DEC-013**：Set-of-Mark（SoM，在截图上为每个候选区域叠加编号标记的提示方式）
  渲染图与 crop 图作为 Artifact 走宿主可控线格式三件套。
- **总体架构 §9.6**：感知优先级 `Accessibility -> OCR -> CV/Detector -> VLM` 已冻结，
  本设计是其执行化。

## 3. 管线总体结构

```text
Android screen
  ├─ Accessibility tree（现有：host ABI GET_UI_TREE -> mira.host.tree.v1 -> UiTreeSnapshot）
  └─ Screenshot（现有：CAPTURE_FRAME -> ScreenFrameDescriptor/ArtifactRef）
       ├─ OCR（OcrBackend：text + bbox）
       └─ 轻量 UI 检测器（UiDetectorBackend：可交互区域提案）
                ↓
        Region fusion / dedup（融合与去重）
                ↓
        GroundingRegion 列表 / ScreenSemanticMap（Observation.perception）
                ↓
        SoM 渲染（请求组装层，Artifact）
                ↓
        VLM 选择 region_id + action（决策 schema 演进）
                ↓
        解析回 ElementRef -> 坐标变换 -> compile_discrete_action（现有 fail-closed 路径）
```

无可靠 region 时的回退：粗网格 -> 裁剪选中格 -> 重渲染网格/SoM -> 再次询问 VLM
（有界深度，见 §8）。

## 4. 数据契约

### 4.1 GroundingRegion（契约草案，基于现有占位扩展）

```cpp
// 契约草案（未实现）。region id 仅在单一 Observation 与 environment epoch 内有效；
// 它是证据，不是永久身份（与现有 ElementRef 语义一致）。
struct GroundingRegion {
    RegionId id;                     // Observation 内稳定分配
    RectF bounds;                    // 所在 CoordinateSpaceId 空间
    Point center;                    // 同空间中心点
    std::string text;                // OCR 文本或 accessibility 标签
    std::string description;         // 可选语义描述
    std::string visual_type;         // icon / text / control / image ...
    bool clickable;
    uint32_t source_mask;            // ACCESSIBILITY | OCR | DETECTOR | VLM
    float confidence;
    // 绑定现有类型：PerceptionEvidence（来源证据）与 ElementRef（融合引用）
};
```

落地方式：来源证据进 `Observation.perception`（`ObservationComponent<
PerceptionEvidence>`，现有字段 `kind="ocr.line"/"detector.box"` 即为本设计预留）；
融合结果以 `ElementRef{source=Fused}` 表达。`ObservationPipeline` 需新增 perception
source 挂钩（现有五类 source 之外），并保持组件 `CaptureSpan/quality/epoch` 纪律。

### 4.2 融合规则

- 重叠证据合并：无标签可点击 accessibility 节点 + 同 bounds OCR 文本 -> 单一 region
  并置 `source_mask` 双位；IoU 阈值与去重次序为确定性配置（可测试、可回放）。
- 冲突时 Accessibility bounds 优先，OCR/detector 仅补 label 与补充提案。
- 置信度校准为融合后统一量纲，进入排序与回退判定，不进入动作安全判定。

## 5. 来源提取与归一化

- **Accessibility**：`UiTreeSnapshot` 节点归一化为 region 候选（bounds/space/label/
  `supported_actions`）；`accessibility_completeness` 与 `truncated` 标志进入覆盖度
  评估，决定是否升级 OCR/检测器。
- **OCR**：`OcrBackend` 接口（契约草案）：`detect_recognize(frame_roi) ->
  vector<TextRegion{box, text, language, recognition_confidence}>`；检测/识别后处理与
  坐标恢复（检测输入尺度 -> 原帧空间）在 Mira C++ 内实现。PP-OCR mobile 为参考后端
  候选（Apache-2.0，许可按供应链流程复核）。
- **检测器**：`UiDetectorBackend` 接口：`detect(frame, target_size∈{320,416}) ->
  vector<Detection{box, label, score}>`，坐标映射回原始截图。候选：OmniParser
  `icon_detect_v3`（YOLOv9 MIT 实现路线）、GPA-GUI-Detector（Ultralytics 训练来源
  需审）；评估期允许 ONNX Runtime 并行对比，生产方向 ncnn（DEC-033 第 4/9 条）。

## 6. SoM 渲染与决策 schema

- 渲染：region id 标注图在请求组装层生成，作为 `ArtifactRef` ImagePart（DEC-013）随
  请求提供；region 清单（id/text/type/bbox 摘要）以有界文本进入请求。渲染产物是派生
  投影，不进 Observation 事实记录（RULE-07）。
- 决策 schema：新增版本（如 `mira-decision-v2`，按 DEC-002 版本化演进，不静默修改
  `mira-decision-v1`），动作闭集扩展为携带 `region_id` 的选择形式；VLM 响应必须先解析
  校验为结构化 Decision（RULE-04）。
- 解析与执行：`region_id -> GroundingRegion -> ElementRef`，经 epoch/freshness 校验后
  由既有 `transform_box_through` 链变换到 CanonicalViewport，中心点进入
  `compile_discrete_action` 的 canonical [0,1] fail-closed 校验与执行路径。无命中
  region 的直接坐标形式保留为回退（W-06：不引入第二套动作执行通道）。

## 7. 事件驱动调度与缓存

1. 屏幕无实质变化时复用缓存感知结果（缓存条目绑定 environment epoch 与帧哈希）。
2. Accessibility 节点充分（文本/bounds/动作齐全）时不做神经推理。
3. OCR 仅在文本缺失或过期时执行；检测器仅在覆盖不足或图标/自绘 UI 场景执行。
4. 动作执行后的重感知只在 Accessibility 事件、帧哈希/差异、超时或 Agent 显式请求指示
   屏幕变化时触发。
5. 推理频率、缓存命中率与失效原因进入诊断事件（§11）。

屏幕变化信号需要 Host ABI 扩展：在 ABI v1 `struct_size` 前向兼容机制下新增
Accessibility/屏幕变化事件推送操作与回调（现有回调面仅 `on_operation_complete` 与
`on_capabilities_changed`）；扩展草案随立项里程碑冻结，未批准前 v1 面不变
（DEC-033 第 8 条）。

## 8. 回退路径

粗网格覆盖（有界格数）-> VLM 选格 -> 裁剪该格重渲染网格/SoM -> 再次询问；深度与
请求次数有硬上限，超限上报失败并升级（Agent 重新观察或交用户），不得无限细化。小
图标优先 crop 细化而非整屏升分辨率。

## 9. Executor 路由与生命周期

沿用 M5 假设记录 §6 纪律并适配：

- OCR/检测推理经 Provider 内部专属 worker 生命周期承载（阻塞/CPU 有界任务）；推理库
  内部线程不构成 Executor 任务，协作取消由 deadline + stale 标记实现。
- 感知编排在 `ObservationPipeline` 的 observe 流程内以组件 deadline 结算；感知组件
  超时不阻塞 screen/structure 组件的既有交付（partial 降级语义）。
- 缓存与失效为纯函数逻辑，可确定性测试与回放。
- shutdown：推理 worker 随 Provider 生命周期回收；未完成推理结果丢弃，不进入
  Observation。

若评估发现 Executor 公开能力不足（如推理与 UI 线程亲和、GPU 队列协调），按 AGENTS.md
登记 `EXE-*` 反馈，不引入平行生命周期。

## 10. 性能、功耗与体积策略

- 检测器输入 320/416 px；OCR 按行 ROI 执行；任何整屏高分辨率推理需基准证据支持。
- 无变化不推理是硬约束（§7），推理频率进入基准指标。
- 模型文件按需加载、带 digest 与版本（模型包 manifest 沿用 M5 假设记录 §5 纪律）；
  未完成的 provenance 检查不得入仓。

## 11. 可观测性

诊断事件（脱敏）：感知执行（后端、输入尺寸、耗时、条目数、stale/degraded）、缓存
命中/失效原因、融合统计（合并/去重计数）、SoM 渲染（artifact 引用与 region 数）、
决策 region_id 解析结果（命中/回退/拒绝及原因）。截图与 SoM 图以 Artifact 稳定引用
表示，不在事件中复制。

## 12. 基准与验收设计

基准集覆盖：native View 与 Jetpack Compose、WebView/Flutter/React Native、
Canvas/游戏/Unity 风格无障碍贫乏屏、中英文、小图标/对话框/菜单/列表/动态变化。

指标（阈值在立项里程碑冻结）：region 提案召回与精度、目标选择/点击成功率、OCR 文本
准确率与 bbox IoU、端到端 p50/p95 延迟、峰值 RSS 与模型加载时间、APK/AAB 体积增量、
CPU/GPU 利用率/能耗/热、缓存命中率与推理频率、融合与 SoM 前后消融。

对照组：仅 Accessibility / +OCR / +检测器；VLM 直接坐标 vs SoM 选区；ONNX Runtime vs
ncnn；FP32/FP16/INT8（按支持情况）。指标体系并入[评估与基准体系]
(evaluation_and_benchmark_design.md)，运行载体衔接 `MNT-202609-28/29`，真机证据走
`MNT-202609-27` 通道。

## 13. 分阶段落地与门禁

实现进入里程碑的前提：`MNT-202609-27` 产出真机需求报告（含定位失败归因），
`MNT-202609-30` 完成 M7 重定义立项。阶段划分（立项后细化，不预分配里程碑编号）：

1. `GroundingRegion` 契约、source mask、confidence 与坐标变换绑定（纯契约，可先行）。
2. Accessibility 节点提取与归一化（纯 C++，无推理依赖）。
3. 截图捕获缓存、屏幕变化检测与失效规则。
4. `OcrBackend` 接口 + PP-OCR mobile 参考后端与坐标恢复。
5. `UiDetectorBackend` 接口 + 候选模型基准（含许可审查）。
6. 融合、去重、label 关联与置信度校准。
7. SoM 渲染 + 决策 schema 版本演进 + region_id 解析回动作。
8. grid/crop 细化回退。
9. 事件驱动调度与延迟/内存/功耗仪表。
10. Android 基准夹具与端到端点击成功率测试。
11. 模型 provenance/许可检查清单与 `THIRD_PARTY_NOTICES`（任何权重入仓前完成）。

测试矩阵至少覆盖：融合确定性、缓存失效正确性、推理超时降级、epoch 失效后 region
解析拒绝、决策 schema 回退路径、shutdown、事件推送 ABI 前向兼容。

## 14. 风险

- 推理后端引入构建/体积/功耗成本超出宿主预算：以候选多点评测与事件驱动调度缓解，
  基准门槛不达即不捆绑。
- 检测器候选许可或来源审查失败：多点候选（`icon_detect_v3`、GPA 审记后备选），审查
  先于任何权重入仓。
- ABI 事件推送依赖宿主配合：与 miracle 侧 `MNT-202609-27` 验收同步；无推送时以
  轮询 + 帧哈希降级运行。
- VLM 在 SoM 上仍选错 region：点击成功率纳入对照基准；失败升级路径（§8）兜底。

## 15. 关联文档

- [DEC-033](../decisions/DEC-033-hybrid-visual-grounding.md)
- [Agent Harness 与 Workflow 架构设计](agent_harness_and_workflow_architecture.md)
- [Observation、坐标与 Android Host ABI 设计](observation_coordinate_android_host.md)
- [本地感知与任务 ONNX 模型设计](local_perception_and_task_models.md)（假设记录）
- [评估与基准体系](evaluation_and_benchmark_design.md)、[直接依赖与许可证](../supply-chain/direct-dependencies.md)
- [DEC-002](../decisions/DEC-002-public-contract-versioning.md)、
  [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md)、
  [DEC-011](../decisions/DEC-011-demo-first-external-validation.md)、
  [DEC-013](../decisions/DEC-013-transport-export-and-image-media.md)
- [阶段 F 后续维护计划](../plans/maintenance-2026-09-post-stage-f.md)
- [Issue #25](https://github.com/Linductor-alkaid/mira/issues/25)
