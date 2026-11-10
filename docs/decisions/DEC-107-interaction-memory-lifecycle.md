# DEC-107：统一交互的记忆生命周期接线

> 状态：Accepted（按用户要求继续推进 M30 开发）
> 日期：2026-11-10
> 负责人：Mira Maintainers
> 冻结里程碑：M30 首批增量
> 替代/被替代：补充 DEC-070/101/106

## 背景与问题

M29 已由 PR #124 合并且 CI 24 项通过。M36 的记忆服务仍需要宿主逐次绑定、
失效和提交任务，统一交互没有生命周期接线。本增量承接 M30-02/03/04/07/08，
不提前关闭承诺实体、完整纠正/删除交互、价值信号或删除后的新世代接纳。

## 决策

1. 宿主在初始化前显式向 InteractionCoordinator 注入 MemoryAgentService。默认
   不启用，既有宿主行为不变。协调层初始化服务，外部 owner 仍负责 service.close、
   Runtime 与 Executor 的关闭；不启动第二套 worker、timer 或自动重试队列。
2. 仅当前已授权且已提交路由的工作可以进入记忆面。默认精确白名单为同主体的
   User scope 和本 Session scope。User/Session/Application 的内部 subject 用
   域分离 SHA-256 映射，tenant 单独保留。Application 映射接受宿主稳定内部标识，
   不会被自动加入白名单。记忆文本或模型输出不能扩展 scope 或生成 grant。
3. stage_input 原子绑定/更新 principal、任务框架及 epoch，增加 revision、取消旧
   作业并封闭请求候选。仅在该工作待投递队列全部成功接纳后，才用对应 revision
   acknowledge_input 并提交一次有界 Recall。排队、拒绝或不确定投递期间不能发布
   新记忆上下文。更新不改变 Task/Session/主体及 scope 集，不复活终态或已撤销工作。
4. Recall 使用当前目标与最新约束，有界结果经既有 Gateway supplier 提供 P4
   候选；请求期不做存储或模型调用。首轮可在召回完成前执行，无命中不倒出 scope。
   主代理仍可通过既有工具主动启动 Record/Compress；Layer 0 独占准入。
5. 首次 record_settlement 关闭请求投影，再显式提交一次 Session scope 的 Record，
   仅消费该 Task 的既有事件，写入仍走 MemoryConsolidator。该步骤不伪造核验事件、
   不提升模型候选等级、不自动批准 Preference，不因记忆失败回滚输入或终态。
   重复回执不重复触发。页数不足时 more_pending 由宿主消费，不在后台无限续跑。
6. 每工作保存有界、进程内的 MemoryLifecycleStatus。失败与作业 future 可由当前
   授权主体查询；提交不等于完成。作业本身仍有 M36 事件和结果。同步错误显式保留，
   不将已成功投递的输入伪装成失败。取消、关闭撤销已绑定工作；直接 Runtime
   控制/接管仍须宿主同步 service.revoke/revise，不能由记忆文本代替当前授权。
7. 重启只恢复 M29 历史投影，不重建记忆作业或旧授权。续办新 Task 以当前输入重新
   建立绑定。M36 删除屏障继续拒绝旧 scope 绑定，不能靠本接线绕过。

## 备选方案与影响

每次由前台模型决定是否做生命周期维护会漏掉结算、纠正或未送达窗口；在请求期查询
或自动循环重试会增加控制路径不确定性。采用显式启用的协调层接线，复用服务的
有界 worker、revision 和取消机制。公开 API 为加法，需要消费者重编译，无新存储
schema、外部头族或模块依赖方向变化。跨进程持久的记忆意图台账归后续 M30 增量。

## 验证方式

M30 测试覆盖确定性 scope、主体/租户/会话隔离、队列接纳边界、纠正期间迟到结果、
拒绝/不确定投递、重复输入/结算、经管线写入、不提升等级、取消/关闭/超时/异常/
Executor 拒绝及实际 Gateway 请求。真实模型质量和平台验收另列补跑条件。

## 关联文档

- [M30 计划](../plans/m30-long-term-memory-integration.md)
- [记忆代理设计](../design/memory_agent_design.md)
- [交互协调 API](../api/interaction-coordinator.md)
