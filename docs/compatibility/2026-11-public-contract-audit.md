# 2026-11 公开契约保留与局部复用审计

> 状态：Active
> 版本：1.0
> 更新日期：2026-11-05
> 负责人：Mira Maintainers
> 依据：MNT-202611-11、DEC-082
> 源码范围：HEAD b81df19 加截至第八轮（DEC-082）的工作树；不代表部署或下游调用统计

后续 DEC-083 的新增事实提交与 VerifiedFact/Checkpoint 变更，单独见
[迁移说明](verified-fact-v1-and-checkpoint-v2.md)，不属于本份局部复用的无接口变更结论。

## 1. 审计原则

本轮处理架构报告点名的公开孤儿候选，不以仓库引用数判断下游是否使用，不批量删除
parser、枚举或 digest。以下“保留”表示接口、字段和枚举值继续有效，不表示宿主已
自动装配该能力。缺少平台供给或自动化的项目保留其原计划归属。

## 2. 逐项处置

| 报告点名项 | 契约与实际依据 | 处置 |
| --- | --- | --- |
| `parse_workflow_publish_rejected` | [workflow_events.hpp](../../include/mira/workflow_events.hpp) 的公开发布审计 reader；与对应 writer 成对 | 保留；新增正常、错误 type/schema、未知字段契约用例及安装后宿主消费，不为制造 src 引用而自动启用 |
| `checkpoint_trigger_name` / `recovery_action_name` | [task_checkpoint.hpp](../../include/mira/task_checkpoint.hpp)、[task_recovery.hpp](../../include/mira/task_recovery.hpp) 为宿主解释触发和恢复决策 | 保留；测试六个 trigger、四个 action 的名称与 Unknown fallback；安装消费者实际读取 |
| `CoordinateSpace` / `CoordinateSpaceKind` | [coordinates.hpp](../../include/mira/coordinates.hpp) 明确多显示器、窗口、帧、UI node 与 ROI 的宿主词汇，不仅是当前 Adapter 的输出子集 | 保留；当前为八个具体空间加 Unknown，修正报告旧数量。逐个原始字节验证解析；未知值是 UnsupportedVersion，不压缩枚举或伪填平台数据 |
| `DecisionSource::StrictToolCall` | [model_schema.hpp](../../include/mira/model_schema.hpp) 的公开结构化决策来源标签 | 保留编号 1；没有当前解析生产点不等于可删除，不强行把 ToolProposals 标成该来源 |
| `ErrorCode::ConfirmationRequired` | [安全 API](../api/security.md) 已定义须人工确认的结果语义，PolicyEngine 自身可通过 decision variant 表达 | 保留编号 16；不要求所有公共错误枚举都在 Core 发射 |
| `OperationState::CompletionReceived` | [core_contracts.hpp](../../include/mira/core_contracts.hpp) 的公开操作状态词汇；当前 Runtime 可直接结算 Settled | 保留编号 4；不为了消费枚举而引入无业务意义的中间状态或迟到 completion 复活路径 |
| `ClockSyncQuality::Synced` | [observation.hpp](../../include/mira/observation.hpp) 的时钟质量声明，由能证明同一 clock domain 的 Adapter 提供 | 保留编号 0；不把未知/估算时钟伪标成 Synced |
| `wire_request_digest` / `prompt_digest` / `tool_snapshot_digest` / `decision_digest` / `data_policy_digest` | [model_digest.hpp](../../include/mira/model_digest.hpp) 定义不同摘要域、secret/volatile 排除与 provenance；[canonical 测试](../../tests/m3/m3_canonical_test.cpp) 和 [dialect 测试](../../tests/m3/m3_dialect_test.cpp) 消费相关契约 | 保留；它们与 `model_request_canonical_digest` 的输入与排除规则不同，不是同一函数的死别名 |
| `scheduling_metrics` / `SchedulingMetricsSnapshot` | [runtime.hpp](../../include/mira/runtime.hpp) 的十三项累计计数快照，MNT-202611-08 负责宿主诊断样板 | 保留；本轮不代替 08 的过载/关闭/异常出口验收 |
| `schedule_global_retention_sweep` | [context_memory_supervisor.hpp](../../include/mira/context_memory_supervisor.hpp) 的受 Executor 管理入口；M31-09 承接清扫宿主与生命周期 | 保留；专项测试和宿主装配由 M31/12 核对，不宣称本轮已补齐 |
| `masked_regions` / `FrameMaskReason` | Observation 覆盖质量与隐私遮蔽的可选平台供给，不是每个平台必有非空输出 | 保留；由 12 记录实际 Adapter 能力与后续立项条件，不构造伪遮蔽区域 |
| `IArtifactSource` / `capture_raw_response` | [模型 API](../api/model-agent-loop.md) 的显式捕获开关与源，涉及脱敏、访问和保留 | 保留 opt-in；由 12 收口，不在复用治理中默认开启原始响应捕获 |

## 3. 复用与保留差异

| 代码族 | 本轮处理 | 保留原因/边界 |
| --- | --- | --- |
| Workflow 20 个 `to_event_payload` 收尾 | 同一文件私有 helper 构造类型、JSON data、分类；重构前 fixture 固定 wire 字节 | 各事件字段、插入顺序、schema 和两个 Critical 分类逐函数显式保留；不共享业务 parser 或 durability |
| OpenSSL/Mbed TLS 错误工厂与 create 尾部 | net 私有 helper 统一准入顺序、构造/setup 与固定错误摘要 | 各后端信任加载、证书/主机名、SNI/IP、握手、socket/BIO 错误均保留，不把 API 外形相似当作语义相同 |
| tool_module / registry 的错误工厂 | tool 私有 helper 消除两份逐字段相同构造 | 其他模块的 domain_code、retryable、operation 归属、消息前缀继续由所属领域决定；不构建全库可配置错误框架 |
| Context 与 SQLite Memory 评分/选择骨架 | 保留各自编排，向量数值边界已由 DEC-077 共享 | Context 按 session/scope asset ACL；Memory 还涉及 kind/claim/verification/expiry、记录裁剪与查询质量。公共排序模板会把这些策略耦合到跨层 API |
| Model wire/dialect/安全大小写转换 | 保留领域语义；本轮只统一已证实等价的 Context/Memory ASCII 原语 | 协议头、URL/host 与安全规则使用各自归一化及匹配规则，不能以同样调用 tolower 为由替换；有明确 locale/协议需求时另行验证 |

## 4. 兼容性边界与证据

本轮没有删除公开符号、改签名、重排枚举、迁移 schema 或调整架构策略。保留编号的
契约测试用于阻止后续误删/重排，不表示这些预留值必须被当前 Runtime 自动生产。
Workflow fixture 固定的是编码器字节输出；部分样本是结构体默认值的低层编码，不能
代替合法性验证。正向与负向 parser/Replay 仍由既有 M8–M14 与新增契约测试覆盖。
fixture 从重构前已构建编码器捕获，正常测试只读取，更新须人工评审协议兼容性。

Linux、TLS loopback、公开头、安装消费者和 sanitizer 的准确命令及结果见
[维护计划第八轮](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)。
其他平台未执行不视为通过；本审计不提供部署统计、完整下游 ABI 扫描或跨平台保证。
