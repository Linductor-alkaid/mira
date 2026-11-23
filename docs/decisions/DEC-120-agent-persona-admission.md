# DEC-120：宿主管理的人格版本与模型请求准入

> 状态：Accepted
> 日期：2026-11-20
> 负责人：Mira Maintainers
> 冻结里程碑：M30
> 依据：[DEC-119](DEC-119-six-memory-systems.md) 的人格契约，延续 DEC-098 与 DEC-101

## 决策

宿主认证并维护人格配置的持久版本，按租户和稳定 Agent ID 持有当前激活配置。
Core 提供有界 AgentPersonaCatalog。宿主在加载、更新、回滚或撤销已提交配置后
同步更新目录。活跃快照依赖一次具体激活，替换、撤销与回滚立即使旧快照失效，
同版本同正文的重复激活幂等。宿主负责校验其持久版本的不可变历史和变更权限。
目录只保留当前版本，模型、记忆候选与回放没有目录变更入口。

Gateway 可选安装目录与任务绑定解析器。解析器只接收经宿主建立的 OperationContext，
在已认证绑定上返回租户与 Agent ID 以及当前授权探针。无绑定可沿用原始请求；
已绑定身份缺少生效配置、绑定失效或解析器失败都封闭调用。启用该入口要求
OperationContext 的 Task、Session 与请求 epoch 已绑定。探针有界且无 I/O。

Gateway 将人格以独立的 Internal System 项加入宿主指令前缀，保护现有指令与工具选择。
附加项由 Layer 0 按完整请求预算准入，摘要计入提示模板 provenance。人格正文和宿主
身份不写入审计事件。已绑定人格的调用使用本地组装请求并清除不透明远端续接状态。
同一次 infer 的重试和 fallback 沿用捕获版本。发送前、流式预览回调和完成后复核
激活与授权。版本失效终止旧请求，下一次新请求重新读取当前配置。

## 影响与验证

新增的宿主 API 使用同步有界目录，复用调用者的 Executor 生命周期。配置提交后
由宿主同步，目录和工作绑定探针各自持有作用域，不持有跨 provider 调用的锁。
ModelGateway 消费者需随公开头文件重新编译。
Linux 离线验证覆盖真实 Gateway 请求、人格回滚、防止旧快照复活、原始 System
与召回 User 角色、跨租户绑定、上下文预算、撤权和在途完成失效。产品持久化接线
与跨平台验证见 M30。

## 关联文档

- [M30-11](../plans/m30-long-term-memory-integration.md#5-工作项)
- [六类记忆设计](../design/m30-memory-systems.md)
- [模型与 Agent Loop API](../api/model-agent-loop.md)
