# Mira 直接依赖与许可证

> 状态：Active
> 更新日期：2026-09-03
> 适用范围：Mira 可构建目标的直接第三方依赖

| 依赖 | 固定版本 | 来源 | 许可证 | 许可证文件 |
| --- | --- | --- | --- | --- |
| Executor | `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`（`v0.4.0-82-g4fd8e60`） | git submodule | MIT | [`third_party/executor/LICENSE`](../../third_party/executor/LICENSE) |
| Mbed TLS | `068ff080b369adfac81509f9b57b2afabaf82dc5`（`v3.6.7`，3.6 LTS；framework `dde0c4a`） | recursive git submodule | Apache-2.0（从双许可证中选择） | [`third_party/mbedtls/LICENSE`](../../third_party/mbedtls/LICENSE) |
| SQLite | `3.53.4`（amalgamation `3530400`，archive SHA-256 `1e71ddf9…e87d`） | vendored source archive | Public Domain | [`third_party/sqlite/LICENSE.md`](../../third_party/sqlite/LICENSE.md) |

### SQLite（M4 vendored 依赖）

M4 起，durable Checkpoint/Memory 参考后端（`mira_state_store` 目标，工作项 `M4-06`/`M4-09`）使用
官方 amalgamation 分发的 SQLite `3.53.4`。选择 vendored archive 而非 submodule 的原因：SQLite
官方仓库不发布预生成 amalgamation，从 canonical source 生成需要 tclsh，会为 Windows/Android CI
引入额外构建依赖。锁定方式为 archive SHA-256 加逐文件 SHA-256
（[`dependencies.lock.json`](../../dependencies.lock.json)），由 `tools/check_sbom.py` 在 CI 校验；
本地不修改 vendored 文件，升级按整包替换并记录。

线程模型：`SQLITE_THREADSAFE=1`（serialized）编译，`SQLITE_OMIT_LOAD_EXTENSION` 关闭动态加载，
SQLite 自身不创建线程。所有数据库访问由单个 Executor blocking-I/O worker 持有的唯一连接执行
（单 writer、有界请求通道、WAL journal）；审计见
[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §16–§17。构建
入口 `third_party/sqlite/CMakeLists.txt`（`MIRA_WITH_SQLITE` CMake option，默认开启）。

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
