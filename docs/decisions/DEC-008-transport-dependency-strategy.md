# DEC-008：M3 传输层不引入第三方 HTTP/TLS 库，采用自研受管 transport 与可插拔 TLS 通道

> 状态：Superseded（2026-09-01 由 [DEC-010](DEC-010-cross-platform-tls-proxy-upload.md) 替代）
> 日期：2026-08-31
> 负责人：Mira Maintainers
> 冻结里程碑：M3（HTTP/1.1 + SSE + Linux TLS 通道）；Windows/Android TLS 通道冻结点顺延至 M4 前置验证
> 替代/被替代：被 [DEC-010](DEC-010-cross-platform-tls-proxy-upload.md) 替代（历史 Linux/OpenSSL 基线继续保留）

## 背景与问题

[M3 计划](../plans/m3-model-provider-agent-loop.md) 工作项 `M3-04` 要求“选择并锁定 C++ HTTP/TLS 依赖”，并在桌面与
Android NDK 上验证 TLS、代理、DNS、redirect、SSE、取消与内部线程/shutdown。候选包括 libcurl、cpp-httplib、
Boost.Beast、mbedTLS 等。

约束条件：

1. Mira 的 Executor 强制约束要求传输阻塞等待由 Executor blocking I/O worker 承载，取消、deadline 与 shutdown
   必须可观测、可结算；任何自带线程池或全局 event loop 的库都必须额外封装。
2. 供应链规范要求新依赖以锁定版本进入 `dependencies.lock.json` 与 SBOM，并完成许可证与升级审计。
3. 当前开发环境无法访问外部网络（无法 clone/升级 submodule 并在本地验证），CI 中 Windows runner 不保证
   OpenSSL/mbedTLS 开发库；Android NDK 交叉编译任何 TLS 库都会显著增加 CI 面积与失败风险。
4. M3 需要的传输面是 HTTP/1.1 + chunked + SSE 读取 + 分阶段 deadline + SSRF 策略，不依赖 HTTP/2 multiplexing、
   keep-alive 池或 WebSockets（[LLM API 设计 §14](../design/llm-api-protocol-design.md)）。

## 决策

1. **HTTP 传输自研**：`adapters/net/socket_transport.{hpp,cpp}` 基于 BSD sockets（POSIX）/Winsock（Windows）
   实现 `IHttpTransport`（`include/mira/model_transport.hpp`）。阻塞等待运行在 Executor blocking I/O worker
   （`mira-provider-io`），所有 socket 等待使用 ≤25ms 的 poll 切片，使取消与分阶段 deadline 在有界延迟内被观察，
   不需要跨线程关 socket。
2. **TLS 是可插拔通道而非内置依赖**：core 只定义 `ITlsChannelFactory`/`ITlsChannel`（poll 驱动、原生句柄以
   `std::intptr_t` 传递）。未配置 TLS 工厂时，https 端点在任何字节写出之前以 `CapabilityMismatch` fail closed。
3. **Linux 桌面提供 OpenSSL 参考通道**：`adapters/net/openssl_tls.cpp` 在 `MIRA_WITH_OPENSSL=ON` 且系统提供
   OpenSSL 开发库时编译为 `mira_openssl_transport`；证书校验与主机名校验强制开启，`TlsOptions.verify_peer`
   不提供关闭路径。
4. **Windows/Android TLS 通道未交付**：对应平台上 https 继续 fail closed，直到有锁定版本的 TLS 依赖通过
   目标平台构建与运行验证（预计 M4 前置）。平台状态记录于
   [平台矩阵](../compatibility/platform-matrix.md)。
5. **代理（HTTP(S) proxy）不在 M3 交付**：`TransportLimits` 预留了策略位置；需要代理的部署在 M4 以同一
   SSRF/凭证边界补齐，见 M3 计划中 `M3-04` 的未完成项记录。

## 备选方案

| 方案 | 结论 | 原因 |
| --- | --- | --- |
| libcurl（系统依赖） | 拒绝 | Android NDK 需交叉编译；多平台版本漂移；内部线程模型与 Executor 结算不透明 |
| cpp-httplib（header-only） | 拒绝 | 默认阻塞线程/keep-alive 池模型与“每个 exchange 有界结算”冲突；TLS 仍需 OpenSSL |
| mbedTLS submodule | 暂缓 | 无法在当前环境 vendor 并本地验证；Windows CI 集成风险；M3 冻结点前无法提供证据 |
| 自研 socket transport + 可插拔 TLS | 采纳 | 全部三个 M3 目标平台共享同一 BSD socket 面；每条语义（deadline/取消/redirect/SSRF）都在测试中可复现 |

## 影响与风险

- 正面：无新供应链条目；Windows/Linux/Android 构建组合面积不增加；传输行为完全由 Mira 契约测试定义；
  https fail-closed 语义明确。
- 负面/风险：
  - 自研 HTTP 客户端需自行覆盖分片、chunked、redirect、部分失败等边角；已由
    `mira_m3_transport_test`（脚本化故障注入 server）覆盖，但真实服务端多样性只有在 `M3-19` 互操作时才检验。
  - Windows/Android 暂无 TLS 通道：生产 https 端点在这些平台不可用（明确报错，不静默降级）。
  - DNS 解析使用 `getaddrinfo`，该调用本身不可中断；阶段预算以事后测量执行，极端慢解析仍受 total deadline
    兜底。此限制同样适用于多数候选库。
- 供应链影响：无新直接依赖；OpenSSL 作为系统组件记录在
  [直接依赖说明](../supply-chain/direct-dependencies.md)（非锁定条目，原因与 CMake/编译器相同）。

## 验证方式

- `mira_m3_transport_test`：loopback 脚本化 server 覆盖同步/chunked/SSE、redirect（同源保留凭证、跨源剥离、
  链上限）、SSRF（环回/私网/元数据地址拒绝、host allowlist、scheme/userinfo 拒绝）、取消（poll 切片内有界
  返回 `ModelCancelled`）、shutdown（队列结算 + worker join）、响应上限、connect deadline、无 TLS 时 https
  fail closed。
- `mira_m3_tls_test`（仅当 OpenSSL 目标存在）：进程内 OpenSSL server + 自签证书完成真实 TLS 握手与 200 响应；
  错误 CA fail closed（`mira.model.tls`）。
- 平台门禁：CI linux/windows/android job 构建 `mira_net_transport`（android job 构建目标列表已含）；
  Windows/Android 的 TLS 正向验证保持未完成并在平台矩阵中标注。

## 关联文档和工作项

- [M3 计划](../plans/m3-model-provider-agent-loop.md)：`M3-04`、`M3-05`
- [LLM API 协议设计 §10/§16](../design/llm-api-protocol-design.md)
- [Model Provider 与 Tool 扩展设计 §13](../design/model_provider_and_tool_design.md)
- [平台矩阵](../compatibility/platform-matrix.md)、[供应链直接依赖](../supply-chain/direct-dependencies.md)
- [OpenAI 兼容性矩阵](../compatibility/openai-compatible-matrix.md)
