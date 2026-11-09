# DEC-101：受管记忆代理与完整请求准入

> 状态：Accepted（2026-11-09 按用户要求采纳研究建议并推进开发）
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 冻结里程碑：M36
> 替代/被替代：补充 DEC-032/068/099，不替换既有记忆写入契约

## 背景与问题

[研究报告](../design/memory-agent-reference-study.md)识别两处缺口：Loop 历史裁剪没有
覆盖完整请求；父任务拥有的子代理不能承担任务结算后的记忆维护。用户要求制定计划、
补齐接线并推进开发。本决策冻结可独立验收的记忆服务与请求准入增量，不关闭 M29–M33。

## 决策

1. ModelGateway 在动态选工具之后、每个候选 provider 派发之前使用 StandardContextManager
   执行最终准入。原请求的角色、顺序、工具往返与最新用户要求保持原样，均为保护集合；
   可选记忆/摘要候选只能为不受保护的 P3/P4 User 数据。工具不能被静默裁掉。
   计入输出预留、图像、工具 schema、输出 schema、provider 扩展和 framing margin。
   每个 fallback 按其自身 profile 重算。估算质量可见，不宣称精确 tokenizer 保证。
2. Gateway 的候选供给是有界非阻塞读取。每次 infer 获取一次不可变快照；派发前及
   返回后检查快照版本。失效结果不能进入后续动作。已准入的在途请求无法追溯撤回。
   同一活动 task/epoch 可通过 refreshable 探针在既有恢复预算内重建请求；设备路径
   重新 Observe，schema repair 同样检查。接管、删除、终态与未送达纠正不可自动刷新。
3. 宿主拥有 MemoryAgentService，注入 Executor、IMemory、EventStore 和可选模型工厂。
   主代理通过有界工具启动/查询/取消记忆工作。服务执行检索、事件提取和带引用的历史
   压缩；模型只提出候选，写入走 MemoryConsolidator，未验证输出不提升验证等级。
   新服务强制使用严格模型候选策略：Unverified 且 namespace 不豁免 kind/scope 门；
   既有 DEC-059 promotion/import 默认兼容行为保持不变。
4. 每个工作显式绑定 principal、Task/Session、epoch、scope allowlist 与修订号。修订、
   接管及 scope 失效使旧缓存/候选失效。服务 erase 与提交共享串行化边界；旧候选不能
   在 erase 后写回。通过服务之外进行的变更要求宿主先调用失效入口。
5. 任务结束封闭请求投影，但允许显式的 terminal 后事件提取；它不能回流唤醒终态任务。
   持久事实仍为既有事件/记忆存储，服务缓存是有界可重建投影；不自动恢复旧 worker。
6. 网络/模型工作由 kairo blocking-I/O worker 管理；服务持有句柄和结果，外部 owner
   消费回执并 close，再 shutdown Executor。每工作至多一个在途 job，容量满明确拒绝。
   不新增线程池、无主任务或新的物理删除协议。
7. 压缩只替换已捕获且逐项 digest 匹配的历史前缀，保留 protected tail 和完整协议边界；
   模型语句必须引用捕获来源。失败回退 raw replay，最终准入仍必须通过。引用存在不
   代表语义忠实；语义质量与长期容量另由真实模型评估及 M31 验证。
   来源前缀和提案保存到宿主 ArtifactStore，控制分区的版本化引用收到 ProcessCrash
   ack 后才发布；擦除屏障与关联 Artifact 清理在服务重建时继续生效。
8. 路由继续走 InteractionCoordinator 的既有建议缝与确定性仲裁；服务不复活任务、
   不修改输入台账、不把记忆中的授权作为 grant。示例证明两个工作的独立装配与纠正。

## 备选方案

仅在 HistoryTransformer 截断无法计入整个请求；全权记忆模型会绕过 Layer 0 和写入门；
让 M34 child 跨越 parent 终态会改变其生命周期。采用宿主服务复用现有边界。

## 影响与风险

新增公开 API 需要 C++ 消费者重编译。无存储 schema 迁移、外部头族或依赖方向变化。
新增版本化控制事件而不新增 SQL 表；费用预留沿用 DEC-084 的计价估算器，完整窗口
准入的 token 估算与账单估算分别报告，不把两者相混。请求 codec 恢复全部 provenance
digest/version，以保持带 selection_digest 请求的 JSON round-trip。
protected 原始历史不能无限增长，必须配合压缩；容量不足显式失败。当前 M30 中承诺实体、
完整用户纠正意图解析、M29 崩溃恢复及 M31/M33 的交付范围保持各自计划，本次不冒充完成。
已删除 scope 保守封闭；重新接纳新知识需要后续世代/来源水位契约，不允许清除屏障
后重提取旧事件。该可用性边界在 API 与设计中显式说明。

## 验证方式

[M36](../plans/m36-memory-agent-context-integration.md)覆盖最终请求预算、fallback、
记忆隔离、写入门、纠正/删除竞态、压缩协议、取消/超时/拒绝/异常/关闭与两工作宿主示例。
真实模型、目标设备与长期资源增长单独记录补跑责任和条件。
