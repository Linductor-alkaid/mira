# DEC-106：交互恢复、澄清回收与只读回补

> 状态：Accepted（按用户要求继续推进 M29）
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 冻结里程碑：M29
> 替代/被替代：补充 DEC-068/102；不改变 Runtime 或 ActionJournal 的提交协议

## 背景与问题

M29 首批增量完成归属与投递边界，尚缺原输入正文、主体注册表和澄清答案的恢复。
普通输入汇只能证明入队，不能证明消费。恢复不得通过重新调用旧 action 试探结果。

## 决策

1. 宿主显式绑定 ArtifactStore 与 CheckpointStore 开启恢复存储。输入正文和任务上下文
   分别保存为 Sensitive 的版本化 Artifact；账本只放引用、摘要及必要的关联字段。
   上下文保存有界的来源 Artifact 闭包；任一来源正文擦除后，该投影不可用于恢复。
   输入进入系统前由宿主脱敏，凭据/grants 不持久化。未绑定时兼容原显式运行模式，
   恢复接口明确不可用；旧记录缺正文引用返回缺失，不补造内容。
2. 既有 v1 事件追加可选字段：输入/上下文引用、接纳/更新序号、澄清原输入及答案关联、
   投递状态。InputStage/RouteIntent 保持闭集。投递按 NotDispatched → Dispatching →
   Delivered 记录；明确拒收回到 NotDispatched；异常或成功后 ack 失败记为 Uncertain。
   Dispatching 必须先耐久，再调用汇；状态不明不得自动重投。Delivered 只表示入队。
   追加失败可能是已提交但回执丢失；账本封锁后续写入，重建耐久索引后才解除，不能
   仅凭内存旧状态再次绑定任务。读取仍是最后已确认投影；宿主关闭并重开协调层协调队列。
   接纳回执不确定时保留正文，避免删掉可能已被耐久引用的内容；写入封锁同时阻止继续
   创建推测性 Artifact。确认无接纳的配额/参数拒绝才立即清理正文。
3. 初始化从有界账本记录和上下文 Artifact 重建主体的工作映射。旧 Task 标为只读历史，
   不绑定旧汇、不创建 Runtime Task。缺失/擦除、过期及不兼容上下文显示明确可用性。
   一个分区单 ledger owner，恢复扫描、索引、正文和页大小均有上限。
4. 澄清答案带 clarification_for 与冻结候选中的 target_task；重新校验主体和窗口。
   投递内容保留原输入并追加答案，以原输入 id 投递。原输入 Routed 时同时认领唯一
   resolved_by，答案单独记录归属但不作为第二次投递。多次答案不可重复认领。
   两条记录之间崩溃时，可从原输入认领记录修复答案回执，不因此重新派发。
5. resume_input 是显式恢复入口，只恢复有正文且尚未路由的输入，或在当前 Runtime
   中尚未派发的已路由输入；不会重新派发 Dispatching/Delivered/Uncertain，也不会
   复活旧 Runtime Task。澄清待选时只返回原候选；历史工作续办必须关联新 Task。
6. replay_inputs 是经过主体权限过滤的有界只读当前投影，按稳定接纳序号翻页，不调用
   resolver、Runtime、模型或汇。页返回投递状态、窗口和过期缺口；不是跨页事务快照。
7. recover_work 复用 RecoveryPlanner 和 journal，适用于历史或当前已结算工作，并做一次有 deadline
   和取消探针的只读 Observe。未知或丢失任务历史不得视为“没有副作用”。存在 pending
   side effects 或 verification_pending 时，必须由宿主验证回调覆盖全部未决项，收到
   耐久且被 reducer 接受的 RecoveryVerificationResult 后才允许续办。已入队而消费不明也必须由该回调核对。
   配置恢复存储后，同进程终态续办不能绕过此门。模型不能授予该权限。
8. 恢复许可只活在当前进程，绑定主体、旧上下文摘要、账本修订和源事件水位，默认 60 秒
   到期，续办前重验并单次消费。环境变更、接管和取消由宿主探针/epoch 与后续动作租约
   继续校验。历史终态可以为显式新工作做只读观察，但不提交旧 Task 状态转换。
9. 所有接口为同步有界宿主操作，由外部 owner 或 Executor 管理的适当 worker 调用；
   不引入线程、私有任务队列或自动恢复 worker。关闭取消探针纳入观察上下文；每个
   阻塞边界后重新检查取消/deadline。RecoveryPlanner 保留原三参数接口并增加带
   OperationContext 的重载，在每页和 checkpoint 读取边界检查协作取消。错误与不确定
   窗口均对调用者可见。
10. 原目标和已提交用户约束继续走执行集；历史 checkpoint 单独作为有界 P3 候选由
    recovered_context_items 提供，authority 固定 UntrustedExternalData、不 pinned。
    宿主融合现有记忆候选后仍由 Layer 0 独占准入，不能将 checkpoint 整段贴进保护的
    主 User/System 项。该接口只读缓存，不在模型调用热路径读取磁盘。

恢复事件使用 `RecoveryObservationRecorded` / `RecoveryVerificationResult` 新类型及各自 v1
payload schema，旧 reducer 将它们保守忽略。两者携带 expected_through_event_sequence；
CheckpointBuilder 在推进水位之前机械校验前缀，过期提案只推进读取位置，不修改观察或
清除 pin。验证还必须引用当前观察及全部未决 ActionId。TaskCheckpoint 可选的
recovery_verification_sequence 保存接受见证，默认 0 且不写入旧 JSON/digest，便于兼容
既有投影；新读者不会因 checkpoint cache 隐藏拒绝统计而把未接受的验证当作成功。
这复用既有 EventStore/Reducer，不引入第二套动作提交协议。

## 备选方案

把 Routed 当作已执行或重启后重投所有输入均无法排除重复副作用。把旧 Task 注册回
新 Runtime 会绕过 epoch 与环境新鲜度。保存正文到事件则扩大隐私与擦除影响面。
以上方案不采用。

## 影响与风险

恢复存储为显式宿主接线，不推断模型语义质量或真实设备行为。普通汇没有消费确认，
因此不承诺跨崩溃 exactly-once；对不确定输入的处置需要宿主核对。Artifact 清理和长期
容量归 M31，本阶段受存储配额及输入窗口限制，配额不足显式拒绝。

## 验证方式

文件存储关闭/重开、澄清认领窗口、投递前后故障、主体隔离和翻页、正文删除/损坏、
恢复取消/超时/接管、不确定 journal 只 Observe/Verify、许可失效与重复续办。
测试、API、设计和验收证据见 [M29](../plans/m29-interaction-coordinator-concurrency-and-recovery.md)。
