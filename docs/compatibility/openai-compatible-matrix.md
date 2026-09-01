# Mira OpenAI-compatible Provider 兼容性矩阵

> 状态：Active  
> 版本：1.2
> 更新日期：2026-09-01
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
| `openai.responses.v1` | `/responses` | `ResponsesV1Mapper`（`src/model/model_dialect.cpp`） | `FixtureVerified`（`mira_m3_dialect_test`、`mira_m3_gateway_test`） | `FixtureVerified`（`mira_m3_sse_test`：分片/配对/唯一 terminal/EOF/取消） | `FixtureVerified` |
| `openai.chat-completions.v1` | `/chat/completions` | `ChatCompletionsV1Mapper`（同上） | `FixtureVerified`（独立 fixtures；不可表示字段显式失败） | Capability-gated（profile 需声明且 fixture 通过后才可用） | `FixtureVerified`（同步） |

`FixtureVerified` 只证明本地 mock/golden contract suite 通过，不构成对任何真实服务的互操作声明；
真实 endpoint 的状态见第 4 节，全部保持 `Unknown`（`M3-19` 未执行）。

## 4. Provider 证据总表

| Provider/profile | Dialect | Endpoint origin | Model/revision | 文档证据 | Fixture | Interop | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| OpenAI Responses reference profile | `openai.responses.v1` | `https://api.openai.com/v1` | 未选择 | `Documented` | `Unknown` | `Unknown` | 仅作为首个实现目标，尚未声明可互操作 |
| OpenAI Chat Completions reference profile | `openai.chat-completions.v1` | `https://api.openai.com/v1` | 未选择 | `Documented` | `Unknown` | `Unknown` | 尚未声明可互操作 |
| DeepSeek | 未选择 | 未登记 | 未登记 | `Unknown` | `Unknown` | `Unknown` | 不按品牌推断 |
| Qwen | 未选择 | 未登记 | 未登记 | `Unknown` | `Unknown` | `Unknown` | 不按品牌推断 |
| OpenRouter | 未选择 | 未登记 | 未登记 | `Unknown` | `Unknown` | `Unknown` | 不按品牌推断 |

上表中的 OpenAI `Documented` 仅表示 2026-08-30 已核对本文第 8 节所列官方页面；尚无 Mira
实现、凭据调用或真实网络测试证据。

## 5. Capability 记录模板

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
bundle 或 `MIRA_INTEROP_MAX_REQUESTS=2|3` 时退出 2；授权后覆盖 Responses 同步与 SSE strict schema，
三请求模式额外覆盖受控 text fixture 的 upload/delete。输出只含 profile digest、requested/resolved model、
usage quality 和 upload cleanup 结论。该 probe 不能单独替代本节要求的 image、Tool、错误、取消和政策记录；
未执行能力仍必须保持 `Unknown`。

授权环境的最小复现入口（key 由受控 secret 注入，不写入命令、日志或仓库）：

```sh
MIRA_INTEROP_MODEL='<approved-model>' \
MIRA_INTEROP_CA_FILE='<approved-ca-bundle.pem>' \
MIRA_INTEROP_MAX_REQUESTS=2 \
./build/debug/tests/mira_m3_interop_probe
```

三请求模式把 `MIRA_INTEROP_MAX_REQUESTS` 设为 `3` 并额外执行 upload/delete；需要显式代理时只接受
不含 userinfo 的 `MIRA_INTEROP_PROXY_URL`，credential 通过 `MIRA_INTEROP_PROXY_AUTH` 单独注入。

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

## 9. 变更记录

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
