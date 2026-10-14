# 实时思考预览维护

> 状态：Completed（Linux 离线 fixture 范围）
> 日期：2026-10-14
> 负责人：Linductor-alkaid
> 所属：[实施总计划](mira-implementation-plan.md)
> 依据：[DEC-055](../decisions/DEC-055-live-thinking-preview.md)、Mirage MIRA-20261014-004

- [x] MNT-THINK-PREVIEW-01：独立 opt-in 思考预览，三方言公开文本路由、UTF-8
  最新有界快照、取消/异常/尝试清空与旧正文语义保持；测试及 API 同步。

## 验证与退出条件

三方言/Provider fixture、gateway/ConversationLoop 回归及 Debug/ASAN/UBSAN、架构
检查通过后关闭 Linux 离线范围；本轮没有供应商在线或 Windows 真机，负责人为维护者，
补跑条件为目标环境与供应商配置到位。Mira 不实现气泡形状、滚动或摘要策略。

## 验证记录

2026-10-14：基于 1befa45 创建独立 worktree，新增公开 opt-in sink 与解析器 pending
片段入口。没有修改内嵌依赖。测试结果待实际执行后追加，不用 admission 代替成功。

2026-10-14：最终相关 Debug 5/5、ASAN 3/3、UBSAN 3/3 和架构检查通过。命令：

```sh
cmake --preset debug -DMIRA_BUILD_EXAMPLES=OFF
cmake --build --preset debug --target mira_m3_thinking_preview_test mira_m3_sse_test mira_m3_anthropic_test mira_m3_gateway_test mira_m3_conversation_loop_test -j4
ctest --test-dir build/debug -R 'mira_m3_(thinking_preview|sse|anthropic|gateway|conversation_loop)_test' --output-on-failure
cmake --preset asan -DMIRA_BUILD_EXAMPLES=OFF
cmake --build --preset asan --target mira_m3_thinking_preview_test mira_m3_sse_test mira_m3_anthropic_test -j4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/asan -R 'mira_m3_(thinking_preview|sse|anthropic)_test' --output-on-failure
cmake --preset ubsan -DMIRA_BUILD_EXAMPLES=OFF
cmake --build --preset ubsan --target mira_m3_thinking_preview_test mira_m3_sse_test mira_m3_anthropic_test -j4
ctest --test-dir build/ubsan -R 'mira_m3_(thinking_preview|sse|anthropic)_test' --output-on-failure
python3 tools/check_architecture.py .
git diff --check
```

Linux x86_64/GCC，实际 tests 在 Provider 返回前收到思考快照；覆盖 gateway 透传、
三方言已公开思考片段、UTF-8/0/小预算裁切、16KiB 快照保留最新、清空、签名/redacted
排除、回调异常计数、取消后停止与断流仍失败。初轮测试夹具的超长单 SSE 行、遗漏
Executor 头与缺 system prompt 已修正，最终命令通过。ASAN 关闭 leak 检测，未声称 LSAN。
完整 Debug 回归另行执行，其结果不由上述选择集代替；Windows/Android/供应商在线
未在本机执行，负责人维护者，补跑条件为目标 CI/凭据环境到位。无新跨上下文任务路径，
TSAN 由后续宿主接线验证。

2026-10-14：完整 Debug 构建与回归 106/106 通过（`cmake --build --preset debug -j8`、`ctest --test-dir build/debug --output-on-failure -j4`）。PR#94 首轮 Linux/Windows/Android 与 ASAN/UBSAN/TSAN job 已通过；quality 的 clang-format 检查发现 Provider 新增语句四处换行，修正后本地 format-check、architecture-check 通过，补推后以当前 CI 为准。
