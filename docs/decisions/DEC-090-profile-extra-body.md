# DEC-090：Profile 级 Chat Completions 厂商请求体扩展

> 状态：Accepted
> 日期：2026-11-06
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-EB-01

## 背景与问题

宿主声明的自建 OpenAI-compatible 端点需要 `chat_template_kwargs` 等请求体字段。
当前固定 mapper 无法表达这些配置，宿主必须额外运行注入代理。依照本次功能请求，
在 Profile 上提供受控扩展，不将厂商协议提升为 canonical generation 契约。

## 决策

`ModelProfile::extra_body` 使用 `JsonValue::Object`，默认空；仅支持
`OpenAIChatCompletionsV1`。宿主在构造固定 profile 快照时配置，模型响应、Decision、
Tool 参数和 `ModelRequest` 均不提供该入口。配置不得包含凭据或敏感输入；manifest
将原样包含这些值，不承诺对任意厂商字段自动识别秘密。

`validate()` 拒绝空键、重复键、标准 Chat 控制字段和 canonical 保留字段，即使本次
请求未输出该键也拒绝；错误使用固定安全消息，不插入配置键或值。顶层值允许 JSON
标量（含 null）或对象，对象内部允许数组；拒绝非有限数、重复对象成员、无效 UTF-8
及超出默认 `JsonLimits` 的数据。JSON 数字序列化先检查范围再尝试整数转换，
使合法的大有限数不发生越界整数转换。扩展自身不得超过 profile 的请求体字节上限。
Mapper 在生成固定字段后追加扩展，不覆盖字段；保留最终 wire 碰撞检查防止后续新增
mapper 字段遗漏保留表。直接 mapper/provider 调用同样校验，provider 在上传前拒绝
无效配置。最终完整请求体仍受 provider 的 `max_request_bytes` 限制；发送前拒绝清空本轮 trace，避免沿用上轮写入
状态而误判为远端模糊完成。

非空扩展进入 manifest、canonical profile digest 和 wire digest，绑定既有路由及
continuation/replay 语义；键顺序不改变 digest。空扩展不输出 manifest 成员，保留原
profile digest。其他方言的非空扩展显式失败，不忽略或自动回退。新增标准 mapper
字段时必须同步保留键表及负向测试。

## 备选方案与影响

本地代理仍可用但增加部署成本；请求级扩展会模糊配置与模型输入边界；单独增加一个
`enable_thinking` 字段则将厂商模板选项引入 Core 通用契约。因此采用 profile 快照。
这是公开结构体的加法变化，二进制消费者需重编译；空配置的线格式不变。不增加并发、
Executor 生命周期或第三方依赖。厂商字段含义、实际支持与性能需按端点单独验证，
本轮只承诺本地编码与拒绝语义。

## 验证与关联

维护项覆盖同步/SSE 请求、冲突与畸形配置、路由拒绝、digest 顺序稳定和变更绑定、
直接 provider 零传输拒绝、其他方言拒绝及空配置回归。

- [维护计划](../plans/maintenance-2026-11-profile-extra-body.md)
- [LLM API 设计](../design/llm-api-protocol-design.md)
