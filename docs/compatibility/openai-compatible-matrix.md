# Mira OpenAI-compatible Provider 兼容性矩阵

> 状态：Active  
> 版本：1.5
> 更新日期：2026-09-02
> 负责人：Mira Maintainers  
> 适用范围：LLM/VLM Provider profile、wire dialect 和互操作证据

## 1. 目的

本文记录 Mira 对具体 Provider、endpoint、model 和 dialect 组合的证据。名称中包含
“OpenAI-compatible”不等于兼容全部字段或语义；空白和 `Unknown` 表示尚未验证，不表示支持或不支持。

协议规范见[LLM API 协议设计](../design/llm-api-protocol-design.md)，方言选择见
[DEC-007](../decisions/DEC-007-llm-api-protocol-strategy.md)。

## 2. 证据等级

| 等级 | 含义 | 能否作为生产能力声明 |
| --- | --- | --- |
| `Unknown` | 没有当前证据 | 否 |
| `Configured` | Profile 可表达该配置，未核对上游行为 | 否 |
| `Documented` | 已核对对应服务官方文档，未运行 Mira 互操作测试 | 否 |
| `FixtureVerified` | Mira mapper 通过本地 mock/golden contract fixtures | 只能证明本地实现 |
| `InteropVerified` | 对明确 endpoint/model/version 完成真实互操作测试 | 是，在记录的范围和有效期内 |
| `Failed` | 在记录环境下测试失败 | 否，需保留失败事实 |

证据等级不能跳过范围：`InteropVerified` 只适用于记录的 Provider、base endpoint、dialect、model/revision、
日期和 capability，不外推到同品牌其他模型或未来版本。

## 3. 方言矩阵

| Dialect ID | Endpoint family | 本地 mapper | 同步 HTTP | SSE | 状态 |
| --- | --- | --- | --- | --- | --- |
| `openai.responses.v1` | `/responses` | `ResponsesV1Mapper`（`src/model/model_dialect.cpp`） | `FixtureVerified`（`mira_m3_dialect_test`、`mira_m3_gateway_test`） | `FixtureVerified`（`mira_m3_sse_test`：官方 `response.*` 事件名、兼容别名、分片/配对/唯一 terminal/EOF/取消） | `FixtureVerified`；MiniMax-M3 的明确范围见第 5 节 |
| `openai.chat-completions.v1` | `/chat/completions` | `ChatCompletionsV1Mapper`（同上） | `FixtureVerified`（独立 fixtures；不可表示字段显式失败） | Capability-gated（profile 需声明且 fixture 通过后才可用） | `FixtureVerified`（同步） |

`FixtureVerified` 只证明本地 mock/golden contract suite 通过，不构成对任何真实服务的互操作声明；
真实 endpoint 的状态见第 4、5 节；未列出的 Provider/model 仍保持 `Unknown`。

## 4. Provider 证据总表

| Provider/profile | Dialect | Endpoint origin | Model/revision | 文档证据 | Fixture | Interop | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| OpenAI Responses reference profile | `openai.responses.v1` | `https://api.openai.com/v1` | 未选择 | `Documented` | `Unknown` | `Unknown` | 仅作为首个实现目标，尚未声明可互操作 |
| OpenAI Chat Completions reference profile | `openai.chat-completions.v1` | `https://api.openai.com/v1` | 未选择 | `Documented` | `Unknown` | `Unknown` | 尚未声明可互操作 |
| MiniMax controlled Responses profile | `openai.responses.v1` | `https://api.minimaxi.com/v1` | `MiniMax-M3` | `Documented` | `FixtureVerified` | `InteropVerified`（仅 text、strict schema、Function Tool、同步/SSE、usage、错误与取消） | image=`Failed`；file/upload、parallel Tool、continuation、429 等保持 `Unknown` |
| SiliconFlow controlled Chat Completions profile | `openai.chat-completions.v1` | `https://api.siliconflow.cn/v1` | `Qwen/Qwen3.5-4B` | `Documented` | `FixtureVerified` | `InteropVerified`（text、strict `json_schema`、data URL image input；2026-09-02 探针单 case 证据） | 无 `/responses` 端点；SSE、file/upload、Tool 等保持 `Unknown`；见 §5.2 |
| DeepSeek | 未选择 | 未登记 | 未登记 | `Unknown` | `Unknown` | `Unknown` | 不按品牌推断 |
| Qwen | 未选择 | 未登记 | 未登记 | `Unknown` | `Unknown` | `Unknown` | 不按品牌推断 |
| OpenRouter | 未选择 | 未登记 | 未登记 | `Unknown` | `Unknown` | `Unknown` | 不按品牌推断 |

