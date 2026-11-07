# DEC-094：Profile 显式授权私网模型端点

> 状态：Accepted
> 日期：2026-11-07
> 负责人：Mira Maintainers
> 工作项：MNT-202611-14 / BUG-20261107-001
> 前置：[DEC-010](DEC-010-cross-platform-tls-proxy-upload.md)

## 背景与问题

OpenAiCompatibleProvider 硬编码 `allow_private_endpoints=false`，导致宿主配置的 SSH
隧道、回环 mock 和私网推理服务在 pre-write 阶段被拒绝。安全设计允许 profile 显式授权，
但公开 ModelProfile 缺少表达该授权的字段；代理自身的授权不能替代目标授权。

## 决策

- ModelProfile 尾部追加 `bool allow_private_endpoint = false`，仅由可信宿主配置，
  不从模型输出或 ProviderInferOptions 获取。Provider 和 OpenAiRemoteFileStore 的
  upload/delete 原样传入 TransportLimits，防止文件请求仍拒绝已授权端点。
- 默认仍拒绝 transport 判为私网的解析地址，包括回环、RFC1918、CGNAT、链路本地和
  metadata 地址。授权适用于目标及其重定向；每跳仍须通过既有 host allowlist、URL、
  deadline、容量及 TLS 检查。Provider 的 allowlist 仍固定为 profile 的 endpoint host。
- ModelProxyConfig 的同名字段只授权代理自身；目标与代理各自检查，两者不互相授权。
- manifest 始终序列化该字段（包括 false），因此 profile_digest 绑定该权限。
  切换授权须重新路由，不能沿用旧摘要绑定的 RouteDecision 或 continuation。

## 备选方案与影响

不以 allowed_hosts 代替地址授权，也不要求宿主为本机推理复刻 IHttpTransport。
本轮不引入逐地址或逐网段授权。开启字段会授权该 profile 允许的 host 所解析出的全部
私网类别，宿主需仅为自己信任的端点配置开启。默认网络策略不变；C++ 消费者须重编译，
新增 manifest 字段使默认 profile 的 digest 也改变，历史证据保留原 digest，绑定须重建。
不新增并发路径或 Executor 生命周期，测试服务使用现有 submit_auto/future fixture。

## 验证与关联

验证默认 pre-write 拒绝、显式授权后 Provider 经官方 socket 到达回环服务、代理两侧
授权独立、重定向 allowlist 不被授权绕过、文件上传/删除共用授权，以及 manifest/digest 随权限变化。
实施与证据见[维护计划](../plans/maintenance-2026-11-private-model-endpoints.md)。
