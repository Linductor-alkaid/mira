# Messages 自适应思考维护

> 状态：Completed（Linux 本地交付）
> 负责人：Linductor-alkaid
> 更新日期：2026-10-07
> 所属计划：[Mira 实施计划](mira-implementation-plan.md)
> 决策：[DEC-051](../decisions/DEC-051-messages-adaptive-thinking.md)

- [x] MT-01：ThinkingMode / ThinkingPart、规范 JSON/摘要、Messages 编码和同步/SSE解析。
- [x] MT-02：完整 assistant 内容在工具结果前回填；非法角色、协议、模式及预算显式拒绝。
- [x] MT-03：Debug/消毒器与 Mirage 真实 MiniMax 回归、依赖提交和 PR。

验收：m3_canonical/schema/dialect/sse/gateway/anthropic/conversation_loop 测试；拆分每个 SSE
字节边界，签名与 redacted 内容回填，保持正文预览隔离；下游空会话恢复及思考选项传递。
Windows、其他供应商和 legacy extended thinking 未验收；负责人维护者，目标平台 CI 可用后补跑。
沿用 pre-kairo 依赖基础，Executor 没有修改或新增 pin。上游合并需在其当前基础复验。

## 2026-10-07 验收

基线 7795e13（Messages PR#80），Linux x86_64/GCC、Debug Ninja，Executor 2ae4fc8。
`cmake --build /tmp/mira-thinking-build --target mira_m3_canonical_test mira_m3_schema_test mira_m3_dialect_test mira_m3_sse_test mira_m3_gateway_test mira_m3_anthropic_test mira_m3_conversation_loop_test -j 4`
及 `ctest --test-dir /tmp/mira-thinking-build -R 'mira_m3_(canonical|schema|dialect|sse|gateway|anthropic|conversation_loop)_test' --output-on-failure`
实际 7/7 通过；`python3 tools/check_architecture.py .` 无新违规。

下游 Mirage native-release 全量 52/52、ASAN/UBSAN runtime_service、ipc_protocol、native_agent_integration
各 3/3；TSAN native_agent_integration 在关闭 ASLR 后 208 checks / 0 failures。
真实私有原生窗口 MiniMax-M3 同一会话 adaptive/disabled 两轮成功，均收到正文预览。
额外真实 adaptive harness 调用 wait：2 model steps / 1 tool call，2 次流式正文预览。
签名完整回填、redacted、每字节 SSE 分割与预算/非法参数/角色拒绝由协议夹具验证。

交付通过独立 `codex/messages-thinking` 分支 PR；当前主干已迁移 kairo，本修复基于 Mirage
获授权的 pre-kairo 基线，不能把下游结果当成当前主干、Windows 或其他厂商在线验证。
