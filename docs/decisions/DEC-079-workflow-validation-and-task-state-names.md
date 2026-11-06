# DEC-079：Workflow 解码复用与 TaskState 名称单一来源

> 状态：Accepted
> 日期：2026-11-05
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-11（局部治理）
> 替代/被替代：无

## 背景与问题

IR、App Model、Learning 各有相同的对象键集合检查与非空有界字符串读取，
但错误码、domain_code 和消息前缀属于不同契约。Interaction 输出的 TaskState
名称与 Checkpoint 读取表也重复，后续新增状态可能产生无法恢复的名称漂移。

## 决策

- 在 workflow 私有头共享键集合扫描与字符串读取。各解码器仍提供错误工厂及
  InvalidShape/UnknownField 对应码；保持先检查未知键、再检查缺失键的顺序、
  诊断文本、字节长度限制及 schema 特有校验，不修改 JSON 序列化格式。
- 在 Core 的 `core_contracts.hpp` 增加同步、无分配的 `task_state_name` 与
  `task_state_from_name`，由 Core 实现中的一张不可变表提供双向映射。
  所有既有名称逐字保持；读取严格区分大小写，不接受空白、别名或未知名称。
  非法枚举输出 `Unknown`，反向解析返回 nullopt；不再将非法枚举伪装为 Idle。
- Interaction 与 Checkpoint 使用此公共契约；不跨模块包含私有实现头。
  TaskState 枚举值、状态转换规则、事件及 checkpoint schema 保持原样。
  公共接口按头文件、API 示例、不变量和契约测试同步交付。

## 备选方案与影响

继续各自维护映射无法消除漂移，跨模块包含私有头违反依赖边界；均不采用。
将 workflow 错误全部统一为一个 ErrorCode 会破坏调用方契约，亦不采用。
本轮没有新的任务、全局可变状态或 Executor 生命周期。未知枚举的诊断输出变化仅
影响此前非法输入；`Unknown` 不是新 TaskState，也不能持久化为合法状态。

## 验证与后续

测试三类公共解码器的非对象、缺字段、未知字段及字符串边界，核对错误分类与摘要。
穷举 uint8 枚举空间验证合法名称往返、非法值不映射 Idle；运行 checkpoint 与
Interaction 回归、公开头/安装消费者、架构与文档检查。Runtime 转移边界等其他
治理仍由 [维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md) 11 跟踪。
