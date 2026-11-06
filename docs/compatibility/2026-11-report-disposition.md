# 2026-11 架构报告处置台账

> 状态：Active
> 版本：1.0
> 更新日期：2026-11-06
> 负责人：Mira Maintainers
> 工作项：MNT-202611-12
> 基线：用户粘贴的《Mira 项目框架与数据流分析报告》；实现证据截至 DEC-087 工作树

本表逐项覆盖报告 §3、§4 和行动清单，并登记实施中发现的时间/预算边界。处置类别
表示修复、归属或立项决定，不等于所有产品能力已交付。负责人均为 Mira Maintainers；
平台/权限条目由其指定 Adapter owner 后才进入专项实现。原始报告行号随重构失效，
不再用“全仓库零引用”、旧行数或重复比例作为当前事实。

## 1. 正确性、组合与观测

| 报告点名项 | 处置类别与结论 | 证据或后续进入条件 |
| --- | --- | --- |
| TaskFactVerified 断供；verified_facts 恒空/记忆通道死路 | 已修复显式生产边界；“恒空/全死路”校正为未装配该入口的路径，其他晋升入口仍存在 | 04、DEC-083；[事实迁移](verified-fact-v1-and-checkpoint-v2.md)及 verified_fact_host_consumer。M30-02/03/04 自动候选/主体映射/选择仍未完成 |
| 建议把 Satisfied、done 或 VerificationResult 直接升为事实 | 不采纳；没有事实内容和独立语义证据不能发布事实 | DEC-083、verified_fact_test 的负向/审批/ACL 矩阵；新原生 verifier 由宿主按 lease/epoch/authority 契约实现 |
| ActionJournal 没接主循环；建议接线或退役 | 原计划承接，保留并已接 AgentLoop 离散输入；不退役。02/03 整体验收仍开放 | 01/13、DEC-074/075/076；action_dispatcher 与九个进程退出窗口。可写 Workflow/tool、连续控制和目标平台不以该离线路径代替 |
| PriceTable 空表等于免费/恒零成本 | 已修复装配缺口并校正结论：空表为 UnknownPrice，库默认允许未知，严格宿主须显式拒绝 | 07、DEC-084；model_price_host_consumer 十二场景；[价格指南](../api/model-price-host.md) |
| 多候选/累计账单硬限额与耐久审计（实施补充） | 待专项立项，未宣称已修复；当前预留检查不是 spent+reserved 全生命周期上限 | 07/DEC-084 的明确限制。进入条件：冻结候选/重试重估、resolved model、费用归属、未知 usage 与审计持久化契约，再补真实账单矩阵；不扩大 M33 的打断/配额范围 |
| FrameCoverage.masked_regions、FrameMaskReason 无填充 | 待专项立项，保留平台供给契约，不伪造非空区域 | [Observation API](../api/environment-observation.md)。进入条件：明确 Adapter 遮蔽来源、隐私需求、权限、坐标与撤销/旋转测试，不能仅凭库内无填充删除 |
| IArtifactSource、capture_raw_response 无组合根启用 | 原计划承接为显式可选能力，保持 opt-in，不自动打开原始响应捕获 | [模型 API](../api/model-agent-loop.md)及 provider/upload fixture；生产启用前明确脱敏、授权、保留/删除与受保护 Artifact owner，平台/宿主条件未满足不计为部署完成 |
| TaskGoalSet/Constraint/Step/Observation 等 reducer 词汇无样板 | 已修复所选消费链；保留其他合法词汇，不为凑齐生产点编造事件 | 01 审计、04/06；checkpoint→Context、真实 Observation 元数据与 journal→Recovery 示例。其余 TaskStep 等由有实际语义的宿主生产者提供，不自动映射 Workflow 事件 |
| valid_runtime_transition 无调用 | 已修复，Runtime 状态写入统一经校验 CAS，异常关闭/准入并发有证据 | 11、DEC-080；runtime_lifecycle_test 与第六轮验证；不外推全部平台状态机已验证 |
| scheduling_metrics 无消费者 | 已修复参考宿主出口；公开路由累计值不等于命令拒绝或当前队列深度 | 08、DEC-085；[调度宿主](../api/runtime-scheduling-host.md)，容量/停止/异常从结果面消费，routing 零值不证明无失败 |
| WorkflowRuntime、StandardContextManager、RecoveryPlanner 缺组合根 | 已修复三个声明边界内的离线宿主，含安装后 SDK 编译运行 | 06、DEC-086；[组合指南](../api/offline-composition-hosts.md)。Workflow 仅只读；Layer 0 不自动接两 Loop；恢复不重发/不复活终态 |
| Layer 1/2、ProviderSemanticConsolidator/ProviderContextCurator 未自动接线 | 原计划承接，不作为库已实现能力退役依据 | Layer 1→Layer 0 的边界由 06 演示；M30-03/04 承接长期记忆 lifecycle。其他模型介导/重排自动化先冻结来源、权限、预算和评估，不把 fixture 当语义质量证据 |
| WorldState、TemporalPolicy、Security 活动组件未接线 | 原计划承接，保持 M27/M26/DEC-004 边界；不重开已关闭里程碑、不直接替换内联安全检查 | [M27](../plans/m27-world-state-projection-core.md)、[M26](../plans/m26-temporal-policy-stage-t1.md)、[安全 API](../api/security.md)。新消费阶段须单独确定权限与平台证据 |
| ConversationLoop、SSE、ProviderContinuation、FileEventStore/FileArtifactStore、会话投影 | 保留现有公共能力和原交付范围；“无仓库示例”等于“生产不可达”的推断不采纳 | [模型 API](../api/model-agent-loop.md)、[Context API](../api/context-memory.md)、[事件 API](../api/events-recovery.md)及各 contract/安装消费者；06 只证明选择的组合，不扩大所有 Provider/部署承诺 |
| schedule_global_retention_sweep 缺宿主/专项测试 | 原计划承接，仍未验收该周期删除入口 | [M31-09](../plans/m31-capacity-admission-and-archival-recovery.md) 保持未勾选。核对当前 tests 无直接专项调用；进入条件：原配额/保护集合、幂等跨 store 擦除、取消/重启与 kairo 周期 owner 就绪 |
| 度量、ActionJournal 驻留容量 | 原计划承接；诊断出口不授予自动淘汰保护状态的权力 | 08 已有出口，容量/保留仍由 M31；新增淘汰规则必须保留未决副作用与审批状态的保护集合 |
| BUG-20261106-001（实施补充） | 已修复新记录恢复时间；旧时间明确缺失，不可追回 | DEC-087、[文件日志 v2](file-event-log-v2.md)、file_event_time_test 与真实旧 reader 探针；旧缓存不自动改写，C++ 消费者需重编译 |
| 存储回执虚报耐久等级（实施补充） | 已校正：内存 Buffered、文件 ProcessCrash；未实现的 PowerLoss 明确拒绝 | DEC-088、event_durability_test；裸内存 journal 零输入。九窗口 Linux 外部 SIGKILL 与定向 TSAN 已取证，系统同步/断电及目标平台仍归 02/03 |

