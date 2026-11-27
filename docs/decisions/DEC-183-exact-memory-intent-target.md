# DEC-183：按主张键解析记忆意图目标

> 状态：Accepted
> 日期：2026-11-27
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-156](DEC-156-m30-implementation-freeze.md)、[DEC-180](DEC-180-explicit-preference-intent.md)、[DEC-182](DEC-182-host-record-forget-intent.md)

## 决策

宿主解释用户请求后，可提交一个作用域类别、记录类型和主张键。协调层按当前工作和
`mira.memory.manage` 授权，推导准确的 User 或当前 Session 范围。解析只返回唯一当前
记录的 ID 和版本。没有匹配返回空结果，多条匹配返回 `VersionConflict`，供宿主澄清。

SQLite 在准确范围和类型内检查当前记录，包含正文相同但 ID 不同的记录。匹配采用现有
主张键规范化。时间、取消和擦除屏障可使解析失败。存储适配器需要实现精确查询；结构化
偏好入口继续支持旧适配器。解释原始自然语言、解析历史记录和选择整个范围另行验收。

宿主把解析得到的 ID 和版本传给既有纠正或遗忘入口。提交阶段继续复核授权和版本，续办
遗忘沿用已绑定的 ID、版本和操作 ID。

## 验证

通过协调层验证准确范围、授权、主张键、当前记录、歧义及后续纠正和遗忘。用两个正文相同
但 ID 不同的记录验证解析能够识别歧义。测试证据归入 [M30-05](../plans/m30-memory-acceptance.md)。
