# 维护计划：持续会话循环

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：ConversationLoop、HistoryTransformer；Mirage MIRA-20261015-002
> 建议发布点：公开契约增量，经 PR 评审
> 更新日期：2026-10-15

## 目标与范围

按 [DEC-056](../decisions/DEC-056-continuous-conversation-loop.md) 交付显式持续模式、
有限请求预算、提交式压缩与有界结果窗口。保持有限模式和现有协议兼容，不修改
设备 AgentLoop 或 kairo，不在本 PR 自动升级 Mirage pin。

## 工作项

- [x] `MNT-202610-16` 请求 scope 与 ledger 准入、算术边界、序列化兼容。
- [x] `MNT-202610-17` 持续循环、提交式压缩、容量与统计、取消/期限控制。
- [ ] `MNT-202610-18` 长脚本、协议/故障回归、sanitizer、文档与 PR 证据。

## 风险与阻塞

请求级预算不限制任务总花费，必须显式 opt-in。宿主 transformer 仍负责摘要质量
与归档，循环只验证协议、最近轮保护和容量。Windows/Android 由 PR CI 验证；
真实模型长程任务由 Mirage 消费后复验，负责人为对应维护者。

## 测试与退出条件

- [x] 有限和零值兼容，持续脚本超过旧限制后回答且不重复副作用。
- [x] 压缩持续提交，最近协议组受保护，历史/逐轮记录容量有界。
- [x] 请求预算有限；未知 usage/价格不变成累计步数停止，溢出不回绕。
- [x] 模型/工具错误、压缩失败/异常、容量拒绝、取消/deadline/shutdown 明确结算。
- [ ] 适用本机验证和文档门禁完成，PR 创建且 CI 结果如实登记。

## 验证记录

2026-10-15：从 `origin/master`（`560da5ec`）建立
`feat/continuous-conversation-loop`；原仓库未跟踪的 `docs/charactor/` 保留。
维护者已明确授权本机 Mira 开发及 PR 流程；不修改 Mirage 的 pinned 源码。


2026-10-15：实现与 Linux Debug 验证（基线 `560da5ec` + 本分支工作树）。
环境：Linux x86_64、GCC 13.3.0、CMake 3.28.3，默认 TLS 构建组合。

- 命令：`cmake --preset debug`、`cmake --build build/debug -j4`、
  `ctest --test-dir build/debug --output-on-failure -j4`；**109/109 通过**。
- 新脚本实际执行 1,025 模型轮 / 1,024 工具，同一 task/epoch，模型单次输出
  上限 16,384；compactor 收到的历史最多 5 条、提交后最多 3 条；结果仅存 4 轮，
  dropped=1,021。注册表 retained_dispatch_count=0。fixture 自身保存全部请求，
  此处证明内部存量边界，不把进程 RSS 宣称为生产长期内存平台期。
- 负向覆盖：有限/零值、单轮工具拒绝、无 compactor / Error / exception / 空返回 /
  拆配对 / 删除最近轮、条目/字节容量、签名多工具（含重复 call 折叠）、模型认证
  失败、工具参数失败回填、恢复与续写耗尽、预取消、执行中取消、deadline、迟到
  准入拒绝、kairo worker 停止与外部 shutdown。
- Request scope canonical 往返与未知 scope / v1 scope 拒绝、默认 Task 字段省略、
  账本逐请求准入、旧未决预约保留、失败仅释放本次、有限包络及账本溢出覆盖。
- `cmake --build build/debug --target format-check docs-check sbom-check
  platform-boundary-check architecture-check` 通过；本轮最终版本 clang-tidy 与
  ASAN+UBSAN、TSAN 定向验证仍在执行，结果后补。
- 初轮新增测试编译因 fixture 缺少 kairo 完整头、错误聚合初始化及误用提交/关闭
  返回类型失败，已按 pinned API 修正；一次增量验证链接到修改前的 ConversationLoop
  对象，错误暴露为 operation 集合仍增长，强制重编译后消失。最终全量构建/109 项
  已覆盖最新源码，不把初轮失败删除。首次 docs 脚本遗漏根目录参数，补 `. ` 后通过。
- 限制：Windows/Android 与远端 quality/sanitizer 待本 PR CI；真实 provider 长任务
  待 Mirage 升级消费后验收，负责人为 Mira/Mirage 维护者。当前不升级 Mirage pin，
  不把上游 fixture 通过当作下游问题已修复。
