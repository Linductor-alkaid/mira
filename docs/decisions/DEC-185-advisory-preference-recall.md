# DEC-185：偏好召回作为带归属的参考信息

> 状态：Accepted
> 日期：2026-11-27
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-070](DEC-070-long-term-memory-behavior-contract.md)、[DEC-108](DEC-108-native-role-memory-recall.md)、[DEC-119](DEC-119-six-memory-systems.md)、[DEC-120](DEC-120-agent-persona-admission.md)

## 决策

`Preference` 描述某个对象或范围内曾记录的偏好。Mira 将偏好作为可选参考，与当前工作、
自身目标及已接下的承诺一起权衡。验证等级说明记录的证据状态。人格配置、任务承诺和
能力授权继续由各自所有者管理。

Memory Agent 在现有 `RetrievedMemory` 框内追加偏好使用说明。偏好行采用 JSON，携带
`kind=preference`、`use=advisory`、`applies_to`、作用域类型、绑定内 `scope_slot`、验证等级
和转义的正文。只有与绑定导出的范围相同的记录才能标为 `current-user` 或 `current-session`。
其他预先获准的范围使用 `authorized-scope`，以绑定内槽位区分。归属字段用相对标签替代
范围元数据中的原始主体和租户标识。事件出处保留在 `ContextItem` 的 provenance。

候选继续使用 P4 可选上下文和既有 User 角色。额度计算包含完整 JSON 和使用说明，装不下的
偏好行报告降级。原始任务输入和宿主人格仍经各自原有的准入路径。会话逐字回放与普通事实
行沿 DEC-108。

## 取舍与验证

直接拼入正文会丢失归属和适用语义。结构化标注保留既有上下文角色和存储格式，增加少量
模型输入字节。Linux 离线验证实际 Gateway 输入、当前任务要求的保留、跨主体隔离、
授权范围槽位、正文转义和渲染后的字节上限。真实模型的选择行为由 M30-07 补跑。
