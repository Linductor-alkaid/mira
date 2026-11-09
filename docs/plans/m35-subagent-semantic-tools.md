# M35：子代理语义选工具与副作用执行

> 状态：In Progress
> 日期：2026-11-09
> 负责人：Mira Maintainers
> 所属计划：[总计划](mira-implementation-plan.md)
> 决策：[DEC-100](../decisions/DEC-100-subagent-semantic-tools.md)

## 工作项与退出条件

- [x] M35-01：保持只读兼容，公开自主目录、发现/加载、策略和效果验证契约。
- [x] M35-02：原生工具动态暴露、可执行副作用、失败封闭审计与父结果元数据。
- [x] M35-03：目标委派到发现/选择/执行/验证/回流集成测试；拒绝、异常、取消、
  deadline、接管、shutdown 与日志失败回归。
- [ ] M35-04：设计/API/示例同步、完整本地检查与 PR 提交。

## 验证记录

2026-11-09：由用户授权在 master `8d7a4f9` 开始，分支
`feat/subagent-semantic-tools`。沿用 M34 的 kairo worker，无新增并发基础设施。
已实现目录快照、分页/可选语义检索、模型选择激活集、原生工具执行与独立 Verify，
父上下文只接收结果。保持默认静态只读模式。新增离线文件发布宿主示例。

首次测试发现 MemoryEventStore 仅提供 Buffered 确认，因此写工具按设计被阻止；
测试改用显式耐久确认 double，示例使用 FileEventStore，并增加弱确认拒绝用例。
生命周期 fixture 首版在有限 worker 等待整个 run，再从外部 shutdown，造成测试
自身违反 producer/worker 关闭次序并超时；改为外部 owner 分离 start/finish 后通过。
首次文档检查把 lambda 的 `](` 当作链接，调整示例空格后通过。失败未计作验收完成。

2026-11-09：Linux x86_64、GCC 13.3.0、C++20、kairo pin `78c6f557`。
完整 Debug 构建与 CTest **155/155**（38.24 秒）通过；ASAN+UBSAN 定向 **7/7**
（8.03 秒）、TSAN 定向 **7/7**（6.51 秒，使用 CI 同样的 `setarch x86_64 -R`）通过。
覆盖 M34 四项兼容/生命周期回归、M35 自主工具专项及两个离线消费者。
六个受影响/新增编译单元经 clang-tidy 18.1.8 严格检查，无用户代码诊断；
format-check（332 文件）、docs/SBOM/platform-boundary/architecture 门禁与 diff 检查通过，
架构 baseline 仍 16 项，无新违规，无第三方修改。

M35 专项验证：131 项目录不注入父模型或初始子请求；分页/语义检索、选择加载、
卸载后拒绝；父只给目标也可发布并独立 Verify。覆盖策略拒绝/异常、未知与重复目录、
缺失 verifier、输入容量、handler/Verify 异常、已应用但 receipt 错误、派发前/后日志失败、
弱耐久确认、未知效果阻断同批后续调用；执行中取消、超时、Human Takeover、shutdown、
close 超时重试，以及取消后的已验证/未知效果保留。文件发布示例使用真实 FileEventStore，
通过重读文件验证写入。本地证据不替代真实模型选工具质量、Windows/Android 验收。

可复现命令：

```bash
cmake --preset debug
cmake --build build/debug -j 6
ctest --test-dir build/debug --output-on-failure -j 4
cmake --preset asan -DMIRA_ENABLE_UBSAN=ON
cmake --build build/asan --target mira_m35_subagent_tools_test mira_m34_subagent_test mira_m34_continuation_test mira_m34_subagent_lifecycle_test mira_m34_subagent_bounds_test mira_subagent_tool_host_consumer mira_subagent_host_consumer -j 4
ctest --test-dir build/asan -R '^mira_m3[45]_|^mira_subagent_.*consumer_test$' --output-on-failure
cmake --preset tsan
cmake --build build/tsan --target mira_m35_subagent_tools_test mira_m34_subagent_test mira_m34_continuation_test mira_m34_subagent_lifecycle_test mira_m34_subagent_bounds_test mira_subagent_tool_host_consumer mira_subagent_host_consumer -j 4
setarch x86_64 -R ctest --test-dir build/tsan -R '^mira_m3[45]_|^mira_subagent_.*consumer_test$' --output-on-failure
clang-tidy -p build/debug --warnings-as-errors='*' src/model/subagent_tool_session.cpp src/model/subagent_child.cpp src/model/subagent.cpp src/model/subagent_tools.cpp examples/subagent_tool_host_consumer.cpp tests/m35/m35_subagent_tools_test.cpp
cmake --build build/debug --target format-check docs-check sbom-check platform-boundary-check architecture-check
git diff --check
```

本地代码、示例和验证就绪，PR 发布与远端 CI 由下一步完成/验收。

## 未执行验证

- [ ] Windows/Android、真实模型和桥宿主：负责人 Mira Maintainers/部署维护者，
  在对应工具链和服务可用时固定模型/profile 补跑。离线脚本不证明模型语义判断质量。
