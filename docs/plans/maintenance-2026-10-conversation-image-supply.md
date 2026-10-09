# 维护计划：会话图片供给缝（Mirage 反馈 #85 消化，2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：M25（Completed）、M3 对话循环（MNT-202610-03/04，Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-09

## 1. 目标

消化 Mirage 第三轮依赖反馈 [mira#85](https://github.com/Linductor-alkaid/mira/issues/85)
（`MIRA-20261009-001`，P2）：为 `ConversationLoop` 提供宿主可控、逐轮有界的结构化
输入供给缝，使宿主工具（截图等）产出的图片能以规范 `ImagePart` 进入下一次模型
请求，支撑「截图工具 → 工具结果 → 模型看图 → 后续工具动作」闭环
（[DEC-053](../decisions/DEC-053-conversation-image-supply-seam.md)）。

## 2. 范围与非目标

### 2.1 范围

- `include/mira/conversation_loop.hpp` / `src/model/conversation_loop.cpp`：
  `ModelInputSupplier` 回调 + `ModelInputSupplyOptions` 有界选项 +
  `set_model_input_supplier()`；每轮 `build_request` 前恰一次解析；非可信纪律
  （User role、`mira.conversation-loop.supply.v1` provenance、Internal authority、
  词表白名单）、条目/字节预算丢弃、错误/异常降级诊断、结果计数。
- 测试：`tests/m3/m3_conversation_loop_test.cpp` 扩展供给缝场景矩阵（合成截图
  工具 + artifact 发布 + 三方言线格式 + 边界矩阵）。
- 文档：[DEC-053](../decisions/DEC-053-conversation-image-supply-seam.md)、
  `docs/api/model-agent-loop.md` ConversationLoop 节。

### 2.2 非目标

- 不改动模型内容契约（`ToolResultPart` 不扩图片形态）、三家方言与路由门禁。
- 不改动 `AgentLoop` 设备闭环与其 Working Context 供给缝（DEC-045）。
- 不在 Mira 侧自动采屏、固定桌面策略或执行平台输入（issue #85 明确不要求）。
- 不做真实识图模型的端到端验证（外部凭据缺失；属 Mirage pin 消费后的下游补验）。

## 3. 设计与决策依据

- [DEC-053](../decisions/DEC-053-conversation-image-supply-seam.md)：本轮全部
  公开契约新增（供给回调、有界选项、失败语义）。
- [DEC-047](../decisions/DEC-047-conversational-loop-and-canonical-tool-parts.md)：
  ConversationLoop 既有循环与规范工具往返不变。
- [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)：供给缝形态
  先例（可选回调、每请求恰一次、降级不阻塞、回调内闭合纪元门控）。
- [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)：工具执行边界
  不变；截图工具仍是普通 BuiltIn 工具，图片经其宿主状态进入回调。

## 4. 工作项

- [x] `MNT-202610-06`（#85 / MIRA-20261009-001）公开 `ModelInputSupplier` /
  `ModelInputSupplyOptions` / `set_model_input_supplier()` 与结果计数；语义按
  DEC-053 冻结：每轮恰一次解析、User/词表/Secret 校验丢弃、`max_items`/
  `max_image_bytes` 丢弃、错误与异常降级、供给内容不进入 tool-history 回放。
  验收：无供给缝时请求装配逐字节零漂移；供给图片以规范 `ImagePart` 进入下一
  请求且 ToolCallPart/ToolResultPart 配对完整。
- [x] `MNT-202610-07`（#85 / MIRA-20261009-001）方言验收矩阵：Chat `image_url`、
  Responses `input_image`、Messages `image`（Auto detail）线格式 fixture；Messages
  + 非 `Auto` detail 与无识图能力 profile 显式失败，不静默降级。
- [x] `MNT-202610-08`（#85 / MIRA-20261009-001）边界矩阵：空供给回归、条目/字节
  预算耗尽丢弃计数、role/词表/Secret 逐项丢弃、回调 Error 与异常降级、供给
  重试重放、取消与 MaxTurns 下的行为、旧文本会话全量回归。
- [ ] `MNT-202610-09`（#85 / MIRA-20261009-001）文档同步：DEC-053、API 手册
  ConversationLoop 节、总计划索引已完成；**issue #85 回复未发布**（原因：发布
  属对外动作，待维护者确认后以 PR 评论或 issue 回复形式发布；负责人：Mira
  Maintainers；补跑条件：本轮 PR 开出时同步回复）。

## 5. 风险与阻塞

- 真实模型「截图内容可见」验收需一家显式声明识图能力的真实模型与凭据；本机
  无法执行，依赖 Mirage pin 消费后的端到端复验（issue #85 验收条件最后一条，
  记录于 §7 限制）。
- 供给图片进入请求后，路由门禁按 `capabilities.image_input` 判定；测试 profile
  默认声明该能力，负向路径需显式关闭能力位验证拒绝面。

## 6. 测试与退出条件

- [x] `MNT-202610-06`：请求装配零漂移回归 + 供给图片规范进入请求 + 配对完整。
- [x] `MNT-202610-07`：三方言线格式 fixture + 两类显式拒绝路径。
- [x] `MNT-202610-08`：边界矩阵全部有断言（丢弃/降级均可在事件面或结果计数
  观察，无静默路径）。
- [x] 全量 ctest 通过（Linux x86_64 GCC Debug）与 `architecture-check`、
  `docs-check` 通过。（本机 104/105，唯一失败 `mira_m3_transport_test` 为预存
  环境性失败，已由 PR #86 CI Linux matrix 覆盖为全绿；`architecture-check`/
  `docs-check`/quality 全部通过，见 §7 第二条记录。）
- [ ] 真实识图模型端到端（**不勾选**，见 §7 限制与补跑条件）。

## 7. 验证记录

2026-10-09：实现轮（工作树 = master `bfcb8e7` + 本轮变更，未提交）。环境：
Linux x86_64，GCC 13.3.0，CMake debug 预设。

- 实现：`include/mira/conversation_loop.hpp`（`ModelInputSupplier` 回调、
  `ModelInputSupplyOptions`、`set_model_input_supplier`、结果三计数器）、
  `src/model/conversation_loop.cpp`（`resolve_supplied_input` 每轮恰一次解析、
  逐条校验与双预算丢弃、Error/异常/非法 options 三类降级、
  `ModelInputSupplied`/`ModelInputSupplyDegraded` 事件）。`architecture-check`
  clean（conversation_loop.cpp 516 行、头文件 171 行，均在 1200 预算内）；
  `docs-check` 通过；触碰文件 clang-format 18.1.3 格式化，重编译 0 警告。
- 测试（Independent-Verification-Agent 独立执行）：新增
  `tests/m3/m3_conversation_input_supply_test.cpp`（1139 行，注册
  `mira_m3_conversation_input_supply_test`），**15 场景全部通过**：零漂移回归
  （无 supplier 零事件零计数）；截图工具→MemoryArtifactStore 发布→供给→下轮
  `ImagePart`（artifact id/digest/byte_size/media_type 逐项一致，
  ToolCallPart/ToolResultPart 原生成对，图仅在 User 条目）；每请求恰一次解析
  （2 请求 resolve==2）；不跨轮累积；六类校验丢弃矩阵（role/empty/vocabulary/
  artifact/media-type/sensitivity 各 1 + 1 合法，drop_reasons 逐键断言）；
  item-budget 与 image-budget 丢弃；supplier Error/异常/非法 options 三类降级
  （循环不阻塞、异常文本不进事件面）；恢复重试重放（网关 RetryBudget 内部
  重试 + 循环恢复轮共 3 次 provider 派发全部携带同图，resolve==2）；预取消
  （零模型请求、supplier 未被调用）；无识图能力 profile 显式 Failed；Messages
  方言 detail 约束（mapper 级 + canonical 循环级，Auto 可用、非 Auto 显式
  `CapabilityMismatch`）；`ModelContentPart` 构造与默认值 static 断言。
- 行为钉死记录：降级轮只发 `ModelInputSupplyDegraded`、不发当轮
  `ModelInputSupplied`；`ImageDetail` 在缝中原样透传（方向中性）。
- 全量回归：`ctest --test-dir build/debug --output-on-failure` **104/105 通过**。
  唯一失败 `mira_m3_transport_test`（`m3_transport_test.cpp:506` 墙上时钟断言
  `elapsed < 2s`，连接 deadline 400ms）经 `git stash` 在干净 HEAD `bfcb8e7`
  上复现同样失败，确认为本机 sandbox 时钟/socket 节流的预存环境问题，与本次
  变更无关；负责人：Mira Maintainers；补跑条件：PR CI Linux matrix。
- 缺陷迭代记录：DEC-053 初稿对 Messages 方言 `ImageDetail` 约束方向描述颠倒
  （写成"要求非 Auto"），验证轮按 `model_anthropic.cpp` 既有映射纠正为"只接受
  Auto，非 Auto 显式 CapabilityMismatch"，文档已同步（实现本身方向中性，
  detail 透传）。
- 限制与补跑：真实识图模型端到端（issue #85 验收条件"至少一家显式声明识图
  能力的真实模型验证截图内容可见"）未执行——原因：本机无外部模型凭据；负责
  人：Mirage 维护者（Linductor-alkaid）；补跑条件：Mirage 完成授权消费、pin
  升级与供应链同步后按 issue #85 端到端复验，届时台账项方可记为 Resolved。
  Windows/Android 编译路径未在本机执行（补跑条件：PR CI matrix）。issue #85
  的上游结论回复未发布（见 `MNT-202610-09`）。

2026-10-09：PR [#86](https://github.com/Linductor-alkaid/mira/pull/86)（分支
`mnt/conversation-image-supply`，commit `f631254` + `25253f5`）CI 验证与合并。

- 第 1 轮（runs [`37874958569`](https://github.com/Linductor-alkaid/mira/actions/runs/37874958569)
  / `37874922126`）：Android arm64/x86_64 通过；Linux GCC/Clang Debug+Release、
  Windows Debug+Release、ASAN/UBSAN/TSAN、quality 全部失败——唯一失败测试为
  `mira_docs_test`：`tools/check_docs.py` 的链接正则不跳过代码围栏，API 手册
  示例中 C++ lambda 的捕获组紧跟参数列表（`[&state]` 后直接 `(const ...)`）
  被误判为 Markdown 链接（全仓库首个围栏内 lambda）。修复 `25253f5` 将示例
  改写为自由函数形式（检查器行为本身未改，避免扩大本轮范围；围栏误报的
  工具缺陷留待后续工具轮处理）。
- 第 2 轮（run [`37876824953`](https://github.com/Linductor-alkaid/mira/actions/runs/37876824953)）：
  **24/24 检查全部通过**——Linux GCC/Clang Debug+Release（105/105 含
  `mira_m3_transport_test`，确证本机该失败为环境性）、Windows Debug+Release、
  Android arm64+x86_64、ASAN/UBSAN/TSAN、quality（clang-tidy、clang-format、
  docs、SBOM、平台边界）。
- 合并：merge commit `d5e6673`（2026-10-09），工作分支远端与本地均已删除。
  本轮维护计划保持 In Progress：仅余真实识图模型端到端（§7 第一条记录的
  补跑条件）与 issue #85 回复发布（`MNT-202610-09`）两项下游/对外动作。
