# 记忆代理与长期上下文管理调研

> 状态：Completed（静态调研范围）
> 负责人：Mira Maintainers
> 更新日期：2026-11-09
> 所属计划：[总计划](mira-implementation-plan.md)
> 关联工作：M29-01、M30-01/03/04/08；后续容量与主动行为分别归 M31、M33
> 依据：[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)、
> [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)、
> [DEC-070](../decisions/DEC-070-long-term-memory-behavior-contract.md)、
> [DEC-099](../decisions/DEC-099-managed-subagent-delegation.md)、
> [DEC-100](../decisions/DEC-100-subagent-semantic-tools.md)

## 目标与范围

`MNT-202611-MA-01`：回答独立记忆 agent 能否承担记录、搜集、上下文候选装配、
会话路由建议与压缩；比较公开一手资料与 Mira 当前代码，提出可评审的责任边界、
一致性协议、落地顺序及验收矩阵。研究输入不改变已生效契约，不预先批准新 API，
不把 M29–M33 的 Planned 状态改为实现完成。

## 工作项与退出条件

- [x] 核对当前代码、现有设计及 M29–M35 的可复用能力与限制。
- [x] 核对外部一手资料，区分论文机制、产品说明和 Mira 侧推论。
- [x] 交付研究报告，覆盖路由、读写、压缩、竞态、取消、删除与长期容量。
- [x] 同步总计划和相关里程碑的研究入口，文档链接与 diff 检查通过。

## 风险与限制

本轮不修改 C++、公开契约、存储或 third_party；不安装外部系统、不调用真实模型，
不执行质量/性能实验。语义准确率、长期漂移和目标平台资源占用未验证，后续由
Mira Maintainers 在专项立项后固定模型、数据集与目标设备补跑。研究本身不新增
并发路径；Executor 能力是否足够须在实现 API 选型时核对，不推定能力缺口。

## 验证记录

2026-11-09：基于 `62ad918` 开始静态调研，分支 `docs/memory-agent-reference-study`。
交付物为[研究报告](../design/memory-agent-reference-study.md)，研究结论与功能交付分开结算。

2026-11-09：静态调研完成。核对 ConversationLoop 的 history 变换/容量检查、完整
request 组装、ContextManager 接口及 M34/M35 生命周期；记录完整请求预算接线、
独立后台 owner、纠正/删除屏障与 terminal 后提取的方案边界。外部证据来自报告内
链接的一手论文与官方文档，访问日期及论文版本已记录；不宣称复现其基准结果。

新增研究报告与本计划，同步总计划和 M29/M30 的研究入口；
`python3 tools/check_docs.py .` 与 `git diff --check` 通过。
未运行 C++ 构建、模型或长期运行实验，因为本轮仅改变文档；功能验收矩阵仍未执行，
实施与补跑责任、环境及条件见“风险与限制”及研究报告第 10–11 节。
