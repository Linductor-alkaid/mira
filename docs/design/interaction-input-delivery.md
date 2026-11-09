# 交互输入归属与投递

> 状态：Active
> 版本：1.1
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 范围：M29 并发、澄清回收、历史投影与恢复；平台验收见计划
> 决策：[DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)、
> [DEC-102](../decisions/DEC-102-interaction-input-arbitration-and-delivery.md)、
> [DEC-106](../decisions/DEC-106-interaction-durable-recovery.md)

## 目标与边界

同一主体可以同时有多个工作。用户的明确引用决定输入归属，模型建议只参与无显式引用的
路由。输入接收、上下文更新、工具执行和任务结算分别有可观察边界。协调层位于 Runtime
之上，不创建另一套 Task 状态机或副作用提交队列。完整请求仍由 Layer 0 决定准入。

澄清、注册表与正文恢复、只读回补和未决副作用恢复的接线由
[M29-03/05/06](../plans/m29-interaction-coordinator-concurrency-and-recovery.md) 交付。
恢复投影不注册旧 Runtime Task，实际控制权仍属于新 Task、epoch 和资源 lease。

## 归属与提交顺序

协调层的 submit_mutex 串行化提交、汇绑定/解绑与关闭；registry mutex 只保护只读投影
复制和短时发布，调用宿主缝不持有它。宿主缝不能重入写接口。

`InputEnvelope.target_task/requested_intent` 与 reply_to 先经过主体归属校验；冲突或未知
引用拒绝。没有显式引用才调用 resolver，随后是唯一活动任务/新任务/有序候选澄清。
权限在重复回执返回前重验。input_id 在一个账本分区内绑定 tenant/user；其他主体复用
不能得到原回执。控制 intent 直接使用 Runtime，检查 Applied/NoOp。

普通输入的顺序为：接纳落账 → 裁决 → 有界上下文纯计算 → 必要的 Runtime 创建 →
路由落账 → 注册表发布 → 有界排队/投递。Routed 不表示已读。追加失败不发布内存账本
状态；已有工作不发布失败纠正；新 Task 路由提交失败时请求取消，取消失败明确返回
清理不确定及内部 TaskId。Runtime 创建命令的跨崩溃意图与投影恢复仍是 M29-05。

Task/Session 数量在创建前检查。默认每 Task 待投递 256 项、关联输入 4096 项，退役
上下文计算 16 个；上限触发返回显式错误。本增量没有建立跨主体字节容量或长期淘汰
机制，这部分继续由 M31 约束。

## 投递失败与插话

汇返回失败必须保证没有接收输入。直接拒收的输入登记 Rejected，不进入有效约束。
绑定汇补投时只删除成功前缀；失败项与后缀保留，新输入不能越过它们。宿主显式再次
register 才尝试补投。汇抛异常时无法判断是否接收，记录 DeliveryUncertain、封闭该
任务后续投递；登记诊断失败也显式返回，不自动重试可能已经接收的输入。

Loop 在轮边界 drain，派发模型前、接收完成后、repair 后及下一次动作/答案准入前检测
待处理输入。在既有恢复预算内重建，设备侧先 Observe。预算耗尽不接受旧完成。已经
进入的单个动作/工具遵循原取消/deadline/journal，不能追溯撤回；工具组中的未启动项
使用失败结果关闭 call/result 协议组，不调用处理器、不计执行数，发出 ToolSkipped。
动作执行后的 Verify 仍进行；插话使旧目标的成功验证不能直接结算更新后的工作。

最后一次队列检查是当前 action/tool 的输入准入边界。之后到来的纠正影响下一单元，
不会把已进入动作伪装成未执行。Human Takeover 与取消继续由 Runtime epoch、宿主
OperationContext 探针和 IActionDispatchLease 关闭动作准入；输入队列不是接管协议。

## Executor 与生命周期

上下文文本准备由注入 Executor 的 submit_auto 执行，只捕获候选工作项的值。成功
返回后由 owner 发布；超时 future 被保留，但其迟到完成不能写回 registry 或引用
已销毁 owner。close 消费全部有限任务并报告迟到错误；析构与旧 shutdown 为尽力清理。
宿主先停输入生产者及 Loop，再 close 协调层，随后关闭 Runtime、存储和 Executor。

