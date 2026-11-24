# DEC-136：按工作流版本和失败签名读取恢复经验

> 状态：Accepted
> 日期：2026-11-24
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、[DEC-030](DEC-030-learning-loop-runtime-semantics.md)、[DEC-119](DEC-119-six-memory-systems.md)

## 决策

宿主在当前学习 scope 内读取恢复经验，明示工作流 ID、IR 摘要、失败原因、可选步骤 ID
和步骤类型。缺少步骤字段表示匹配同样缺少该字段的经验。授权查询在存储访问前后运行，
发布结果前核对学习绑定。结果逐条校验规范化文档、来源、验证等级、当前状态和匹配字段。

SQLite 1.8 在现有 `RecoveryLesson` 的规范 JSON 上建立部分表达式索引，先匹配授权
scope 和完整的版本化失败键，再裁剪候选。旧库直接索引原记录，保留其 ID 和来源。失效、
墓碑、导入记录及无结构的旧文档不能参加当前经验读取。擦除仍随原记录事务生效。

本增量交付 `WorkflowRuntime::recall_recovery_lessons` 宿主入口。DEC-030 的自动失败
升级继续使用原 Episode/经验组与诊断约定，其接入专用经验索引在后续单独验收。

## 验收

验证从 1.7 重开、索引存在、旧记录不重写、版本和失败签名隔离、租户隔离、授权变化、
擦除与坏数据边界。见 M30-31。
