# DEC-084：参考宿主的价格策略与结算显示

> 状态：Accepted
> 日期：2026-11-06
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-07

## 决定

提供只使用公开 Core API 的离线价格宿主。PriceTable 显式注入 ModelGateway，
条目包含人工 fixture model、USD 币种、版本、生效时间和 fixture 来源；数字只用于
确定性核算，不代表任何供应商报价。严格宿主显式设置 require_known_price=true，
容许未知费用的宿主显式设置 false，并保留 token/output/request 上界。

库中该配置的既有默认值是 false，本轮不改变默认值、公开布局或算法。修正文档中
“未知价格默认拒绝”和“迟到响应不结算”的错误描述。迟到的成功响应先结算 usage，
随后 admission 拒绝其任务影响；admitted=false 不能被宿主理解为未计费。

样板区分已知价格、未知价格拒绝/允许、resolved model 缺价、Missing/Partial usage、
准入前拒绝、发送前取消、取消后成功响应及显式 AmbiguousCompletion。只有完整
Reconciled 才输出已核算金额；其他状态输出 unknown/partial 与审计需求，同时输出
ledger 的实际预留余额。不能仅凭金额字段为 0 或 reserved_remainder=false 判断
免费/已完全释放；后者是本次核销质量标志，不是账户余额快照。

## 边界

每个场景使用独立 task、单 profile、一次 infer，禁用重试；不存在真实网络、凭据或
平台输入。使用明确的 provider fixture 控制取消阶段，不能外推真实 transport 的
取消、退款或账单行为。模糊完成不重发，保留金额预留等待外部账单核查。

现有 Task budget 检查累计未核销 reservation，spent_micros 另行记账，不是完整的
累计账单硬限额。Gateway 只按首选 selector 预估，多候选/重试重估和 resolved model
变更不是本样板的证明范围；require_known_price 也不保证响应最终一定可定价。
这些边界由维护项 12 登记并核查专项立项，不能借样板宣布产品级费用约束已交付，
也不扩大 M33 的自主打断/配额范围。

宿主串行运行 Gateway；其 ledger/circuit 不是并发写入 API。外部 Executor owner
initialize 后通过 submit_auto 调用并消费 future，停止生产者后由非 worker 执行
shutdown(true)。样板内存账本/事件只供进程内演示，生产审计需宿主另行持久化。

## 验证

新增可构建 CTest consumer，以 provider 调用次数、事件 priced/settlement_quality、
结算状态、spent/reserved 和 late admission 断言证明实际产物被宿主消费；对缺价
与金额超限拒绝证明零 provider 调用。沿用 M3 budget/gateway/取消回归与 sanitizer。
API、协议设计、参考指南和计划同步；没有新 Executor 能力缺口或第三方修改。

关联：[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)、
[LLM API 设计](../design/llm-api-protocol-design.md)。
