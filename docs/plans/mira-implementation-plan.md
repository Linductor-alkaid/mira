# Mira 实施总计划

> 状态：In Progress
> 负责人：Mira Maintainers  
> 更新日期：2026-09-01
> 设计依据：[Mira Runtime 设计](../design/mira_runtime_design.md)、[Context 与 Memory 设计](../design/context_and_memory_design.md)、
> [LLM API 协议设计](../design/llm-api-protocol-design.md)

## 1. 计划目的

本文是 Mira 从空仓库走向可发布 Runtime 的交付入口，只维护范围、里程碑依赖、发布点和通用
门禁。每个里程碑的工作项、测试、风险和逐轮验证记录放在独立文件中。

勾选规则遵循[项目管理与文档规范](../project/project_management_and_documentation.md)：只有实现、
测试、必要文档和验收证据同时完成，工作项才可标记为 `[x]`。环境不足导致的未运行验证保持
未勾选，并记录环境、负责人和补跑条件。

## 2. v1 交付边界

### 2.1 v1 包含

- C++20 平台无关 Agent Core 和稳定的宿主集成边界，首期构建组合覆盖 Linux、Windows、Android。
- `Observe -> Reason -> Plan -> Act -> Verify` 可中断闭环。
- OpenAI-compatible 外部 LLM/VLM Provider。
- Screenshot、结构化 UI、本地 OCR/检测/任务 ONNX 组成的 Observation Pipeline。
- 离散输入、连续轨迹和摇杆控制。
- Task、Session、事件、Checkpoint、Memory、Replay 和 Human Takeover。
- Simulator 参考环境及至少一个真实平台 Adapter 的契约验证；首个真实目标为 Android Host/NDK。
- 受治理的任务模型数据导出、蒸馏、ONNX 发布和设备侧评估契约；训练工具链与 Core 解耦。

### 2.2 v1 不包含

- Core 内本地运行通用 LLM/VLM。
- 具体产品 UI 或完整 Android 应用。
- 未经目标设备实测的硬实时保证。
- 自动将 Runtime 事件、截图或 Memory 转为训练数据。
- 任意代码执行、任意 shell 或模型绕过 Policy 直接调用平台。
- 分布式 Runtime、跨设备一致性和云端控制面。

## 3. 不可破坏的项目约束

- [ ] `RULE-01` Core 不依赖 Android、Windows、Linux 等平台 SDK；平台能力只经 Adapter 注入。
- [ ] `RULE-02` 所有 Mira 发起的异步、阻塞、定时、串行控制和实时任务由 Executor 管理。
- [ ] `RULE-03` Task 状态只有串行控制面可以提交，迟到 completion 不能复活旧 epoch 或终态。
- [ ] `RULE-04` 模型输出只形成结构化 Decision；Action 在本地经过 capability、freshness、权限和
  SafetyPolicy 校验。
- [ ] `RULE-05` 外部副作用采用至多一次派发语义；不确定结果必须 Observe/Verify，禁止盲目重发。
- [ ] `RULE-06` 连续控制具有最大时长、watchdog 和可验证的 `Up/Cancel` 安全收敛。
- [ ] `RULE-07` EventStore 是已提交事实的权威记录；Checkpoint、Memory 和索引是可重建投影。
- [ ] `RULE-08` 所有队列、缓存、上下文、动作、模型请求和并发 operation 都有容量或预算上限。
- [ ] `RULE-09` Observation、Memory、Tool 和模型响应中的外部内容均是不可信数据，不能提升为
  SystemPolicy 或授权。
- [ ] `RULE-10` 性能、实时性、兼容性和跨平台声明必须由目标环境证据支撑。
- [ ] `RULE-11` Executor 能力不足时登记 `docs/executor_feedback/ledger.md`，不得静默引入平行生命周期。
- [ ] `RULE-12` 训练数据导出默认关闭，必须经过授权、脱敏、来源追踪、删除传播和审计。

## 4. 里程碑与发布点

| 里程碑 | 目标 | 前置 | 建议发布点 | 状态 |
| --- | --- | --- | --- | --- |
| [M0](m0-engineering-baseline.md) | 仓库、跨平台构建组合、Executor 集成和质量基线 | 无 | 内部工程基线 | Completed |
| [M1](m1-core-contracts.md) | 公共契约、状态机、持久化和安全边界冻结 | M0 | Core contract alpha | Completed |
| [M2](m2-observation-simulator-android-host.md) | Observation、坐标、Simulator 与 Android Host ABI | M1 | Environment alpha | Completed |
| [M3](m3-model-provider-agent-loop.md) | OpenAI-compatible Provider 和视觉离散闭环 | M2 | Agent loop alpha | In Progress（代理/upload 已交付；目标平台 TLS 证据与 interop 开放） |
| [M4](m4-context-memory-recovery.md) | Context/Memory、Replay 和恢复 | M3 | Stateful agent beta | Planned |
| [M5](m5-local-perception-task-models.md) | 本地视觉、任务模型注册与 ONNX 推理 | M3 | Local perception beta | Planned |
| [M6](m6-realtime-control-takeover.md) | 连续控制、实时路径和 Human Takeover | M2、M5 | Control beta | Planned |
| [M7](m7-tools-evaluation-platform-v1.md) | Tool 模组（[DEC-009](../decisions/DEC-009-tool-module-boundary.md)）、Tool 隔离、评估体系、生产加固和跨平台验证 | M4、M5、M6 | v1.0 | Planned |

