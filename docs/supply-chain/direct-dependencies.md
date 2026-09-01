# Mira 直接依赖与许可证

> 状态：Active
> 更新日期：2026-09-01
> 适用范围：Mira 可构建目标的直接第三方依赖

| 依赖 | 固定版本 | 来源 | 许可证 | 许可证文件 |
| --- | --- | --- | --- | --- |
| Executor | `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`（`v0.4.0-82-g4fd8e60`） | git submodule | MIT | [`third_party/executor/LICENSE`](../../third_party/executor/LICENSE) |
| Mbed TLS | `068ff080b369adfac81509f9b57b2afabaf82dc5`（`v3.6.7`，3.6 LTS；framework `dde0c4a`） | recursive git submodule | Apache-2.0（从双许可证中选择） | [`third_party/mbedtls/LICENSE`](../../third_party/mbedtls/LICENSE) |

### 系统组件（非锁定直接依赖）

M3 起，可选参考目标 `mira_openssl_transport` 在检测到系统 OpenSSL 开发库时编译（当前决策依据
[DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)）。OpenSSL 与 CMake、编译器、
C/C++ 运行库一样属于构建/系统组件：由目标平台的包管理提供版本，不进入
`dependencies.lock.json`；未检测到时该目标跳过，https 端点 fail closed。Linux 本地证据基于
OpenSSL 3.0.13（2026-08-31，`mira_m3_tls_test`）。生产跨平台通道由锁定的 Mbed TLS 提供，宿主
必须注入 PEM CA bundle；Mbed TLS 不创建线程，nonblocking BIO 继续由 Executor 受管 socket worker
驱动。其 release tag 在 git checkout 中携带固定的 `mbedtls-framework` 子模块，故 clone 必须使用
`--recurse-submodules`。

机器可读锁定信息位于 [`dependencies.lock.json`](../../dependencies.lock.json)，CycloneDX SBOM
位于 [`sbom.cdx.json`](sbom.cdx.json)。M0 关闭 Executor 的可选 GPU 后端，因此当前 Mira 构建
没有额外 CUDA/OpenCL 直接依赖。CMake、编译器、C/C++ 运行库和操作系统库属于构建/系统组件，
不列为随 Mira 源码锁定的直接第三方包。
