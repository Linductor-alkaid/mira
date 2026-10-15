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
- [ ] `MNT-202610-18` **In Progress**：长脚本、协议/故障回归、sanitizer、文档与 PR 交付证据。本机验收完成，PR 评审与目标平台 CI 交付中。

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
  已覆盖最新源码，不把初轮失败删除。首次 docs 脚本遗漏根目录参数，补 `.` 后通过。
- 限制：Windows/Android 与远端 quality/sanitizer 待本 PR CI；真实 provider 长任务
  待 Mirage 升级消费后验收，负责人为 Mira/Mirage 维护者。当前不升级 Mirage pin，
  不把上游 fixture 通过当作下游问题已修复。


2026-10-15：上游提交 `7e2b73e`，创建
[PR #99](https://github.com/Linductor-alkaid/mira/pull/99)，尚未合并。

- 最终源码的四个变更实现单元独立 clang-tidy 通过（`clang-tidy -p build/debug
  src/model/conversation_loop.cpp src/model/model_budget.cpp src/model/tool_executor.cpp
  src/model/model_contracts.cpp`）；无自研代码诊断。
- ASAN+UBSAN 联合构建：`cmake -S . -B build/asan-ubsan-continuous
  -DCMAKE_BUILD_TYPE=Debug -DMIRA_ENABLE_ASAN=ON -DMIRA_ENABLE_UBSAN=ON
  -DMIRA_WITH_MBEDTLS=OFF -DMIRA_WITH_OPENSSL=OFF`。
- TSAN 构建：相同配置目录换为 `build/tsan-continuous`，sanitizer 开关换为
  `-DMIRA_ENABLE_TSAN=ON`；两个配置均用 `cmake --build <目录> --target
  mira_m3_continuous_conversation_test mira_m3_conversation_loop_test
  mira_m3_conversation_history_transform_test mira_m3_supervisor_budget_test
  mira_m3_canonical_test mira_m3_tool_executor_test -j3`。
- 测试正则：`^mira_m3_(continuous_conversation|conversation_loop|conversation_history_transform|supervisor_budget|canonical|tool_executor)_test$`。
  ASAN_OPTIONS=`detect_leaks=1:halt_on_error=1`、UBSAN_OPTIONS=
  `halt_on_error=1:print_stacktrace=1`，ASAN+UBSAN **6/6 通过**；TSAN 使用
  `setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 ctest --test-dir
  build/tsan-continuous -R <上述正则> --output-on-failure`，**6/6 通过**。
  本轮 sanitizer 不构建 TLS adapter，不据此声明 TLS 的 sanitizer 验收；默认
  TLS 的完整 Debug 已由 109/109 验证，PR CI 另跑默认 sanitizer 全量组合。
- PR CI 为 push 与 pull_request 两套矩阵，当前结果在 PR checks 中跟踪；
  MNT-202610-18 和最后退出条件保持未勾选，待目标平台与完整 quality 结果。
- Mirage 已关联 PR 并保持反馈 Open；本 PR 不自动合并、不升级下游 pin。


2026-10-15：首轮远端 GCC Release 构建发现新增签名 thinking fixture 的
vector<variant>::insert(begin, ThinkingPart) 在 GCC 13 `-O3` 下触发
`-Werror=maybe-uninitialized`（[失败 job](https://github.com/Linductor-alkaid/mira/actions/runs/38694641466/job/116288246288)）。
本机从 debug compile_commands 提取该测试 TU 的原命令，另写 `/tmp` 对象并追加
`-O3 -DNDEBUG`，修改前 exit=1 可复现；将 fixture 改为依次 emplace thinking、
追加 calls 后同命令 exit=0。未降低告警门禁，生产实现未变。
修正 fixture 与补充头文件注释后的 Debug/ASAN+UBSAN/TSAN 复验正在执行；
旧提交的 CI 被取消以释放额度，最终矩阵以 PR 当前 head 为准。


2026-10-15：补充持续模式退出条件审查。Continuous 的恢复/输出续写额度改为
连续失败/截断段上限，完整工具轮结算后重置；累计数改为饱和 uint64。避免
任务持续有进展却因多个独立小故障耗尽整任务额度，Finite 原行为保留。
新增四段独立恢复/截断均能完成、同配置 Finite 仍按整任务额度停止的对照测试。
同时保留旧 ConversationSettled.turns 的记录数含义，用新增 total_turns 报推进
尝试数，避免改变旧观察者语义。该增量须重跑 Debug、sanitizer 与 PR CI。


2026-10-15：最终实现 `23e3ff0` 复验完成，生产源码此后保持不变。

- `cmake --build build/debug -j4` 与全量 `ctest --test-dir build/debug
  --output-on-failure -j4`：**109/109 通过**（37.65 s）。
- 前述六个目标在 ASAN+UBSAN / TSAN 目录重编译后，以同一正则和 sanitizer
  选项重跑：分别 **6/6 通过**（5.91 s / 10.05 s）。
- 新增对照：四段独立恢复或截断、段额度=1，Continuous 最终 Answered、9 次
  推进/4 个工具、累计恢复/续写=4；同配置 Finite 在第二段停止，工具计数=1。
- `-O3 -DNDEBUG` 的最终测试 TU 编译 exit=0；变更 loop 的独立 clang-tidy
  再次通过。format/docs/SBOM/platform-boundary/architecture 门禁全部通过。
- [PR #99](https://github.com/Linductor-alkaid/mira/pull/99) 的当前 head checks
  是远端矩阵结果入口；此前中间提交的运行已取消，不能把 cancelled 计为通过。
  MNT-202610-18 保持 In Progress，等待 PR 评审及目标平台交付；不以本机验收
  关闭整体维护计划。最终 CI 结论同时记录在该 PR 描述中，不为追记 CI 状态
  反复触发没有代码变化的整套矩阵。
