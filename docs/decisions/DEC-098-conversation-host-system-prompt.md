# DEC-098：ConversationLoop 宿主 system 模板

> 状态：Accepted
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 替代/被替代：无（补充 DEC-047）

## 背景与问题

`BUG-20261109-001`：内建 system 要求最终文本作答，与 mira-bridge 的「发声必须经
发送工具，终态文本无出口」契约冲突。goal 与输入供应缝均为 User 内容，不能承担
宿主 system 纪律。按用户要求采用完整替换方案 B，不以追加相互矛盾的句子解决。

## 决策

`ConversationLoopConfig::system_prompt` 是可选的 `ConversationSystemPrompt`：
`text` 为完整文本，`template_id` 为宿主模板身份，`template_digest` 可显式指定。
不配置保持内建文本、来源与 digest 不变；配置后逐字替换唯一的 System 项，不再
追加内建的工具/文本作答句子。信封仍由 loop 固定为 System、Internal，来源为
`template_id`。此缝仅在构造时接收可信宿主配置，不接收模型/工具返回内容。

文本和身份不得为空白；显式 digest 不得为零。无 digest 时对完整 `text` 计算
SHA-256；显式 digest 作为宿主声明的模板摘要，loop 不要求它等于渲染文本摘要。
配置错误在 run 的请求组装和所有宿主回调前返回 InvalidArgument，不能退回内建模板。
`prompt_provenance.system_template_digest` 使用最终选定摘要。每次组装自定义请求
成功后产生 `ConversationSystemPromptSelected` 元数据事件，包含组装 request ID、模板摘要
与实际文本 SHA-256，不复制文本或身份原文。事件沿用 best-effort 存储契约。

DEC-097 的 `set_tool_selector()` 已提供动态暴露：每次组装按本轮输入/工具结果
和宿主状态重新选择，gateway 重试复用快照。选择器能看见完整自定义 system；模板
不随工具选择改变。历史回放、工具准入和未选工具拒绝保持原契约。

## 备选方案与后果

追加 system_note 仍保留冲突的文本作答指令；仅改 goal 无法提供系统级纪律。
完整替换让宿主负责工具使用与终态说明；空模板拒绝，避免意外清空纪律。
`Answered` 仍表示无工具调用的终态文本，是否发送给人由宿主决定；模板覆盖不强制
模型一定调用发送工具，也不改变解析器、权限和 Verify。宿主应自行验证发声结果。
公开配置布局扩展，C++ 消费者需重新编译；不新增 wire schema 或 Executor 路径。

## 验证与关联

离线测试验证默认兼容、完整替换、digest 参数化/自动计算、错误配置零回调/零 infer、
三方言、工具往返与动态暴露、连续/有限模式、恢复与取消、元数据脱敏。
真实 Qwen3.5-9B、桥宿主发声效果与目标平台行为留给部署环境复验。

- [维护计划](../plans/maintenance-2026-11-conversation-host-system.md)
- [API](../api/model-agent-loop.md)
- [持续会话设计](../design/continuous-conversation-loop.md)
