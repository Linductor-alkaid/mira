# 维护计划：桥接宿主真机反馈轮（wire 分隔 / 助手发言记忆 / 解码参数缝）

> 状态：Completed（Linux 离线范围；平台与真实宿主补跑项见风险与限制）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M36 后维护轮）
> 前置：DEC-047、DEC-053、DEC-016、DEC-090、ConversationLoop、M17
> 更新日期：2026-11-09

## 1. 目标

消化 mira-bridge 真机捕获代理逐字节对比发现的三项反馈，修复宿主集成中的三个
结构缝：

1. `BUG-20261109-002`：Chat Completions 纯文本消息多 `TextPart` 裸拼接丢失
   part 边界（goal/信封行/结算记录在 wire 里粘连成一行）。依据
   [DEC-103](../decisions/DEC-103-dialect-text-part-separator.md)。
2. 跨 burst 记忆没有"个体说过什么"：事件存储与会话视图不表达 assistant 发言，
   宿主回放前缀缺"自己答了什么"，结算条目措辞不区分发言通道。依据
   [DEC-104](../decisions/DEC-104-assistant-speech-events.md)。
3. 宿主没有解码参数缝：`ConversationLoopConfig` 无采样字段，
   `extra_body` 保留键挡死 `temperature`/`top_p`/`seed`。依据
   [DEC-105](../decisions/DEC-105-profile-decoding-defaults.md)。

## 2. 范围与非目标

### 2.1 范围

- `src/model/model_dialect.cpp`：Chat 无图纯文本分支以 `\n` 连接非空
  `TextPart`（`MNT-202611-BR-01`）。
- `include/mira/model_profile.hpp` / `src/model/model_profile.cpp`：
  `ProfileDecodingOptions` + `ModelProfile::decoding` + manifest/digest/
  validate + `effective_generation_options()`；三方言 mapper 接入
  （`MNT-202611-BR-03`）。
- `src/model/conversation_loop.cpp`：每轮发射 `AssistantSpeech` 事件（工具轮
  + 终文本，有界、带状态与 sensitivity）；`include/mira/conversation_log.hpp` /
  `src/model/conversation_log.cpp`：`Kind::AssistantSpeech` 条目投影与
  `ConversationSettled` Answered 措辞区分（`MNT-202611-BR-02`）。
- `src/context/context_retrieval.cpp`：M17 分段条目行 `assistant: ` 前缀。
- 测试与文档同步见各决策记录验证方式。

### 2.2 非目标

- 不改动 AgentLoop（设备闭环）的发言事件面与 `LoopSettled` 语义。
- 不给 `ConversationLoopConfig` 增加每 run 解码覆盖（DEC-105 备选，无真实
  需求不加）。
- 不放开 `extra_body` 保留键名单；不改动三方言的 multipart/content 块渲染
  分支（仅 Chat 无图纯文本分支）。
- 不替宿主决定发言的角色折叠与"哪些工具是发声通道"——Core 只产出事实条目。
- 不做真实模型/真实端点的采样效果验证（外部凭据缺失；属宿主部署验证）。

## 3. 设计与决策依据

- [DEC-103](../decisions/DEC-103-dialect-text-part-separator.md)：wire 分隔契约。
- [DEC-104](../decisions/DEC-104-assistant-speech-events.md)：AssistantSpeech
  事件、视图条目与结算措辞。
- [DEC-105](../decisions/DEC-105-profile-decoding-defaults.md)：profile 解码默认
  与合并语义。
- [DEC-053](../decisions/DEC-053-conversation-image-supply-seam.md)（折叠进
  user 消息的供给内容受益于 BR-01）、
  [DEC-090](../decisions/DEC-090-profile-extra-body.md)（保留键与 manifest
  审计先例）、[DEC-016](../decisions/DEC-016-conversation-events-and-user-messages.md)
  （视图投影与 fail-closed 纪律）。

## 4. 工作项与验收

- [x] `MNT-202611-BR-01`：Chat 纯文本分支非空 part 以 `\n` 连接；单 part 与
  空 part 字节不变；带图分支不受影响；方言测试固定契约。
