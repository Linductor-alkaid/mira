# 维护计划：会话终态投影

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：DEC-016、ConversationLoop、M17
> 更新日期：2026-11-07

## 目标与范围

修复 `BUG-20261107-002`：build_conversation_view 未识别 ConversationSettled，静默
丢失运行结果。依据 [DEC-016 补充](../decisions/DEC-016-conversation-events-and-user-messages.md)、
[持续会话设计](../design/continuous-conversation-loop.md) 和
[Context Intelligence 设计](../design/context_intelligence_design.md)。
保留 LoopSettled；新增 ConversationSettled 投影及计数摘要；明确未知事件跳过与
已识别载荷 fail closed 的边界。不修改生产者、持久化 schema、checkpoint reducer、
长期记忆固化器或 Executor 生命周期。

## 工作项与验收

- [x] `MNT-202611-15`：同时投影两类终态；旧 steps 摘要不变，新事件呈现可用计数。
- [x] 真实 ConversationLoop 的用户注入/工具往返/终态事件能重建视图和 M17 分段；
  顺序、源 ID、时间戳、水位及重复重建保持正确。
- [x] 空会话、可选字段缺失、无关畸形事件跳过；三类已识别事件的损坏载荷返回 DataLoss。
- [x] 相关构建/回归、文档、架构与平台边界检查通过；API、设计和决策同步。

## 风险与限制

重建后的视图会多出先前遗漏的 LoopOutcome；缓存和下游派生资产须由宿主决定重建。
运行结果摘要不是模型答案。不会把其他直接消费事件的模块自动升级为 ConversationLoop
消费者。本项只验收 Linux 离线 fixture。

- [ ] Windows/Android 当前变更验证未执行：本轮缺少对应环境，负责人 Mira Maintainers；
  补跑条件为对应 CI/工具链及需要设备的运行验证。
- [ ] mira-bridge 跨突发真实回放未执行：本轮无桥接宿主环境，负责人部署宿主维护者；
  补跑条件为升级后重建真实事件日志视图及派生资产，保存回放证据。

## 验证记录

2026-11-07：开始实施，先以真实循环输出和投影契约测试复现；未执行项不计为通过。

2026-11-07：定向回归已复现并修复。

- 环境：`adb5c71` 基础上的未提交工作树（保留同会话私网端点修复），Linux x86_64、
  GCC 13.3.0、C++20、Debug、tests/warnings-as-errors 开启，kairo pin `78c6f55`。
  仅使用脚本化模型与 MemoryEventStore，无真实网络请求或平台输入。
- 修复前：配置并构建新增测试后，运行
  `ctest --test-dir build/debug -R '^mira_m3_conversation_projection_test$' --output-on-failure`
  失败于真实循环后的 `view && view.value().size() == 2`；循环本身已 Answered，
  证明终态事件存在而投影遗漏，未以合成事件代替生产者闭环复现。
- 修复后：同一测试及既有 AgentLoop 投影、工具桥接、M17 检索/分段测试 **4/4** 通过
  （0.44 秒）。覆盖两类终态、真实工具往返、元数据/重建/分段、未知畸形事件跳过、
  三类已识别事件损坏返回 DataLoss，以及可选计数和 int64 上限精确展示。

  ```bash
  cmake --preset debug
  cmake --build build/debug --target mira_m3_conversation_projection_test mira_m3_tool_executor_test mira_m3_tool_loop_test mira_m17_context_retrieval_test -j 4
  ctest --test-dir build/debug -R 'mira_m3_(conversation_projection|tool_executor|tool_loop)_test|mira_m17_context_retrieval_test' --output-on-failure
  ```

- 同步：DEC-016 增补、公共头契约、模型 API、持续会话设计、Context Intelligence 设计
  与总计划入口。全量构建和测试仍在执行，本记录不替代最终验收。

2026-11-07：最终验收，按上述 Linux 离线范围关闭 `MNT-202611-15`。

- 同一工作树 Debug 全量构建通过，CTest **152/152** 通过、无失败/跳过，用时
  37.64 秒。包含真实循环投影回归、旧 AgentLoop 工具/投影、M17/M19、公共头独立包含、
  安装消费者及同会话私网端点回归。

  ```bash
  cmake --build build/debug -j 6
  ctest --test-dir build/debug --output-on-failure -j 4
  python3 tools/check_docs.py .
  python3 tools/check_architecture.py
  python3 tools/check_platform_boundary.py
  clang-format --dry-run --Werror tests/m3/m3_conversation_projection_test.cpp
  git diff --check
  ```

- 文档链接/围栏、架构、Core 平台边界、新测试格式及空白检查全部通过；没有新增依赖、
  并发路径或 Executor 能力绕行，没有修改架构 baseline 或第三方代码。
- 本轮只修复可重建视图，保留生产者事件与持久化格式。Windows/Android 和真实 bridge
  回放未执行；风险节中的负责人、补跑条件及未勾选项继续保留，不以本地结果替代。


2026-11-07：PR 提交前基于最新主干重新验收。

- 从原 M29 工作分支提取本次修复，独立分支 `fix/model-endpoint-and-conversation-view`
  基于 `4917397`（#116 extra_body、#117 流式连接策略均保留），不携带未合并的 M29 提交。
  原分支与其 151/152 项历史证据保留，不把历史结果当作新基线的验证。
- 冲突处理脚本曾截断文件，首次配置失败；差异核对发现后恢复完整主干文件并重新合并，
  未推送该中间状态。发布前全仓格式检查发现的两处格式问题也已修正。
- 最终 Linux GCC 13.3.0 / Debug / warnings-as-errors 构建与全量 **145/145** 通过，
  37.65 秒，无失败/跳过。差异确认只包含本次两项修复；新增私网端点、会话终态投影，
  主干 extra_body/连接策略及公共头/安装消费者回归均通过。
- 命令：`cmake --preset debug`、`cmake --build build/debug -j 6`、
  `ctest --test-dir build/debug --output-on-failure -j 4`；
  `cmake --build build/debug --target format-check`（316 文件）、
  `python3 tools/check_docs.py .`、`python3 tools/check_architecture.py`、
  `python3 tools/check_platform_boundary.py`、`git diff --check` 全部通过。
- 发布门禁：本 PR 最终 head 的 GitHub Actions 全部通过后才合并，commit/run 与最终
  状态由关联 PR 的 Checks 保存。CI 可提供 Windows contract 和 Android 交叉编译证据，
  不替代真实宿主/真机/SSH 隧道互操作；前述未执行项的设备级范围仍须补跑。
