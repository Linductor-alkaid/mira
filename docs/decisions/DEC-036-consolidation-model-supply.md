# DEC-036：语义固化与 Working Context 的模型供给口径（可用源模型，不要求专用小模型）

> 状态：Accepted
> 日期：2026-09-14
> 决策人：Mira Maintainers（维护者指示）
> 需求来源：维护者 2026-09-14 指示（针对 [issue #39](https://github.com/Linductor-alkaid/mira/issues/39)
> 与 [issue #48](https://github.com/Linductor-alkaid/mira/issues/48) 中的"独立小模型"供给假设）
> 修订对象：[DEC-032](DEC-032-context-intelligence-layered-context.md) 第 2/5 条、
> [DEC-035](DEC-035-context-curator-working-context.md) 非目标、
> [Context Intelligence 设计](../design/context_intelligence_design.md) §5.4/§12/§13/§14、
> [Context Curator 设计](../design/context_curator_design.md) §6/§13/§14

## 背景与问题

DEC-032（源自 issue #39）与 DEC-035（源自 issue #48）把 Layer 3 语义固化
（`ISemanticConsolidator`）与 Stage W2 `IContextCurator` 的模型供给默认方向设为
"独立小模型"（Qwen ~0.6B 级本地模型或既有 Provider 廉价云模型），并把"固化小模型
供给通道（供应链复核）"设为 Stage D 真实模型轮与 Stage W2 的门禁前置。

维护者 2026-09-14 指示：上下文压缩/固化不需要专用小模型——直接调用当前可用的源
模型即可，包括生成原输出上下文的主模型。

## 决策

1. **供给口径**：固化 / 压缩 / Curator 的模型供给经 `IModelProvider` 直接调用已配置
   的可用源模型，**包括生成原输出上下文的主 Agent 模型**；不要求、不等待专用小模型。
2. **"独立小模型"降级为可选优化**：Qwen ~0.6B 级本地模型、廉价云模型等不再是默认
   方向、推荐配置或里程碑门禁。宿主仍可出于成本/延迟自行注入其他 profile；若引入
   新的本地或捆绑模型，仍按 DEC-032 §14 供应链复核执行。使用既有 Provider 已配置
   的模型不新增供应链项。
3. **其余纪律不变**：Core 不绑定任何模型；`generated_by` 记录实际模型 profile；
   固化/快照产物仍是 `UntrustedExternalData` 派生投影（RULE-09）、可重建
   （RULE-07）、有界（RULE-08）；语义质量声明仍受 RULE-10 约束（只能出现在有
   真实模型证据的对应阶段）。
4. **受影响口径同步**：Stage D 真实模型轮、Stage E/F 与 Stage W2 的"小模型供给
   通道"门禁改为"可用源模型供给"。M17–M20 已交付契约、测试与评估基线作为历史
   证据保留，不追溯改写。

## 非目标

- 不改变接口：`ISemanticConsolidator` / `IContextCurator` 仍经 `IModelProvider`
  注入，宿主可配置任意 profile。
- 不强制主模型承担固化：宿主可按成本/延迟选择其他可用源模型；本决策只移除
  "必须专用小模型"的约束，不引入"必须主模型"的反向绑定。
- 不修改 M17–M20 已交付的契约、测试与评估基线记录（含 M19 harness 报告中的
  "real small model" 限制文案，其为该轮评估时的历史口径）。

## 备选方案

- **维持"独立小模型"为默认方向并等待供应链复核**（否决）：为上下文整理引入一条
  额外模型依赖与复核等待链。其收益（成本优化）本就是宿主可选能力，而在可用源
  模型已经过互操作验证（如 SiliconFlow `Qwen3.5-4B` 受控 profile，见
  [兼容性矩阵](../compatibility/openai-compatible-matrix.md)）的情况下，等待链
  没有必要性，只会拖延真实模型证据的产生。
- **把固化强绑定主 Agent 模型**（否决）：与"固化不绑定 Agent 主模型"的既有接口
  纪律冲突（DEC-032 §5），也剥夺宿主的成本优化空间。正确表述是"可用源模型皆可，
  主模型含在内"。

## 影响与风险

- 解除 Stage W2、Stage D 真实模型轮与 Stage E/F 的"小模型供给"等待项：真实模型
  与真机评估可走既有 Provider 通道执行。
- 风险：主模型承担固化抬高 token 成本与请求延迟。处置：固化调用经
  `ContextMemorySupervisor` 的 Deferrable 路由与 coalescing（DEC-035 §6、
  Curator 设计 §8），且宿主可随时改配低成本 profile——成本优化是可选项，不是门禁。
- 风险：文档中残留的"小模型"表述与决策口径不一致。处置：活文档（设计、决策修订
  注、README、公开头注释）同步修订；历史计划与评估报告保留原文不改写。

## 验证方式

- 文档同步以本决策链接为准：两份设计文档、DEC-032/DEC-035 修订注、README 状态表、
  固化评估基线的前瞻口径、`context_consolidation.hpp` 公开注释。
- 公开头文件注释属源码变更：编译与既有门禁（M19/M20 测试与评估 harness）复验，
  结果不因本决策改变（注释级变更，行为无 diff 语义）。

## 关联文档和工作项

- [DEC-032](DEC-032-context-intelligence-layered-context.md)、
  [DEC-035](DEC-035-context-curator-working-context.md)
- [Context Intelligence 设计](../design/context_intelligence_design.md)、
  [Context Curator 设计](../design/context_curator_design.md)
- [固化管线评估 v1](../benchmarks/context-intelligence-consolidation-v1.md)
- [Issue #39](https://github.com/Linductor-alkaid/mira/issues/39)、
  [Issue #48](https://github.com/Linductor-alkaid/mira/issues/48)
