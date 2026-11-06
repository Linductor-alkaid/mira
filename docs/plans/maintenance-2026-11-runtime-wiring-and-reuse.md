# 维护计划：运行时接线、恢复证据与复用治理（2026-11）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M1/M3/M4、M25、M28 已交付；各实现项的附加前置见 §4
> 建议发布点：Host contract alpha 维护增量；拆分交付，不形成新的产品里程碑
> 更新日期：2026-11-06

## 1. 目标

把本轮用户提供的《Mira 项目框架与数据流分析报告》转成有证据、依赖和退出条件的
修复队列。先修动作副作用日志与可直接复现的计数错误，再提供组合根示范，最后做
有行为基线的复用重构。首轮实施推进 05 与新增的 journal 确认前置项 13；完整
动作派发接线仍归 01–03，不以底层修正代替闭环验收。第二轮依 DEC-075 冻结输入
接线边界并完成 AgentLoop 的离散派发路径；03 的完整崩溃/通用 journal 矩阵仍开放。

本计划以工作树 `b81df19` 为源码复核基线。报告中“仓库内无调用”不能推导成
“下游无人使用”，测试和示例也不能等价于部署证据。行数、重复比例及全库“零引用”
结论未经完整重测，不作为本计划的验收数字。

## 2. 范围与非目标

### 2.1 范围

- 动作 durable start、执行结果、取消与恢复之间的接线，以及对应故障注入。
- 已验证事实的生产契约与现有记忆管线衔接；长期记忆自动接入沿用 M30。
- Workflow、Layer 0 与恢复的离线参考宿主，价格配置与宿主度量出口。
- attempts 覆盖、局部死代码、SQLite 初始化与余弦相似度的定向治理。
- 报告剩余条目的归属登记；不把所有未使用接口一律删除或自动启用。

### 2.2 非目标

本轮不改 `third_party/`，不新增并发框架，不自动开启原始模型响应捕获，不把模型文本或
`done` 声明当作已验证事实，不引入 Core 对具体 SQLite 实现的反向依赖。
WorldState、Temporal Policy、Security、流式与 continuation 的全面集成不在本轮范围。
真实模型语义质量、真机、实时性及其他平台声明继续服从原有证据门禁。

## 3. 依据与报告校正

### 3.1 设计与决策依据

