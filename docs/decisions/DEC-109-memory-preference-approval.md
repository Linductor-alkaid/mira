# DEC-109：长期偏好的持久提案与显式确认

> 状态：Accepted（用户要求在最新 master 上继续推进 M30）
> 日期：2026-11-10
> 负责人：Mira Maintainers
> 冻结里程碑：M30-03/05 首批偏好管理增量
> 替代/被替代：补充 DEC-070/101/107，保留 DEC-108 原生角色召回

## 背景与问题

主干 `23f1291` 已接通生命周期与带框事实召回，但模型提出的 Preference 只能留在
临时 consolidation 报告中，前台缺少可跨重启的预览、确认与拒绝协议。本增量完成
该边界，不宣称交付承诺、完整自然语言意图识别、选择性物理删除或删除后的新世代。

## 决策

1. MemoryAgentService 提供 propose/preview/decide 的有界同步宿主 API，调用方通过
   注入 Executor 的有限任务执行并消费结果。主代理只得到 memory_propose_preference
   工具；确认/拒绝不暴露为模型工具，必须来自已认证宿主的明确用户选择。
2. 提案固定为绑定主体的 User scope、Preference kind、Unverified。statement 必须
   为词法 key=value，模型负责语义规范化；Core 不猜测同义表达。候选仍走
   MemoryConsolidator，新 policy 开关强制所有修改先待审批，包括 HumanCorrection。
   默认关闭该开关，保持旧调用行为。批准授权持久化，不自动提升事实验证等级。
3. 首次提案冻结 pipeline 产生的完整 MemoryMutation（包括新 ID、目标、版本、原因和
   provenance），正文仅进入 Sensitive Artifact。控制事件只记录主体、scope、摘要、
   引用及状态。修改既有同 claim key 的值须预览被替代记录和版本，确认时不可重新规划
   到另一个目标。可选显式 target/version 进一步约束提案；同 claim 重复不提升等级。
4. 宿主预览和决定需当前有效的 mira.memory.manage grant，version 与身份版本一致，
   resource selector 为空（主体自己的全部偏好）或精确 memory/User scope。认证主体、
   提案主体和 identity_version 必须一致，摘要必须精确匹配。Scope/授权不能来自记忆
   文本。审批不依赖旧 Task 仍活动，允许跨会话查看同主体的待确认提案。
5. 批准前先耐久记录 approval-intent，再通过既有 apply_pending 提交冻结 mutation，
   最后记录完成。mutation id 幂等，版本冲突不静默覆盖。确认同一服务内 claim 前置
   状态；服务外写入必须先成功 invalidate_scope 并收敛并发写入，该操作同时耐久作废
   旧提案。MemoryAgentService 是相关 scope 的单一写入 owner。
6. 追加失败/弱 ack 将偏好管理面封锁，不能把未知提交当作未提交。重开服务从事件重建；
   未结算 intent 只允许当前授权宿主显式重试，绝不自动重写。过期且未结算的审批明确
   报告需核对，不声称没有发生过写入。已完成回执是历史事实，不代表记录仍为当前值。
7. 提案默认 24 小时过期，总量默认 256（含已完成回执）；满额显式拒绝，清理归 M31。
   拒绝/作废清理正文；scope erase 及重建时清理所有关联提案正文。删除屏障仍封闭该
   scope，不允许审批、旧 Artifact 或重试把被删内容写回。已知无引用的失败注册
   Artifact 至多一份/封锁窗口，后续物理回收归 M31，不能为省空间丢弃未知耐久引用。
8. 批准前失效同 scope 的请求候选/压缩并取消旧作业；即使 apply 或完成日志失败也不
   恢复旧快照。接口有 deadline/取消检查，I/O 仍须 Adapter 配合，超时不等于抢占。

## 影响与验证

新增 API、policy 加法字段与版本化事件，不新增 SQL 表、写入后端或并发设施。
文档/测试覆盖审批门、纠正原因与版本、重复提及、主体/租户/版本/grant、重开、
未知 ack、擦除、过期、取消/关闭/Executor 拒绝及 DEC-108 召回回归。真实 UI、模型
语义质量与跨平台验证另行取证。

## 关联

- [M30 计划](../plans/m30-long-term-memory-integration.md)
- [记忆代理 API](../api/memory-agent.md)
