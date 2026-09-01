# DEC-010：锁定 Mbed TLS 跨平台通道并完成受管代理与远端文件生命周期

> 状态：Accepted
> 日期：2026-09-01
> 负责人：Mira Maintainers
> 冻结里程碑：M3
> 替代/被替代：替代 [DEC-008](DEC-008-transport-dependency-strategy.md)

## 背景与问题

[DEC-008](DEC-008-transport-dependency-strategy.md) 在外部依赖不可获取时先交付自研 HTTP/1.1 socket
transport 与 Linux OpenSSL 参考通道，但把 HTTP(S) proxy、Windows/Android TLS 和远端
upload/delete 留为 `M3-04`、`M3-15` 的开放子项。该状态不能满足 M3 的完整退出条件：64 位 Winsock
句柄也不能安全套用现有 OpenSSL `SSL_set_fd(int)` 参考实现，Android NDK 没有可依赖的系统 OpenSSL。

2026-09-01 已可访问 Mbed TLS 官方仓库；`v3.6.7` 属于 3.6 LTS release tag，支持 CMake 子项目和
Windows/Android，许可证可选 Apache-2.0。Mira 仍需保持 Executor 对 socket wait、取消、timer 和
shutdown 的唯一生命周期所有权。

## 决策

1. 保留 `SocketHttpTransport` 作为唯一 HTTP/1.1、chunked、SSE、redirect、DNS/SSRF 和 deadline
   实现；不引入第二套 HTTP client 或隐藏 event loop。
2. 以 recursive git submodule 锁定 Mbed TLS `v3.6.7`（commit `068ff080b369`，framework
   `dde0c4a0e448`），Mira 选择 Apache-2.0 许可。`mira_mbedtls_transport` 通过 nonblocking BIO
   callback 直接读写 transport 拥有的 POSIX/Winsock socket，不创建线程，不拥有 socket，不改变
   Executor worker/shutdown 顺序。
3. Mbed TLS 通道必须使用宿主注入的 PEM CA bundle，强制 TLS 1.2+、peer 和 hostname verification；
   不提供关闭验证的生产路径。系统 OpenSSL 通道保留为 Linux 参考/交叉校验实现，不再承担跨平台声明。
4. `ModelProxyConfig` 是 versioned profile 的显式部分。HTTP 使用 absolute-form；HTTPS 先向已独立
   DNS/SSRF/allowlist 校验的 HTTP proxy 发 `CONNECT`，再在 tunnel 内对原目标执行 TLS hostname
   verification。proxy credential 通过独立 `SecretRef` 在 transport 边界解析，只进入
   `Proxy-Authorization`，不得进入目标请求、事件、digest 或错误。
5. `OpenAiRemoteFileStore` 实现 `/files` multipart upload、provider file ID 绑定和 `DELETE /files/{id}`。
   临时 ID 不进入 canonical request；mapper 仅在本次 provider attempt 的 artifact binding 中使用。
   retention 为零时结算前删除，正值使用 Executor `submit_delayed_with_handle()`；owner 保存 handle 和
   future，shutdown 取消并消费所有 future。失败、排队拒绝和 shutdown 取消进入只含 ID digest 的审计台账。
6. 真实 Provider 互操作仍必须使用受控账户、明确模型和费用上限；新增 probe 默认拒绝联网，只有显式
   凭据、CA bundle 和 `MIRA_INTEROP_MAX_REQUESTS` 就绪时才执行。

## 备选方案

| 方案 | 结论 | 原因 |
| --- | --- | --- |
| 继续让 Windows/Android https fail closed | 拒绝作为 M3 终态 | 无法满足目标平台 Provider 闭环 |
| Windows Schannel + Android 单独 TLS 库 | 拒绝 | 两套状态机、证据和错误语义，维护面显著扩大 |
| 系统 libcurl/平台 HTTP API | 拒绝 | HTTP/event-loop/线程生命周期不再由同一 Executor 边界控制 |
| 锁定 Mbed TLS，仅实现 TLS channel | 采纳 | 跨平台、无隐藏线程，可复用既有 socket transport 的全部策略 |
| proxy/userinfo 直接放 URL | 拒绝 | Secret 会进入 profile、digest、日志和 URL 解析面 |

## 影响与风险

- 新增锁定直接依赖和 recursive submodule；lock、SBOM、许可证与 CI clone 必须同步维护。
- Mbed TLS 不读取平台系统 trust store；宿主必须交付经过治理的 CA bundle。这使 trust material 明确可复现，
  也增加部署责任。
- Windows/Android 的源码与 CI 构建目标已接入，但在对应 runner/设备结果回填前只能声明 `Configured`，
  不能声明目标平台 runtime verified。
- `getaddrinfo` 仍不可协作取消；沿用原有事后 DNS deadline 与 total deadline 限制。
- upload cleanup 是外部副作用；删除失败不会把已经完成的模型结果改写为失败，但通过审计台账保持可见，
  后续恢复 owner 可据此重试。

## 验证方式

- `mira_m3_transport_test`：HTTP absolute-form、proxy credential、proxy/target 双重 policy、CONNECT 407、
  header injection、取消、shutdown、SSRF、redirect 与资源上限。
- `mira_m3_tls_test` 与 `mira_m3_mbedtls_test`：OpenSSL server 上的真实 handshake、错误 CA fail-closed，
  以及 HTTPS CONNECT 后 TLS 正向路径；两种 channel 共享 contract source。
- `mira_m3_upload_test`：multipart `user_data`、Responses `file_id`、立即删除、延时 timer、shutdown 结算、
  敏感数据拒绝和删除失败审计。
- Windows MSVC 与 Android NDK CI 显式构建 `mira_mbedtls_transport`；无 runner 结果时保持未验证。
- `mira_m3_interop_probe`：缺少显式环境时退出 2 且不联网；授权环境覆盖同步、SSE strict schema，
  可选 upload/delete，并只输出脱敏 evidence metadata。

## 关联文档和工作项

- [M3 计划](../plans/m3-model-provider-agent-loop.md)：`M3-04`、`M3-15`、`M3-19`
- [LLM API 协议设计](../design/llm-api-protocol-design.md)
- [Model Provider 与 Tool 设计](../design/model_provider_and_tool_design.md)
- [平台矩阵](../compatibility/platform-matrix.md)
- [OpenAI-compatible 矩阵](../compatibility/openai-compatible-matrix.md)
- [直接依赖与许可证](../supply-chain/direct-dependencies.md)