submit_input 是同步且可能等待命令与有限计算的宿主操作。调用方使用外部 owner 或
Executor 的阻塞 I/O 边界，不要让同一个有限池的全部 worker 等待协调层在该池提交的
上下文任务。回调有界、无阻塞且异常隔离；错误使用固定文本，不持久化 exception.what。

## 耐久、安全与恢复限制

参考账本要求 EventStore 的 ProcessCrash ack。MemoryEventStore 的 Buffered ack
不符合生产耐久要求；测试回执模拟不能作为进程崩溃证据。FileEventStore 的实际关闭/
重开测试证明输入绑定、归属与幂等记录可重建。

既有 v1 事件追加可选引用、澄清候选和不确定投递诊断；不写输入正文、grants 或模型异常
原文。宿主仍需在信封进入系统前脱敏正文，Loop 既有文本事件纪律沿用 DEC-016。
按 id 查询账本以及结果登记是受信 owner 面，不能直接作为远端用户接口。

rebuild 恢复账本后，配置了恢复存储的协调层枚举有界记录并载入 Sensitive 工作 Artifact，
恢复主体、session/task、目标和约束映射。旧工作为 historical，只读；缺失/擦除、过期、
不兼容状态明确暴露。未配置存储的旧接口保持显式运行模式，不伪造跨重启恢复能力。
原始正文与必要上下文分离存储，快照带有来源 Artifact 闭包；缺少任一来源则拒绝恢复。
输入、投影和页都有字节/条数上限，长期容量、孤立 Artifact 清理及整体遗忘由 M31 承担。

追加失败可能已提交但回执丢失。账本将后续 mutation 封锁到 rebuild，读取只反映最后已
确认投影；接纳不确定时保留正文。这样重启能够找回已经提交的引用，也不会在内存仍为
Accepted 时将同一 input_id 再绑定别的 Task。

澄清答案引用原输入及冻结候选；原输入认领唯一 resolved_by，保留原文并追加答案，仍以
原 input_id 投递。答案记录标 NotRequired。二者之间崩溃可修复回执，不自动再派发。

replay_inputs 在权限检查后只读当前窗口内的主体投影，按接纳序号翻页。resume_input
只显式处理 Accepted 或当前 Runtime 内 NotDispatched 的记录。Dispatching/Delivered/
Uncertain 不自动重投；普通汇只证明入队，不证明消费，无法承诺跨崩溃 exactly-once。

recover_work 对历史或当前已结算工作复用 RecoveryPlanner，先新 Observe，再交给宿主
对未决副作用/缺失验证/不明投递核验。观察与验证使用新的版本化任务事件，携带期望前缀
水位。CheckpointBuilder 在推进水位前机械校验；验证必须引用当前观察及所有未决 action。
插入的迟到记录使提案过期，不会被一次旧验证清除。接受见证持久化在
TaskCheckpoint.recovery_verification_sequence，旧 JSON/digest 缺省为 0。

恢复许可仅在当前进程内，绑定上下文摘要、主体、账本修订及事件水位，默认 60 秒、单次
消费。续办创建新 session/task，关联 resumed_from。只读观察不会恢复旧 Task；后续
动作仍需重新观察、当前 epoch 和资源租约。停止旧生产者、收敛在途动作与接管探针属于
宿主职责；不能以 Runtime 已取消替代对实际副作用的核验。

执行集保留原目标和已提交用户约束；checkpoint 通过 recovered_context_items 产生有界
P3/UntrustedExternalData 候选，不 pinned。该 API 只读缓存，无磁盘热路径；宿主与记忆
候选融合并遵守总采集上限，最后交给 Layer 0。完整 checkpoint 不能粘进受保护 User 项，
否则会把可裁剪的历史升级成最小执行集。

## 验证与后续

`tests/m29/` 覆盖多工作归属、实际并发重复提交、权限撤销、账本失败原子性、文件重开、
补投后缀保留、汇异常、队列背压、有限任务超时/关闭、无记忆代理的两类 Loop 插话、
repair、工具组中断、已执行动作 Verify，以及 Runtime 取消/接管的迟到完成。
新增文件重开、原文/答案认领中断、丢失接纳/路由/投递回执、源擦除、恢复取消/超时/
接管/关闭、提交前窄窗口注入，以及 P3 在真实 Gateway 中的准入/预算丢弃测试。
M28/M36/M4 回归与 Sanitizer 是兼容性证据；目标平台和真实模型单独验收，具体结果与
补跑条件见 M29 验证记录。
