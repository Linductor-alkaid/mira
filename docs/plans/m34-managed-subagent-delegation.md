# M34：受管子代理委派

> 状态：Completed（Linux 离线首期范围，quality 修复已本地验证）
> 负责人：Mira Maintainers
> 更新日期：2026-11-09
> 所属计划：[总计划](mira-implementation-plan.md)
> 前置：DEC-044、M24/M25、DEC-097、DEC-098
> 决策：[DEC-099](../decisions/DEC-099-managed-subagent-delegation.md)
> 设计：[受管委派](../design/managed-subagent-delegation.md)
> 建议发布点：单层无观察推理子代理

## 目标、范围与非目标

交付模型可调用、有界且可取消的子会话委派，子结果能在父模型空闲时自动推动同一
active run 继续。复用 ConversationLoop/Session/Task，不引入独立 Executor owner。
不交付递归团队、任意消息、跨进程 worker 恢复、完整历史 fork 或共享设备写操作。

## 工作项

- [x] `M34-01`：固定决策、公开契约及 continuation source 的有界 fail-closed 语义。
- [x] `M34-02`：受管 parent/child、隔离模型资源、工具入口和准确执行句柄。
- [x] `M34-03`：权限、累计预算/容量、结果产物/事件及取消/接管/关闭闭环。
- [x] `M34-04`：模型工具往返、等待后自动继续、失败/竞态/拒绝和小型默认池测试。
- [x] `M34-05`：宿主示例、API/设计/术语、构建/回归/静态与 sanitizer 验收证据。

## 测试与退出条件

- [x] parent spawn → child Loop → 产物/事件 → parent 后续请求，保持历史和准确来源。
- [x] 无 continuation 默认兼容；source 错误/异常/越界、child 工具越权与递归拒绝。
- [x] 活跃/累计/结果/输入/输出/请求预算、Executor 提交拒绝和关闭后新委派均明确失败。
- [x] 创建中/执行中取消、超时、Human Takeover、close 超时重试和迟到结果不复活。
- [x] 两 worker 默认池仍能完成多个 child；相关 Debug/ASAN/UBSAN/TSAN 回归及门禁通过。

## 风险与未执行验证

- [ ] Windows/Android 与真实模型/桥宿主未执行：本轮优先 Linux 离线 fixture；负责人
  Mira Maintainers/部署维护者，在对应 CI/工具链及真实服务可用后固定 profile 和 SDK
  版本补跑，不据 Linux 证据声明目标平台或真实模型服从。

进程崩溃后的自动继续与跨进程恢复为非目标，不纳入首期退出条件；不得使用 replay
重发外部副作用。

## 验证记录

2026-11-09：按用户授权开始 M34。分支 `feat/subagent-delegation` 暂基于 PR #120
`eb2448d`，其 system 模板缝为依赖；调研文档一并保留。尚未执行验收。


2026-11-09：M34-01..05 按 Linux 离线首期范围完成。基于 `eb2448d` 的工作树，
Linux x86_64、GCC 13.3.0、C++20、Debug、warnings-as-errors，kairo 保持仓库 pin
`78c6f557`。全目标构建成功，全量 CTest **153/153**（37.66 秒，无失败或跳过），
含四个 M34 专项、离线宿主、公共头独立包含与安装消费者。ASAN+UBSAN 与 TSAN
定向均 **5/5**（分别 3.07 秒、2.34 秒），未修改第三方代码或架构 baseline。

验收覆盖真实 parent/child Loop 工具往返、子会话独立模型/工具权限、结果产物与
User 来源、分批结果继续、JSON Schema 和显式上下文供给；规范工具边界上的查询/
取消；累计请求/输入估算/输出额度、活跃/累计/结果容量、坏 profile/工具/输入。
创建中和执行中取消、父 deadline、child timeout、Human Takeover、Executor shutdown、
close 超时重试/自等待拒绝、迟到成功不复活；通过预占准确 worker 名强制 Executor
提交拒绝，并验证事件/产物失败。默认两个有限 worker 均阻塞时父/子仍能完成。

