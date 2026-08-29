# DEC-004：权限、能力授权和 Human Confirmation

> 状态：Accepted  
> 日期：2026-08-30  
> 负责人：Mira Maintainers  
> 冻结里程碑：M1  
> 替代/被替代：无

## 背景与问题

LLM、网页、OCR、UI Tree、Tool 输出和 Memory 都可能包含恶意指令。仅凭模型置信度或自然语言
“用户同意”不能授权支付、删除、发送、凭据输入等高风险副作用。

## 决策

- 宿主在 Session 建立时注入经认证的 `PrincipalContext` 和 capability grants；模型不能创建或
  扩大 grant。
- 默认拒绝未知 Action、Tool、资源和风险类型。PolicyEngine 在每次副作用前输出结构化
  `Allow`、`Deny` 或 `RequireConfirmation`。
- Human Confirmation 使用 challenge/response，不使用模型生成文本作为授权。challenge 绑定
  Principal、Session、Task/epoch、environment epoch、action digest、目标摘要、风险、到期时间和
  单次 nonce。
- 任何绑定字段变化、超时、取消、Takeover 或消费后都使确认失效。确认只授权被展示的一个动作
  或明确有界 action set，不授权后续模型自由发挥。
- 高风险输入、截图、Memory、Event 和训练导出按数据分类执行最小披露、脱敏、保留和删除。

## 备选方案

- 让模型在 Decision 中返回 `confirmed=true`：不可证明来自用户，不采用。
- Session 开始时一次性授权全部动作：权限过宽且不能绑定动态目标，不采用。
- 仅按 action type 授权：无法区分目标、参数和当前环境，不采用。

## 影响与风险

确认 UI 属于宿主可信界面，Core 只验证协议。若宿主身份或 UI 被攻破，Core 无法证明用户真实意图；
平台设计必须声明可信宿主边界。确认摘要必须避免泄露完整密码等敏感参数，同时 action digest 要
覆盖实际执行参数。

## 验证方式

对 token 重放、参数替换、跨 Task/tenant/epoch、过期、Takeover、屏幕变化、未知风险类型和确认
后 action 重编译建立负向测试。

## 关联文档和工作项

- [威胁模型与确认协议](../security/threat_model_and_confirmation.md)
- [M1 核心契约](../plans/m1-core-contracts.md)：`M1-12` 至 `M1-16`

