# M30 自动记忆与目标执行参考

> 状态：Active
> 更新日期：2026-11-25
> 负责人：Mira Maintainers
> 实施归属：[M30 工作项](../plans/m30-long-term-memory-integration.md#5-工作项)
> 当前证据：[M30 验收](../plans/m30-memory-acceptance.md#2-按交付能力查证)

| 参考 | Mira 的接入点 |
| --- | --- |
| [Hindsight Retain](https://github.com/vectorize-io/hindsight/blob/main/skills/hindsight-docs/references/developer/retain.md) | 将来源消息、发言者和时间作为提取输入，保存来源事件与提取结果的关联，按对象及时间召回。 |
| [Graphiti](https://github.com/getzep/graphiti) | 复用 Mira 已有的双时态字段与关系。指代和跨来源同一性先形成带证据的判断，新的反证可以修正有效区间。 |
| [ExpeL](https://arxiv.org/abs/2308.10144) | 从实际结算的成功和失败轨迹提取可复用经验，通过技能环境、发布版本与失败键召回。 |
| [Letta 状态化 Agent](https://docs.letta.com/v1-sdk/concepts/stateful-agents) | 将当前说话者和少量相关画像组装进请求；详细记忆按类型和范围检索。 |
| [Temporal Schedule](https://docs.temporal.io/schedule) | 以稳定触发 ID 连接到期发现、任务提交和回执。显式处理停机后的补跑、重叠和重试。执行使用 Mira 已有协调层和 Executor。 |

## Mira 的接入约束

- 社交消息先持久化来源事件和账号，再分别解析当前发言者、引用参与者及正文提及。
  跨来源账号同一性、属性和关系候选均保留证据及反证，召回保留来源和时态。
- 到期目标沿既有协调层派发。稳定目标、触发标识与回执贯穿提交和核对，执行结果未知时
  按原操作 ID 查询。重复触发、提醒和任务结算各自保留状态。
- 通用提取沿当前授权下的持久事件水位有界消费，按六类路由到各自的写入入口，记录
  处理区间、失败回执和来源。
