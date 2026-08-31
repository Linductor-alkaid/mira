# DEC-009：工具模组边界与能力协商

> 状态：Accepted  
> 日期：2026-08-31  
> 负责人：Mira Maintainers  
> 冻结里程碑：M7  
> 替代/被替代：无

## 背景与问题

不同终端设备能够提供的工具集不一致，而 Mira 的模型层对平台无感：模型只能看到每次请求
真实暴露的工具 schema。这要求设备差异在 Runtime 内被转化为明确的“可用性”事实，而不是
让模型或任务自己去发现。当前设计存在三个缺口：

1. `ToolRegistry` 是扁平的工具集合，没有设备/宿主贡献工具的聚合单元，无法整组版本化、
   验证、启用和撤销。
2. `CapabilityId`（`security.hpp`）只是授权用的字符串，没有受治理的词汇表、没有与
   `EnvironmentCapabilities` 的映射，也没有“模组需要什么、环境提供什么”的协商机制。
3. 工具有两类执行侧消费者：LLM 经 `ToolIntent` 调用，蒸馏出的任务 policy model 也要引用
   同一批工具产生候选意图。两者缺少同一份可绑定的工具事实源。

## 决策

- `ToolModule` 是工具进入 Runtime 的唯一定义单元。每个模组携带 manifest：模块 ID、
  SemanticVersion、来源与签名（`BuiltIn`/`HostProvided`/`OutOfProcess`）、成员工具
  `ToolSpec`、能力与资源需求、冲突声明。没有 manifest 的工具不注册。
- 模组可用性由能力协商决定，不由设备型号或平台名决定。能力词汇收录于受治理的
  `CapabilityCatalog`；`EnvironmentCapabilities` 派生出 `env.*` 能力。未知能力、缺失能力、
  ToolId 冲突一律 fail closed，不得降级为“部分可用”。
- `ModuleRegistry` 使用与 `ModelPackage` 同型的状态机
  （`Discovered -> Verified -> Staged -> Active -> Deprecated -> Revoked`，失败进
  `Quarantined`）。注册只发生在 Runtime/Session 初始化和显式部署动作；运行中只允许状态
  降级（含 revoke），不允许动态新增模组。
- 单一事实源、三类投影：LLM 侧按请求物化为 `ExposedToolSpec`（沿用既有
  `tool_snapshot_digest`）；蒸馏 policy model 侧在 ModelPackage manifest 声明绑定的模组，
  只能对绑定模组的工具产生 `ToolIntent` 候选；Replay 使用记录的 module digest，不加载
  真实模组。两个执行类消费者都必须经 DecisionParser/Planner/Policy 同一门禁，模组不构成
  新的 authority。
- 模组准入不改变单工具调用协议（`ITool`、Policy、confirmation、幂等、至多一次派发），
  也不改变 Executor 生命周期规则；模组只回答“这台设备上哪些工具存在且可用”。

## 备选方案

- 按平台名/设备型号硬编码工具集：违反“Core 不按平台名猜能力”的既有边界，无法承载同一
  设备的能力动态变化（权限撤销、epoch 失效），不采用。
- 每个工具独立注册、由任务临场拼装：没有可版本化交付的聚合单元，无法整组启用/撤销，
  蒸馏控制器没有稳定的绑定目标，不采用。
- 给蒸馏控制器开私有工具直连通道：绕过 Decision/Planner/Policy 单一汇合点，违反
  DEC-006 与总计划 `RULE-04`，不采用。
- MCP 式运行时动态发现与热插拔：与“运行中不能由模型新增 native Tool”的安全立场冲突；
  远端模组分发可作为未来演进方向，需届时另立 DEC，v1 不采用。

## 影响与风险

- 增加 manifest 校验、Registry、协商和事件的实现与测试成本；属于 M7（Tool 隔离与产品面）
  的前置设计，M5 的 policy candidate 路径将提前引用其绑定契约。
- `CapabilityCatalog` 治理不当会演变为新的隐性耦合：能力词汇的新增和语义变更必须经过
  评审，不能由单个模组私造。
- `HostProvided` 来源信任首期采用“宿主显式注入 + allowlist”的暂定默认值，attestation
  机制最迟在 M7 冻结（负责人：Mira Maintainers）。
- 模组冲突解析首期保守（后注册者被拒绝并记录），可能要求宿主调整模组划分而不是由
  Runtime 自动仲裁。

## 验证方式

- 协商为纯函数：对 `EnvironmentCapabilities` 组合与 Registry snapshot 的 golden 契约测试，
  同输入同输出 digest。
- fail closed 负向测试：未知 capability、缺失 capability、跨模组 ToolId 冲突、签名/digest
  校验失败、资源上限超限。
- 双消费者一致性测试：policy model 绑定集 ⊆ Session 协商通过集 ⊆ Active 模组成员；LLM
  暴露集与 policy 绑定集源自同一 Registry snapshot；越权 ToolIntent 候选被拒绝。
- 生命周期测试：shutdown 顺序（producer → revoke → cancel → 进程结算 → Registry 关闭）、
  revoke 时在途 invocation 结算且不开启后续动作、迟到 completion 不复活。

## 关联文档和工作项

- [工具模组设计](../design/tool_module_design.md)
- [Model Provider 与 Tool 扩展设计](../design/model_provider_and_tool_design.md)：ToolSpec、
  调用协议与 per-request 暴露
- [本地感知与任务 ONNX 模型设计](../design/local_perception_and_task_models.md)：policy
  candidate 的模组绑定
- [Mira 实施总计划](../plans/mira-implementation-plan.md)：M7（M5 policy candidate 引用）