上表中的 OpenAI `Documented` 仅表示 2026-08-30 已核对本文第 8 节所列官方页面；尚无对
OpenAI endpoint 的凭据调用。MiniMax 结论只适用于第 5.1 节的明确范围。

## 5. Capability 记录模板

### 5.1 MiniMax-M3 / Responses 受控 profile

| 字段 | 记录值 |
| --- | --- |
| Profile ID/version/digest | `8d8e365190e84a508f0908be9b53f70d` / `1.0.0` / `2a498cea2bec7f41899ff185c987eed70becad9b6347e5830636aca9b7b96a9b` |
| Provider/service | MiniMax 开放平台；受控开发测试凭据，不记录 credential 或账户标识 |
| Base endpoint/API version | `https://api.minimaxi.com/v1`，`POST /responses` |
| Dialect | `openai.responses.v1` |
| Model selector/resolved revision | 请求与返回均为 `MiniMax-M3`；Provider 未返回更细 revision |
| Text/image/file input | text=`InteropVerified`；Responses image=`Failed`（2026-09-02 补测排除 data URL 编码因素——MiniMax 400 明确要求 `http(s)://` 或 `data:...;base64` 格式，有效 1×1 基线与公网 URL 均服务端 5xx，chat 纯文本对照 200，归因 MiniMax 服务端图片管道或账号能力未开通）；file=`Unknown` |
| Strict JSON Schema/dialect subset | `InteropVerified`：object、required、additionalProperties=false、boolean const=true；schema digest `79a8e4f7f22c08f2fbd0847e5e2803f3c771318088bae2c03ba20798ecdc6122` |
| Function Tool/parallel Tool | 单个命名 Function Tool=`InteropVerified`；parallel=`Unknown`。实际 endpoint 接受 named choice，官方 Responses 页面只列 `none`/`auto`，作为已知文档偏差保留 |
| Synchronous HTTP/SSE | 均为 `InteropVerified`；真实 SSE 使用 `response.output_item.*` 等官方前缀事件，促成解析器修复与回归 fixture |
| Token count/usage detail | input/output usage 为 `ProviderReported`；cached/reasoning 细项本轮未单独断言 |
| Idempotency endpoint/header/semantics | `Unknown`，未发送重复副作用请求 |
| Conversation/continuation/store | `store=false` 请求被接受；continuation 与远端删除语义=`Unknown` |
| Upload/delete lifecycle | `Unknown`；MiniMax 文档中的 vendor Files API 未被假定等价于 Mira 的 OpenAI `/files` lifecycle，本轮未上传任何文件 |
| Rate-limit headers/`Retry-After` | `Unknown`；未故意触发 429/5xx。image 的 5xx 是真实失败事实，不作为限流证据 |
| Supported generation parameters | `max_output_tokens` 实测；temperature/top_p/service tier 仅 `Documented`，其余 `Unknown` |
| Request/context/output/byte limits | 本轮只使用小型公开 fixture 与 64 output-token 上限；更大限制未验证 |
| Region/retention/ZDR policy | region/ZDR=`Unknown`；wire 明确发送 `store=false`，无用户数据、Memory 或真实 Tool output |
| Evidence level/date/owner | 上述通过项=`InteropVerified`，image=`Failed`，其余逐项如表；2026-09-02；Mira Maintainers |
| Fixture or test report | Linux x86_64 Ubuntu 24.04、GCC 13.3.0、Debug、Mbed TLS `3.6.7`；branch `codex/m3-final-acceptance`，worktree based on `d57e4cf99aee7114830f2fe833b638e469a4a0e0`；完整探针请求上限 6，退出 1 仅因 image Failed |
| Known deviations | Responses/Chat Completions image 均为服务端 5xx（data URL 稳定 `system error (1033)`，http URL `unknown error (1000)`）；裸 base64 被确定性 400 拒绝（`image url must be http(s):// or data:...;base64 (2013)`），即 MiniMax 要求的正是 Mira 已发送的 data URL 格式；MiniMax `detail` 枚举为 `low`/`default`/`high`、无 `auto`，Mira 默认 `ImageDetail::Auto` 会发送 `auto`，属 image 通道恢复后需修正的潜在偏差；Chat Completions 纯文本 200/`Completed` 但 strict `json_schema` 决策未通过本地校验（64 token 上限时 `Incomplete`，1024 时 `Completed`），疑与 thinking 输出或 `response_format` 支持相关，未深入；upload、parallel Tool、continuation、429/Retry-After、region/ZDR 未运行。任何这些字段都不得从 text/Tool 成功外推 |

