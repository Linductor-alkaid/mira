# DEC-045：Agent Loop 的 Working Context 快照供给缝与宿主编排边界（宿主集成轮）

> 状态：Accepted
> 日期：2026-09-23
> 决策人：Mira Maintainers
> 需求来源：[M23](../plans/m23-memory-promotion-stage-w4.md)/[M24](../plans/m24-context-curator-stage-w5.md)
> 头注与遗留清单点名的「宿主集成轮」；M24 §2/§5 留痕「接线验收形态无任何
> 已冻结文档，八问不可答」
> 上位决策：[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)（双平面
> 与 W-06/W-07 约束）、[DEC-035](DEC-035-context-curator-working-context.md)
> 第 2 条（快照只经 Layer 0 候选转换进入模型请求）、
> [DEC-044](DEC-044-multi-agent-context-fork-boundary.md)（子代理与宿主显
> 式编排边界）
> 关联计划：[M25](../plans/m25-host-integration-round.md)（宿主集成轮）

## 背景与问题

DEC-035 链的 W1–W5 已全部交付（[M20]–[M24]），但 Working Context 在 Runtime
的生产消费者为零：Agent Loop 是上下文盲的——`build_request`
（`src/model/agent_loop.cpp:251-330`）直接拼装 system / goal / 用户指令 /
工具结果，绕开 `IContextManager` 与 Working Context；`examples/` 无任何
working context 消费（本决策前 grep 取证：`include/mira/agent_loop.hpp` 与
`src/model/agent_loop.cpp` 零 `WorkingContext`/`Curator` 引用）。快照经
`context_items_from_working_context`（Layer 0 唯一准入转换）的条目因此从未
真实进入模型请求，W4/W5 产物被 M23/M24 头注指定为「宿主集成轮」的输入形态
锚点，而接线形态无任何已冻结文档。

需要决策：Agent Loop 以什么形态获得 Working Context 参与能力；W3 自动触发
信号、W4 晋升、W5 fork/merge 在接线后由谁驱动；验收形态与 RULE-10 口径。

## 决策

1. **薄缝形态：一条加法式「快照供给缝」，不是自动化、不是门面**。
   `AgentLoop` 增加一个**可选**的快照供给依赖（沿既有
   `set_event_store`/`set_tool_registry` 的 setter 注入先例）：宿主注入
   "取当前会话已提交快照" 的只读回调；`build_request` 在装配请求时消费它，
   经既有 `context_items_from_working_context` 转换（Layer 0 唯一准入通道，
   kind 分区与 `UntrustedExternalData` authority 语义不变）后，以确定性带
   标签块渲染进请求（provenance source `mira.agent-loop.working-context.v1`，
   RULE-09：模型介导派生投影永不提升 authority）。未注入该依赖时，请求装配
   与现状**逐字节一致**（零漂移，M3 契约的加法扩展）。
2. **注入语义（冻结）**：每步 `build_request` 调用供给回调恰一次；仅
   **已提交**快照进入请求；**身份对齐门槛**（2026-09-23 修订：落在 Loop 实
   际持有的标识符上）——快照 `session_id`/`task_id`/`task_epoch` 与当前
   `AgentLoopSpec` 任务帧（`include/mira/agent_loop.hpp:93-99`）完全一致才
   注入，不一致（含父/子会话错配、陈旧 task_epoch）则本步跳过注入并记诊
   断计数，绝不把其他任务帧的状态拼进当前请求。**环境纪元边界（显式冻
   结）**：`AgentLoopSpec` 与 Loop 均不持有 `environment_epoch`，Loop 不做
   环境纪元比较、M3 契约不为此扩面；需要环境纪元门控的宿主在供给回调内闭
   合（回调闭包当前环境纪元并在失配时返回 nullopt），快照条目的 epoch 标
   注照常随渲染进入请求（可审计）。渲染块有界（per-request 条目数与字节上
   界，超出按固定顺序截断并标注，RULE-08）。供给回调返回"无已提交快照"为
   正常空态（零条目）；供给**失败**（store 不可用等）为降级：本步请求不含
   快照条目、发诊断事件、Loop 继续运行——与设计 §9「Curator 失败不阻塞
   Agent loop，调用方继续使用上一已提交快照」的降级精神一致；回调异常按
   Observer 回调纪律隔离转诊断，不得逃逸进请求装配。
