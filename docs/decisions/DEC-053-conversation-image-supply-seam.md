# DEC-053：ConversationLoop 的逐轮多模态输入供给缝

> 状态：Accepted
> 日期：2026-10-09
> 决策人：Mira Maintainers
> 需求来源：[mira#85](https://github.com/Linductor-alkaid/mira/issues/85)
> （Mirage 下游反馈 `MIRA-20261009-001`，P2 多模态工具接入能力缺口；
> 阻塞 Mirage M7-05 图片回填与 M7-07 VLM/SoM 工具闭环）
> 上位决策：[DEC-047](DEC-047-conversational-loop-and-canonical-tool-parts.md)
> （无观察对话循环与规范工具 part）、[DEC-045](DEC-045-agent-loop-working-context-seam.md)
> （供给缝形态先例：可选回调、每请求恰一次、降级不阻塞）、
> [DEC-015](DEC-015-builtin-tool-execution-boundary.md)（工具执行边界不变）
> 关联计划：[维护计划 2026-10 会话图片供给](../plans/maintenance-2026-10-conversation-image-supply.md)

## 背景与问题

`ConversationLoop`（DEC-047）已交付无观察的 model → tool → result → model 循环，
工具往返以规范 `ToolCallPart`/`ToolResultPart` 进入后续请求。模型内容契约自 M4 起
支持 `ImagePart`，三家方言（Chat `image_url`、Responses `input_image`、Messages
`image`）均已具备图片线格式与 artifact 取数能力。缺口在通用循环本身：宿主工具
（如截图）执行完成后，没有任何公开入口能把工具产出的图片作为规范 `ImagePart`
送入下一次模型请求——工具结果只能是 JSON 文本，`large_payload` 只是 ArtifactRef
元数据，不构成图片输入。

Mirage 在其侧自建第二个 model/tool 循环可以绕开该缺口，但会重复
ConversationLoop 已交付的预算、取消、准入、工具回填与终态逻辑；这正是 issue #85
点名反对的临时方案。需要决策：ConversationLoop 以什么形态接纳宿主逐轮结构化
输入，以及身份、预算、淘汰与失败语义。

## 决策

1. **薄缝形态：一条加法式的 `ModelInputSupplier` 供给回调，不是新循环、不是
   队列**。`ConversationLoop` 增加可选依赖 `set_model_input_supplier(supplier,
   options)`（沿 `set_event_store`/`set_tool_registry`/
   `set_working_context_supplier` 的 setter 注入先例，run() 前接线一次）。回调
   签名 `Result<std::vector<ModelInputItem>>(const AgentLoopSpec &)`，在每轮
   `build_request` 前被调用**恰一次**；返回空向量为正常空态。未接线时请求装配
   与现状**逐字节一致**（零漂移）。
2. **非可信数据纪律（冻结）**：宿主供给内容一律按 User 侧处理——条目 role 必须
   为 `ModelRole::User`，装配时并入循环自有的主 User 条目信封（role User、
   provenance `mira.conversation-loop.request.v1`、authority
   `Sensitivity::Internal`），宿主输入永不获得独立条目或更高权限（RULE-09：
   外部内容永不提升为 System/Developer 指令权限）；供给归因走事件面
   （`ModelInputSupplied` 计数）与结果计数器。条目内容词表限于
   `TextPart`/`ImagePart`/`FilePart`；携带 `ToolCallPart`/`ToolResultPart`/
   `ThinkingPart` 的条目整条丢弃并计数（规范工具往返与 provider 推理回放是循环
   的专有产物）。`ImagePart` 逐项校验：artifact 引用非空、`media_type` 非空且为
   `image/*`、`source.sensitivity != Secret`；违规整条丢弃并计数。
3. **有界与淘汰（RULE-08，冻结）**：`ModelInputSupplyOptions` 声明
   `max_items`（默认 8）与 `max_image_bytes`（默认 8 MiB）两个每请求上界。超界
   条目按供给顺序丢弃并计数，不静默。供给内容只进入**当轮请求**：循环的
   tool-history 回放不复制供给项，下一轮请求重新解析回调——旧截图因此不会逐轮
   累积；是否跨轮重放同一 ArtifactRef 由宿主在回调内决定（重放只是引用，字节
   留在 artifact store，无副作用）。
4. **失败语义（冻结）**：回调返回 Error 或抛出异常 → 本轮请求不含供给内容 +
   一次 `ModelInputSupplyDegraded` 诊断事件（`supplier-error` 附错误码 /
   `supplier-exception`，异常文本不进入事件面），循环继续；与 DEC-045 降级纪律
   相同。逐轮重试（`max_recoveries`）会重新解析回调，宿主可重放同图，不存在
   重试路径静默丢图。事件面：接线后每轮发一次 `ModelInputSupplied` 状态事件
   （offered/accepted/dropped/image_bytes/drop_reasons 计数）；
   `ConversationLoopResult` 累计 `supplied_input_items`/`supplied_input_dropped`/
   `supply_degradations`。
5. **装配位置**：存活的供给 part 追加进当轮主 User 条目（goal 文本与恢复
   feedback 之后），请求消息形状与既有带图用户消息完全一致——三家方言零改动
   （Chat `image_url`、Responses `input_image` 已验证图片线格式；Messages 方言
   按其既有映射只接受 `ImageDetail::Auto`，非 `Auto` detail 按既有
   `CapabilityMismatch` 语义显式失败，不静默降级；`ImagePart` 默认值即为
   `Auto`，默认供给在三家方言均可用）。
6. **身份边界**：`AgentLoopSpec` 不因本缝扩面；task/session/epoch 门控在回调内
   闭合（回调收到正在运行的 spec 引用）。循环不缓存、不转发、不跨轮持有供给
   内容，跨任务帧泄漏在结构上不可能。
7. **零并发面**：回调在 run() 调用者的 Executor worker 上同步执行；不新增线程、
   队列、生命周期或 fire-and-forget 工作。图片字节仍按既有契约留在 artifact
   store，请求内只携带 ArtifactRef 与摘要。

## 备选方案

- **push 式 `enqueue_model_input()` 队列**（沿 `enqueue_user_message` 先例）：
  需要队列互斥、逐轮清空、run 结束遗留丢弃与跨 run 纪元隔离等额外状态机；
  pull 回调一次解析即天然有界，宿主保留策略更直接。落选。
- **扩展 `ToolResultPart` 携带图片**（多模态工具结果契约）：需要动 JSON 往返、
  canonical digest 与三家方言的工具线格式，契约面远大于需求；DEC-047 刚冻结
  该 part 的 JSON 形态。留待真实需求出现再评审。
- **Mirage 自建 model/tool 循环**：issue #85 明确反对（重复预算/取消/准入/回填
  逻辑，形成私有临时方案）。

## 影响与风险

- 公开 API 扩面：`ConversationLoop` 新增一个 setter 与结果计数，`AgentLoopSpec`
  与模型契约零改动；纯文本行为零漂移（回归 pin）。
- 供给图片使请求带 `ImagePart`，路由门禁按既有 `capabilities.image_input` 判定；
  无识图能力的 profile 显式失败（CapabilityMismatch），不静默。
- Messages 方言按既有映射只接受 `ImageDetail::Auto`；`ImagePart` 默认值即为
  `Auto`，面向该方言的宿主无需也不得改设其他 detail，否则在方言编码处显式
  `CapabilityMismatch` 失败（已由测试钉死，见下）。
- 回调在热路径上同步执行：宿主回调只读状态/引用，不做阻塞工作；该纪律与
  `WorkingContextSupplier` 相同，写入头注释与 API 文档。

## 验证方式

- 循环级 fixture：截图工具发布 artifact → 回调供给 → 下一请求携带规范
  `ImagePart`（引用逐项断言），ToolCallPart/ToolResultPart 配对不丢失。
- 三方言 fixture：Chat `image_url`、Responses `input_image`、Messages `image`
  （Auto detail）线格式；Messages + 非 `Auto` detail 显式拒绝；无识图能力
  profile 显式拒绝。
- 边界矩阵：空供给（零漂移回归）、条目/字节预算丢弃、role/词表/Secret 校验丢弃、
  回调错误与异常降级、取消、MaxTurns、旧文本会话回归。
- 真实识图模型端到端（「截图内容可见」）需外部凭据，Mira 侧以 fixture 证据为
  准；该验收项由 Mirage 在 pin 消费后补跑（见维护计划 §验证记录）。

## 关联文档和工作项

- [维护计划：2026-10 会话图片供给缝](../plans/maintenance-2026-10-conversation-image-supply.md)
- [docs/api/model-agent-loop.md](../api/model-agent-loop.md) ConversationLoop 节
- 下游跟踪：mira#85（`MIRA-20261009-001`），Mirage M7-05 / M7-07