验证过程中的失败与修正：首次专项的事件查询漏传 SessionId，修正 fixture 后通过；
ASAN 改变完成分批时序，暴露脚本只为两个 child 预留三轮父响应，补足合法的第四轮
后通过，未据此改变生产等待语义。首次 Debug 全量只有文档项失败，lambda 被链接
检查器误识别，调整示例空格后全量通过。首次 TSAN 因 ASLR 出现三项启动前
unexpected memory mapping；按仓库 CI 的 `setarch x86_64 -R` 执行后全部通过，
没有将启动失败或未执行的检查记为成功。

可复现命令：

```bash
cmake --preset debug
cmake --build build/debug -j 6
ctest --test-dir build/debug --output-on-failure -j 4
cmake --preset asan -DMIRA_ENABLE_UBSAN=ON
cmake --build build/asan --target mira_m34_subagent_test mira_m34_continuation_test mira_m34_subagent_lifecycle_test mira_m34_subagent_bounds_test mira_subagent_host_consumer -j 4
ctest --test-dir build/asan -R '^mira_m34_|^mira_subagent_host_consumer_test$' --output-on-failure
cmake --preset tsan
cmake --build build/tsan --target mira_m34_subagent_test mira_m34_continuation_test mira_m34_subagent_lifecycle_test mira_m34_subagent_bounds_test mira_subagent_host_consumer -j 4
setarch x86_64 -R ctest --test-dir build/tsan -R '^mira_m34_|^mira_subagent_host_consumer_test$' --output-on-failure
cmake --build build/debug --target format-check
python3 tools/check_docs.py .
python3 tools/check_architecture.py
python3 tools/check_platform_boundary.py
git diff --check
```

格式、文档链接/围栏、架构（存量 baseline 16 项）、Core 平台边界和空白检查通过。
DEC-044 修订、DEC-099、设计/API/术语、研究入口、示例与总计划同步。Windows/Android
和真实模型/桥宿主仍未执行，责任人与补跑条件保留，不扩大本地交付声明。

2026-11-09 发布准备：PR #120 已合并为 `4befd54`，与原依赖 `eb2448d` 的源码树
完全一致。本次分支转到该最新主干独立提交，保留上述验收，rebase 前后检查 feature
源码树一致；远端 CI 的最终 head 验收由维护者完成，不自动合并。

2026-11-09：PR #121 的 push/PR 两次 quality 作业均在 `subagent.cpp:365` 失败，
`const auto failure` 阻止返回值自动移动，触发启用 warnings-as-errors 的
`performance-no-automatic-move`。此前本地格式/架构等检查不代表已运行 clang-tidy，
保留原验证记录并重新打开 M34-05，补齐本次变更编译单元的静态分析与定向回归。
失败证据：[push quality](https://github.com/Linductor-alkaid/mira/actions/runs/42550955441/job/128946191690)、
[PR quality](https://github.com/Linductor-alkaid/mira/actions/runs/42550976786/job/128946260290)。

2026-11-09 quality 修复验收：基于 `570cd99` 的工作树，clang-tidy 18.1.8 使用仓库
`.clang-tidy` 与 `--warnings-as-errors='*'`，本地先复现同一行失败，再将局部错误对象
改为非 const，保持 promise 保存错误与调用者返回错误的语义。未禁用检查或改动
并发/取消/关闭路径。五个受影响模型层编译单元全部通过严格分析（公共头同时被检查），
随后相关构建成功，M34 四项、宿主示例和文档回归 **6/6**（1.37 秒）通过，包括
Executor 拒绝父 worker 的错误返回路径。

```bash
for source in src/model/{conversation_loop,conversation_continuation,subagent,subagent_child,subagent_tools}.cpp; do
  clang-tidy -p build/debug "$source" --warnings-as-errors='*'
done
cmake --build build/debug --target mira_m34_subagent_test mira_m34_continuation_test mira_m34_subagent_lifecycle_test mira_m34_subagent_bounds_test mira_subagent_host_consumer -j 6
ctest --test-dir build/debug -R '^mira_m34_|^mira_subagent_host_consumer_test$|^mira_docs_test$' --output-on-failure
cmake --build build/debug --target format-check docs-check sbom-check platform-boundary-check architecture-check
git diff --check
```

quality 后续五项门禁均通过：329 文件格式、文档、SBOM、平台边界、架构；存量
baseline 16 项不变。本轮仅修复返回对象 const 限定，未重复全量或 sanitizer；原记录
继续保留，新 head 的完整远端 CI 尚待维护者验收，不把定向通过当作远端已全绿。
