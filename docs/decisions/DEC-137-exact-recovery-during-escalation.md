# DEC-137：自动失败升级优先读取精确恢复经验

> 状态：Accepted
> 日期：2026-11-24
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-030](DEC-030-learning-loop-runtime-semantics.md)、[DEC-031](DEC-031-agent-recovery-orchestration.md)、[DEC-136](DEC-136-exact-recovery-lesson-recall.md)

## 决策

失败驱动的 `WaitingAgent` 使用 run 固定的 Workflow、IR 摘要和最后失败签名，在宿主
已绑定的学习 scope 查核验过的 `RecoveryLesson`。先按完整失败键读取，再用剩余额度
按原有规则查询 `Episode`。两次有条件的读共享总条数、文本预算和软截止。检查点交接
不发起查询。

恢复经验使用宿主精确入口的记录校验。超时、后端失败或记录不符合契约时保留已取得的
合规结果，记一次升级检索故障。查不到精确恢复经验时可以用 Episode 补充上下文，旧版
或不同失败原因的恢复经验不会通过宽泛查询回填。Episode 继续适用 DEC-031 的诊断。

切换学习绑定、退出本次 WaitingAgent、取消载体或关停会丢弃迟到结果。发布检查使用
run 世代、学习绑定版本和载体恢复世代。重复取得的续跑视图复核已装配经验的绑定版本。
查询沿用驱动线程和现有存储 worker。

## 验收

验证版本和原因匹配、预算耗尽、Episode 回补、单次查询失败、截止与学习绑定失效，
并复跑等待、检查点与恢复编排用例。见 M30-32。