- [x] `MNT-202611-BR-02`：ConversationLoop 每轮发射 AssistantSpeech（载荷、
  有界、截断标记、状态、sensitivity）；视图新增 AssistantSpeech 条目与
  渲染；损坏载荷 DataLoss；ConversationSettled Answered 措辞按
  tool_executions 区分；M17 分段 `assistant: ` 行。
- [x] `MNT-202611-BR-03`：`ModelProfile::decoding` 校验（范围、策略冲突、
  Messages seed）+ manifest/digest + `effective_generation_options` + 三方言
  mapper 套用与请求值优先；空 decoding 回归既有字节。

## 5. 风险与限制

- 事件面新增条目会改变 `build_conversation_view` 的重建结果；宿主缓存与
  派生资产由宿主决定重建（DEC-016 先例）。
- `ConversationEntry::Kind`、`ModelProfile` 均为公开类型加法变化，C++ 消费者
  需重编译。
- AssistantSpeech 记录模型自产内容（有界、`Internal`）；宿主经工具转发敏感
  输入时的把关在宿主层，事件面不承诺内容脱敏。
- 采样参数对真实端点的实际效果、以及桥接宿主升级后的回放收益，需宿主部署
  验证；本轮只验收 Linux 离线 fixture。

- [ ] Windows/Android 当前变更验证未执行：本轮缺少对应环境，负责人 Mira
  Maintainers；补跑条件为对应 CI/工具链及需要设备的运行验证。
- [ ] mira-bridge 真机回放与逐字节对比复验未执行：本轮无桥接宿主环境，负责人
  部署宿主维护者；补跑条件为升级后重跑捕获代理对比，撤除宿主侧尾换行绕过
  （提交 `18b7694`），保存回放证据。

## 6. 验证记录

2026-11-09：开始实施；三处代码修复与决策记录（DEC-103/104/105）落盘，设计/API/
总计划文档同步，Debug 全量构建通过后交独立验证代理执行测试环节。

2026-11-09：独立验证完成，三个工作项按 Linux 离线范围关闭 `BUG-20261109-002`。

- 环境：`feat/interaction-concurrency-recovery` 未提交工作树，Linux x86_64、
  GCC 13、C++20、Debug、warnings-as-errors 开启；仅脚本化模型与
  MemoryEventStore，无真实网络请求或平台输入。
- 测试由 Independent-Verification-Agent 独立编写与执行（仅动 `tests/`）：
  3 处过期契约按新契约更新（`m3_conversation_projection_test` 视图 2→4 条与
  新结算措辞；`m34_subagent_test`、`m35_subagent_tools_test` 的事件载荷
  断言收紧为「仅允许出现在 AssistantSpeech，工具结果仍禁止」——DEC-104
  明确工具参数进入事件存储、Internal 标注、不承诺脱敏），新增
  `tests/m3/m3_assistant_speech_test.cpp`（目标 `mira_m3_assistant_speech_test`：
  事件矩阵、有界/截断、executed/failed/skipped/not_executed 状态、未发声
  输出零发射、Continuous 顺序、Chat wire 行独立）及方言分隔、投影渲染/
  损坏矩阵/措辞/M17 前缀、decoding 三方言 wire/校验/manifest-digest 等用例，
  分布于 `m3_dialect_test`、`m3_extra_body_test`、`m3_conversation_projection_test`。
- 修复前基线：3 个测试失败，均为上述测试过期，无实现缺陷归属。
- 修复后：

  ```bash
  cmake --build build/debug -j 6
  ctest --test-dir build/debug --output-on-failure -j 4
  ```

  Debug 全量 **166/166** 通过、无失败/跳过，用时约 39 秒。
- 门禁：`python3 -I tools/check_docs.py .`（Markdown links and fences: OK）、
  `python3 -I tools/check_architecture.py`（architecture policy is clean）、
  `python3 -I tools/check_platform_boundary.py`（Mira Core platform boundary
  is clean）、改动文件与新增测试 `clang-format --dry-run --Werror` 全部通过、
  `git diff --check` 干净。
- 已知口径：视图投影对 `arguments` 非字符串按缺失处理（仅省略该段），
  wire_name/status 等必需字段损坏才 fail closed；已由测试固定。
- 限制：真实端点对 wire 分隔与采样参数的实际行为、mira-bridge 升级后的
  回放收益、Windows/Android 变更验证均未执行——条件与负责人见上节风险
  与限制，不得以本地 fixture 结论代替。