脱敏证据：同步 canonical response digest 为
`62bb15a259e032a6f45128b6e47215864e68a4c0d51687c4928f8e9524f65d2e`，Provider response ID 仅记录摘要
`4fc7966678127cbf13381948cff510cc32da75690faa9ec95896f891001c8d5a`。凭据来自本地忽略文件，经进程环境
注入；文件内容、Authorization 和原始响应均未写入日志或仓库。

### 5.2 SiliconFlow / Qwen3.5-4B 受控 profile

2026-09-02 用 Mira 探针新增单请求诊断 case 验证；这是首个 image input 达到
`InteropVerified` 的 profile，为 Mira 视觉闭环提供可用 VLM 通道。

| 字段 | 记录值 |
| --- | --- |
| Profile ID/version/digest | 探针临时 profile（复用探针固定 ID，未固化独立 digest）；正式接入前需建立独立 profile 并固化 |
| Provider/service | 硅基流动 SiliconFlow；受控开发测试凭据，不记录 credential 或账户标识 |
| Base endpoint/API version | `https://api.siliconflow.cn/v1`，`POST /chat/completions`；官方文档未提供 `/responses` 端点 |
| Dialect | `openai.chat-completions.v1`（同步） |
| Model selector/resolved revision | 请求 `Qwen/Qwen3.5-4B`（命名无 VL 后缀但实测为 VLM，由维护者确认并经 image case 验证）；返回 revision 未断言 |
| Text/image/file input | text=`InteropVerified`（strict schema + 本地 decision 校验）；image=`InteropVerified`（64×64 纯红 PNG data URL，base64 含 `+`，模型正确返回 `{"red":true}`）；file=`Unknown` |
| Strict JSON Schema/dialect subset | `InteropVerified`：`response_format: json_schema` 生效，`{"red":boolean}` 决策通过本地校验 |
| Function Tool/parallel Tool | `Unknown`，未测 |
| Synchronous HTTP/SSE | 同步=`InteropVerified`；SSE=`Unknown`（Chat Completions SSE 需 profile 声明并过 fixture，未运行） |
| Token count/usage detail | 返回 usage（curl 对照可见 `prompt_tokens`/`completion_tokens`）；质量分级未断言 |
| `max_completion_tokens`/`detail` 字段 | Mira 发送 OpenAI 新字段 `max_completion_tokens` 与 `image_url` 对象内 `detail:"low"` 均被接受（未断言生效语义） |
| Upload/delete lifecycle | `Unknown` |
| Rate-limit headers/`Retry-After` | `Unknown` |
| Region/retention/ZDR policy | `Unknown`；请求未含用户数据 |
| Evidence level/date/owner | 上述通过项=`InteropVerified`；2026-09-02；Mira Maintainers |
| Fixture or test report | Linux x86_64 Ubuntu 24.04、GCC 13.3.0、Debug、Mbed TLS `3.6.7`；`mira_m3_interop_probe` 单请求 case（text-chat、image-red-chat）+ curl 隔离实验；交叉验证模型 `Qwen/Qwen3-VL-8B-Instruct` |
| Known deviations | 无 `/responses` 端点，配置必须走 Chat Completions；base64 含 `+` 直接可用，预转义 `%2B` 被 400 拒绝（服务端不做 URL 解码，符合标准语义）；损坏 PNG 被确定性 400 拒绝（`Verify image file failed`），校验行为健康 |

### 5.3 空白模板

每个实际 profile 增加一张表，不要直接把“总表结论”复制为支持：