3. **零自动化：Loop 不驱动任何 Working Context 生命周期操作**。W3 触发信
   号上报（`WorkingContextAutoCurator::on_signal`/`flush`/`drain`）、W4 终
   态晋升（`ContextMemorySupervisor::submit<WorkingContextPromotionReport>`
   泛型 Deferrable，不新增 Supervisor 方法）、W5 fork/merge（三纯函数 +
   既有 §5.2 提交）全部保持**宿主显式编排**；Loop 内零 Supervisor 调用、
   零隐藏后台循环/定时器（AGENTS.md Executor 纪律；M23 §5/M24 §5 决策表与
   DEC-044 第 7 条的宿主显式冻结零触碰）。宿主操作序沿既有冻结语义：任务
   边界 = `flush` future 有界等待先于终态置位；合并 = 子返回事件进入父会
   话 → 父水位严格前进 → `merge_working_context_delta` → 既有
   `commit_working_context` 提交（同水位异 digest `conflicting-watermark`
   fail-closed 不豁免）。
4. **消费场景列明（防投机 API）**：本缝的最小消费方为两类——主 Loop 会话
   与子代理 Loop 会话（[DEC-044](DEC-044-multi-agent-context-fork-boundary.md)
   的子 Session 内同样可注入，子请求消费子链已提交快照）。两者都进入
   [M25](../plans/m25-host-integration-round.md) 验收面；超出这两类的消费
   形态（如跨会话共享供给）不在本决策授权内。
5. **验收与口径**：三层确定性验收——契约/生命周期测试矩阵
   （`tests/m25/`，label `integration;m25`，IVA 编写运行与 sanitizer 取
   证）、单系统闭环集成测试（`tests/integration/`，label `integration`，沿
   `agent_harness_test.cpp` 的 MiraRuntime 会话 + scripted provider 形态）、
   参考宿主示例（`examples/`，label `consumer`，离线可跑，沿
   `mira_stateful_consumer` 先例）。无真实模型：不声明语义质量、token 收益
   或 continuation correctness（RULE-10）；issue #48 的 A/B/C 三臂对照指标
   归真实模型轮与 DEC-032 Stage E（`MNT-202609-27` 证据通道）。示例与测试
   中的编排**不是契约**——契约面 = 公开头文件 + [M25] §4 + API 手册条目 +
   测试矩阵；编排自动化不得渗入 Loop 实现。
6. **范围边界**：本轮钉死在 Core 参考宿主（simulator/offline、脚本化
   provider）；平台 Adapter（Android 等）的宿主接入另行立项。自动 fork/merge、
   自动晋升触发与模型介导语义合并均不并入宿主集成轮——三者分别被 M24 §5
   决策表、DEC-044 第 7 条与 DEC-044 第 5 条冻结为须独立上位决策的形态；
   若未来立项自动化，须按项目管理规范 §8 显式推翻对应决策记录并论证动机，
   不得实现期顺手带入。

## 备选方案

- **示例先行轮（零 Core 变更，编排全落 examples）**（否决）：AgentLoop 无
  任何上下文注入点，示例只能绕开 Loop 直驱 ModelGateway——「Agent Loop 接
  线」名存实亡，接线不成契约则宿主重抄样板，行为等价性无从评审。
- **Loop 内建自动化接线（Loop 依赖 Supervisor 并自动发信号/自动晋升/自动
  fork-merge）**（否决）：正面违反 M23 §5 决策表（触发方式 = 宿主显式）与
  DEC-044 第 7 条（自动化须上位决策、不得顺手带入）；把 M3 冻结的 Loop 契
  约扩到 M24 决策表否决过的「冻结面膨胀」形态；无真实模型下收益不可声明
  （RULE-10）。
