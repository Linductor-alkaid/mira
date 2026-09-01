# Mira 平台构建与 Adapter 兼容性矩阵

> 状态：Active
> 版本：0.3
> 更新日期：2026-09-01
> 适用范围：Mira Core、构建组合和 Platform Adapter 发布门禁

## 1. 证据等级

| 状态 | 含义 |
| --- | --- |
| `Boundary checked` | Core 平台边界检查通过；不代表目标平台可运行 |
| `Configured` | 仓库提供可复现的 CMake/toolchain/CI 入口；尚未有本次验收的目标环境结果 |
| `Build verified` | 指定工具链完成 configure/build |
| `Runtime verified` | 指定设备或操作系统完成适用运行/contract tests |
| `Planned` | 设计和目录边界已确定，尚未交付实现 |

## 2. 当前矩阵

| 目标 | 编译组合 | Core/Simulator/Host Adapter 证据 | 真实平台能力 | 运行门禁 |
| --- | --- | --- | --- | --- |
| Linux x86_64 | GCC 13、Clang 18；`debug`/`release`/sanitizer | `Build verified`（M0，Ubuntu 24.04） | `Planned`（`adapters/linux`，M7） | Linux GCC/Clang CI |
| Windows x64 | MSVC、Visual Studio 17 2022；`windows-debug`/`windows-release` | `Configured`；CI 配置已提交，待 Windows runner 结果 | `Planned`（`adapters/windows`，M7） | Windows configure/build/test CI |
| Android arm64-v8a | NDK 26.3.11579264，API 24；`android-arm64-release` | `Build verified`（CI run 33303882772，`mira_core` 与 `mira_simulator_adapter`；`mira_android_adapter` 于 CI run
[`33322113637`](https://github.com/Linductor-alkaid/mira/actions/runs/33322113637) 复验） | `Boundary checked`（M2 冻结 Host ABI 与 Adapter 骨架，fake host 契约验证；见 [android-host-abi.md](android-host-abi.md)） | Android NDK configure/build CI；真机/模拟器由 M7 |

### M3 传输 Adapter

| 目标 | `mira_net_transport`（HTTP/SSE/proxy） | `mira_mbedtls_transport` | OpenSSL 参考通道 | 证据 |
| --- | --- | --- | --- | --- |
| Linux x86_64 | Runtime verified（direct + HTTP proxy + HTTPS CONNECT） | Runtime verified（direct/CONNECT handshake、错误 CA） | Runtime verified | 本地 `mira_m3_transport_test`、`mira_m3_mbedtls_test`、`mira_m3_tls_test`（2026-09-01）；既有 socket CI run [`33332557571`](https://github.com/Linductor-alkaid/mira/actions/runs/33332557571) |
| Windows x64 | 既有 CI build/runtime verified（Winsock） | Configured（MSVC 全量 build 会构建；本轮 runner 未执行） | 不构建 | socket 证据为 CI run [`33332557571`](https://github.com/Linductor-alkaid/mira/actions/runs/33332557571)；Mbed TLS 补跑条件见下文 |
| Android arm64-v8a | 既有 CI build verified | Configured（Android job 已显式加入目标；本轮 NDK 未执行） | 不构建 | socket 证据为 CI run [`33332557571`](https://github.com/Linductor-alkaid/mira/actions/runs/33332557571)；Mbed TLS 补跑条件见下文 |

跨平台 TLS、proxy、CA bundle 和 fail-closed 语义见
[DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md)。未配置 TLS 工厂时 https 端点在
model request 字节写出前以 `CapabilityMismatch` 拒绝。Windows/Android 当前只能声明
`Configured`：负责人 Mira Maintainers；补跑条件为 recursive submodule checkout 后分别执行
MSVC Debug/Release 全量构建，以及 NDK 26.3/API 24 arm64 构建 `mira_mbedtls_transport`；runtime 声明
还需在对应平台执行 direct TLS、CONNECT TLS 和错误 CA contract。

所有目标共享同一套平台无关 `mira_core` 公共头和 `IEnvironment` 边界。平台 SDK、JNI、权限、
生命周期和线程亲和逻辑只能进入对应 Host/Adapter；没有真实 Adapter 或目标环境运行证据时，
不能把 `Configured` 或 `Build verified` 表述为平台运行支持。

## 3. 可复现入口

Linux：

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Windows PowerShell：

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
```

Android（设置 `ANDROID_NDK_HOME` 或 `ANDROID_NDK_ROOT`）：

```sh
cmake --preset android-arm64-release
cmake --build --preset android-arm64-release --target mira_core mira_simulator_adapter mira_android_adapter mira_net_transport mira_mbedtls_transport
```

Android toolchain 不把 SDK/NDK 路径写入仓库；CI 使用 `ANDROID_NDK_VERSION=26.3.11579264`，
本地可以使用兼容的已安装 NDK 并保留 API/ABI 配置。

## 4. 相关门禁

- `platform-boundary-check` 扫描 `include/mira` 和 `src`，拒绝 Android、Windows、Linux 平台
  SDK 头文件及平台宏。
- `mira_platform_boundary_test` 在测试构建中执行同一检查。
- `.github/workflows/ci.yml` 的 `linux`、`windows` 和 `android` job 分别负责目标组合；未运行或
  失败的目标必须保留其状态和补跑条件。
- Android CI run 33303882772 的 Android、Windows、Linux、sanitizer 和 quality jobs 全部成功；该
  run 验证 Android arm64-v8a Core/Simulator 构建。此前 runs 33301936164、33303233207、33303509993
  的兼容性失败及修复记录保留在 M0 验证记录中。
- 真实输入、截图、权限和生命周期 contract tests 在 Adapter 交付后加入 M2/M7，不能由 Simulator
  结果替代。
