# DEC-050：显式Anthropic Messages方言

> 状态：Accepted
> 日期：2026-10-06
> 依据：维护者要求；Mirage MIRA-20261006-001；M3-21

在既有canonical模型层新增anthropic.messages.v1，不按请求失败隐式切换协议。沿用受管transport/SecretRef/ArtifactSource、规范工具与Gateway预算，支持文本、内联图片和普通工具；max_tokens必须显式生成。添加SecretRef认证方案（默认Bearer兼容旧调用，Messages用x-api-key）并在transport内解析，跨origin重定向不得携带密钥。SSE须配对block生命周期且message_stop后才能结算，EOF/错误/越界失败，预览不进入历史或执行工具。

不支持extended thinking和签名回填，遇到相应响应明确失败；不丢弃thinking后执行工具。不可表示的生成选项、continuation、远程存储、结构化输出与服务端工具拒绝。未来增加能力需独立契约和fixture证据。