| 字段 | 记录值 |
| --- | --- |
| Profile ID/version/digest | 待填写 |
| Provider/service | 待填写 |
| Base endpoint/API version | 待填写 |
| Dialect | 待填写 |
| Model selector/resolved revision | 待填写 |
| Text/image/file input | `Unknown` |
| Strict JSON Schema/dialect subset | `Unknown` |
| Function Tool/parallel Tool | `Unknown` |
| Synchronous HTTP/SSE | `Unknown` |
| Token count/usage detail | `Unknown` |
| Idempotency endpoint/header/semantics | `Unknown` |
| Conversation/continuation/store | `Unknown` |
| Upload/delete lifecycle | `Unknown` |
| Rate-limit headers/`Retry-After` | `Unknown` |
| Supported generation parameters | 待填写 |
| Request/context/output/byte limits | 待填写 |
| Region/retention/ZDR policy | 待填写 |
| Evidence level/date/owner | 待填写 |
| Fixture or test report | 待填写 |
| Known deviations | 待填写 |

能力字段必须逐项记录。不能用一个总布尔值表示“兼容”。

## 6. 互操作验证要求

一次可接受的 `InteropVerified` 记录至少包含：

- 日期、commit/worktree、OS/架构、编译器、HTTP/TLS 库和构建配置；
- Provider、base endpoint、API version、dialect、profile digest、请求 model 和返回 model/revision；
- 使用的受控测试账户/项目类别，不记录 credential；
- text、image、strict schema、Tool、usage、错误、取消，以及所声明 streaming 能力的实际结果；
- 请求/响应 schema digest、HTTP/provider request ID 的脱敏摘要、执行命令和退出码；
- 429/5xx 等无法安全触发的用例应由官方 sandbox、可控 stub 或明确未运行项表示，不能伪造生产故障；
- 数据分类、region、`store`/retention 和 remote upload cleanup 结果；
- 已知偏差、失败项、负责人和补跑条件。

真实请求只在测试数据、费用预算、endpoint/proxy allowlist 和授权凭据已就绪时执行。禁止用生产用户的
截图、Memory 或 Tool output 做 capability probe。

仓库提供默认不联网的 `mira_m3_interop_probe`。它在缺少 `MIRA_INTEROP_API_KEY`、明确 model、PEM CA
bundle 或显式请求上限时退出 2。两请求模式覆盖 Responses 同步与 SSE strict schema；三请求模式额外覆盖
受控 text fixture 的 upload/delete；六请求模式还覆盖 Function Tool、1×1 合成 PNG、无效 model 错误映射
和 Executor timer 驱动的协作取消。`MIRA_INTEROP_MAX_REQUESTS=1` 配合
`MIRA_INTEROP_CASE=image|image-red|image-red-b64|image-url|image-red-chat|text-chat`
可用于单请求脱敏诊断：分别覆盖 1×1 data URL 基线、64×64 data URL、裸 base64、公网 http URL、
Chat Completions data URL 与 Chat Completions 纯文本对照（`MIRA_INTEROP_IMAGE_URL`、
`MIRA_INTEROP_MAX_TOKENS` 可覆盖默认值）。诊断经只改写 `image_url` 编码的传输装饰器执行，
保留真实 Executor/TLS/provider 栈，并打印 HTTP 状态与 4xx/5xx 错误体摘要。输出只含 digest、
model、usage quality 和 capability 结论。未执行能力仍必须保持 `Unknown`。

授权环境的最小复现入口（key 由受控 secret 注入，不写入命令、日志或仓库）：

```sh
MIRA_INTEROP_MODEL='<approved-model>' \
MIRA_INTEROP_CA_FILE='<approved-ca-bundle.pem>' \
MIRA_INTEROP_MAX_REQUESTS=2 \
./build/debug/tests/mira_m3_interop_probe
```

三请求模式把 `MIRA_INTEROP_MAX_REQUESTS` 设为 `3` 并额外执行 upload/delete；完整 capability 模式设为
`6`。需要显式代理时只接受不含 userinfo 的 `MIRA_INTEROP_PROXY_URL`，credential 通过
`MIRA_INTEROP_PROXY_AUTH` 单独注入。私网测试代理还需逐次显式授权
`MIRA_INTEROP_PROXY_ALLOW_PRIVATE=1`；该开关不放宽目标 endpoint 的 SSRF 校验。

## 7. 失效与降级

- Provider API version、model revision、dialect mapper、profile digest、Structured Output schema subset 或
  关键 header 变化时，相关证据至少降为 `Documented`，直至重跑 fixtures/interop。
