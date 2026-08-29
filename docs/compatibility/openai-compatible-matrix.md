# Mira OpenAI-compatible Provider 兼容性矩阵

> 状态：Active  
> 版本：1.0  
> 更新日期：2026-08-30  
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
| `openai.responses.v1` | `/responses` | 尚未实现 | Planned | Planned | `Configured` |
| `openai.chat-completions.v1` | `/chat/completions` | 尚未实现 | Planned | Capability-gated | `Configured` |

`Configured` 只说明设计和 profile ID 已冻结，不表示源码存在。

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

真实请求只在测试数据、费用预算、endpoint allowlist 和授权凭据已就绪时执行。禁止用生产用户的
截图、Memory 或 Tool output 做 capability probe。

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

这些资料用于定义 OpenAI reference profile，不是 DeepSeek、Qwen、OpenRouter 或其他兼容服务的证据。

## 9. 变更记录

### 2026-08-30：建立基线

- 范围：冻结两个 dialect ID、证据等级和 profile 记录模板。
- 验证：只核对官方文档和仓库设计；未实现 mapper，未执行 API 调用。
- 限制：所有真实互操作状态保持 `Unknown`。
- 关联：[M3 Model Provider 与 Agent 闭环](../plans/m3-model-provider-agent-loop.md)。

