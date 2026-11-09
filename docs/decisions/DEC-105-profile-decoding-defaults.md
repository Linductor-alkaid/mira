# DEC-105：Profile 级解码参数默认（decoding 段）

> 状态：Accepted
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-BR-03
> 替代/被替代：无（与 [DEC-090](DEC-090-profile-extra-body.md) 互补）

## 背景与问题

宿主没有解码参数缝：`ConversationLoopConfig` 无任何解码参数字段；
`ModelProfile::extra_body` 的保留键名单（DEC-090）含 `temperature`、`top_p`、
`seed`，声明即拒绝。宿主没有任何合法路径设置采样参数，只能吃服务端默认。
对工具纪律敏感的小模型部署，采样温度是纪律调优的最后一个自由度，不能要求
宿主靠注入代理绕过（DEC-090 已否决该形态）。

## 决策

`ModelProfile` 新增宿主配置段 `decoding`（`ProfileDecodingOptions`：
可选 `temperature`、`top_p`、`seed`）。语义：

- **默认而非覆盖**：三个方言 mapper 在 canonical 请求未设置对应字段时套用
  profile 默认；显式请求值永远优先。合并通过公开函数
  `effective_generation_options(profile, generation)` 完成，
  `max_output_tokens`、`reasoning_effort`、`thinking`、`service_tier` 永不
  由 profile 默认，保持 request-owned。
- **能力策略门控**：`validate()` 拒绝 profile 自身 generation policy 声明为
  `Unsupported` 的 decoding 参数（注册期失败闭合，不在首次派发时失败）；
  合并函数对 `Unsupported` 字段防御性跳过，直连未校验 profile 也不能绕过
  方言门。Messages 方言无 seed 字段：mapper 显式拒绝 `decoding.seed`，
  即使策略误声明为可用。
- **范围校验**：temperature 非有限、负值或超过方言上限（OpenAI 系 2.0、
  Messages 1.0）拒绝；top_p 非有限、≤0 或 >1 拒绝；seed 任意 uint64。
- **审计面**：非空 decoding 进入 manifest 与 `profile_digest()`，绑定既有
  RouteDecision/continuation/replay 语义；任一字段为空则省略 manifest 成员，
  digest 与既有行为一致。额外键禁入：decoding 段只有三个固定字段（结构体，
  非 JSON map），不存在自由键。
- **保留键名单不变**：`temperature`/`top_p`/`seed` 在 `extra_body` 中继续
  拒绝——decoding 段是这些键唯一的宿主侧合法路径，配置与厂商扩展两条通道
  不混用。

## 备选方案

- `ConversationLoopConfig` 增加每 run 覆盖：被否决为本轮方案——system、
  tool selector 等 config 缝已持续膨胀，且采样属端点特性而非任务特性；
  请求级显式设置本来就已可用（canonical generation 字段），无需新缝。
- 放开 extra_body 保留键：被否决——把标准控制字段降级为"厂商扩展"会绕过
  `GenerationParamPolicy` 门控与方言差异处理（如 Messages 的 temperature
  上限、seed 不存在），审计面也更差。

## 影响与风险

- 公开结构体加法变化，二进制消费者需重编译；空 decoding 的线格式与 digest
  不变。
- 默认值改变 wire 内容：同一 canonical 请求在不同 profile 下可产生不同
  wire 采样参数。这与 extra_body 的既有语义一致（profile 快照绑定
  RouteDecision），wire digest 已覆盖最终请求体，continuation/replay 绑定
  不受影响。
- 不新增并发、Executor 生命周期或第三方依赖；合并为纯函数，每请求 O(1)。
- 厂商端点对参数的实际接受度需按部署验证；本轮只承诺本地校验、合并与
  拒绝语义。

## 验证方式

测试覆盖：请求未设字段时三方言 wire 套用 profile 默认、显式请求值优先、
`Unsupported` 策略注册拒绝、越界值拒绝、Messages 拒绝 seed 默认、manifest
与 digest 含/不含 decoding 段的稳定性、空 decoding 回归既有字节。

## 关联文档和工作项

- [维护计划](../plans/maintenance-2026-11-bridge-host-feedback.md)
- [DEC-090](DEC-090-profile-extra-body.md)（extra_body 保留键与审计先例）
- [LLM API 设计](../design/llm-api-protocol-design.md)
- [模型 API](../api/model-agent-loop.md)