## 2. 公开词汇与复用

| 报告点名项 | 处置类别与结论 | 依据 |
| --- | --- | --- |
| parse_workflow_publish_rejected、checkpoint_trigger_name、recovery_action_name | 已完成契约消费/保留审计，不按引用数删除 | 11、DEC-082；[逐项公开审计](2026-11-public-contract-audit.md)、正反 reader/名称契约与安装消费者 |
| CoordinateSpace / CoordinateSpaceKind | 已完成保留审计，纠正旧数量；不压缩枚举或伪造空间数据 | 同上；八个具体空间加 Unknown 的字节/未知值契约 |
| StrictToolCall、ConfirmationRequired、CompletionReceived、ClockSyncQuality::Synced | 已完成保留处置，不强行制造生产转换或提升时钟质量 | 同上；编号与用途逐项登记，预留值不等于当前自动生产能力 |
| stash_headers 残留、outcome.attempts 被覆盖 | 已修复 | 11 清理残留；05/DEC-074 固定跨候选累计 provider 调用次数并回归重试/熔断 |
| wire_request_digest / prompt_digest / tool_snapshot_digest 等 | 已完成保留审计，不误当同一摘要的死别名 | 11/DEC-082；不同域与排除规则、canonical/dialect 测试和 API 说明 |
| SQLite 两个 open 与 schema 解析漂移 | 已修复共享初始化/原子迁移编排，保留各 store DDL 和必要错误差异 | 10/DEC-078、store_open 故障与重开矩阵 |
| cosine_similarity 零范数语义漂移 | 已修复，统一有效性与余弦边界，无效向量不参与结果 | 09/DEC-077、embedding_math 与 Context/SQLite 回归 |
| workflow check_keys/bounded_string、TaskState/name 双份 | 已修复局部复用 | 11/DEC-079，保留各 schema 错误映射与持久化拼写 |
| 检索评分/选择骨架同构 | 已完成保留处置，未抽成跨层通用模板 | 11 审计；Context 的 session/asset ACL 与 Memory 的 scope/kind/claim/verification/expiry 不同，数值边界已共享 |
| to_event_payload 尾部、module Error 双份、Context marker/lowercase | 已修复证实等价部分 | 11/DEC-081/082；20 个 writer 的重构前 wire fixture、Error 字段/marker 大小写与副本独立性测试 |
| TLS 公共段、其他 Error 工厂/大小写函数 | 已修复 TLS 公共准入/构造；必要领域差异保留 | 11/DEC-082；OpenSSL/Mbed TLS 双后端回归；协议/URL/安全大小写、错误领域码和后端错误映射不机械合并 |
| JSON/SHA/ID/时间工具“已无重复”、重复约 1.2% 与全库零线程结论 | 不作为本轮量化保证，保留约束与实际检查结果 | 仅报告已执行的架构/平台边界/格式检查及具体复用证据，不以旧仓库规模推导当前重复量、部署或并发保证 |

## 3. 完成口径与剩余门禁

12 的收口表示以上条目已明确“修复/保留依据、原计划承接或待立项条件”，以及相关
文档/验收证据同步；不是实现所有可选能力、周期删除或产品费用框架。原报告的五项
行动清单分别由 01–05/13、06、11、09/10/11、08 及本台账承接，没有无主条目。

02/03 的目标平台与真实故障退出条件继续未完成，维护轮保持 In Progress；不因
本台账收口自动关闭 RW-G1–G6。第十四轮已补 Linux 外部 SIGKILL 与定向 TSAN；
Windows/Android、真实账单/模型/隐私需求、目标平台终止和断电未验证项的负责人
仍为 Mira Maintainers，补跑条件在各计划与逐轮记录。
准确命令、失败历史和证据范围见
[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md) 第四至十四轮。
