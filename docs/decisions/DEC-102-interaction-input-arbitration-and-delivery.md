# DEC-102：交互输入仲裁、投递边界与恢复分期

> 状态：Accepted（按用户要求继续推进 M29）
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 冻结里程碑：M29
> 替代/被替代：补充 DEC-068，不修改 Runtime 状态机或副作用提交协议

## 背景与问题

M28 提供输入台账、路由建议缝和 Loop 队列，但显式引用的优先级、投递失败后的
保序及在途模型完成与新输入的关系尚未闭合。M36 的快照失效依赖宿主接线，普通
Loop 输入汇也需要独立阻止旧模型结果变成下一次动作。M29 的恢复部分还需要正文
引用、主体注册表投影和 RecoveryPlanner 接线，不能把账本 rebuild 当成完整恢复。

## 决策

1. 提交在协调层串行化。仲裁顺序为主体权限、输入幂等身份、显式 task/intent、
   `reply_to`、通过验证的模型建议、唯一活动任务、澄清。未知显式引用拒绝，
   不回落到其他工作。显式 NewTask 不需要 resolver；显式取消/暂停不调用 resolver。
   终态任务只允许关联创建新任务，取消终态为 Runtime 的幂等 NoOp。
2. `input_id` 在一个账本分区内唯一，并绑定 `(tenant_id,user_id)`。其他主体不能用
   同一 id 读取重复回执。权限必须在返回重复回执前重验。旧 accepted 事件中的
   `principal_ref` 用于恢复绑定；缺失绑定的旧记录只允许存储 owner 查询，不可重复派发。
   该接口不接受从记忆或模型输出推导的授权。
3. 澄清返回冻结顺序的 task id 候选及自然语言问题；候选与仲裁依据写入既有事件的
   可选字段。模型建议不能覆盖显式引用。完整澄清回收还需持久化原输入引用，单纯
   将后续文字路由到候选任务不算完成 `M29-03`。
4. 路由先获 ProcessCrash ack，才允许输入进入 Loop。`Routed` 表示归属已提交，
   不表示已读或已执行。尚无汇时有界排队；补投只移除成功的前缀，一次失败保留
   失败项和整个后缀，新输入不得越过。明确拒绝表示未接受输入；汇抛异常视为投递
   结果不确定，封闭该任务的进一步补投并要求宿主处置，禁止盲目重试。
5. Loop 在轮边界 drain。请求组装后、模型完成后、schema repair 后、下一次
   tool/action/终态发布前检查新输入；有新输入就消耗既有有界恢复预算，设备循环
   重新 Observe。已经获准进入的单个 action/tool 仍按既有取消、deadline、journal
   语义完成并验证；纠正不能追溯撤销已发生副作用。工具组尚未进入执行时整体放弃；组内已执行工具保留结果，尚未执行工具返回
   superseded 失败结果，不能留下无结果的 provider call，也不能继续旧副作用。
6. 上下文准备是 Executor 的有限纯计算，只捕获值。超时结果不再写注册表；owner
   消费所有 future 后才能销毁，不保留捕获裸 `this` 的迟到工作。回调异常使用固定
   脱敏错误，不把 exception.what 写入事件。运行时命令须检查 Applied/NoOp，
   收到 CommandOutcome 不等于命令成功。
7. 恢复按后续增量交付：输入正文用受控 Artifact 引用，主体映射用可重建投影；
   断线回补是带主体过滤、游标、窗口和页上限的只读结果流，绝不重派发。
   接收但未路由的输入可显式恢复；投递中或已投递而消费不明的输入先核对，
   不能宣称跨崩溃 exactly-once。恢复的旧 Task 只读，不伪装成新 Runtime 的活动任务。
8. 未决副作用复用 RecoveryPlanner 的 pending_side_effects；存在不确定性时只允许
   Observe/Verify，不重放 action。缺失、擦除、超窗或不兼容历史必须返回明确的
   不可恢复结果。相关适配未完成前，M29 保持 In Progress。

2026-11-09 后续：[DEC-106](DEC-106-interaction-durable-recovery.md) 细化并接线本决策
第 3/7/8 项，交付状态与 Linux 验收在 M29 计划跟踪，不据账本 rebuild 复活旧 Task。

## 备选方案

以模型建议覆盖显式归属会把授权仲裁交给语义组件；先投递再记账会在持久化失败后
失去可追踪归属；整队取出后失败只放回一项会丢失后续输入。均不采用。
跨崩溃重发所有输入也不采用，因为普通队列接收回执无法证明消费与副作用情况。

## 影响与风险

保持 InputStage 与 RouteIntent 闭集。新增信封字段和事件可选字段需宿主明确传递；
普通自然语言继续使用建议缝。串行提交并不提供跨进程多 writer 协调；一个分区只有
一个 ledger owner。真实语义准确率、平台与恢复交付各自取证，不由本决策宣称完成。

## 验证方式

`tests/m29/` 覆盖显式归属冲突、越权重复、追加失败原子性、补投失败保序、回调异常、
执行中插话和迟到完成；恢复门禁按 M29 计划独立保留未完成状态。

## 关联文档和工作项

- [M29 计划](../plans/m29-interaction-coordinator-concurrency-and-recovery.md) `M29-01..08`。
- [DEC-068](DEC-068-interaction-coordinator-and-principal-input-entry.md)、
  [DEC-101](DEC-101-memory-agent-context-admission.md)。
- [交互 API](../api/interaction-coordinator.md)。
