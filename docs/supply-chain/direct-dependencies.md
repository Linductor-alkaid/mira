# Mira 直接依赖与许可证

> 状态：Active
> 更新日期：2026-08-30
> 适用范围：M0 可构建目标的直接第三方依赖

| 依赖 | 固定版本 | 来源 | 许可证 | 许可证文件 |
| --- | --- | --- | --- | --- |
| Executor | `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`（`v0.4.0-82-g4fd8e60`） | git submodule | MIT | [`third_party/executor/LICENSE`](../../third_party/executor/LICENSE) |

机器可读锁定信息位于 [`dependencies.lock.json`](../../dependencies.lock.json)，CycloneDX SBOM
位于 [`sbom.cdx.json`](sbom.cdx.json)。M0 关闭 Executor 的可选 GPU 后端，因此当前 Mira 构建
没有额外 CUDA/OpenCL 直接依赖。CMake、编译器、C/C++ 运行库和操作系统库属于构建/系统组件，
不列为随 Mira 源码锁定的直接第三方包。
