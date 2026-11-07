# 维护计划：私网模型端点授权

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M3；DEC-010
> 更新日期：2026-11-07

## 目标、范围与依据

修复 `BUG-20261107-001`：宿主可通过 ModelProfile 显式授权私网/回环模型端点，
默认拒绝策略保持。依据 [DEC-094](../decisions/DEC-094-profile-private-endpoint-consent.md)、
[LLM API 设计](../design/llm-api-protocol-design.md) 和
[安全设计](../security/threat_model_and_confirmation.md) §10。
不修改地址分类器、第三方依赖或 Executor 生命周期，不新增网络实现。

## 工作项

- [x] `MNT-202611-14`：新增宿主授权字段、传递 transport limits、绑定 manifest/digest；
  通过官方 socket 回环与代理集成测试，完成 API、设计、安全、兼容性说明。

## 测试与退出条件

- [x] 默认 pre-write 拒绝，授权后模型请求/响应成功；代理与目标独立授权，重定向仍受 allowlist 约束。
- [x] 文件上传默认拒绝，授权后 upload/delete 经同一官方 transport 成功。
- [x] manifest 明确记录 false/true；仅切换授权即改变 digest，并绑定新 RouteDecision。
- [x] 相关构建、M3/M4 回归及 docs/architecture/platform-boundary 检查通过。

## 风险与限制

授权覆盖 transport 现有私网类别，不是逐 IP/网段授权；默认 digest 也随 manifest 字段改变，
消费者须重编译、重新路由并重建 continuation 绑定。仅计划验证本机 Linux fixture，
不宣称 SSH/vLLM/Ollama 或目标平台互操作；真实服务验证由部署宿主负责人在具备
端点和凭据后补跑，Windows/Android 由维护者在对应 CI/工具链环境补跑。

以下为范围外后续验证，不作为本项 Linux fixture 的完成依据：

- [ ] 真实 SSH 隧道/vLLM 等服务互操作：缺宿主端点和凭据；负责人为部署宿主维护者，
  补跑条件为使用授权 profile 完成真实模型请求与目标验证并保存事件证据。
- [ ] 当前变更 Windows/Android 验证：本轮未提供对应工具链/设备；负责人为 Mira Maintainers，
  补跑条件为对应 CI 构建/测试及需要设备的运行验证。

## 验证记录

2026-11-07：工作树开始实施；尚未执行验证，不计作通过。

2026-11-07：`MNT-202611-14` 按 Linux 本地 fixture 范围完成。

- 环境：`adb5c71` 基础上的未提交工作树，Linux x86_64、GCC 13.3.0、C++20、
  Debug、`MIRA_BUILD_TESTS=ON`、`MIRA_WARNINGS_AS_ERRORS=ON`；kairo pin `78c6f55`。
  测试仅访问进程内脚本 HTTP 服务，凭据为 fixture 值。
- 实现：ModelProfile 尾部追加默认 false 的宿主授权；推理和文件 upload/delete 传递
  同一策略；manifest 始终序列化该字段，切换授权即改变路由摘要。
- 回归：`mira_m3_private_endpoint_test` 覆盖回环、RFC1918、CGNAT、metadata/link-local、
  IPv6 回环默认 pre-write 拒绝，IPv4 回环授权成功、代理两侧独立授权、跨 host
  redirect 拒绝，以及文件默认拒绝/授权上传删除；canonical 测试验证 manifest/digest/route。
- 命令及结果：以下配置、最终构建与全量测试通过，**151/151**，无失败/跳过；其中
  M3 26 项、M4 16 项、公共头独立包含与安装消费者均通过。全量 CTest 用时 38.34 秒。

  ```bash
  cmake --preset debug
  cmake --build build/debug -j 6
  ctest --test-dir build/debug --output-on-failure -j 4
  python3 tools/check_docs.py .
  python3 tools/check_architecture.py
  python3 tools/check_platform_boundary.py
  clang-format --dry-run --Werror tests/m3/m3_private_endpoint_test.cpp
  git diff --check
  ```

- 静态检查：文档链接/围栏、架构与 Core 平台边界、新测试格式和 diff 空白检查均通过，
  无新增架构违规，未修改 third_party 或架构 baseline。
- 同步：DEC-094、LLM API 设计、模型 API 及配置示例、安全设计、兼容性矩阵与总计划入口。
  真实服务及目标平台未执行项、负责人和补跑条件保留在上一节，不用本轮结果替代。


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
