# Subagent 执行与协作能力调研

> 状态：Completed（静态调研范围）
> 负责人：Mira Maintainers
> 更新日期：2026-11-09
> 所属计划：[总计划](mira-implementation-plan.md)
> 依据：[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)、[M24](m24-context-curator-stage-w5.md)、[M25](m25-host-integration-round.md)

## 目标与范围

`MNT-202611-SA-01`：核对 Mira 现状，对 DeepSeek Harness 与 ZCode 的当前源码做
定点比较，识别可复用能力、执行层缺口与后续决策边界。交付研究输入，不修改公开
契约、运行时或 Executor 集成，不把研究建议视为功能承诺。

## 工作项与退出条件

- [x] 固定三方源码基线，区分已实现代码、测试意图、旧计划与未运行结论。
- [x] 比较上下文、身份/运行、模型工具配置、结果回流、取消关闭、容量与恢复。
- [x] 映射 Mira 现状、提出分阶段范围及验收矩阵，并明确 DEC-044 修订需求。
- [x] 研究报告、总计划入口和文档链接检查同步。

## 风险与限制

不执行外部仓库依赖安装、构建、真实模型调用或性能测试。三方运行语义只有静态
源码/测试证据，不声明当前平台互操作；实现立项后由 Mira Maintainers 在固定模型、
宿主和工具链上执行验收。研究本身不新增异步任务，不推定任何 Executor 能力缺口。

## 验证记录

2026-11-09：按用户要求开始调研，独立分支 `docs/subagent-reference-study` 基于
主干 `edef847`，与 PR #120 的 system 模板实现分开。外部仓库只读，固定 commit
与源码入口见[研究报告](../design/subagent-reference-study.md)。

2026-11-09：完成三方静态核查，交付研究报告和总计划入口。DSH 固定 `fcc0ad4`，
ZCode 固定 `a9fe401`；通过源码和测试文件核对 activation 容量/结果、完整轮 fork、
子 runtime 权限、后台通知/idle wake、owner close，明确旧计划与当前代码的区别。
Mira 缺口归属于执行与协作闭环，M24 投影契约与完成证据保持不变。

验证命令 `python3 tools/check_docs.py .`、`git diff --check` 均通过。只新增/修改文档，
未执行 C++ 构建、外部测试或模型实验；研究退出条件不包含运行互操作。P0/P1/P2
均未立项实施，需要新决策、稳定工作项与对应测试后才能标记功能完成。
