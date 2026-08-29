# DEC-003：EventStore 事实源与副作用日志协议

> 状态：Accepted  
> 日期：2026-08-30  
> 负责人：Mira Maintainers  
> 冻结里程碑：M1  
> 替代/被替代：无

## 背景与问题

Agent 会执行无法事务回滚的平台副作用。进程可能在“准备动作”“记录动作”“实际输入”和“收到
receipt”之间崩溃，普通业务事务无法同时覆盖本地存储和外部设备。

## 决策

- EventStore 保存已提交的 Runtime/Session/Task 事实并分配严格递增 session sequence；Checkpoint、
  Memory、索引和前端视图均是投影。
- ArtifactStore 使用内容寻址保存大载荷，Event 只引用 immutable artifact descriptor。
- 副作用使用 intent logging：先持久化 `ActionDispatchStarted`，收到 durable ack 后才允许调用
  Environment。随后记录明确 receipt 或 `ExecutionUncertain`。
- 该协议不声称 exactly-once。崩溃发生在 durable start 与 receipt 之间时，恢复必须 Observe/Verify，
  禁止自动重发。
- Event append 支持 event ID 幂等；尾部损坏、sequence gap、hash/CRC 失败不可静默忽略。
- 隐私删除允许 payload 不再可回放；保留最小 tombstone 和审计事实，文档明确 Replay quality 降级。

## 备选方案

- 先执行再记录：崩溃后可能完全不知道副作用，不采用。
- 记录 start 后认为动作一定执行：错误地把 intent 当结果，不采用。
- 依赖 Memory 或模型聊天历史恢复：它们不是权威事实源，不采用。

## 影响与风险

动作前多一次 durable write，离散输入延迟增加；连续动作只在整段开始前写一次，不在实时采样循环
写盘。磁盘不可用时必须拒绝新的不可逆副作用或进入明确降级，不能无日志继续自主操作。

## 验证方式

在 artifact temp/publish、event append、durable ack、platform call 和 receipt append 每一边界注入
崩溃，验证恢复分类且副作用不会被自动重发。

## 关联文档和工作项

- [事件、资产与崩溃一致性设计](../design/event_artifact_crash_consistency.md)
- [M1 核心契约](../plans/m1-core-contracts.md)：`M1-07` 至 `M1-11`

