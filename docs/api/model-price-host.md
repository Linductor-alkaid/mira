# 模型价格与预算的离线宿主

> 状态：Active
> 版本：1.0
> 更新日期：2026-11-06
> 负责人：Mira Maintainers
> 依据：[DEC-084](../decisions/DEC-084-model-price-reference-host.md)、MNT-202611-07

## 构建与消费

[model_price_host_consumer.cpp](../../examples/model_price_host_consumer.cpp) 只使用公开
Core API，使用自有 IModelProvider fixture，不访问网络、不加载凭据、不操作设备。

```bash
cmake --preset debug
cmake --build build/debug --target mira_model_price_host_consumer -j 4
ctest --test-dir build/debug -R '^mira_model_price_host_consumer_test$' --output-on-failure
./build/debug/mira_model_price_host_consumer
```

输出首行明确标注 OFFLINE FIXTURE PRICES。人工 model `fixture-priced-model`、USD、
entry version=7、epoch 生效时间和 fixture source 只是测试数据，不是任何真实模型
价格。PriceEntry 单位是 **每百万 token 的 micro currency units**；本例 input 为
1,000,000、output 为 2,000,000，即每个 input/output token 分别为 1/2 micro USD。
输入 `fixture` 在当前估算器下为 3 token，generation 上限为 4，预留费用 11。
Provider fixture 返回相同的 3/4 usage，已知价格场景最终核算 11、余额为零。

## 显式策略

```cpp
mira::ModelGatewayConfig policy;
policy.require_known_price = true; // 本宿主选择；库默认值仍是 false。
policy.retry_budget.max_attempts = 1;
mira::ModelGateway gateway(executor, router, nullptr, prices, policy);
```

严格场景缺价或金额上限为 10 时，provider 调用次数为零。允许未知场景显式设置
false，保留 max_input_tokens、max_output_tokens、max_requests 和 generation 上限；
有费用字段不代表未知费用获得金额保护。金额零只有在明确完整核算时才能作为金额
展示，不能把空 PriceTable 当作免费配置。

PriceTable lookup 匹配 model、currency、effective_from；输入必须由宿主维护，
别名与 resolved revision 需要各自可用条目。require_known_price 检查发送前 selector，
响应 resolved model 缺价仍可能返回 UnknownPrice；本例也断言这一结果并保留预留。
库不会从网络自动更新价格，也不推导两个 model 名称的账单等价关系。

## 十二个可执行场景

| 场景 | 宿主必须消费的结果 |
| --- | --- |
| known | Reconciled；spent=11，实际 reservation 余额归零 |
| unknown rejected | ResourceExhausted；零 provider 调用 |
| unknown allowed | UnknownPrice；cost=unknown，预留 token/request 仍在，审计不能仅检查金额余额 |
| cost rejected | 已知估算 11 超过上限 10；零 provider 调用 |
| resolved unknown | selector 已定价但 resolved model 无条目；UnknownPrice，保留金额预留 |
| missing usage | MissingUsage；预留不释放，未知费用等待审计 |
| partial usage | PartialUsage；只核算报告的 input，保留未报告 output 和金额余额 |
| admission rejected | 已撤回的任务准入在 dispatch 前拒绝，零 provider 调用 |
| cancel before write | provider 遵守取消探针，明确未写请求；预留释放 |
| cancel late success | 返回完整 usage 后仍核算费用；admitted=false，不消费其任务输出 |
| cancel late missing | admitted=false 且 MissingUsage；不能因为任务取消而清空预留 |
| cancel ambiguous | 写后取消无终态，由 provider 报 AmbiguousCompletion；不重试，保留金额待审计 |

成功响应还读取 ModelRequestPrepared.detail.priced 与
ModelResponseReceived.detail.settlement_quality，断言事件和结果一致；这些元数据
不包含提示词、凭据或模型正文。事件为进程内 MemoryEventStore，费用账本也未持久化。

## 所有权与实际限制

Gateway 的 infer/ledger/circuit 由宿主串行消费；样板每个场景使用独立 task、单
profile、一次 infer，使用外部 kairo owner 提交并 get future 后读取 ledger，最后
从主线程 shutdown(true)。生产宿主须串行化自己的入口并持久化未决审计记录，不能
把销毁内存 Gateway 当成账单已结清。

reserved_remainder 表示本次结算未能完全核销的质量，不是整个账户余额快照。实际
余额须从 ledger.reserved(task) 读取；usage 小于估算时，即使 Reconciled 仍可能有
比例释放后的余额。BudgetLedger::close 只禁止后续 reserve，**不会清空余额**。

Task scope 约束累计未核销 reservation，spent_micros 另行累计；该检查不等价于
“spent + reserved”的全生命周期账单硬上限。首选 profile 的预估也不能作为所有
fallback、重试和 resolved model 的费用上限证明。本例不改变这些算法；维护项
12 登记并核查专项立项，不以离线一次调用证明完整产品费用约束；M33 的自主
打断/配额范围不因本指南扩大。

取消场景只模拟明确阶段和 provider 输出，不证明真实供应商取消后不计费或退款。
Windows/Android/真实 transport 与账单验证须在对应环境补跑，不能外推 Linux fixture。
