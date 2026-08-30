# Mira 平台构建与 Adapter 兼容性矩阵

> 状态：Active
> 版本：0.1
> 更新日期：2026-08-30
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

| 目标 | 编译组合 | Core/Simulator 证据 | 真实平台能力 | 运行门禁 |
| --- | --- | --- | --- | --- |
| Linux x86_64 | GCC 13、Clang 18；`debug`/`release`/sanitizer | `Build verified`（M0，Ubuntu 24.04） | `Planned`（`adapters/linux`，M2/M7） | Linux GCC/Clang CI |
| Windows x64 | MSVC、Visual Studio 17 2022；`windows-debug`/`windows-release` | `Configured`；CI 配置已提交，待 Windows runner 结果 | `Planned`（`adapters/windows`，M2/M7） | Windows configure/build/test CI |
| Android arm64-v8a | NDK toolchain，API 24；`android-arm64-release` | `Configured`；CI run 33303233207 暴露 ID hash 窄化和 ABI wrapper 配置问题，修复后待重跑 | `Planned`（Host/NDK Adapter，M2） | Android NDK configure/build CI；真机/模拟器由 M2/M7 |

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
cmake --build --preset android-arm64-release --target mira_core mira_simulator_adapter
```

Android toolchain 不把 SDK/NDK 路径写入仓库；CI 使用 `ANDROID_NDK_VERSION=26.3.11579264`，
本地可以使用兼容的已安装 NDK 并保留 API/ABI 配置。

## 4. 相关门禁

- `platform-boundary-check` 扫描 `include/mira` 和 `src`，拒绝 Android、Windows、Linux 平台
  SDK 头文件及平台宏。
- `mira_platform_boundary_test` 在测试构建中执行同一检查。
- `.github/workflows/ci.yml` 的 `linux`、`windows` 和 `android` job 分别负责目标组合；未运行或
  失败的目标必须保留其状态和补跑条件。
- Android CI runs 33301936164、33303233207 的编译失败已登记为 `BUG-20260830-001`；修复提交后的 runner 成功
  结果才可将 Android Core/Simulator 证据提升为 `Build verified`。
- 真实输入、截图、权限和生命周期 contract tests 在 Adapter 交付后加入 M2/M7，不能由 Simulator
  结果替代。