- [项目管理与文档规范](../project/project_management_and_documentation.md)。
- [DEC-003](../decisions/DEC-003-event-sourced-persistence.md) 与
  [崩溃一致性设计 §6](../design/event_artifact_crash_consistency.md#6-副作用提交协议)：
  副作用前 durable ack，未决结果恢复时先观察，禁止自动重发。
- [Runtime 设计](../design/mira_runtime_design.md)、
  [DEC-018](../decisions/DEC-018-takeover-input-release-and-operation-admission.md)：
  lease、epoch、操作准入与 Human Takeover。
- [Context 与 Memory 设计](../design/context_and_memory_design.md)、
  [DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)、
  [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)：
  上下文边界与 Working Context seam 不改变。
- [DEC-070](../decisions/DEC-070-long-term-memory-behavior-contract.md)、
  [M30](m30-long-term-memory-integration.md)：候选提出、scope 映射与记忆选择接线。
- [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)、
  [M31](m31-capacity-admission-and-archival-recovery.md)：容量、保留及归档恢复。
- [DEC-043](../decisions/DEC-043-architecture-policy-and-baseline.md) 与
  [架构治理](../project/architecture_governance.md)：共享代码的依赖方向与文件预算。

本计划不冻结新的公开接口或持久化格式。实现前若需要改变这些契约，必须在
`MNT-202611-01` 中补充对应 DEC、设计和兼容性方案，不能以本计划代替决策。

首轮确认门槛与计数语义由
[DEC-074](../decisions/DEC-074-journal-ack-and-gateway-attempt-accounting.md) 冻结，
不预先冻结完整派发接线接口。

### 3.2 基线已复核事实与处置

| 报告条目 | 本轮证据与校正 | 处置 |
| --- | --- | --- |
| ActionJournal 未进入主循环 | [AgentLoop](../../src/model/agent_loop.cpp) 发 `ActionDispatched` 后直接执行；[ActionJournal](../../src/storage/action_journal.cpp) 在 src/examples/adapters 无构造点；DEC-003 要求动作前 durable start | P0，01–03；不是可以任选退役的接口 |
| TaskFactVerified 断供 | src 仅有 [checkpoint 消费](../../src/storage/checkpoint_store.cpp) 与 [consolidation 消费](../../src/context/memory_consolidation.cpp)，示例手造事件；[ILoopVerifier](../../include/mira/agent_loop.hpp) 只返回 Verdict，没有事实 key/value/provenance | P1，04；不能把 `VerificationResult` 改名作为修复 |
| 所有 verified_facts 恒空、记忆通道全死 | 上述结论仅能限定在未注入事实事件的路径；Working Context 晋升等入口独立存在 | 不推广为全系统结论；04 与 M30-03 对接 |
| attempts 无效 | [gateway](../../src/model/model_gateway.cpp) 连续写 `++total_attempts` 与 `attempt`，累计值被覆盖 | P1，05；先明确跨候选计数契约 |
| 空 PriceTable 等于零成本 | [model_budget.hpp](../../include/mira/model_budget.hpp) 明确缺项为 unknown、不是 zero；gateway 有拒绝未知价格策略 | P1，07；配置与说明缺口，不预设记账实现错误 |
| 余弦零向量语义分叉 | [context 检索](../../src/context/context_retrieval.cpp) 返回 0，[memory store](../../src/storage/sqlite_memory_store.cpp) 返回 -1 | P2，09；确认过滤/排序影响后再统一，不直接替换常量 |
| SQLite open 重复 | 两个 store 均有打开、schema 检查与初始化编排；具体可抽取范围及行为等价性还需实现前逐项核对 | P2，10；保留 store 特有 schema 和错误域 |
| 无仓库消费者的公开能力可删 | `scheduling_metrics`、解析器、digest、枚举等属于潜在下游契约 | 08、11；先审计公开性与兼容性 |
| 生产 stream 恒 false、库能力生产不可达 | Mira 是宿主装配的库，仓库调用点不足以证明部署状态；已有模型流式测试/probe 与对话维护记录 | 12 归类，不作为删除或缺陷依据 |
| WorldState 未接线 | [M27 §2](m27-world-state-projection-core.md) 明确消费者零接线 | 12 记录原计划边界，不重新打开已关闭里程碑 |

## 4. 工作项与交付顺序

各项负责人均为 Mira Maintainers；复选框表示实现验收状态，实施证据按轮追加于 §9。
优先级 P0 表示本轮首先处理的契约风险，不代表已证明所有部署都受影响。

### 4.1 第一批：副作用安全与直接缺陷

- [x] `MNT-202611-13`（P0，01/02 的前置修正；Linux 离线范围）校验 journal 的持久化回执。
  每个 append 校验 achieved，弱确认/非法枚举返回 DataLoss，不推进本地 phase，
  底层拒绝错误保持原样。测试四种事件的拒绝与弱确认、强确认重试及文件重开后
  start 无 receipt 的 journal/checkpoint 恢复；真实耐久性不能由内存替身证明。
  契约与边界见 DEC-074，本项不覆盖 Loop/Workflow 接线和全部 crash 窗口。
- [x] `MNT-202611-01`（P0，设计冻结范围）冻结接线方案与影响清单。
  审计 AgentLoop、Workflow、可写工具和连续控制的真实副作用入口，列明已有准入、lease、
  epoch、事件所有者与未覆盖边界；先确定本轮实现覆盖面及剩余项归属。
  推荐宿主注入公共 port/依赖，由单一组件执行 journal 协议，Core 不构造存储实现。
  定义 event append 失败、无 durable store、epoch 过期、动作结果未知的返回值与事件。
  审核 pipe payload 的分隔符、字段完整性、重建分页和旧事件兼容性；若修改 schema/API，
  先补 DEC 与迁移测试。验收：入口矩阵、设计时序、兼容性表与可执行测试清单齐全。
- [ ] `MNT-202611-02`（P0，依赖 01；离线实现已落地，待 03 完整验收）接通 AgentLoop 的动作日志路径。
  顺序为 prepare → lease → durable start ack → 再校验 epoch/准入 → execute →
  receipt/uncertain → 释放或收敛 → Observe/Verify；具体 lease 所有者按 01 冻结。
  start 写入失败时 Environment 调用次数为零；动作后写入失败时保留未决状态并向调用方
  暴露失败，不依赖普通 `void emit` 承担关键提交。验收：通过 03 的故障矩阵。
- [ ] `MNT-202611-03`（P0，依赖 02）证明日志、checkpoint 与恢复闭环。
  第二轮已验证输入次数/日志顺序/lease 释放、嵌套 Loop 事件与 flat JSON 兼容、
  撤回/epoch/deadline/未知结果与终态保守恢复；仍待补齐下面的完整窗口和通用 journal 矩阵。
  第三轮已补通用 journal 身份/pipe/分页矩阵与九个突然退出窗口、ASAN+UBSAN；
  真实平台、OS 强杀与断电等剩余门禁见第三轮验证记录，保持本项未完成。
  在 start 前后、真实调用前后、receipt 提交处注入失败/重启；区分明确拒绝、成功、
  结果未知、结果迟到。至少覆盖 start 无 receipt 与显式 uncertain，证明
  `verification_pending`/未决副作用按契约保留，RecoveryPlanner 先 Observe/Verify，
  不自动重发，终态不复活。覆盖 action 身份对齐、重复事件和旧 pipe/JSON 事件读回。
- [x] `MNT-202611-05`（P1，可与 01 独立推进；Linux fixture 范围）修正累计尝试计数。
  确认 `ModelCallOutcome::attempts` 与事件消费者约定；推荐表示本次 infer 实际调用
  provider 的总次数。覆盖单 provider 成功、重试后成功、切换候选成功、熔断跳过和
  耗尽失败；断言返回值、事件与实际调用次数一致。若既有公开语义不同，先补 DEC。

### 4.2 第二批：事实契约与宿主装配

- [x] `MNT-202611-04`（P1，依赖 01；Linux 离线显式宿主范围，自动记忆接线依赖 M30）建立事实生产边界。
  对照 checkpoint 的 key/value 与 MemoryConsolidator 的 scope/kind 要求，明确可信
  验证证据、稳定 key、来源引用、时效、冲突与幂等规则。先提供宿主显式提交事实的
  最小参考路径；是否扩展 verifier 或新增提交 API 由 DEC 冻结。
  验收：有证据事实进入 checkpoint，经既有 consolidation 管线产生可检索记录；
  无事实内容的 Satisfied、模型 done、Invalid、不匹配身份、取消后迟到和重复提交
  均不得错误提升为 Verified。保留既有 marker 过滤、敏感类别审批和 ACL。
  自动候选触发、主体映射和跨会话选择由 M30-02/03/04 实现，本项不另建写入管线。
  实现契约见 [DEC-083](../decisions/DEC-083-explicit-verified-fact-publication.md)，
  Checkpoint 2.0 与 C++ 消费者迁移见
  [兼容性说明](../compatibility/verified-fact-v1-and-checkpoint-v2.md)；本地取证见第九轮。
- [x] `MNT-202611-06`（P1，Linux 离线组合范围；恢复样板依赖 03 的 Linux 日志/恢复契约证据）提供三个可构建离线参考宿主。
  分别演示 WorkflowRuntime 从创建到终态、StandardContextManager 从 checkpoint/
  检索输入到有界请求、RecoveryPlanner 从耐久存储重建到下一步安全状态。
  使用 scripted provider/simulator，声明样板的持久化等级与能力边界，保留 M25
  Working Context seam。接入 CTest consumer 门禁，断言产物确实被下一跳消费，
  不只展示构造成功；不把 Layer 0 自动接进两个 Loop。变更自动装配另行决策。
  [DEC-086](../decisions/DEC-086-offline-composition-hosts.md) 明确离线前置和消费边界；
  本项不关闭 03 的真实平台、OS 强杀/断电等整体门禁。
  三个 [组合宿主](../api/offline-composition-hosts.md) 及安装后编译运行已通过，证据见第十二轮。
- [x] `MNT-202611-07`（P1，可独立；Linux 单 profile 离线 fixture 范围）补齐价格与未知费用的宿主装配说明。
  提供确定性 fixture PriceTable 注入样板，验证已知价格的 reserve/reconcile、
  未知价格的拒绝/允许策略、usage 缺失与取消时的预算结算。
  不硬编码外部实时价格；样板清楚标注 unknown，避免宿主把空表成本当作免费。
  当前按 [DEC-084](../decisions/DEC-084-model-price-reference-host.md) 实施公开 API
  单 profile fixture；库默认策略、迟到费用与实际预留余额须由宿主显式消费。
  已交付 [价格宿主指南](../api/model-price-host.md) 和十二场景 consumer，证据见第十轮。
- [x] `MNT-202611-08`（P2，可独立；Linux 离线诊断宿主范围）兑现调度度量的宿主出口。
  在参考宿主读取 `scheduling_metrics` 并展示拒绝/背压的可解释结果，确认累计值与
  当前值的语义及采样边界；采样走 kairo，诊断消费者不得阻塞 Runtime 控制面。
  验收：过载、关闭中提交与异常均能从结果/事件观察到，度量能够辅助解释。
  不因仓库无读取点删除公共 API，不并入 M31 的配额/容量算法。
  当前按 [DEC-085](../decisions/DEC-085-runtime-scheduling-diagnostic-host.md) 实施
  [独立采样宿主](../api/runtime-scheduling-host.md)，路由累计值与串行命令结果分开消费。
  公开 13 项计数、局部拒绝计数与当前未决数已消费；验收和限制见第十一轮。

### 4.3 第三批：复用与兼容性治理

- [x] `MNT-202611-09`（P2，可独立；Linux 离线验收）统一向量有效性与相似度语义。
  先对齐零范数、维度不匹配、非有限值、负相关与阈值含义，区分“无有效相似度”与
  数值 0/-1；用现有检索结果建立行为基线。共享逻辑放在可被两模块依赖的恰当边界，
  不让 Core 引用 storage 私有头。验收：两调用方在相同输入下采用同一有效性规则，
  无效向量不能因占位分数进入结果，正常排序、ACL 与预算截断无回退。
- [x] `MNT-202611-10`（P2，可独立，建议在 03 后合入；Linux 离线验收）抽取 SQLite 初始化公共编排。
  范围为连接打开、PRAGMA、schema-version 解析与失败清理；依 DEC-078 统一事务提交/
  回滚编排，DDL 与迁移内容保留 store 特有策略，复用既有 sqlite_support 边界。
  验收：两个 store 的新库、现有库、非法/过大/未来版本、损坏 schema、忙/只读、
  中途失败后重开均有对照测试；除 DEC-078 明确收紧的版本/只读错误分类外，错误映射、
  关闭和 Executor 单写者语义保持，迁移与版本标记原子回滚。
- [x] `MNT-202611-11`（P2，Linux 离线复用与报告点名契约审计范围）处理有证据的局部孤儿和重复。
  已清理 stash_headers、按 DEC-079/080/081 完成 workflow JSON 校验、TaskState 名称、
  Runtime 全部状态写入、Context/Memory marker 与简单错误构造；按 DEC-082 完成
  20 个 Workflow 事件封装尾部、TLS 公共准入/构造及 module 错误工厂复用。
  每批证据见第四至八轮记录；默认值、字段、错误域和必要后端差异保持。
  报告点名的公开 parser、诊断名、digest、CoordinateSpace 与预留枚举已逐项登记
  [保留依据](../compatibility/2026-11-public-contract-audit.md)，补直接消费者和兼容性
  契约测试，没有按引用数删除或激活公共词汇。检索选择骨架及其他领域错误/大小写映射
  经审计保留各自策略，不建立跨层通用模板。未执行的目标平台/TSAN 条件仍在记录中
  开放，本项验收不关闭 02/03、12 或维护轮全局门禁。
- [ ] `MNT-202611-12`（P2，依赖各批证据）关闭报告处置台账与文档同步。
  每项登记已修复、原计划承接或待立项的结论，关联 API、设计、示例、决策和测试；
  保持原里程碑的范围与真实未执行项。验收：§5 无无主条目，各实现项有复现证据，
  不用“仓库没调用”证明可以删除，不用 fixture 成功证明真实部署可用。

建议 PR 顺序：01 设计冻结 → 02/03 动作日志 → 05 计数修复 → 04 事实参考路径 →
06 组合根 → 07 价格 → 08 度量 → 09 向量语义 → 10 SQLite → 11 局部治理 → 12 收口。
05/07/08/09/11 可独立准备；不要求无关修复等待整个维护轮结束。

## 5. 其余报告条目的归属

| 条目 | 承接位置与进入实现的条件 |
| --- | --- |
| BUG-20261106-001：文件事件恢复重建 timestamp | 12 单独冻结日志时间持久化与旧日志兼容；04 用载荷 observed_at/valid_from 避免依赖该时间，但不宣称全局已修复。负责人 Mira Maintainers，决策与格式矩阵就绪后补跑 |
| Gateway 多候选价格与累计账单边界 | 07 只验收单 profile/一次 infer；12 登记专项费用约束立项，核查 fallback/重试重估、resolved 缺价、spent+reserved 及审计持久化，不扩大 M33 自主打断/配额范围。负责人 Mira Maintainers，专项设计/矩阵就绪后补跑 |
| TaskGoalSet/Constraint/Step/Observation 等投影词汇 | 01 审计事件生产者与 schema，04/06 提供端到端样板；逐事件确认必要性，不为凑齐词汇制造事实 |
| masked_regions / FrameMaskReason | 12 登记为平台供给契约核查；实际遮蔽来源、隐私需求和 Adapter 能力确认后专项立项，不能伪造非空值 |
| IArtifactSource / capture_raw_response | 12 登记为显式可选能力；启用前必须明确脱敏、访问、保留与删除规则，当前保持 opt-in |
| Layer 1/2 检索、重排、语义 consolidator/curator | 06 演示输入输出边界，M30-03/04 承接长期记忆生命周期；其他自动化另行立项 |
| WorkflowRuntime、FileEventStore/FileArtifactStore、RecoveryPlanner、会话投影 | 06 演示所选真实装配链，声明其覆盖面；测试/公共库入口不能仅凭无 examples 判为不可达 |
| ConversationLoop、SSE、ProviderContinuation | 12 核查既有会话维护记录与公共入口；保留已交付能力，不根据本报告重开或退役 |
| WorldState / TemporalPolicy / Security 活动组件 | 12 保留 M27/M26 与 DEC-004 原边界；有明确消费需求时专项立项，安全组件不直接替换现有权限检查 |
| schedule_global_retention_sweep | M31-09（周期清扫宿主）承接；12 核对实际覆盖及缺失测试，删除仍按原计划门禁启用 |
| 度量与 ActionJournal 驻留容量 | 08 提供出口；M31 承接容量/保留策略，不让日志接线自动扩大保护数据淘汰权 |
| CoordinateSpace、死枚举、parser、digest/name 工具 | 11 逐项兼容性审计；闭集保留值与 wire 数值不随意移除 |
| SQLite/检索选择骨架/workflow/TLS/错误工厂/大小写转换重复 | 09/10/11；先对齐语义再抽取，排序/ACL 和 marker 词表作为回归边界 |

## 6. 并发、所有权与失败约束

Executor 由参考宿主或现有外部 owner 提供。有限任务继续使用 kairo 公共提交能力并消费
future；SQLite 写入继续沿用 StoreChannel。需要阻塞 I/O 的新增路径先核对 kairo
公开 worker API；模型/平台等待必须可解除阻塞，循环检查取消与 epoch。

动作 intent、journal、lease、任务句柄和失败传播的所有者在 01 中逐项明确；不得让
Runtime/Loop/Adapter 隐藏 Executor。停止顺序为停止生产 → 取消 → 回收 worker/控制路径
→ 等待有限任务 → 非 worker owner 执行 `shutdown(true)`。新增异步路径覆盖正常、异常、
拒绝、执行中取消、超时、shutdown 与 Human Takeover，迟到结果不得产生新动作或复活终态。

诊断事件不承担 durable gate。关键日志提交失败必须成为结构化失败；Replay 默认仅消费
录制结果，禁止真实输入/触网。若确认 kairo 缺口，按现有反馈台账流程处理，不先引入替代设施。

## 7. 测试与退出条件

| 门禁 | 关联工作项 | 验收证据 |
| --- | --- | --- |
| RW-G1 副作用提交与崩溃恢复 | 01–03 | 故障注入下实际 Environment 调用次数、事件水位、恢复动作；start 无 ack 零输入、未决不重发 |
| RW-G2 事实可信性与记忆 | 04 | 正向事实→checkpoint→consolidation→检索；负向/跨身份/迟到/幂等与审批用例 |
| RW-G3 尝试数与预算 | 05、07 | 重试/换候选实际调用数；known/unknown usage 与价格策略一致 |
| RW-G4 参考宿主与诊断 | 06、08 | 安装后 consumer 编译运行、三条产物消费断言、背压与异常可见、Replay 零真实副作用 |
| RW-G5 复用行为等价性 | 09–11 | 向量无效值与排序、两个 store 故障矩阵、状态转换、序列化兼容性回归 |
| RW-G6 质量与证据同步 | 12 | 受影响测试与适用全量回归、架构/平台边界/格式/文档检查；跨平台与 sanitizer 按影响面取证 |

- [ ] 工作项及 RW-G1–G6 均有通过证据，实施范围与剩余项归属明确。
- [ ] 公开契约变化对应 DEC、设计、API、示例与兼容性说明同步。
- [ ] 涉及并发的改动完成取消/拒绝/超时/shutdown/Takeover 矩阵及适用 sanitizer。
- [ ] 未执行验证保留未完成状态，记录原因、负责人和补跑条件。

实现轮从现有 `tests/m1`、`tests/m3`、`tests/m4`、`tests/m17`、`tests/m25`、
`tests/consumer` 的相关用例扩展，具体测试名在提交中登记。建议基础命令：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --test-dir build/debug --output-on-failure
python3 tools/check_architecture.py
python3 tools/check_platform_boundary.py .
python3 tools/check_docs.py .
```

这些是后续实现验收命令，本轮未执行构建与运行测试；平台/真实 Provider 未验证，
不宣称跨平台、实时性或语义效果。

## 8. 风险与阻塞

- **持久化失败语义**：部分宿主使用内存 EventStore，不能把内存 ack 当作进程崩溃耐久性；
  01 要冻结能力要求与明确拒绝/降级策略，02 验证无旁路。
- **日志内容与兼容性**：现有 pipe 格式与 safe_message 不能直接扩成任意用户内容；
  01 要决定验证、编码、版本与脱敏边界，已有事件必须可读。
- **错误提高可信度**：Verify 的 Satisfied 只说明 verifier 判定；ModelDoneVerifier
  明示用于 scripted 测试，不能作为真实事实验证器。04 不能自动晋升模型自述。
- **重复建设**：M30/M31 已有候选、选择、容量与 retention 计划。04/08/12 只补前置与
  参考边界；依赖原工作项的自动接线保留未完成，负责人 Mira Maintainers。
- **跨模块抽取**：Core 不可依赖 state_store 私有实现。09/10 若需要新依赖，先走架构
  决策与 policy 更新，不刷新 baseline 隐藏新违规。
- **外部环境**：本计划不要求外部凭据来证明离线闭环。目标平台构建/运行、真实 Provider
  互操作与语义评估未执行，负责人 Mira Maintainers；实现后提供对应 CI 工具链、设备和
  凭据条件再补跑。原有外部证据门禁不被本计划解除。

## 9. 验证记录

2026-11-05：计划编制与定向源码复核，基线 `b81df19` + 既有未提交文档变更。
本次仅新增本计划并追加总计划入口，没有实现修复或修改既有未提交文件。

- 已复核：使用 `rg -n` 检查 src/examples/adapters/include 的事实与 journal 事件、
  检查 AgentLoop 派发和 verifier 接口、gateway attempts 与 unknown price 分支、
  两处 cosine 实现及 M27/M30/M31 的既有归属。结论见 §3.2。
- 文档检查：`python3 tools/check_docs.py .` 退出码 0，仓库 Markdown 链接与围栏通过。
- 补丁检查：`git diff --check` 退出码 0；另对新文件检查行尾空白，结果通过。
- 结构检查：Python 检查本计划恰有 12 个唯一且未勾选的工作项（01–12）、一个一级
  标题及成对围栏，退出码 0；后续构建命令已对照 CMakePresets，M31 清扫归属已对照原计划。
- 未执行：所有实现测试、构建、跨平台与真实 Provider 验证，因为本次交付仅为计划；
  负责人 Mira Maintainers，实现对应工作项后按 §7 补跑。所有实现项维持未完成。

### 2026-11-05：第一轮实现（05 / 13）

- 工作树：`b81df19` + 本轮变更，分支 `fix/runtime-wiring-foundations`；保留既有
  M29/M31/M33 与 superagent 文档工作树内容，不纳入本轮修复。
- 范围：05 累计调用数及失败诊断 `total_attempts`；13 journal 每次 append 的
  achieved 校验。无新增并发路径，沿用现有 Executor owner 与测试夹具关闭逻辑。
- 依据与同步：DEC-074、两份 API 手册、LLM 与崩溃一致性设计、总计划入口。
- 01 定向入口审计：AgentLoop 在 `src/model/agent_loop.cpp` 直接 execute；
  Workflow 在 `dispatch_tool_invocation` 通过 kairo future 调用 BuiltinToolRegistry；
  registry 在 `src/model/tool_executor.cpp` 校验身份/schema 后执行 handler。
  这些路径未构造 ActionJournal，单纯修 journal 不会让它们获得 durable gate。
- 01 尚待冻结：宿主准入/lease 所有者与 journal 依赖 port、全任务/环境身份再校验、
  pipe 字段与恢复扫描边界、无耐久 store 的拒绝策略、连续控制与可写工具的接线范围。
  01–03 保持未完成，下一批先完成这些设计与故障矩阵，再修改主循环。
- 环境：Linux x86_64、GCC 13.3.0、CMake 3.28.3、clang-format 18.1.8；Debug，
  `MIRA_BUILD_TESTS=ON`、`MIRA_WARNINGS_AS_ERRORS=ON`，scripted provider，无外部凭据。
- 构建：`cmake --preset debug`；
  `cmake --build build/debug --target mira_m1_action_journal_test mira_m1_core_test mira_m3_gateway_test mira_m3_agent_loop_test mira_m4_recovery_test -j 4`
  均退出码 0。构建日志位于本机 `/tmp/mira-wiring-build.log`，为临时诊断文件，
  以上命令为可复现来源，不依赖日志长期保留。
- 回归：`ctest --test-dir build/debug -R '^mira_(m1_(core|action_journal)|m3_(gateway|agent_loop)|m4_recovery)_test$' --output-on-failure`
  **5/5 通过**，退出码 0。随后收紧文件重开断言为 `ResumeVerifying`，并增加
  弱 start 回执后的新 journal 重建断言；重建 journal target 后执行
  `ctest --test-dir build/debug -R '^mira_m1_action_journal_test$' --output-on-failure`
  **1/1 通过**，退出码 0。
- 质量：`python3 tools/check_architecture.py`、`python3 tools/check_docs.py .`、
  `python3 tools/check_platform_boundary.py .`、变更 C++ 文件的
  `clang-format --dry-run --Werror` 及 `git diff --check` 均退出码 0。
- 05/13 按上述 Linux 离线范围完成；维护轮其余门禁和完整接线不关闭。
- 限制：Linux 本地验证不能替代 Windows/Android、真实模型或强杀/断电测试；
  本轮未跑全量回归或 sanitizer（仅两处同步逻辑修正，无新增并发）；
  负责人 Mira Maintainers，后续集成/CI 扩大回归范围，具备对应设备/Provider 环境后补跑。其他平台范围
  保持未完成，不宣称已具备完整副作用崩溃恢复保证。

### 2026-11-05：第二轮输入接线（01 / 02，03 部分证据）

- 工作树：同分支 `fix/runtime-wiring-foundations`，基线 `b81df19` 加前后两轮变更。
  既有 M29/M31/M33 与 superagent 文档变更继续保留。
- 决策与设计：DEC-075 与 `journaled_input_dispatch_design.md` 冻结输入边界；
  AgentLoop 必须显式注入 Dispatcher，宿主拥有资源 lease、生命周期/epoch/权限准入
  与取消/interrupt 桥。工具与连续控制覆盖清单独立列出，不宣称全入口已接通。
- 实现：`action_dispatcher.hpp` / `src/model/action_dispatcher.cpp`，持有注入 store，
  每次调用仅保存一个动作的 journal 本地状态。prepare → lease → durable start →
  校验 → execute → receipt/uncertain；未知/错误/异常路径在 lease 释放前 interrupt。
  所有关键提交失败可见，已开始动作不自动重发，输入正文与平台回显不进入 journal。
- Loop 行为：缺失 Dispatcher 零输入；Unknown/Dispatched/Rejected+side-effect 未证实
  时不产生清除 pin 的 VerificationResult；取消或 deadline 后的迟到结果不进入 Completed。
  新路径由 journal start/receipt 表达动作，停止发会重复计入 recent_actions 的旧
  ActionDispatched；旧事件仍可读回。
- 额外接线缺陷：原 checkpoint reducer 只识别 flat JSON，实际 LoopSettled 的
  task_id/task_epoch/detail envelope 被误判 malformed。现同时接受两种形态，
  验证嵌套身份与 epoch，旧 epoch 不覆盖新状态。加入身份错误、负数/旧 epoch、
  非对象 detail 的负向矩阵；实际 Loop Failed 终态与未决 pin 同时可被恢复读出。
- 参考宿主：新增 `examples/journaled_action_consumer.cpp`（FileEventStore + Simulator
  + 外部 Executor future + 重开恢复）；与 Working Context 样板共享限定 Simulator
  的资源预留 helper。旧样板与测试已迁移，MemoryEventStore 明确仅作进程内替身。
- 构建环境：Linux x86_64，GCC 13.3.0、CMake 3.28.3、clang-format 18.1.8，
  Debug / `MIRA_BUILD_TESTS=ON` / warnings-as-errors，无真实模型凭据。
  `cmake --preset debug`、`cmake --build build/debug -j 4` 最终退出码 0。
- 首轮全量回归：`ctest --test-dir build/debug --output-on-failure -j 4`，120/122
  通过；Working Context 示例因未迁移必需派发器停在握手点超时，集成审计仍检查旧事件
  而失败。补齐示例 lease/dispatcher、审计改为检查 durable start 后，第二次全量
  **122/122 通过**（37.63 秒，退出码 0）。没有移除测试或放宽超时。
- 最后补充：在未知/仍在派发和执行错误/异常分支，在 lease 释放前 interrupt，并覆盖
  interrupt 失败的结果可见性；`cmake --build build/debug -j 4` 退出码 0，随后
  `ctest --test-dir build/debug --output-on-failure -j 4` 再次 **122/122 通过**
  （37.64 秒，退出码 0）。日志 `/tmp/mira-dispatch-release-ctest.log` 仅为本机临时
  诊断附件，复现以上命令即可，不依赖附件长期保留。
- 最终质量：`cmake --build build/debug --target architecture-check docs-check platform-boundary-check format-check -j 4`
  退出码 0（286 个 C++ 文件格式检查）；对 examples/tests 下不在该 target glob 内的
  新增/修改 helper 头单独执行 `clang-format --dry-run --Werror` 通过。
  `python3 tools/check_docs.py .` 与 `git diff --check` 均退出码 0。
- 文档同步：API 索引与手册、README 中英版、术语表、崩溃一致性设计、总计划。
- 剩余与补跑：02 保持未完成等待 03 的完整退出矩阵。通用 ActionJournal 的恢复分页、
  任意 intent/pipe 字段与缓存身份绑定、强杀窗口、真实 Adapter Takeover/输入释放、
  Windows/Android 和 sanitizer 本轮尚未验证；负责人 Mira Maintainers。通用 journal
  补实现和矩阵后复验，平台项具备 CI/设备后补跑。可写工具/Workflow/Controller 的
  共用日志边界仍需后续实施，不把本批离散输入证据推广成平台或实时性保证。

### 2026-11-05：第三轮通用 Journal 与恢复边界（03 部分证据）

- 工作树：`fix/runtime-wiring-foundations`，基线 `b81df19` 加三轮累计变更；既有
  M29/M31/M33 与 superagent 文档未纳入本轮范围。
- 依据：[DEC-076](../decisions/DEC-076-journal-identity-and-bounded-recovery.md)。
  ActionId 的缓存绑定完整 runtime/session/task、epoch、摘要和 target；所有公开
  操作先校验身份，冲突不写存储。恢复仅消费四类 journal 事件，持久化冲突、无 start
  的结果、dispatch 后重新 prepare 明确 DataLoss；查询不重新授权派发。
- pipe 共享解析：新增 storage 内部助手，journal/checkpoint 对 ID、digest、uint64
  溢出、字段界限和控制字符使用同一规则，保留尾部 `|` 文本。合法 ActionPrepared
  仅投影身份/epoch，不误记 malformed，也不设置待验证标志。
- 恢复边界：可配置 Journal page/scan 上限，严格校验跨页 sequence 与 session；
  RecoveryPlanner 同样计入其他任务事件并检查末页，拒绝无进展/越界/错误身份。
  回执绑定本次 event ID 与有效 sequence；存储异常以固定摘要 Internal 返回。
- 架构预算：共享解析后 `checkpoint_store.cpp` 为 1169 行，低于现有 1200 行预算；
  经核对人工移除该文件一条已失效的 baseline 记录。未改变 policy，现存基线从
  17 条降到 16 条，architecture-check 无新违规、无 stale 条目。
- 突然退出矩阵：CTest 显式管理 writer/verifier 两个进程，writer 使用 `_Exit`
  跳过 C++ 析构；九个点为 before/after prepare、before/after start、after execute、
  before/after receipt、after uncertain 与 terminal。第二进程检查磁盘上的模拟输入
  次数与两次恢复结果，未执行为 0、已执行为 1，恢复不增加输入，终态不复活。
- 早期失败记录：初次编译被 `-Wsign-conversion` 捕获字符有符号转换，已改为显式转换。
  crash probe 初次使用带连字符的 UUID 字符串，而 Mira ID 需要 32 位十六进制，
  因此启动失败；已改用 constexpr Id128 字节构造，未放宽测试或协议。
- 环境：Linux x86_64、GCC 13.3.0、CMake 3.28.3、clang-format 18.1.8；Debug、
  warnings-as-errors、离线 fixture。`cmake --preset debug` 与
  `cmake --build build/debug -j 4` 最终退出码 0。
- Debug 最终回归：`ctest --test-dir build/debug --output-on-failure -j 4`，
  **123/123 通过**（37.64 秒，退出码 0；较上轮新增一个包含九窗口的 crash matrix）。
- Sanitizer：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`；
  `cmake --build build/asan --target mira_m1_action_journal_test mira_m1_journal_crash_probe mira_m3_action_dispatch_test mira_m4_checkpoint_test mira_m4_recovery_test -j 4`；
  `ctest --test-dir build/asan -R '^mira_(m1_(action_journal_test|journal_crash_matrix)|m4_(checkpoint|recovery)_test|m3_action_dispatch_test)$' --output-on-failure`。
  最终 **5/5 通过**，退出码 0，无 ASAN/UBSAN 报告；这是定向验证，不是 sanitizer 全量回归。
- 质量：`cmake --build build/debug --target architecture-check docs-check platform-boundary-check format-check -j 4`
  退出码 0（287 个 C++ 文件）；内部 helper 头单独执行
  `clang-format --dry-run --Werror src/storage/action_journal_internal.hpp` 通过；
  `python3 tools/check_docs.py .`、`git diff --check` 均退出码 0。
- 同步：DEC-076、核心/事件恢复 API、输入派发与崩溃一致性设计、总计划。本机临时
  日志为 `/tmp/mira-journal-final-test.log`、`/tmp/mira-journal-sanitize-final-test.log`、
  `/tmp/mira-journal-final-quality.log`；复现以以上命令为准，不依赖临时日志留存。
- 未执行与补跑：02/03 继续开放。`_Exit` 不等于 OS SIGKILL/TerminateProcess 或断电；
  真实 Adapter Takeover/输入释放、Windows/Android、TSAN 和其他崩溃/损坏窗口尚未
  取证，负责人 Mira Maintainers。具备对应平台 CI/设备与可控故障环境后补跑；
  TSAN 在其构建环境跑同步缓存与宿主并发矩阵。Workflow/可写工具与 Controller 的
  共用 journal 接线仍单独推进，本轮不替代这些门禁。

### 2026-11-05：复用治理轮（09 / 10；11 首批局部清理）

- 工作树：继续 `fix/runtime-wiring-foundations`，保留前几轮及用户已有文档变更。
  本轮为用户明确要求的复用治理，独立推进 09/10，不把 02/03 的未执行门禁改为完成。
- 依据：DEC-077 冻结共享向量数值规则，DEC-078 冻结共享 SQLite 打开与事务编排。
  没有新增模块依赖、外部头族、policy 例外或 Executor 注册面。
- 09：Core 公共 `embedding_math.hpp` 与 `src/support/embedding_math.cpp` 替换两份
  cosine。nullopt 表示空/维度/非有限/零范数；有效负相关不再与错误混为 sentinel，
  检索仍采用非负门槛。SQLite blob 解码到每次查询复用的有界对齐暂存，避免假设
  blob 的 float 对齐。Context 零向量原先以 0 分参与召回的偏差已修复。
- 09 验收：数值契约覆盖同向/反向/正交、无效输入、极大/极小 float；Context 与
  SQLite 两个实际调用方均证明零向量不入选、关键词回退有效。既有排序、ACL、
  预算与检索评估套件随全量回归通过。公共契约四件套为头文件、API 手册可编译片段/
  不变量与 `tests/contract/embedding_math_test.cpp`；无异步或隐藏状态。
- 10：storage 私有 `sqlite_store_open` 复用连接、schema 判定、严格版本解析、
  诊断与事务编排；两 store 保留 DDL、迁移内容、策略错误域和 worker 构造。
  新库 DDL/kind/version 及旧库迁移/版本标记各自在同一事务完成；只读不迁移，
  非法/溢出版本不再截断或接受尾随文本。
- 10 验收：`m4_store_open_test` 通过两个公开 factory 覆盖新库/重开、十一种坏版本、
  未来/缺失版本、只读、错误 kind、未知/损坏库与忙锁，失败后修复可重开且元数据保留。
  Memory 1.0→1.1 在版本更新处用 SQLite trigger 注入失败，验证新增列与版本均回滚；
  去掉故障后迁移成功。原有 SQLite 两 store 回归覆盖正常数据、迁移、关闭和拒绝路径。
- 11 首批：移除构造后立即丢弃的 `stash_headers` lambda，没有变更真实响应头收集路径。
  Runtime 转移校验、workflow 校验助手/事件封装、状态名称、大小写/marker、TLS 与
  公共孤儿接口的兼容性审计仍未完成，11 保持未勾选。
- 环境：Linux x86_64、GCC 13.3.0、CMake 3.28.3、clang-format 18.1.8；Debug、
  warnings-as-errors；离线数据/SQLite fixture，无真实模型或外部服务依赖。
- 构建：`cmake --preset debug`；`cmake --build build/debug -j 4`，退出码 0。
  `ctest --test-dir build/debug --output-on-failure -j 4` **125/125 通过**
  （37.64 秒，退出码 0）。新增数值契约与 store-open 两个 target；随后数值用例归入
  contract 目录，内容不变，重建后单独 `ctest --test-dir build/debug -R '^mira_embedding_math_test$' --output-on-failure`
  **1/1 通过**。
- Sanitizer：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`；
  `cmake --build build/asan --target mira_embedding_math_test mira_m4_store_open_test mira_m4_sqlite_checkpoint_test mira_m4_sqlite_memory_test mira_m4_memory_retrieval_test mira_m17_context_retrieval_test -j 4`；
  `ctest --test-dir build/asan -R '^mira_(embedding_math_test|m4_(store_open|sqlite_checkpoint|sqlite_memory|memory_retrieval)_test|m17_context_retrieval_test)$' --output-on-failure`
  **6/6 通过**（1.55 秒），退出码 0，无 ASAN/UBSAN 报告。契约用例迁移目录后
  再次重建 ASAN target，单项 **1/1 通过**；未把定向验证写成 sanitizer 全量结果。
- 质量：`cmake --build build/debug --target architecture-check docs-check platform-boundary-check format-check -j 4`
  退出码 0（292 个 C++ 文件）；私有头单独执行
  `clang-format --dry-run --Werror src/storage/sqlite_store_open.hpp` 通过；
  `python3 tools/check_docs.py .` 与 `git diff --check` 通过。baseline 仍为 16 条，
  无新违规或 stale 条目。
- 同步：两条 DEC、Context/Memory 设计、Context 与事件恢复 API/索引、总计划与本记录。
  本机临时诊断为 `/tmp/mira-reuse-full-test.log`、`/tmp/mira-reuse-sanitize-test.log`、
  `/tmp/mira-reuse-quality.log`，复现以命令为准。
- 范围与补跑：09/10 按 Linux 离线范围验收；其他平台、TSAN、性能与真实召回质量
  未验证，负责人 Mira Maintainers。具备对应 CI/工具链后补跨平台/TSAN；性能与真实
  数据质量有明确 profile 后另验，不从复用或 fixture 通过推导收益。
  RW-G5/RW-G6 与维护轮整体仍未关闭，11 的其他治理和原 02/03 平台门禁保持开放。

### 2026-11-05：第五轮，Workflow 校验与 TaskState 名称复用

- 归属：`MNT-202611-11` 的两个独立局部治理点；依据
  [DEC-079](../decisions/DEC-079-workflow-validation-and-task-state-names.md)。
  基线仍为 `b81df19`，分支 `fix/runtime-wiring-foundations` 的累计未提交工作树；
  本记录只验收本轮增量，不改写前轮证据。
- 实现：workflow 私有 `workflow_decode.hpp` 共用键集合扫描及非空有界字符串读取；
  IR、App Model、Learning 保留各自错误工厂和 ErrorCode/domain_code/消息前缀。
  测试固定未知字段优先于缺失字段、非对象、必选/可选字段、空字符串、字节边界与
  类型错误；不统一各 schema 的必要差异，不改变版本或 canonical JSON/digest。
- 名称：Core 的单一不可变表提供 `task_state_name` / `task_state_from_name`；
  Interaction、Checkpoint 解析、序列化及 digest 使用该表。15 个既有拼写保持不变；
  非法枚举输出 Unknown，未知名称严格拒绝，不再伪装为 Idle。Checkpoint 对未校验
  内存对象的旧 Failed 序列化回退保持原样，合法终态 JSON 往返通过。
  新增契约测试穷举全部 256 个 uint8 值，覆盖大小写、空白、嵌入 NUL；Checkpoint
  测试确认所有合法名称可折叠、非法名称不产生终态。安装消费者实际调用两公开函数。
- 所有权：新增代码仅同步读取调用方输入和不可变常量，无新任务、worker、可变全局
  或 Executor 生命周期；未新增跨模块私有依赖，未修改策略、baseline 或第三方代码。
- 首次验证及修正：新增 Workflow 字符串 fixture 缺少必选 step，定向测试首次 1/2；
  补齐合法 ToolCall 步骤后 2/2。接入 Checkpoint 序列化时首次全量构建发现 JsonValue
  不隐式接受 string_view，改为边界显式构造 string 后全量构建成功；未通过的尝试
  不作为行为验收证据。
- Linux Debug：`cmake --preset debug`、`cmake --build build/debug -j 4` 成功；
  `ctest --test-dir build/debug --output-on-failure -j 4` **127/127**，37.63 s。
  包含 M8/M12/M13 解码回归、M4 checkpoint、Interaction、公开头及安装消费者。
- ASAN+UBSAN：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`，确认两个开关均 ON；
  `cmake --build build/asan --target mira_task_state_names_test mira_workflow_decode_test mira_m4_checkpoint_test mira_m8_ir_test mira_m12_contract_test mira_m13_contract_test -j 2`
  成功；
  `ctest --test-dir build/asan -R '^mira_(task_state_names|workflow_decode|m4_checkpoint|m8_ir|m12_contract|m13_contract)_test$' --output-on-failure`
  **6/6**，0.28 s，无 sanitizer 报告。
- 质量检查：`python3 tools/check_architecture.py .`、`python3 tools/check_docs.py .`、
  `python3 tools/check_platform_boundary.py .`、
  `cmake -DROOT_DIR=/home/linductor/mira -P cmake/CheckFormat.cmake`（295 个 C++ 文件）、
  `clang-format --dry-run --Werror src/workflow/workflow_decode.hpp` 与
  `git diff --check` 均通过；架构仍为 16 条存量基线，无新增或 stale 条目。
  API 示例、状态机/Workflow 设计、DEC 与总计划同步更新。
- 临时诊断：`/tmp/mira-governance-full-test.log`、
  `/tmp/mira-governance-asan-test.log`；复现以以上命令为准，不依赖临时文件留存。
- 剩余：11 仍为 In Progress，Runtime 全部状态写入/CAS 边界、事件封装、Error 工厂、
  lowercase/marker、TLS 与公开孤儿契约审计尚未验收。02/03 平台门禁及其他工作项
  保持开放。Windows/Android/TSAN 本轮未执行，负责人 Mira Maintainers；具备对应
  工具链/CI 后补跑相关构建与契约集，本轮结果仅证明 Linux fixture 兼容性。

### 2026-11-05：第六轮，Runtime 状态写入与关闭准入

- 归属：`MNT-202611-11` 的 Runtime 转移校验子项，依据
  [DEC-080](../decisions/DEC-080-runtime-lifecycle-transition-boundary.md)、DEC-001 与
  核心状态机设计 §6.1。分支仍为 `fix/runtime-wiring-foundations`，HEAD `b81df19`，
  证据对应累计未提交工作树；本轮不修改前轮验收结果。
- 复核与修正：原实现全部 store/CAS 绕过公共校验，未初始化与初始化失败对象最终
  直接写 Stopped；公共表遗漏设计已允许的 Failed→Stopping。现在所有状态写入由
  私有状态单元执行受校验 CAS，补齐失败后的清理出边，禁止 Stopping/Stopped 复活。
  未初始化或初始化失败的关闭不提交任务，不触发 Executor 懒初始化。
- 所有权与准入：初始化、命令准入及 future 登记、停止准入共享 admission mutex；
  命令登记使用独立 commands mutex，不与平台回调持有的 records mutex 耦合。
  外部 finalizer 单独串行化，等待时释放准入锁。begin_operation 同样在停止后拒绝。
  thread_local 只标识当前 Runtime 控制回调的动态范围，用于在自等待前拒绝收尾；
  不创建线程、队列、timer、调度器或全局 Executor。
- 关闭失败：容量拒绝仍立即返回 ResourceExhausted。finalizer 消费已登记的 kairo
  submission future 后至多补交一次 shutdown barrier，补交结果写入报告诊断；
  late submission 异常补齐未结算 receipt/outcome。平台 interrupt 失败/异常逐会话
  隔离，其他会话仍尝试释放；任务取消后 Runtime 保留 Failed，报告不因 Executor
  join 成功而变 clean。重复最终收尾返回原报告，不自动重开实例。
- 测试：新增 `tests/contract/runtime_lifecycle_test.cpp`，固定 256×256 Runtime 枚举
  转换组合、未初始化/三种零容量配置/重复初始化/重复关闭、停止后的 operation 拒绝。
  阻塞一个串行 interrupt 并排入七个命令，确定性占满八个 in-flight slot，证明关闭
  拒绝后仍能收敛剩余任务、消费全部命令并报告补交。覆盖 Adapter 返回 PlatformError、
  DeadlineExceeded 与抛异常（异常原文不进入报告），以及控制回调内 finish_shutdown
  不自等待。外部 kairo 执行两项有限调用，交错初始化与停止 24 轮，由测试主线程
  最终关闭 Runtime；这不是 TSAN 或真实平台取消证明。
- Linux Debug：`cmake --preset debug`、`cmake --build build/debug -j 4` 成功；
  `ctest --test-dir build/debug --output-on-failure -j 4` **128/128**，37.67 s。
  覆盖新契约、M1、Human Takeover/Interaction、Workflow、安装消费者与原有回归。
  首轮定向 `mira_runtime_lifecycle_test` / `mira_m1_core_test` 为 2/2；加入
  DeadlineExceeded 返回用例后由全量与最终 sanitizer 构建重新验证。
- ASAN+UBSAN：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`；
  `cmake --build build/asan --target mira_runtime_lifecycle_test mira_m1_core_test mira_m28_lifecycle_test -j 2`
  成功；
  `ctest --test-dir build/asan -R '^mira_(runtime_lifecycle|m1_core|m28_lifecycle)_test$' --output-on-failure`
  **3/3**，0.72 s；
  `ctest --test-dir build/asan -R '^mira_runtime_lifecycle_test$' --repeat until-fail:20 --output-on-failure`
  **连续 20 次通过**，5.05 s，无 sanitizer 报告。
- 质量：`python3 tools/check_architecture.py .`、`python3 tools/check_docs.py .`、
  `python3 tools/check_platform_boundary.py .`、
  `cmake -DROOT_DIR=/home/linductor/mira -P cmake/CheckFormat.cmake`（296 个 C++ 文件）
  与 `git diff --check` 通过。手动核查 Runtime 全部写入只有状态单元的受校验 CAS；
  架构基线仍为 16 条，无新违规或 stale 条目，未修改策略、baseline 与第三方代码。
- 同步：Runtime 头文件注释、API 的失败收尾示例、核心状态机设计、DEC 与总计划。
  临时诊断在 `/tmp/mira-lifecycle-full-test.log`、`/tmp/mira-lifecycle-asan-test.log`、
  `/tmp/mira-lifecycle-asan-repeat.log`；复现以命令为准，不依赖临时文件留存。
- 范围与剩余：Runtime 校验子项按 Linux fixture 验收；11 的事件封装、Error 工厂、
  lowercase/marker、TLS 与公共孤儿契约审计仍开放，维护轮和 RW-G5/RW-G6 不关闭。
  本轮验证选择 Linux Debug/ASAN+UBSAN，Windows/Android 与 TSAN 专项未执行，
  不推导跨平台或无数据竞争保证。负责人 Mira Maintainers；在对应平台/TSAN CI
  执行同一契约集与生命周期回归后补充证据。平台 interrupt 必须有界、幂等且可解除
  阻塞，本次不承诺强制终止外部调用。

### 2026-11-05：第七轮，Context/Memory marker 与 Error 复用

- 归属：`MNT-202611-11` 的 Context/Memory lowercase/marker 和六处简单 Error
  工厂子项，依据 [DEC-081](../decisions/DEC-081-context-marker-and-error-reuse.md)。
  分支 `fix/runtime-wiring-foundations`，HEAD `b81df19`，对应累计未提交工作树；
  本轮不修改历史验证记录、其他里程碑状态或第三方代码。
- 复核：MemoryConsolidator 只折叠正文，直接用配置 marker 搜索；自定义大写 marker
  漏拦，空 marker 则全匹配。Context 已折叠双方、拒绝空配置。四类公共配置还各自
  复制默认词表，Memory 的候选 key 另复制 ASCII 小写函数。
- 实现：`content_markers.hpp` 两个公共工厂为四类配置生成独立 vector，固定既有
  词条、顺序和字段类型；私有 `context_text.hpp` 共用 ASCII 小写/子串原语，Memory
  不再依赖含模型 request/schema 组装的较大 Context helper。Memory 候选 key 的
  ASCII 归一化保持，非 ASCII 与 NUL 字节不改写；claim key 规则保持独立。
  空 marker 返回 memory 域 InvalidMutation，在模型/存储访问前失败；空 vector
  仍合法。默认配置保持既有行为，自定义大写 marker 与空 marker 的修正已明示于 API。
- 准入边界：Memory 禁止内容仍检查所有来源，注入仅检查 model_assisted；Context
  consolidation/curation 保留各自逐语句校验、来源和置信度门槛。未改变审批、验证
  等级、scope/ACL、schema 或存储格式，不将词表当作完整敏感内容识别保证。
- 错误工厂：私有 `context_error.hpp` 只共用 consolidation、curation、Working
  Context、fork、auto trigger、promotion 的简单 ErrorCode 字段构造；保留六个
  调用方绑定及既有 domain、domain_code=0、retryable=false、消息和空可选字段。
  make_context_error / make_memory_error 的领域码映射及其他模块工厂未合并。
- 契约与回归：新增 `tests/contract/context_policy_test.cpp` 固定默认词表内容/顺序、
  四类默认配置一致性、返回值独立性、空 marker/空 vector 边界，以及六个错误构造
  面的公共错误字段。Probe 证明无效 Memory policy 不调用模型、query 或 apply。
  M4 SQLite 回归覆盖大小写自定义 marker、中文前缀、NUL 后匹配、拒绝后无写入、
  注入的 model_assisted 边界、非 ASCII 候选 key 与 duplicate-noop；M17 验证相同
  marker 在索引侧拒绝且不入库。原审批及上下文 model/rerank 回归继续通过。
- 公共契约四件套：新增头文件、Context/Memory API 的可编译配置消费示例、匹配与
  所有权不变量、上述 contract 测试齐备。公开头隔离登记该头；安装消费者实际
  调用两个工厂，验证 Core 链接与默认值。纯同步工具没有新任务、线程、timer、可变
  全局 policy、Executor owner 或跨模块私有依赖。
- Linux Debug：`cmake --preset debug`、`cmake --build build/debug -j 4` 成功；
  初次定向 `mira_context_policy_test` / `mira_m4_consolidation_test` /
  `mira_m17_context_retrieval_test` 为 **3/3**，0.11 s；
  `ctest --test-dir build/debug --output-on-failure -j 4` **129/129**，37.65 s。
- ASAN+UBSAN：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`，确认两个开关均 ON；
  `cmake --build build/asan --target mira_context_policy_test mira_m4_consolidation_test mira_m17_context_retrieval_test mira_m19_context_consolidation_test mira_m21_context_curator_test mira_m22_auto_trigger_test mira_m23_promotion_test mira_m24_fork_merge_test -j 2`
  成功；
  `ctest --test-dir build/asan -R '^mira_(context_policy|m4_consolidation|m17_context_retrieval|m19_context_consolidation|m21_context_curator|m22_auto_trigger|m23_promotion|m24_fork_merge)_test$' --output-on-failure`
  **8/8**，2.04 s，无 sanitizer 报告。
- 质量：`python3 tools/check_architecture.py .`、`python3 tools/check_docs.py .`、
  `python3 tools/check_platform_boundary.py .`、
  `cmake -DROOT_DIR=/home/linductor/mira -P cmake/CheckFormat.cmake`（299 个 C++ 文件）、
  `clang-format --dry-run --Werror src/context/context_text.hpp src/context/context_error.hpp src/context/context_support.hpp`
  与 `git diff --check` 通过。基线仍为 16 条，无新违规或 stale 条目；未修改策略
  或 baseline。API/索引、设计版本、DEC、总计划和本记录同步。
- 临时证据：`/tmp/mira-markers-full-test.log`、`/tmp/mira-markers-asan-test.log`，
  复现以命令为准，不依赖临时日志留存。
- 剩余与限制：本批按 Linux 离线 fixture 验收，11 整体仍为 In Progress；事件封装、
  TLS 公共段、其他模块错误映射/大小写语义与公开孤儿契约审计保持开放，不将域名、
  协议字段或安全检测的必要差异自动合并。02/03 平台门禁及维护轮 RW-G5/RW-G6
  未关闭。Windows/Android/TSAN 本轮未执行，本轮选择 Linux Debug/ASAN+UBSAN；
  负责人 Mira Maintainers，在对应 CI 执行头文件/消费者与同一契约回归后补证据。

### 2026-11-05：第八轮，事件/TLS 复用与公开契约审计收尾

- 归属：`MNT-202611-11` 剩余范围；依据
  [DEC-082](../decisions/DEC-082-local-event-tls-reuse-and-public-retention.md) 与
  [公开契约保留审计](../compatibility/2026-11-public-contract-audit.md)。环境为 Linux
  x86_64、GNU C++ 13.3.0；分支 `fix/runtime-wiring-foundations`，HEAD `b81df19`，
  对应累计未提交工作树。前轮失败、限制与平台门禁保留。
- Workflow：20 个 writer 只共享同文件私有 `encode_payload`，事件名、schema、字段
  顺序和分类仍由各 writer 显式提供。两个 Critical 和其余 State 不使用默认分类。
  重构前以固定输入从已构建编码器捕获 `tests/fixtures/workflow-event-wire-v1.json`，
  SHA-256 为 `1830705fd61ad6a2106db981ba2ef50d26db4a76eed9f89594e6c0c1a9a55203`。
  新契约测试逐个比较 type/classification/data 字节，包含转义及 optional 字段；
  fixture 中低层默认值编码不作为语义合法性的证明，正常测试不刷新 fixture。
  既有 M8–M14 parser/Replay 回归保持，durability 和事件 append 不变。
- TLS：net 私有 `tls_support.hpp` 共用错误工厂及 create 的初始化/verify_peer
  检查、延迟构造与 setup 失败尾部。原始错误 domain、code、retryable、消息及
  检查顺序保持。OpenSSL/Mbed TLS 的信任加载、证书/主机名、SNI/IP、握手、socket/BIO
  与各自错误映射保留；新增工厂准入用例在同一测试源中分别跑两个后端，另跑 pinned
  Mbed TLS portable 测试。没有新 worker、timer、异步任务或 Executor owner。
- 错误工厂：tool 私有 `tool_module_error.hpp` 收敛 module 与 registry 的相同构造，
  两个公共错误入口验证 domain/code/domain_code/retryable/消息和空可选字段。
  其他领域工厂保留不同的领域码/上下文策略，不做全库通用错误框架迁移。
- 公开契约：报告点名的 parser、checkpoint/recovery 诊断名、digest、坐标空间及
  预留枚举逐项审计后保留，不按仓库引用数删除或制造 src 生产点。新增发布拒绝
  reader 的正反用例、诊断名称与枚举编号/全部坐标原始字节契约测试；安装消费者
  实际调用 reader 和两个诊断名。坐标词汇为八个具体空间加 Unknown，未知值保留
  UnsupportedVersion。度量、retention、平台遮蔽、raw capture 继续沿用 08/M31/12
  的原归属，不以本次审计冒充接线完成。
- 其他重复处置：Context 与 Memory 评分/选择保留 session/scope ACL、kind/claim/
  verification/expiry 和裁剪策略；已共享数值边界，不扩成跨层公共排序模板。
  协议头、URL/host、安全与 Context 的大小写转换按各自语义保留，决定与源文件
  链接见审计表。这些保留属于有依据的处置，不再作为 11 的待抽取清单。
- 首次验证修正：定向初次 **4/5**，新增保留契约测试误将未知坐标错误预期写为
  InvalidArgument；源码原行为为 UnsupportedVersion。仅修正测试预期，重跑 **5/5**，
  4.08 s。Workflow 字节及三组 TLS 初次即通过；未将失败尝试记为验收成功。
- Linux Debug：`cmake --preset debug`、`cmake --build build/debug -j 4` 成功；
  `ctest --test-dir build/debug --output-on-failure -j 4` **131/131**，37.67 s，
  含公开头、安装消费者、TLS loopback、Workflow 与 Module 全库回归。
- ASAN+UBSAN：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`，两个开关均 ON；
  `cmake --build build/asan --target mira_workflow_event_encoding_test mira_public_contract_retention_test mira_m3_tls_test mira_m3_mbedtls_test mira_m3_mbedtls_portable_test mira_m7_tool_module_test mira_m7_tool_module_registry_test mira_m8_events_test -j 2`
  成功；
  `ctest --test-dir build/asan -R '^mira_(workflow_event_encoding|public_contract_retention|m3_tls|m3_mbedtls|m3_mbedtls_portable|m7_tool_module|m7_tool_module_registry|m8_events)_test$' --output-on-failure`
  **8/8**，3.62 s，无 sanitizer 报告。
- 质量：`python3 tools/check_architecture.py .`、`python3 tools/check_docs.py .`、
  `python3 tools/check_platform_boundary.py .`、
  `cmake -DROOT_DIR=/home/linductor/mira -P cmake/CheckFormat.cmake`（301 个 C++ 文件）、
  `clang-format --dry-run --Werror adapters/net/tls_support.hpp src/tool/tool_module_error.hpp tests/support/workflow_event_samples.hpp`
  与 `git diff --check` 通过。基线保持 16 条，无新违规或 stale 条目；没有修改架构
  policy、baseline 或第三方代码。DEC、兼容性审计、模型/Workflow/恢复 API、模型
  协议设计、总计划与本记录同步。
- 本机临时诊断为 `/tmp/mira-audit-target-test.log`（首次失败）、
  `/tmp/mira-audit-target-final-test.log`、`/tmp/mira-audit-full-test.log`、
  `/tmp/mira-audit-asan-test.log`。测试复现以命令及仓库 fixture 为准，不依赖临时文件留存。
- 验收：结合第四至七轮，11 的 Linux 离线复用及报告点名公开契约审计范围已完成，
  复选框勾选。维护轮仍 In Progress，02/03 的平台证据、04 事实、06 样板、07 价格、
  08 度量和 12 总收口继续开放，RW-G1–G6 的整体退出条件不随本项自动关闭。
  Windows/Android/TSAN 本轮未执行，本轮选择 Linux Debug/ASAN+UBSAN；负责人 Mira
  Maintainers，在对应 CI 补构建、TLS/消费者和生命周期契约，不从既有其他平台历史
  证据推导本工作树已验证，也不从本次静态审计宣称已扫描所有下游 ABI。

### 2026-11-06：第九轮，显式事实生产与既有记忆管线

- 归属：`MNT-202611-04`；依据
  [DEC-083](../decisions/DEC-083-explicit-verified-fact-publication.md) 与
  [迁移说明](../compatibility/verified-fact-v1-and-checkpoint-v2.md)。环境为 Linux
  x86_64、GNU C++ 13.3.0，分支 `fix/runtime-wiring-foundations`，HEAD `b81df19`，
  对应累计未提交工作树；保留所有前轮失败与未执行平台记录。
- 公开入口：新增 Core `VerifiedFactPublisher`，由宿主显式提供有限期 claim、稳定
  publication EventId、ObservationRecorded 引用/载荷摘要与可信语义 lease。同步
  协议检查完整身份、两级 epoch、来源 observed_at、有效期、取消/deadline、扫描
  预算及耐久回执；异常转为固定安全错误。lease 覆盖准入到 append，宿主取消、
  暂停、接管与终态必须共用同一 owner 门；不把哈希校验当作语义证明。
- 事件与投影：新 TaskFactVerified v1 使用嵌套 claim、精确 scope（含 tenant）、
  verifier、证据和有效区间；畸形/未知 schema 拒绝且不回退 legacy。Checkpoint
  schema 升为 **2.0**，旧 1.x 事实继续读取，期限字段只允许用于新版投影；增量和
  base 重建清理无效期事实。旧 reader 拒绝新版，C++ 消费者须重编译，不能伪装
  schema 降级。私有 fact wire codec 拆分使文件保持预算内，没有刷新 baseline。
- 固化：新版事实只在精确 scope 且当前有效时进入既有 MemoryConsolidator；保留
  marker、kind、冲突、人工审批和 ACL。两级 provenance、有效期和稳定验证时间
  传给 MemoryRecord；同值 duplicate-noop 不刷新 TTL，较旧/同时间冲突不覆盖新
  事实，无期限 legacy 不反向覆盖有期限记录。User/Preference 仍待人工批准。
  HumanCorrection 与模型辅助策略不变；M30 自动候选/主体映射/跨会话选择没有开启。
- 可执行宿主：新增 `mira_verified_fact_host_consumer_test`，以 simulator 实际
  screen width 为命题，由原生 lease 验证后写 FileEventStore；重开并用同 ID 重试，
  再由 CheckpointBuilder、既有 consolidation、SQLite Memory/query 消费。实际
  Human Takeover 和 cancel 后的新 ID 发布被拒绝，Runtime 收敛后由外部 owner
  shutdown Executor。有限任务使用 kairo 并消费 future，没有新并发设施或隐藏 owner。
- 契约矩阵：新增 `mira_verified_fact_test` 覆盖空/错误语义、内容 marker、无证据/
  错误摘要、跨身份/epoch、取消/撤回/deadline、分页预算、提交拒绝、弱/错回执、
  存储/authority 异常、幂等冲突、终态/旧 epoch、期限 codec/base 清理、租户隔离、
  过期、审批与 legacy 兼容。另断言 Satisfied、Invalid、模型 done、未知/畸形
  schema 不产出 checkpoint 事实或 Memory candidate；恢复后的信封时间不参与
  新协议证明时间校验。公开头隔离与安装 consumer 验证新入口和 schema 版本。
- 失败与修正：首次定向 **3/5**（0.18 s），暴露 checkpoint_from_json 未调用新增
  有效区间校验、样板重开重试失败；补齐 codec 验证后 **1/2**（0.06 s）。样板
  失败定位为 FileEventStore 恢复重建 timestamp，改用持久化 observed_at 后 **2/2**
  （0.11 s）。中间一次 fixture 编译误用 MemoryMutationResult.version，已改为
  new_version。未把这些尝试计为通过；随后补齐冲突、接管与负向消费断言再取最终证据。
- 新发现 **BUG-20261106-001**：现有文件日志不持久化原信封 timestamp，recover
  通过 MemoryEventStore::append 赋恢复时间。新协议用 payload 的 observed_at/
  valid_from/valid_until，Memory recorded_at 沿用持久化 valid_from；这只保证本
  路径重放稳定。全局时间恢复仍开放，已在 §5 与兼容性说明登记，由 12 承接独立
  格式决策、旧日志兼容及重开/故障矩阵，负责人 Mira Maintainers。
- Linux Debug：`cmake --preset debug`、`cmake --build build/debug -j 4` 成功；
  最后一次 `ctest --test-dir build/debug --output-on-failure -j 4` **133/133**，
  37.65 s，含公开头、安装消费者及原 M4/Context/Workflow/Runtime 回归。
- ASAN+UBSAN：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`，两个开关均 ON；
  `cmake --build build/asan --target mira_verified_fact_test mira_verified_fact_host_consumer mira_m4_contracts_test mira_m4_checkpoint_test mira_m4_consolidation_test mira_m4_recovery_test mira_m4_sqlite_checkpoint_test mira_m4_sqlite_memory_test -j 2`
  成功（最后追加负向断言后再构建 mira_verified_fact_test）；
  `ctest --test-dir build/asan -R '^mira_(verified_fact|verified_fact_host_consumer|m4_contracts|m4_checkpoint|m4_consolidation|m4_recovery|m4_sqlite_checkpoint|m4_sqlite_memory)_test$' --output-on-failure`
  **8/8**，2.18 s，无 sanitizer 报告。
- 质量：架构、文档、平台边界与格式检查通过，格式覆盖 305 个 C++ 文件；另检查
  `src/support/ascii_text.hpp`、`src/context/context_text.hpp`、
  `src/storage/checkpoint_fact_wire.hpp` 的 clang-format。架构基线仍为 16 条，无
  新违规或 stale 条目，本轮未修改 policy、baseline 或第三方代码。DEC、设计、
  API/索引、README、中英文迁移说明入口、总计划和本记录同步。
- 本机临时日志：`/tmp/mira-facts-target-test.log`、`/tmp/mira-facts-fix-test.log`、
  `/tmp/mira-facts-second-build.log` 保留初次失败；最终为
  `/tmp/mira-facts-acceptance-test.log`、`/tmp/mira-facts-asan-acceptance-test.log`。
  复现以仓库测试与上述命令为准，不依赖临时文件留存。
- 验收：04 按 Linux 离线显式宿主范围完成，工作项累计 **7/13**；维护轮仍为
  In Progress，02/03 平台证据、06 三个组合宿主、07 价格、08 度量与 12 总收口
  继续开放。Windows/Android/TSAN、真实模型/平台语义本轮未执行；当前仅有 Linux
  simulator fixture。负责人 Mira Maintainers，在对应 CI/设备实现语义 authority，
  补跑头文件/消费者、取消与接管/并发矩阵后记录证据；本项不外推跨平台保证，
  不关闭 M30、BUG-20261106-001 或维护轮全局门禁。

### 2026-11-06：第十轮，价格策略与预算宿主

- 归属：`MNT-202611-07`；依据
  [DEC-084](../decisions/DEC-084-model-price-reference-host.md) 与
  [价格宿主指南](../api/model-price-host.md)。环境为 Linux x86_64、GNU C++ 13.3.0，
  分支 `fix/runtime-wiring-foundations`，HEAD `b81df19` 加累计未提交工作树。
- 新增 `examples/model_price_host_consumer.cpp` 与 CTest consumer，只依赖公开
  Core/kairo。显式注入有 model/currency/entry version/effective_from/source 的
  人工 PriceTable；固定 input/output rate 与 3/4 usage 核算为 11 micro USD。
  不加载凭据、不访问真实供应商、不读取外部报价，也不产生平台副作用。
- 十二场景：known、unknown 拒绝/允许、金额估算超限、resolved model 缺价、
  Missing/Partial usage、准入撤回、发送前取消、取消后成功响应/usage 缺失、写后
  显式 AmbiguousCompletion。断言零 dispatch 拒绝、完整结算、未决预留、模糊完成
  不重试与 late admitted=false；实际消费 reservation、spent、余额及事件
  priced/settlement_quality。stdout 明确区分 unknown、partial、已核算和未发送，
  不把 unknown 的数值 0 展示为免费。
- 样板显式选择 require_known_price=true/false，库默认值保持 false；修正文档中
  “默认拒绝未知价格”和“迟到响应不结算”的错误描述。迟到成功响应仍先核销 usage，
  然后拒绝任务影响；Missing/UnknownPrice/模糊完成保留审计状态。修正 close 的
  公开注释：只关闭新 reserve，不清空余额。没有改变公开布局、事件 schema 或算法。
- 所有权：每个场景独立 task、单 profile、一次 infer，禁用重试；Gateway 串行运行，
  宿主消费 submit_auto future 后才读 ledger，外部主线程最终 shutdown(true)。
  内存账本和 MemoryEventStore 不承担生产耐久审计；没有新 Core 并发机制或隐藏
  Executor owner，kairo 使用依据仓库集成指南的 tasks/lifecycle 路径。
- 范围核查：Task scope 当前约束未核销 reservation，spent 另行累加；首选 selector
  的估价不能证明 fallback/重试/最终 resolved model 的金额上界。reserved_remainder
  也不是整个账户余额快照。这些真实边界已同步 API/设计/指南并在 §5 归属 12
  登记专项费用约束立项，不扩大 M33 自主打断/配额范围，也不宣称已交付累计账单硬限额。
- 首次构建因样板误用 TransportTrace 字段及 JSON accessor 失败；修正为公开的
  write_started/as_boolean，并核对 AmbiguousCompletion 现有错误码为
  ExecutionUncertain。随后定向 `model_price_host_consumer`、`m3_supervisor_budget`、
  `m3_gateway` **3/3**，0.07 s；直接运行样板十二场景通过。未把构建失败计为通过。
- Linux Debug：`cmake --preset debug`、`cmake --build build/debug -j 4` 成功；
  `ctest --test-dir build/debug --output-on-failure -j 4` **134/134**，37.66 s，
  含公开头、安装消费者、预算/gateway、Loop 与其余全库回归。
- ASAN+UBSAN：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`，确认两开关均 ON；
  `cmake --build build/asan --target mira_model_price_host_consumer mira_m3_supervisor_budget_test mira_m3_gateway_test -j 2` 成功；
  `ctest --test-dir build/asan -R '^mira_(model_price_host_consumer|m3_supervisor_budget|m3_gateway)_test$' --output-on-failure`
  **3/3**，0.54 s，无 sanitizer 报告。
- 质量：`python3 tools/check_architecture.py .`、`python3 tools/check_docs.py .`、
  `python3 tools/check_platform_boundary.py .`、
  `cmake -DROOT_DIR=/home/linductor/mira -P cmake/CheckFormat.cmake`（306 个 C++ 文件）
  与 `git diff --check` 通过。架构基线仍为 16 条，无新违规/stale；本轮不修改
  policy、baseline 或第三方代码。DEC、协议设计、API/索引、README 中英文入口、
  总计划和本记录同步。临时日志为 `/tmp/mira-price-build.log`（初次构建失败）、
  `/tmp/mira-price-target-test.log`、`/tmp/mira-price-full-test.log`、
  `/tmp/mira-price-asan-test.log`，复现以仓库样板和上述命令为准。
- 验收：07 按 Linux 单 profile fixture 范围完成，累计 **8/13**；维护轮仍
  In Progress。02/03 平台证据、06 组合宿主、08 度量与 12 总收口继续开放。
  Windows/Android/TSAN、真实 transport 取消/供应商账单未执行；负责人 Mira
  Maintainers，在对应 CI/设备补跑 consumer，并具备供应商安全测试账号和账单
  核对条件后验证取消阶段与计费，不用本次 fixture 结果外推跨平台或真实账单保证。

### 2026-11-06：第十一轮，调度度量与宿主结果诊断

- 归属：`MNT-202611-08`；依据
  [DEC-085](../decisions/DEC-085-runtime-scheduling-diagnostic-host.md) 与
  [调度宿主指南](../api/runtime-scheduling-host.md)。环境为 Linux x86_64、
  GNU C++ 13.3.0，分支 `fix/runtime-wiring-foundations`，HEAD `28470fe` 加本轮
  未提交工作树；原有 M29/M31/M33 与 Super-agent 文档改动保持独立。
- 覆盖边界核查：对照 kairo 公开 SchedulingMetrics、API 的调度度量/反馈章节、
  submit_on_with_handle 公开模板与相关串行接纳测试，确认计数对象是路由决策。
  当前 Runtime 串行命令不走 submit_auto 路由，容量/停止拒绝和业务异常可与零路由
  计数并存。修正公共注释中会让人误以为该快照覆盖全部队列拒绝的描述；不改字段
  布局或 Core 实现，不将应用结果面职责登记成 Executor 缺口。
- 新增 `runtime_scheduling_host_consumer.cpp` 和 CTest consumer：保留原样的 13
  项 routing 累计值，另消费入口 Result、CommandHandle outcome、ShutdownReport。
  HostView 最多保留 16 个 handle，展示本宿主显式提交/拒绝数量与当前保留窗口的
  pending/settled/failed。内部 finalizer 补交不计入宿主显式提交，不把局部窗口
  冒充全局审计或队列深度；各字段弱一致，不能用于同步或从重叠分类计算任务数。
- 场景证据：max_in_flight=4，以有界 interrupt gate 阻塞一个 close，排入
  pause→resume→pause。超限命令与 shutdown 两次 ResourceExhausted，停止准入后
  再一次 InvalidState；busy pending=4。1 ms outcome 等待超时没有终止回调。
  gate 释放后 finalizer 至多一次补交，最终 submitted=12、admitted_handles=9、
  settled=9、pending=0、任务 Cancelled、report clean=true；无物理队列已满声明。
  另一实例 interrupt 异常使 takeover/shutdown 两个 outcome Failed，report
  clean=false/Failed、pending=0。上述各阶段原始 routing 均为零，拒绝/异常仍可见。
- 采样与消费：独立 kairo Executor 提交有限采样任务并消费 future，复制有界宿主
  视图，只读 Runtime routing/state 与 outcome(0ms)，不争用记录锁；pending 轮询
  不误计为执行超时。不变样本在控制收尾后输出，日志消费者不进入控制回调。独立
  sampler 的路由接受数单独标识，不假冒 Runtime 计数。对象存活覆盖采样 future，
  外部 owner 最后 shutdown sampler；没有新周期器、worker 或调度设施。
- 首次定向 **1/2**，0.07 s：样板原先排入三次 pause，却预期全部业务成功；既有
  转换表不允许 Paused→Paused。改为 pause→resume→pause，不修改 Core 状态机，
  重建样板后 **2/2**，0.05 s。直接运行已消费五阶段输出与独立 sampler 计数；
  另以 Python subprocess 连续运行样板 **20/20**（每次 timeout=15 s）验证固定
  gate 下采样/结算口径稳定。复现循环为对
  `./build/debug/mira_runtime_scheduling_host_consumer` 进行 20 次同步 subprocess.run，
  检查每次返回码，stdout/stderr 仅在失败时诊断；没有把首次失败隐藏为通过。
- Linux Debug：`cmake --preset debug`、`cmake --build build/debug -j 4` 成功；
  `ctest --test-dir build/debug --output-on-failure -j 4` **135/135**，37.65 s，
  含公开头、安装消费者、Runtime 生命周期与其余全库回归。
- ASAN+UBSAN：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`，两开关均 ON；
  `cmake --build build/asan --target mira_runtime_scheduling_host_consumer mira_runtime_lifecycle_test -j 2` 成功；
  `ctest --test-dir build/asan -R '^mira_(runtime_scheduling_host_consumer|runtime_lifecycle)_test$' --output-on-failure`
  **2/2**，0.45 s，无 sanitizer 报告。
- 质量：`python3 tools/check_architecture.py .`、`python3 tools/check_docs.py .`、
  `python3 tools/check_platform_boundary.py .`、
  `cmake -DROOT_DIR=/home/linductor/mira -P cmake/CheckFormat.cmake`（307 个 C++ 文件）
  与 `git diff --check` 通过。基线保持 16 条，无新增或 stale，未调整 policy、
  baseline 或第三方；DEC、核心设计、API/索引、公开契约审计后续说明、README
  中英文入口、总计划同步。首次/最终诊断分别在本机
  `/tmp/mira-metrics-target-test.log`、`/tmp/mira-metrics-fix-test.log`、
  `/tmp/mira-metrics-full-test.log`、`/tmp/mira-metrics-asan-test.log`，临时文件不作为
  长期依赖，复现以仓库样板与上述命令为准。
- 验收：08 按 Linux 离线宿主范围完成，累计 **9/13**；维护轮仍 In Progress，
  02/03 平台证据、06 组合宿主与 12 总收口保持开放。没有开启周期遥测/Observer
  后端，不改变 M31 配额/容量算法。Windows/Android/TSAN、真实平台中断与延迟
  分布本轮未执行；负责人 Mira Maintainers，在相应 CI/设备补跑 consumer 和
  生命周期矩阵后取证，不从本次固定 gate 推导跨平台或实时性保证。

### 2026-11-06：第十二轮，Workflow / Context / Recovery 组合宿主

- 归属：`MNT-202611-06`；依据
  [DEC-086](../decisions/DEC-086-offline-composition-hosts.md) 与
  [组合指南](../api/offline-composition-hosts.md)。环境为 Linux x86_64、GNU C++
  13.3.0，分支 `fix/runtime-wiring-foundations`。本轮先按用户要求提交此前 08 的
  验收更新为 `bbd3f1c`，随后在该 HEAD 上实施本轮未提交变更；原有四项独立文档
  变更没有混入提交或覆盖。
- 前置澄清：恢复样板以第三轮已验收的 Linux journal/身份/扫描/退出窗口契约为
  前置，只交付离线消费闭环；03 的目标平台和 OS 强杀/断电门禁不属于该样板的
  保证范围，02/03 复选框保持未完成。没有借本项改写原跨平台或故障证据。
- Workflow：新增 `workflow_runtime_host_consumer`，外部四 worker Executor、一个
  async drive、MiraRuntime session、只读 BuiltIn 工具和 MemoryEventStore。
  create/start/wait 实际驱动 simulator screen-width 读取，再由 Verify step 消费
  原生 240 宽度的 step_result；断言 Completed、两 step、satisfied、一次读取和
  零输入。终态事件通过公开 reader 比对 run/状态，终态禁止重启；按 Workflow→
  MiraRuntime→外部 Executor 顺序收尾。不宣称可写 Workflow 已有 durable gate。
- Context：新增 `context_assembly_host_consumer`，从事件投影 checkpoint 和 JSON
  往返读取 Goal/Constraint，宿主显式映射任务框架；带 session grant 的检索只返回
  本会话候选。会话片段映射 HistoricalPayload/UntrustedExternalData，保留来源，
  另有 disposable 历史验证 512 window 下的有界选择。相同 digest 稳定，跨 epoch/
  最小集溢出在 provider 前拒绝。prepared input/tools/profile/epoch/digest 复制给
  ModelRequest，provider 实际检查 goal 与检索内容且后者仍为 User/Public；费用
  显式为 UnknownPrice。不自动改两个 Loop、M25 seam 或 M30 装配。
- Recovery：新增 `recovery_planner_host_consumer`，FileEventStore ProcessCrash
  日志与 SQLite/WAL checkpoint 先关闭再重开，journal 增量位于 base checkpoint
  之后。覆盖六种场景：start 无 receipt、显式 uncertain、真实 simulator 动作完成
  无 Verify、带历史 pin 的取消终态、NoState、无未决动作框架。前三种恢复到新
  read-only Observe，证据不足保持 AwaitingExternalVerification；不发会清除
  pending/pin 的 VerificationResult，也不重发。终态/无状态不 observe/execute，
  干净框架观察后仅交回新规划入口。新观察元数据摘要/水位进入更新 checkpoint，
  回读 digest 一致，恢复阶段输入数不增。文件重开不是 OS kill，恢复信封时间
  BUG-20261106-001 仍开放，也未伪造一个运行中的 Runtime task 或复用旧授权。
- 交付消费门禁：三个样板均用公开 API；除了各自 CTest consumer，还在
  tests/consumer 中以 find_package(Mira/kairo) 编译同一源文件并逐个执行，确认
  安装后 SDK 也能支撑消费链。SQLite 在主线程 close、任务 future 消费后才关闭
  Executor，临时文件在 worker 收尾之后删除；没有新并发设施或第三方改动。
- 初次定向 **0/3**，0.07 s：样板漏显式请求 screenshot、误写 simulator 640 宽度，
  并把会话候选套进不匹配 authority 的 RetrievedMemory。修正 screen 请求、原生
  240 宽度与不受信历史类型后 **2/3**，0.11 s。恢复样板还误将 start 无 receipt
  等同于显式 uncertain pin；按既有 reducer 区分 verification_pending 与
  uncertain_side_effects，增加独立场景，不修改 Core。最终三个样板加安装消费者
  定向 **4/4**，17.15 s。文档 metadata 尾随空白导致一次 diff-check 失败，已清理；
  保留初次失败记录，不将上述修正归咎于库能力缺口。
- Linux Debug：`cmake --preset debug`；
  `cmake --build build/debug --target mira_workflow_runtime_host_consumer mira_context_assembly_host_consumer mira_recovery_planner_host_consumer -j 4`；
  `ctest --test-dir build/debug -R '^mira_(workflow_runtime_host_consumer|context_assembly_host_consumer|recovery_planner_host_consumer|installed_consumer)_test$' --output-on-failure`；
  `cmake --build build/debug -j 4` 和
  `ctest --test-dir build/debug --output-on-failure -j 4` 均成功，最终 **138/138**，
  37.66 s，含原 M9 控制/取消、M4 Context/恢复及其余全库回归。
- ASAN+UBSAN：`cmake --preset asan -DMIRA_ENABLE_UBSAN=ON`；
  `cmake --build build/asan --target mira_workflow_runtime_host_consumer mira_context_assembly_host_consumer mira_recovery_planner_host_consumer mira_m9_execution_test mira_m4_context_manager_test mira_m4_recovery_test mira_m4_sqlite_checkpoint_test -j 2`；
  `ctest --test-dir build/asan -R '^mira_((workflow_runtime|context_assembly|recovery_planner)_host_consumer|m9_execution|m4_context_manager|m4_recovery|m4_sqlite_checkpoint)_test$' --output-on-failure`
  **7/7**，1.76 s，无 sanitizer 报告；安装 SDK 的实际编译运行证据为上面的 Debug
  consumer 门禁，不把专项 sanitizer 冒充安装/全库 sanitizer 验证。
- 质量：`python3 tools/check_architecture.py .`、`python3 tools/check_docs.py .`、
  `python3 tools/check_platform_boundary.py .`、
  `cmake -DROOT_DIR=/home/linductor/mira -P cmake/CheckFormat.cmake`（310 个 C++ 文件）
  与 `git diff --check` 通过。基线仍为 16 条，无新增/stale；未修改 policy/baseline。
  DEC、三份相关设计、API/指南/索引、README 中英文入口、总计划同步。临时日志为
  `/tmp/mira-composition-target-test.log`（初次）、`/tmp/mira-composition-fix-test.log`、
  `/tmp/mira-composition-final-target-test.log`、`/tmp/mira-composition-full-test.log`、
  `/tmp/mira-composition-asan-test.log`，复现以源文件/上述命令为准，不依赖临时文件。
- 验收：06 按三个声明边界内的 Linux 离线组合完成，累计 **10/13**；维护轮仍
  In Progress，02/03 平台与故障门禁、12 总收口和既有已登记问题继续开放。
  Windows/Android/TSAN、真实平台中断、真实模型语义、强杀/断电本轮未执行；负责人
  Mira Maintainers，在对应 CI/设备补跑 SDK/consumer 与生命周期、故障矩阵后取证，
  不以重开或 fixture 成功替代这些保证。
