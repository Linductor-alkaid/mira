# DEC-083：宿主显式验证事实提交

> 状态：Accepted
> 日期：2026-11-06
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-04

## 契约与信任边界

新增 `VerifiedFactPublisher`：由宿主显式提供有内容的 claim、稳定 publication EventId、
ObservationRecorded 证据 EventId/载荷摘要、verifier 标识、task/environment epoch 和
有限有效期。Publisher 绑定 runtime/session/MemoryScope；仅支持 Environment、Application
和 User scope，kind 映射沿用现有固化管线。它不是模型输出解析器，不扩展 ILoopVerifier，
Satisfied、done 或空事实本身不能产生事实。

宿主注入受信的 lease acquirer。lease 必须独立验证 claim 的语义，并将任务准入、
epoch、主体权限与取消/终态收敛序列化到 append 完成；这不是 Core 从一段哈希中证明
事实为真的能力。没有 lease、无内容、无真实 ObservationRecorded 引用、跨身份、
错误摘要、无效/过期时间或取消/deadline 均拒绝。示例只验证 simulator 的屏幕宽度，
不宣称通用语义验证或模型判断准确。所有宿主取消/暂停/接管/完成必须遵守同一 lease
边界；绕过 owner 或仅并发翻转 cancellation probe 不构成原子撤回保证。

## 事件、时效与兼容性

- 事件名仍为 TaskFactVerified，新 payload 使用 `mira.task-fact-verified.v1`，claim
  嵌套为 key/value，并含完整身份、scope、证据、verifier、valid_from/valid_until。
  旧 reader 找不到顶层 key/value 会拒绝，不能把忽略时效后的新事实当作旧事实。
- 新 reader 严格解析 v1；有 schema 但无效/未知的记录不能回退旧格式。无 schema
  的旧 flat key/value 继续按既有受信宿主事件读取，不把旧格式升级成新的验证保证。
- 有效期上限一天，时间为统一 wall 纳秒；新事实只在区间内成为可用投影/记忆。
  Publisher 检查源事件身份、epoch、摘要及先后关系，来源扫描有页数/事件预算。
  默认 forbidden/injection markers 在写事件前检查，不把凭据或指令形文本记录为事实。
- publication EventId 是幂等键，重试必须重用同一 claim/证据/时间与 ID；EventStore
  冲突拒绝相同 ID 不同内容。不维护无界驻留缓存，重开后仍依赖 store 幂等。
  append 弱确认/错误身份/非法序号为 DataLoss，store 异常隔离为固定摘要。错误不
  保证 append 未发生；调用方必须按原 ID 恢复/重试，不生成新的 ID。
- Checkpoint schema 升为 2.0，VerifiedFact 增加可选有效区间与 valid_at。读取 1.x
  旧事实保持原语义；2.0 投影使旧 reader 明确拒绝，防止忽略期限而复活已过期事实。
  Builder 对新事件做终态/epoch 检查并按 now 清理过期事实；base 重建同样清理。
  字段 codec 私有拆分用于维持文件预算，不刷新架构 baseline。
- MemoryConsolidator 对新事件验证完整格式、精确 scope 与有效期，将证据/期限传入
  既有 MemoryRecord，仍走 marker、kind、冲突、幂等、人工审批与 ACL；不建第二套写入
  管线。重复事实不应延长已有记录期限；需要续期时用新的证据与 publication ID。

ObservationRecorded 还须含持久化 `observed_at`。BUG-20261106-001：现有 FileEventStore
在恢复时通过 MemoryEventStore 重建信封，其 timestamp 是恢复时间，不是原写入时间。
新协议以 observed_at/valid_from/valid_until 校验时序，并以 valid_from 作为 Memory
的稳定验证记录时间，不用可重建的信封时间。它不是磁盘 commit 时钟；该基础存储问题
由维护计划 12 单独跟踪，不在本次静默变更日志格式。

同值重放继续 duplicate-noop，连同新证据也不会自动延长尚存记录期限。不同值的
确定性 VerifiedEvent 不能用较旧或同时间的证据覆盖已有同类事实。带期限的新证明
可替换无期限 legacy 行；无期限 legacy 不能反向覆盖带期限事实。HumanCorrection
及模型辅助管线仍沿用原策略。旧写入方迁移后如需更正有期限事实，须走新版证明入口。

## 所有权与验证

Publisher 是同步、有界协议，不拥有 Executor/worker。宿主通过 kairo 调度，保存并
消费 future；后端 I/O 必须有解除阻塞路径，timeout 不强制终止 append。lease 析构
不得抛异常，取消先于准入则没有新事实；准入提交完成后取消不回滚已验证历史。

交付公开头、API/设计不变量、可构建离线宿主和 contract 测试。覆盖可信事实经
FileEventStore→Checkpoint→SQLite Memory→检索、重开重复与冲突、无证据/无内容、
错误 scope/身份/epoch、语义拒绝、取消/终态/接管、截止时间、提交拒绝/弱回执/异常、
过期与旧格式兼容。M30 的自动候选触发、主体映射与跨会话选择不在此处启用。

关联：[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)、
[Context/Memory 设计](../design/context_and_memory_design.md)。
