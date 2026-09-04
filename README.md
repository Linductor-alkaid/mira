# Mira

Mira 是使用现代 C++ 构建的跨平台原生 AI Agent Runtime。核心运行闭环为：

```
Observe -> Reason -> Plan -> Act -> Verify
```

Agent Core 与具体平台完全解耦：Android、Windows、Linux、模拟器等宿主能力只通过
Platform Adapter 接入，Core 不依赖任何平台 SDK。所有异步任务、定时任务和生命周期由
[`third_party/executor`](third_party/executor) 统一管理，Mira 自研代码不创建私有线程。

## 当前能力状态

Mira 已交付 M0–M4（见[实施总计划](docs/plans/mira-implementation-plan.md)）：

| 能力 | 状态 | 证据 |
| --- | --- | --- |
| 公共契约、状态机、EventStore、安全边界 | 已交付（M0/M1） | — |
| Observation、坐标、Simulator、Android Host ABI v1 | 已交付（M2） | — |
| OpenAI-compatible Provider、视觉离散闭环 | 已交付（M3） | [Agent loop alpha](docs/releases/agent-loop-alpha.md) |
| Context/Memory、Checkpoint、崩溃恢复、Replay | 已交付（M4） | [Stateful agent beta](docs/releases/stateful-agent-beta.md) |

本地 ONNX 感知（M5）与连续控制（M6）已按
[DEC-011](docs/decisions/DEC-011-demo-first-external-validation.md) 终止；能力验证与需求
发现由独立仓库的 demo 产品承载，后续里程碑待其证据重定义。Mira 仓库不包含产品 UI 或
完整应用。

## 构建与测试

要求：C++20 编译器、CMake ≥ 3.20、Ninja。Executor、Mbed TLS 与 SQLite 以 pinned
submodule/vendored 方式提供，无需系统安装。

```bash
git submodule update --init --recursive
cmake --preset debug          # 或 release / asan / ubsan / tsan
cmake --build --preset debug
ctest --test-dir build/debug --output-on-failure
```

Windows 使用 `windows-debug` / `windows-release` preset；Android arm64 交叉构建使用
`android-arm64-release`（目标构建验证，真机运行不在声明范围）。常用 CMake 选项：
`MIRA_BUILD_TESTS`、`MIRA_WITH_OPENSSL`、`MIRA_WITH_MBEDTLS`、`MIRA_ENABLE_CLANG_TIDY`。

平台与编译器支持等级（GCC/Clang/MSVC/NDK 的已验证组合）见
[平台矩阵](docs/compatibility/platform-matrix.md)；Provider 兼容性见
[OpenAI-compatible 互操作矩阵](docs/compatibility/openai-compatible-matrix.md)。

## 安装与外部消费

Mira 以安装包形式对外提供全部公共能力，外部项目（包括 demo 产品）只经此边界消费：

```bash
cmake --build --preset release
cmake --install build/release --prefix /path/to/install
```

消费方 `CMakeLists.txt`：

```cmake
find_package(Mira 0.1 CONFIG REQUIRED)   # 自动解析 executor 0.4 与 Threads 依赖

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE
    Mira::core                 # 契约、状态机、环境、模型网关、闭环
    Mira::state_store          # SQLite/WAL Checkpoint 与 Memory 参考后端
    Mira::simulator_adapter    # Simulator 参考环境
)
```

可选目标：`Mira::android_adapter`、`Mira::net_transport`、`Mira::openssl_transport`、
`Mira::mbedtls_transport`。安装消费路径由 `mira_installed_consumer_test` 在 CI 全矩阵
持续验证。最小示例见 [`examples/minimal_consumer.cpp`](examples/minimal_consumer.cpp)；
有状态 Agent（Checkpoint + Memory + supervised shutdown）完整示例见
[`examples/stateful_agent_consumer.cpp`](examples/stateful_agent_consumer.cpp)。

## 文档索引

- [API 手册](docs/api/index.md)——公共头文件逐模块参考
- [设计文档](docs/design/mira_runtime_design.md)——架构、状态机、Context/Memory、协议
- [决策记录](docs/decisions/DEC-001-runtime-executor-ownership.md)——`DEC-001` 起
- [实施计划](docs/plans/mira-implementation-plan.md)——里程碑、状态与验证证据
- [发布说明](docs/releases/stateful-agent-beta.md)、[安全](docs/security/threat_model_and_confirmation.md)、
  [供应链](docs/supply-chain/direct-dependencies.md)、[Executor 反馈台账](docs/executor_feedback/ledger.md)

协作约定见 [AGENTS.md](AGENTS.md) 与
[项目管理与文档规范](docs/project/project_management_and_documentation.md)。
