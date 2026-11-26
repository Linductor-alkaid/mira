# DEC-162：从已提交的验证事实记录结构化世界观察

> 状态：Accepted
> 日期：2026-11-26
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-083](DEC-083-explicit-verified-fact-publication.md)、[DEC-127](DEC-127-world-fact-address-and-freshness.md)、
> [DEC-156](DEC-156-m30-implementation-freeze.md)

M30-03 使用现有 `VerifiedFactPublisher` 的 `TaskFactVerified` v1 作为结构化世界观察的
输入。宿主按当前运行、会话及任务，从授权事件库读取精确序号和事件 ID。只接受匹配
任务世代、来源 scope 与已配置事实键的验证事件。来源限定为 Environment/Application。
宿主配置目标世界 scope、实体和属性，且来源与目标须属于同一租户。正文和有效区间
取自验证事件，记录时间取自持久事件时间。
世界观察直接引用验证事件，该事件继续引用原始观察及摘要。

此次写入使用现有 `make_world_observation` 与 `MemoryConsolidator`，返回标准处置报告。
稳定身份由原有地址和证据事件派生，重开后用同一引用重试。原有受信宿主构造入口及世界
事实读取保持兼容。普通模型候选继续走现有未验证候选路径。宿主保证该事件源中的验证
发布仅由受信生产者写入。

宿主在由 Executor 管理的有限作业内调用同步入口。读取前获取当前写入租约并钉住相关
来源和目标权限，提交前复核租约、取消及截止。宿主通过同一租约边界协调撤权、来源失效
和相关擦除，已退役的来源拒绝重放。标准生产宿主的事件选择、触发和跨重启消费由 M30-38
验收，通用删除后的写入世代由 M30-09 验收。
