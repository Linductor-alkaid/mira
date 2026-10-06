# Conversation streaming maintenance

> 状态：Completed（Linux验证范围）；负责人：Linductor-alkaid；更新日期：2026-10-05。
> 所属：[实施总计划](mira-implementation-plan.md)；依据：[DEC-049](../decisions/DEC-049-conversation-stream-previews.md)。

Mirage维护者授权修复MIRA-20261005-001：由Mira提供真实、有界、非权威的文本预览，
支持Chat Completions SSE并让ConversationLoop透传流式和思考深度选项。
无新线程、队列或调度器，无桌面工具或业务策略。

- [x] MNT-STREAM-01 两种协议的有界预览，回调异常/取消不影响规范响应。
- [x] MNT-STREAM-02 Chat SSE字节分片、工具参数拼接、usage及严格终态校验。
- [x] MNT-STREAM-03 ConversationLoop选项透传、最后一次usage和工具执行计数。

退出条件：相关Debug/Release、ASAN/UBSAN测试与架构检查通过；公开API/设计同步。
Windows等未运行平台由维护者在对应CI补跑，不以Linux结果声明跨平台互操作。

## 验证记录

2026-10-05：基于3716dbf（保持随Mira交付的Executor），不修改内嵌依赖。

```sh
cmake -S . -B /tmp/mira-stream-build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMIRA_BUILD_TESTS=ON
cmake --build /tmp/mira-stream-build --target mira_m3_sse_test mira_m3_conversation_loop_test mira_m3_gateway_test mira_m3_canonical_test mira_m3_dialect_test mira_m3_tool_loop_test -j2
ctest --test-dir /tmp/mira-stream-build -R 'mira_m3_(sse|conversation_loop|gateway|canonical|dialect|tool_loop)_test' --output-on-failure
python3 tools/check_architecture.py .
```

上述六项Debug测试与架构检查通过。覆盖真实回调在infer返回前发生、字节级中文、
Chat工具参数分片、兼容端逐chunk累计usage/type:null、断流/非法终态、预算、回调异常
和取消抑制；ConversationLoop测试验证选项、规范工具回填、usage及工具执行计数。
Mirage下游四项会话/协议/原生集成测试在Debug/Release/ASAN/UBSAN通过；ASAN关闭
系统图形库的leak检测，不能据此声称LSAN或全平台完成。SiliconFlow Chat及MiniMax
实际请求在返回前收到预览，两者均实测规范wait工具结果后的第二轮推理。

Windows、其他Provider及完整供应商协议矩阵未运行；负责人Linductor-alkaid在目标CI
补跑。上游当前master已采用kairo，本补丁仅增加模型API/协议能力，基于迁移前提交以
满足Mirage仍依赖Executor的约定；上游合入需在其当前依赖版本复验，不能以此记录声明CI通过。

PR#79首轮CI发现旧interop probe的聚合初始化在-Wmissing-field-initializers下失败；
新增选项使用显式默认成员初始化修正，保留旧调用源码兼容。CI状态以PR当前结果为准。
