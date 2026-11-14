# DEC-112：恢复决策的显式输出协议与生成预算

> 状态：Accepted；日期：2026-11-14；工作项：M14-08
> 来源：Mirage 维护者已授权补齐恢复接口并进行真实模型验收

## 决策

WorkflowRecoveryConfig 增加 decision_output_mode（StrictJsonSchema 默认；可显式选择
JsonObject 或 Text）、max_output_tokens（默认 2048，128～8192）及可选 thinking。
不支持 StrictFunctionTool，不自动探测/降级或重试协议。模型选择与模式能力声明属宿主。

非 StrictJsonSchema 模式把同一闭集决策 schema 作为系统约束供给，继续 ToolChoice::None。
远端协议约束程度不同，但本地 parse_workflow_recovery_decision、patch 校验、运行状态/
权限/epoch 检查完全保留；文本不是可执行命令。坏 JSON/未知字段/不合格 patch 按原有
repair budget 处理，没有未经验证的桌面动作。

理由：当前 AnthropicMessagesV1Mapper 明确仅支持 Text；不能伪称 strict JSON capability
来让既有模型工作。真实 OpenAI-compatible 路由亦可能拒绝其 strict schema 子集（Mirage
在线轮次已得到 InvalidModelRequest，具体远端参数原因未确认）。配置应能明确选择已
支持的文本协议，同时由本地冻结解析/校验形成最终边界。

## 验证

默认严格模式回归；Text/JsonObject 下系统含 schema、无工具、显式 token/思考预算；
错误模式/预算拒绝；坏文本无副作用、合法决策仍沿同一 patch/resume 管线。真实模型
验收另记 Mirage M15，不把协议可映射等同模型质量或目标桌面成功。

## 关联

[M14](../plans/m14-recovery-orchestration.md)、
[恢复 API](../../include/mira/workflow_recovery.hpp)。
