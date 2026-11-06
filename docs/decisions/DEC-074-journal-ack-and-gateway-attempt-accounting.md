# DEC-074：动作日志确认门槛与跨候选模型尝试计数

> 状态：Accepted
> 日期：2026-11-05
> 负责人：Mira Maintainers
> 冻结里程碑：2026-11 运行时接线维护轮
> 替代/被替代：无（落实 DEC-003，不改变其副作用协议）

## 背景与问题

`ActionJournal` 请求 `ProcessCrash` 后直接接受任意成功回执并推进本地 phase。
如果自定义存储返回较弱的 `Buffered`，调用方会误以为可以越过副作用边界。
请求了耐久性不等于存储实现已经完成耐久提交。

`ModelGateway` 原有累计计数紧接着被候选内的 `attempt` 覆盖，导致主 provider
失败后切换候选成功时，返回值和成功事件只报告最后一个候选的次数。

## 决策

1. Journal 每次 append 必须检查 `achieved`：至少达到请求的 `ProcessCrash`，
   `PowerLoss` 也可接受；较弱或不属于已定义枚举的值返回 `DataLoss`，不推进
   本地 phase、不缓存失败回执。底层 append 错误原样返回。
2. 回执不满足门槛不代表事件没有写入，也不保证回滚。若 start 实际写入，重建仍须
   视为可能已执行，先 Observe/Verify；失败后不得直接调用 Environment。
   `MemoryEventStore` 仅为进程内测试替身，不能证明进程崩溃耐久性；真实宿主必须
   提供如 FileEventStore 的耐久实现。此检查不能识别一个谎报耐久性的 store。
3. `ModelCallOutcome::attempts` 与 `ModelResponseReceived.detail.attempts` 表示
   一次 gateway infer 中实际调用 provider infer 的累计次数，跨候选累计；
   被跳过的熔断候选不计数。候选内部重试策略仍使用候选内的次数。
4. `ModelAttemptFailed.detail.attempt` 保持候选内次数，新增可选诊断字段
   `total_attempts` 表示跨候选累计值。全失败仍返回原有 Error，不新增 outcome，
   可以通过失败事件了解实际尝试数。新字段为诊断载荷加法兼容，不改变事件名称。

## 备选方案

- 只检查 append Result：无法阻止弱确认推进 phase，不采用。
- 保留最后候选的 attempts：会低报整次 infer 的外部调用数量，不采用。
- 把 retry budget 改为全候选共享：会改变路由/重试行为，本轮不采用。

## 影响与风险

接口签名和持久化格式不变；返回弱确认的自定义 store 将收到明确失败。
读取成功事件 attempts 的宿主在发生 fallback 时会看到更大的正确数值。
本决策不代表 AgentLoop、Workflow 或可写工具已接入 journal，相关工作继续保持开放。
不新增并发或 Executor 生命周期，现有外部 owner 和关闭顺序不变。

## 验证方式

- 每个 journal 阶段注入弱确认/存储拒绝，证明 phase 不推进，恢复后可重新请求强确认；
  覆盖非法 durability 与 PowerLoss。
- FileEventStore 关闭重开，验证 start 无 receipt 的 journal 重建与 checkpoint
  verification_pending；此用例为存储重开取证，不声称覆盖所有强杀/断电窗口。
- 单次成功、同候选重试、跨候选成功、全部失败、熔断跳过，对照 provider 实际请求数、
  返回值和事件。拒绝的 gate 与预算仍沿用既有测试。

## 关联文档和工作项

- [DEC-003](DEC-003-event-sourced-persistence.md)
- [运行时接线维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)：
  `MNT-202611-05`、`MNT-202611-13`；动作日志完整接线仍归 01–03。
- [模型 API](../api/model-agent-loop.md)、[核心 API](../api/core-runtime.md)
