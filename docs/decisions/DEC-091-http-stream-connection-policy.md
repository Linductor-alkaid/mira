# DEC-091：HTTP 流式请求的显式连接策略

> 状态：Accepted
> 日期：2026-11-07
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-SC-01

## 背景与问题

宿主报告 NapCat OneBot `GET /_events` 对 `Connection: close` 敏感：响应头成功但
正文无数据，独立 keep-alive 客户端可收到事件。现有 SocketHttpTransport 固定发送
close，且普通 headers 中的 Connection（不区分大小写）会在发送前被拒绝，宿主无法
声明另一种连接策略。本轮以该报告为兼容性动机，不将其泛化为所有 Node/Express
SSE 的行为。依据 [RFC 9112 §9.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.3)，
close 控制当前响应完成后的连接持久性，并不要求停止当前响应正文。

## 决策

在 `HttpRequest` 末尾增加 `bool keep_alive = false`。false 保持唯一的
`Connection: close`；true 发送唯一的 `Connection: keep-alive`。采用提案最后的显式
头序列化方案，而非注释中的“省略 Connection”，使 wire 意图可直接验收。Connection
继续是 transport 自有保留头，拒绝调用方通过普通 headers 覆盖或制造重复值。

`OpenAiCompatibleProvider` 对 `ProviderInferOptions::stream=true` 自动设置此字段，
覆盖 Responses、Chat Completions 与 Messages 共用的传输入口；非流式保持 false。
普通事件流宿主显式设置 `request.keep_alive=true`。该值不是 TCP SO_KEEPALIVE，
不保证服务端持续推送，也不关闭已有 deadline、取消、字节上限或 EOF 校验。

每个 Exchange 仍拥有一个 socket：Content-Length 完整收齐、chunked 终止、正常
EOF、错误、取消、超时或 shutdown 后都释放；不增加池化、复用、心跳、重连或后台
任务。重定向保留同一请求策略，每跳重新连接并校验策略；HTTP 正向代理使用同一
字段，HTTPS CONNECT 的隧道建立策略不变，字段作用于隧道内的最终 HTTP 请求。
宿主关闭前须先停止生产者并取消活动 context，再 shutdown transport；shutdown 本身
不是新增的请求取消源。现有 Executor owner/worker/队列/future/关闭顺序保持不变，
无新并发路径或能力缺口。

## 备选方案与影响

全局改为 keep-alive 会改变所有普通请求；放开 Connection 普通头会产生多重来源与
歧义；连接池超出本问题范围。因此采用默认 false 的请求级策略，并显式接线模型
stream 意图。公开结构体布局增加成员，二进制消费者需重编译。标准 HTTP/1.1 原可
省略该头，本轮选择显式值，未来需更细策略时另行评审。

## 验证与边界

真实 socket fixture 按请求连接策略模拟“只发响应头”的敏感服务端，验证默认失败与
启用后的分片 chunked SSE、响应结束后客户端主动关闭；覆盖取消、idle 超时、shutdown、
截断和字节上限、保留头拒绝、重定向、代理与 TLS。模型 provider mock 验证三方言
流式/非流式字段，既有 SSE/parser/transport 生命周期回归保持有效。

真实 NapCat/Express 互操作由部署宿主维护者在固定服务版本和配置后补跑；本地 fixture
仅证明 Mira 的 wire 策略和生命周期，不构成该产品互操作或 Node 普遍缺陷声明。

关联：[维护计划](../plans/maintenance-2026-11-http-stream-connection-policy.md)、
[LLM API 设计](../design/llm-api-protocol-design.md)。
