# DEC-169：记忆代理的结构化提案与确定性记录诊断

> 状态：Accepted
> 日期：2026-11-26
> 负责人：Mira Maintainers
> 依据：[DEC-101](DEC-101-memory-agent-context-admission.md)、[DEC-114](DEC-114-extraction-model-supply.md)、MIRA-20261126-003

内置记忆处理器经 Gateway 发起单次无工具请求。Recall、Record、Compress 各有独立的
严格 JSON 输出契约。Record 使用当前事件页的局部引用，提示中省略事件载荷的内部身份及
摘要元数据。每项候选在进入原写入管线前复核类型和引用。合法的数组中，未知类型或无效
引用的候选单独丢弃并计数。整个响应无法按协议解析时返回结构化错误。

宿主可以沿用自定义处理器。省略处理器时 Record 使用现有确定性写入，成功结果及诊断明示
`deterministic_record_only`。记忆子作业的处置与用户任务的完成状态分别记录。
