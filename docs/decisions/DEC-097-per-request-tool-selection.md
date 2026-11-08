# DEC-097：每请求工具选择与 profile 工具限额

> 状态：Accepted
> 日期：2026-11-08
> 负责人：Mira Maintainers
> 替代/被替代：无

## 背景与问题

桥接宿主可注册 174 个工具，但 loop 全量暴露触发 canonical 128 个工具上界。
`ProfileLimits::max_tools_per_request` 默认 64 却未参与校验。宿主已选择完整的 B 方案。

## 决策

两类 loop 增加 `set_tool_selector`。回调接收运行 spec 和已组装请求（tools 为完整
注册表快照），返回有序 ToolId 子集。每次组装调用一次，包括恢复后的新请求；gateway
内部重试沿用所选快照。空结果表示不暴露工具，未配置则保留全量行为，不静默裁剪。
回调错误、异常、重复或未知 ID 都在 infer 前 fail closed，错误不回显异常文本。
模型只能调用本次选中的工具；注册表仍保留全部 handler。历史工具调用/结果继续回放，
不要求历史工具出现在当前暴露集。成功/失败选择产生有界元数据事件。

回调同步运行于 run 调用者所在 Executor worker，宿主须保持有界、不阻塞，并在 run
前配置；不新增并发任务、生命周期或 Executor 缺口。请求输入只读，schema/identity
由注册表映射，回调不能改写工具契约。工具选择不提升权限，也不替代动作准入。

保留 canonical 128 上界；profile 工具限额范围为 0..128（0 表示不暴露新工具），默认
64 不变。路由检查请求数量，直接 provider 在上传前及各 mapper 编码前检查限额。
诊断包含当前数量与限额。profile digest 已包含该字段，不改变序列化格式。

## 备选方案

仅接活 profile 字段无法支持全量注册的大目录；扩大硬上界不能提供按需暴露。
不引入默认关键词选择或静默截断，选择策略由宿主掌握。

## 影响与风险

原先发送 65..128 个工具且使用默认 profile 的宿主须显式提高限额（最多 128），
或接入选择器。公开类布局/RouteQuery 变化要求 C++ 消费者重编译。配置注册表的宿主
可以保持原接线；默认不配置选择器时仍可能因目录超限而失败，但诊断现在含数量。

## 验证方式

离线测试覆盖 174 个工具目录、动态选择、两类 loop、未选工具拒绝、空选择、重复/
未知 ID、回调错误/异常、取消、恢复和 gateway 重试，以及三方言/profile/provider
边界；运行 M3 回归及格式、文档、架构、平台门禁。不声明真实 OneBot 互操作。

## 关联文档和工作项

- [维护计划](../plans/maintenance-2026-11-tool-selection.md)：`MNT-202611-TS-01`
- [模型与工具设计](../design/model_provider_and_tool_design.md)
- [API](../api/model-agent-loop.md)
