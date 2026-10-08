# 方言思考输出维护（Chat Completions / Responses）

> 状态：Completed（Linux 本地交付）
> 负责人：Linductor-alkaid
> 更新日期：2026-10-08
> 所属计划：[Mira 实施计划](mira-implementation-plan.md)
> 决策：[DEC-052](../decisions/DEC-052-dialect-thinking-output.md)
> 反馈来源：Mirage MIRA-20261008-001

- [x] DT-01：Chat Completions 非流式 `reasoning_content` 与流式 `delta.reasoning_content`
  映射为有界 `ThinkingPart`（预算失败闭合，不进正文预览）。
- [x] DT-02：Responses 终态 `reasoning` item（raw `reasoning_text` 优先、否则 summary
  按 redacted 保留）与 SSE reasoning 事件族（身份 + 预算校验，权威内容取终态 body）。
- [x] DT-03：工具循环回放门控 `dialect_accepts_thinking_replay`；两方言思考输出不
  进入权威历史，Messages 行为不回退。
- [ ] DT-04：真实 reasoning 模型在线增量可见复验（宿主侧）。未执行原因：需真实凭据与
  付费请求，Mira 仓库环境不可用；负责人 Mirage 维护者，补跑条件为 Mirage 升级 pin 至
  本修复并按其台账 MIRA-20261008-001 复验（两方言各一轮真实增量可见）。

## 验证记录

2026-10-08：DT-01/02/03 完成。基线 master `d5c7d06`（PR #82 之后），Linux x86_64/GCC
Debug（`/home/linductor/mira/build/debug`），Executor `2ae4fc8` 未变。为满足
`tools/architecture-policy.json` 的 1200 行文件预算，`ResponsesV1Mapper` 与
`decode_responses_terminal_body` 从 `model_dialect.cpp` 纯移动到新文件
`model_responses.cpp`，共享辅助移入 `model_dialect_internal.hpp`；公开头文件契约不变。

测试由独立验证代理编写并执行：新增 `m3_dialect_test.reasoning_maps_to_bounded_thinking`、
`m3_sse_test.chat_reasoning_stream_maps_to_thinking`、
`m3_sse_test.responses_reasoning_stream_events_and_terminal_reduction`、
`m3_conversation_loop_test.chat_reasoning_stays_observability_only_in_tool_rounds`，
覆盖两方言增量/终态/逐字节切点/越界失败闭合/redacted 语义/畸形失败闭合/混排顺序，
以及工具轮回放门控（Chat 思考不回放且编码成功、Messages signed thinking 回放对照）。
`ctest -R 'mira_m3_(canonical|schema|dialect|sse|gateway|anthropic|conversation_loop)_test'`
7/7 通过；更宽的 18 个 `mira_m3_*` 测试 17/18 通过，唯一失败
`mira_m3_transport_test` 经 `git stash` 在干净 HEAD 复现（本机沙箱网络下连接 deadline
不生效），与本改动无关、不在传输路径上。`format-check`/`docs-check`/
`architecture-check` 通过。

已知限制：(1) JSON 解析层 `JsonLimits::max_string_bytes`（1 MiB）对超限字符串先行
失败闭合，方言层 4 MiB 思考预算校验为纵深防御，其专用错误消息在公开解析路径上不可
观测（Messages 方言同性质）；fail-closed 语义由 fixture 按"decode 失败"断言。
(2) Chat SSE 纯 reasoning 流终态多出一个空 `MessageOutput`（`finish()` 恒序列化
`content:""` 的既有行为），与非流式 `content: null` 的 1 项结果不对称；无下游危害，
已按实际行为断言。Windows、其他厂商在线请求与真实 reasoning 模型增量保持未验收
（DT-04），由维护者在具备目标配置后补跑。