- **组合门面组件（Loop 零改动，新增编排门面包装 Loop+Supervisor+stores）**
  （否决）：门面无法把快照送进 Loop 私有的 `build_request`（无注入点）——
  要么退化为"绕开 Loop 直驱 gateway"（接线名存实亡），要么仍须在 Loop 上
  开缝回到本决策；多出一个新公共组件却未解决核心问题，投机面更大。
- **并入真实模型 canary 冒烟**（推迟）：受控凭据通道（`MNT-202609-27` 同
  源）是本轮不需要的外部依赖；如日后需要，作为独立工作项单独冻结、单独
  manifest、不进回归门禁，结论按冒烟级措辞，不承载对照指标。

## 影响与风险

- `include/mira/agent_loop.hpp` 与 `src/model/agent_loop.cpp` 加法变更（core
  模块内，`tools/architecture-policy.json` 零差异：`core` requires 不变）；
  M3 AgentLoop 契约为加法扩展，未注入依赖的行为逐字节不变。
- 风险：供给缝的注入时序语义写错成为回归源。处置：注入恰一次、身份对齐门
  槛、有界渲染、降级语义全部跑前冻结（M25 §4）并落测试断言（HI-G1/HI-G2）。
- 风险：示例/集成测试中的编排被误当契约，或编排自动化渗入 Loop 实现（M24
  §2 禁止的「顺手带入」）。处置：DEC-045 第 5 条声明"示例非契约"；HI-G3
  断言 Loop 全程零 Supervisor/AutoCurator/fork/merge 调用。
- 风险：显式编排被误读为「显式 API 不可用」而催生不当自动化需求。处置：
  高质量参考宿主与 API 手册条目；宿主实际编排时点可作为观察项记录（不作
  指标声明），供未来自动化上位决策论证动机。
- 风险：语义价值空窗——接线交付后仍无端到端价值声明。处置：诚实口径（本
  决策第 5 条），价值验证锚定真实模型轮与 Stage E（`MNT-202609-27` 通道，
  In Progress 可追踪）。

## 验证方式

- 由 [M25](../plans/m25-host-integration-round.md) 交付并验证：门禁
  `HI-G1`–`G6`（`tests/m25/` + `tests/integration/working_context_host_test.cpp`
  + `examples/working_context_host_consumer.cpp`）；本地门禁与 PR CI 沿
  M23/M24 口径（全量 ctest、三 sanitizer、五检查、clang-tidy、NDK 两 ABI）。
- 本决策若需修订（如未来引入 Loop 内自动化或模型介导合并），按项目管理规
  范 §8 走决策变更；门禁冻结后不得静默放宽（阶段冻结协议 §6）。

## 关联文档和工作项

- [M25：宿主集成轮——Agent Loop 快照供给缝与宿主编排参考](../plans/m25-host-integration-round.md)
- [DEC-035](DEC-035-context-curator-working-context.md)（Layer 0 准入与
  W1–W5 分阶段）、[DEC-044](DEC-044-multi-agent-context-fork-boundary.md)
  （子代理边界与宿主显式编排）、[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)
  （双平面；W-06/W-07）
- [M24](../plans/m24-context-curator-stage-w5.md)（fork/merge 契约与宿主操
  作序）、[M23](../plans/m23-memory-promotion-stage-w4.md)（晋升入口与泛型
  Deferrable 路由）、[M22](../plans/m22-working-context-stage-w3.md)（
  AutoCurator 触发与 flush 屏障语义）
- `include/mira/context_working_context.hpp`（`context_items_from_working_context`
  Layer 0 转换）、`include/mira/context_working_context_auto.hpp`（
  on_signal/flush/drain）、`include/mira/context_memory_supervisor.hpp`（
  submit 泛型路由与 schedule_working_context_* 既有路由）、
  `include/mira/context_working_context_fork.hpp`（fork/delta/merge）