- 最近一次同范围互操作失败时记录 `Failed`，不能保留旧的绿色结论而只在备注中隐藏失败。
- 超过产品规定证据有效期时降为 `Unknown` 或 `Documented`；有效期应在 M3 实施中冻结。
- 运行时发现 capability mismatch 时该 operation 失败并产生 health/compatibility 事件，不自动切换
  dialect。后续路由只使用仍满足数据政策和已验证 capability 的 profile。

## 8. 当前官方资料

2026-08-30 查阅的 OpenAI 官方协议资料：

- [Create a model response](https://developers.openai.com/api/reference/resources/responses/methods/create)
- [Streaming API responses](https://developers.openai.com/api/docs/guides/streaming-responses)
- [Structured model outputs](https://developers.openai.com/api/docs/guides/structured-outputs)
- [Function calling](https://developers.openai.com/api/docs/guides/function-calling)
- [Conversation state](https://developers.openai.com/api/docs/guides/conversation-state)
- [Upload a file](https://developers.openai.com/api/reference/resources/files/methods/create)
- [Delete a file](https://developers.openai.com/api/reference/resources/files/methods/delete)

这些资料用于定义 OpenAI reference profile，不是 DeepSeek、Qwen、OpenRouter 或其他兼容服务的证据。

2026-09-02 核对的 MiniMax 官方资料：

- [Responses 对话生成](https://platform.minimaxi.com/docs/api-reference/responses-create)
- [OpenAI SDK 与 MiniMax-M3 能力](https://platform.minimaxi.com/docs/api-reference/text-openai-api)
- [速率限制](https://platform.minimaxi.com/docs/guides/rate-limits)
- [错误码](https://platform.minimaxi.com/docs/api-reference/errorcode)

## 9. 变更记录

### 2026-09-02：SiliconFlow Qwen3.5-4B 视觉互操作（首个 image `InteropVerified`）

- 范围：`mira_m3_interop_probe` 单请求 case（`text-chat`、`image-red-chat`）走 Mira 真实
  Executor/TLS/chat mapper 栈，`Qwen/Qwen3.5-4B`；辅以 curl 单变量隔离实验与
  `Qwen/Qwen3-VL-8B-Instruct` 交叉验证。公开合成 fixture，无用户数据。
- 结果：text + strict `json_schema` 通过（200/`Completed`/本地 decision 校验）；image input
  通过（64×64 纯红 PNG data URL，base64 含 `+`，模型正确返回 `{"red":true}`）。
  隔离实验确认：裸 `+` 直接可用，`%2B` 预转义被 400 拒绝（服务端不做 URL 解码）；损坏 PNG
  被确定性 400 拒绝。配置勘误：原配置 `wire_api="responses"` 有误（硅基流动无 `/responses`
  端点），已改 `chat`；`model_reasoning_effort` 仅个别模型支持，已移除。
- 过程更正：初轮 image 探针失败（`broken PNG (bad header checksum)`）系探针红色 fixture
  转录缺陷（132/134 字节），非 SiliconFlow 问题；fixture 已程序化重嵌并复验，重跑通过。
- 关联：本节 5.2、[M3 计划验证记录](../plans/m3-model-provider-agent-loop.md)、
  [SiliconFlow Chat Completions 文档](https://api-docs.siliconflow.cn/docs/api/chat-completions-post)。

### 2026-09-02：MiniMax-M3 image 编码因素排查（补充）

- 范围：单请求诊断 case（`MIRA_INTEROP_CASE=image|image-red|image-red-b64|image-url|
  image-red-chat|text-chat`），公开合成 fixture（1×1 PNG、64×64 纯红 PNG）与公网 PNG URL，
  `store=false`，共 7 请求；未使用用户数据。凭据注入方式同日首条记录。
- 结果：基线（Responses + data URL + 1×1）稳定复现 HTTP 500 `system error (1033)`，排除瞬时故障；
  裸 base64 得到确定性 HTTP 400 `invalid param: image url must be http(s):// or data:...;base64
  (2013)`，证明 MiniMax 要求的正是 Mira 已发送的 data URL 格式；公网 http URL 500
  `unknown error (1000)`；Chat Completions + data URL 500 (1033)，Chat Completions 纯文本对照
  HTTP 200。事后更正：当轮 64×64 红色 PNG fixture 存在转录缺陷（132/134 字节，损坏 IDAT），
  硅基流动对同一损坏图返回确定性 400 而非 5xx，故该轮“image-red 500”不能单独作为尺寸排除证据；
  但有效 1×1 基线与公网 URL 均 500，MiniMax 服务端图片管道归因不变。有效 fixture 的 image-red
  重跑（1 请求）待执行。
- 结论：image 维持 `Failed`，归因候选为 MiniMax 服务端图片管道缺陷或该测试账号多模态能力未开通
  ——与官方文档"MiniMax-M3 原生支持图片/视频输入"冲突，需供应商侧确认；Mira 侧 wire 格式无需改动。
  补跑条件：供应商确认或能力开通后重跑上述诊断 case。
- 新记录偏差：`detail` 枚举无 `auto`（Mira 默认发送 `auto`，待 image 通道恢复后需 profile 级映射）；
  Chat Completions 纯文本 200/`Completed` 但 strict `json_schema` 决策未过本地校验（64 token 时
  `Incomplete`，1024 时 `Completed`），未作为任何能力声明。
- 关联：`M3-19`、本节 5.1、[M3 计划验证记录](../plans/m3-model-provider-agent-loop.md)。

### 2026-09-02：MiniMax-M3 Responses 真实互操作

- 范围：受控测试凭据、公开合成 fixture、最多六请求、`store=false`；未使用用户数据。
- 结果：同步/SSE strict schema、Provider-reported usage、命名 Function Tool、无效 model 错误映射和
  10ms 协作取消通过；Responses image 返回 5xx 并记为 `Failed`。完整探针因该已知失败退出 1，支持项依据
  逐 capability 结果标为 `InteropVerified`，未执行项保持 `Unknown`。
- 修复：真实 SSE 暴露官方 `response.output_item.*` 前缀缺口；解析器现接受官方闭集并保留既有兼容别名，
  `mira_m3_sse_test` 增加官方事件名回归。
- 安全：credential 内容未读取到会话、未打印、未提交；本地 secret 精确 gitignore 且权限 `0600`。
- 关联：`M3-19`、本节 5.1 profile、[MiniMax Responses 文档](https://platform.minimaxi.com/docs/api-reference/responses-create)。

### 2026-08-31：方言 fixture 证据

- 范围：两个 dialect 的同步 mapper、Responses SSE 状态机、错误映射与 `Retry-After` 解析的本地实现。
- 验证：`mira_m3_dialect_test`（golden 编解码、不可表示字段 fail-closed、错误状态映射）、
  `mira_m3_sse_test`（任意分片、事件配对、唯一 terminal、EOF 无 terminal 判定为
  `AmbiguousCompletion`、预览隔离）、`mira_m3_gateway_test`（经过 mock transport 的端到端
  canonical 结果）。传输层证据见 [DEC-008](../decisions/DEC-008-transport-dependency-strategy.md)。
- 限制：全部为本地 fixture；没有对任何真实 endpoint 的调用。互操作状态保持 `Unknown`，
  `M3-19` 待受控凭据就绪后执行。
- 关联：[M3 Model Provider 与 Agent 闭环](../plans/m3-model-provider-agent-loop.md)。

### 2026-09-01：代理、TLS 与 upload fixture 证据

- 范围：HTTP/HTTPS proxy、锁定 Mbed TLS 通道、remote `/files` upload/delete 和 fail-closed interop
  probe admission。
- 验证：Linux Debug 29/29；`mira_m3_mbedtls_test` 覆盖 direct/CONNECT TLS 与错误 CA；
  `mira_m3_upload_test` 覆盖 file ID、retention timer、shutdown 和删除失败；probe 缺凭据退出 2 且未联网。
- 限制：没有受控 API credential，因此 Provider 表的 `Interop` 仍全部为 `Unknown`；Windows/Android
  Mbed TLS 目标证据见平台矩阵。
- 关联：[DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)、`M3-04`、`M3-15`、`M3-19`。

### 2026-08-30：建立基线

- 范围：冻结两个 dialect ID、证据等级和 profile 记录模板。
- 验证：只核对官方文档和仓库设计；未实现 mapper，未执行 API 调用。
- 限制：所有真实互操作状态保持 `Unknown`。
- 关联：[M3 Model Provider 与 Agent 闭环](../plans/m3-model-provider-agent-loop.md)。
