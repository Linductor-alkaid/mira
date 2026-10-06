# DEC-051：Messages 自适应思考与规范回填

> 状态：Accepted
> 日期：2026-10-07
> 依据：Mirage MIRA-20261006-002；维护者已授权依赖修复并要求 PR

新增规范 ThinkingMode（Disabled / Adaptive）和 ThinkingPart（正文、签名、redacted 标志）。
生成参数通过 GenerationParamPolicy 明确声明；Messages 将 reasoning_effort 映射到
output_config.effort（不接收 minimal），扩充规范 XHigh / Max。其他方言不能接收 thinking
参数或回填内容，必须显式拒绝，不能静默丢弃。具体供应商和模型的可用档位由宿主按官方
API 声明，Mira 不猜测模型能力，也不因请求失败切换协议。

同步和 SSE 解析 thinking / redacted_thinking，保持内容顺序。SSE 的思考正文与签名共同
计入文本累积预算；思考不混入正文预览。ConversationLoop 在工具结果前回填完整 assistant
内容，保留签名、正文和工具调用顺序；签名不是工具指令。规范 JSON、验证与摘要覆盖新内容。
当前不交付 legacy enabled/budget_tokens、between_tools、服务端工具或用户会话级历史保存；
ConversationLoop 的回填范围是一轮 harness 内的工具迭代。