主干依赖先经过 `M0 -> M1 -> M2 -> M3`，随后分为 `M3 -> M4` 与 `M3 -> M5 -> M6` 两条
可并行交付链，最终在 M7 汇合；实际关键路径由 M4 与 M5/M6 两条链的完成时间决定。M6 同时复用
M2 的坐标/宿主契约和 M5 的本地状态识别能力。任何里程碑都不得以“后续再补取消、安全或验证”
关闭。

M4–M7 的范围、稳定工作项、Executor 路由、测试矩阵、风险、退出条件和验证记录已拆入各自阶段
文档。`Planned` 仅表示范围和验收方式已明确，不表示前置已满足或实现已开始。M3 当前开放的
Windows/Android TLS 目标证据和真实 Provider 互操作仍由 `M3-04`、`M3-19` 原编号跟踪；后续里程碑
不得通过复制或改号绕过 M3 退出条件。

## 5. 首批架构决策

| 决策 | 主题 | 状态 | 冻结点 |
| --- | --- | --- | --- |
| [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md) | Runtime 的 Executor 所有权与串行控制面 | Accepted | M0 |
| [DEC-002](../decisions/DEC-002-public-contract-versioning.md) | 公共契约、结果和版本化边界 | Accepted | M1 |
| [DEC-003](../decisions/DEC-003-event-sourced-persistence.md) | EventStore 事实源与副作用日志协议 | Accepted | M1 |
| [DEC-004](../decisions/DEC-004-security-authority-confirmation.md) | 权限、能力授权和 Human Confirmation | Accepted | M1 |
| [DEC-005](../decisions/DEC-005-observation-coordinate-host-boundary.md) | Observation 坐标与 Android Host 边界 | Accepted | M2 |
| [DEC-006](../decisions/DEC-006-local-perception-task-models.md) | 本地感知和任务 ONNX 模型边界 | Accepted | M5 |
| [DEC-007](../decisions/DEC-007-llm-api-protocol-strategy.md) | LLM API 规范契约与协议方言策略 | Accepted | M3 |
| [DEC-008](../decisions/DEC-008-transport-dependency-strategy.md) | 历史 M3 传输基线 | Superseded by DEC-010 | M3 |
| [DEC-009](../decisions/DEC-009-tool-module-boundary.md) | 工具模组边界与能力协商 | Accepted | M7 |
| [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md) | 锁定 Mbed TLS、受管代理与远端文件生命周期 | Accepted | M3 |

“Accepted”表示架构方向已生效，不表示对应实现工作项已经完成。具体实现仍由里程碑复选框和
验证记录证明。

## 6. 通用质量门禁

- [ ] 所有公开头文件可由最小外部 consumer 独立包含和链接。
- [ ] 状态转换、取消、shutdown、背压、提交拒绝、异常和不确定副作用有自动化测试。
- [ ] 每个 Provider/Adapter 通过共同 contract test，capability 与实际行为一致。
- [ ] OfflineReplay 不执行网络、Tool 或输入副作用。
- [ ] ASAN/UBSAN 常规运行；TSAN 在支持环境运行，不能运行时保留未完成门禁和补跑条件。
- [ ] 敏感字段、截图、UI Tree、Memory、模型请求和训练导出通过脱敏与权限负向测试。
- [ ] Android 真机或受支持模拟环境覆盖旋转、前后台、权限撤销、宿主销毁和输入释放。
- [ ] benchmark 记录硬件、OS、构建、Executor/模型配置、样本量和百分位。
- [ ] 依赖锁定、许可证和 SBOM 可重复生成。
- [ ] 文档、决策、计划状态和验证记录与实现同步。

## 7. 计划维护规则

- 总计划只更新里程碑状态、依赖、范围和通用门禁；实施细节写入阶段文件。
- 新增里程碑或改变关键路径时，更新受影响的决策、设计和阶段前置条件。
- 里程碑只能在全部工作项和退出条件完成、且验证记录可复现后改为 `Completed`。
- 若某项被拆到后续阶段，原项保持未完成，除非范围变更经决策记录批准并明确迁移编号。
