# DEC-056：持续会话循环与请求级预算

> 状态：Accepted
> 日期：2026-10-15
> 负责人：Mira Maintainers
> 冻结里程碑：2026-10 持续循环维护
> 替代/被替代：局部修订 DEC-047 的可选运行模式；有限模式保留

## 背景与问题

Mirage 反馈 `MIRA-20261015-002`：正常长任务在累计 16 轮/32 工具处停止。
将上限换成最大整数仍会停止，并使整环预算溢出。现有历史压缩只改变请求回放，
私有历史和结果中的逐轮记录仍累积，不能直接取消循环上限。

## 决策

新增显式 `ConversationLimitMode::Continuous`，忽略累计 `max_turns` 与
`max_tool_executions`，默认仍为 Finite，零值保持原语义。每次模型调用、工具批次、
恢复及输出续写仍有界；取消、deadline、准入拒绝和终态阻止新工具派发。
调用者继续由外部 kairo owner 管理，不新增线程、队列或调度器。

持续模式生成有限的请求级预算（`ModelBudgetScope::Request`），与默认 Task 预算
明确区分。请求级准入校验本次估计，不因旧调用的未知价格/usage 残余拒绝新请求；
残余仍在任务 ledger 与诊断中保留，不伪造零花费。不承诺整任务花费上限，宿主
需显式选择本模式。有限模式预算包络保持兼容，算术溢出在派发前拒绝。

持续模式成功的 HistoryTransformer 结果提交为后续规范历史，须为完整工具协议，
保留最近未压缩轮（含签名 thinking 与工具组）；失败回退原历史并诊断。历史条目/
逻辑字节设有限容量，下一次模型派发前检查；容量不足明确 Failed，不静默删掉
执行事实。宿主负责语义摘要与持久事实归档。结果仅保留有限最近轮记录，累计统计
与事件独立，计数使用饱和算术。容量不是任务累计步数限制。

## 备选方案

- 提高整数上限：仍截断正常任务，拒绝。
- 周期性重启 loop：丢失私有协议与副作用连续性，拒绝。
- 不限制历史和记录：长期资源不可控，拒绝。

## 影响与风险

新增公开枚举/字段，需重编译消费者；默认请求的 canonical JSON 保持不变，非默认
预算 scope 显式序列化。Request scope 使用 model contract 2.0，旧 reader 明确拒绝；
新 reader 接受 1.x/2.x，请求与响应默认仍为 1.0，不改有限模式 digest。请求模式不等于免费或无限资源，供给/工具输出和持久事件
仍受各自预算。逻辑字节不等于 RSS；临时模型响应和宿主回调分配由其边界约束。
本轮不增加持久 step/resume，也不实施平台相关无进展策略。

工具注册表的公开 execute 保留全生命周期 operation 去重。持续 loop 使用仅向
ConversationLoop 开放的私有派发入口：每轮 gateway 为新请求生成身份、桥接消除
重复提案、run 同步向前逐条消费且不暴露/缓存旧提案，所以无需永久保留 operation
墓碑；工具身份/schema/取消和失败检查仍走同一个实现。此入口不得用于 checkpoint
重放、外部提案或异步续派；若未来新增这些路径，须重新建立可验证的准入水位。

## 验证方式

脚本任务超过 256 模型步骤/128 工具仍完成；压缩后私有存量平台期；有限/零值兼容；
多工具与签名 thinking 完整；压缩异常、容量、模型/工具错误、拒绝、运行中取消、
deadline 和 kairo shutdown 均有结果。Linux Debug/sanitizer 与 CI 分平台记录证据。

## 关联文档和工作项

- [持续循环维护计划](../plans/maintenance-2026-10-continuous-conversation.md)
- [设计与所有权契约](../design/continuous-conversation-loop.md)
- [API 手册](../api/model-agent-loop.md)
- [DEC-047](DEC-047-conversational-loop-and-canonical-tool-parts.md)
