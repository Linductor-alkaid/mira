# 安全与权限

> 头文件：`mira/security.hpp`

权限、能力授权、风险分级与 Human Confirmation 的核心契约
（[DEC-004](../decisions/DEC-004-security-authority-confirmation.md)）。原则：模型输出
只是建议，任何动作在本地经过能力、新鲜度、权限与 SafetyPolicy 校验（RULE-04）；
Observation、Memory 与模型响应中的外部内容都是不可信数据（RULE-09）。

## Principal 与授权

- `PrincipalContext`：操作主体（含 `AuthenticationStrength`：`Anonymous` / `Session` /
  `Strong`）。
- `CapabilityGrant`：能力授权（`GrantSource`：`Host` / `Administrator` / `System`），
  `ResourceSelector` 描述目标资源范围。授权只能来自宿主/管理员/系统，不能来自模型
  文本或检索结果。

## 风险与决策

- `ActionRisk`：动作风险分级。
- `PolicyInput` / `PolicyDecision`：`PolicyEngine.evaluate(PolicyInput)` 的确定性评估
  入口；`AllowDecision` 是放行结果的结构化形态。高风险动作要求确认时返回
  `ConfirmationRequired`（见 `ErrorCode`），而不是静默降级。

## Human Confirmation

`ConfirmationChallenge`：一次确认质询（`ConfirmationId`、principal、task/session/epoch
绑定、动作 digest）；`ConfirmationDecision`（`Approve` / `Reject`）回填后动作才可能
重新进入派发路径。绑定 task epoch 确保迟到确认不能复活旧 epoch 的动作。

## 脱敏

`RedactionRecord` 记录脱敏决定。全库约定：API key、Authorization header、用户密码与
输入法敏感内容不进入事件、日志或 Artifact 明文；`Error.safe_message` 与模型层的
`sanitize_wire_for_events()` / `redact_url_for_log()` 是既成的脱敏出口。

## 相关文档

- [威胁模型与权限确认协议](../security/threat_model_and_confirmation.md)
- [事件与可观测性约定](index.md#通用约定)（脱敏与事件分层）
