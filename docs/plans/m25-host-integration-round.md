# M25：宿主集成轮——Agent Loop 快照供给缝与宿主编排参考（Working Context 的首个 Runtime 生产消费者）

> 状态：Completed（2026-09-23 立项并交付关闭；`HI-G1`–`G6` 全绿，
> [PR #69](https://github.com/Linductor-alkaid/mira/pull/69) CI 24/24 全绿）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（DEC-035 链的下游
> 消费轮；接线形态与边界见
> [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)，控制面
> 约束遵循 [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md)
> 双平面与 W-06/W-07）
> 前置：Stage W4 关闭（已满足，[M23](m23-memory-promotion-stage-w4.md)，PR
> #67）；Stage W5 关闭（已满足，[M24](m24-context-curator-stage-w5.md)，PR
> #68）；接线验收形态冻结（已满足——M24 §2/§5 留痕「接线验收形态无任何已
> 冻结文档，八问不可答」，随本立项以
> [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md) 冻结，
> 本文件 §4 承载其契约面）
> 建议发布点：非发布物；产出 Agent Loop 快照供给缝契约与宿主编排参考（真实
> 模型轮与 Stage E 评估矩阵的接线前提）
> 更新日期：2026-09-23（立项并交付关闭）

## 1. 目标

按 [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md) 冻结
的接线形态，交付宿主集成轮（契约 + 集成 + 示例三层，全部确定性验证）：

- **快照供给缝（唯一 Core 契约变更）**：`AgentLoop` 增加可选的快照供给依赖
  （沿 `set_event_store`/`set_tool_registry` setter 注入先例）；`build_request`
  装配请求时经既有 `context_items_from_working_context`（Layer 0 唯一准入
  转换）消费当前会话**已提交**快照并以确定性带标签块渲染进请求。未注入时
  请求装配与现状逐字节一致（M3 契约的加法扩展，零漂移）。
- **注入语义冻结**：每步恰一次供给调用；身份对齐门槛（快照 `session_id` /
  `task_id` / `task_epoch` 与当前 `AgentLoopSpec` 任务帧一致，否则跳过并计
  诊断；环境纪元不比较——Loop 不持有该值，门控归宿主回调，见 §4.2）；渲染
  有界（固定顺序截断 + 标注，RULE-08）；供给失败降级（本步无快照条目 +
  诊断事件，Loop 继续，回调异常隔离不逃逸）。
- **宿主编排参考（零自动化）**：W3 信号上报（`on_signal`/`flush`/`drain`）、
  W4 终态晋升（`submit<WorkingContextPromotionReport>` 泛型 Deferrable）、
  W5 fork/merge（三纯函数 + 既有 §5.2 提交）全部由宿主显式编排并在集成测
  试与参考宿主中示范；Loop 内零 Supervisor/AutoCurator 调用、零隐藏后台循
  环。宿主操作序沿既有冻结语义：`flush` future 有界等待先于终态置位；合并
  仅在父水位严格前进时提交（同水位异 digest `conflicting-watermark`
  fail-closed 不豁免）。
- **验收三层**：契约/生命周期矩阵（`tests/m25/`）+ 单系统闭环集成测试
  （`tests/integration/`）+ 参考宿主示例（`examples/`，离线 consumer 门
  禁）；无模型，不声明语义质量与 token 收益（RULE-10）。

## 2. 范围与非目标

范围：`include/mira/agent_loop.hpp` 与 `src/model/agent_loop.cpp` 加法扩展
（供给依赖 setter、缝选项、`build_request` 注入与降级路径——core 模块内，
`tools/architecture-policy.json` 零差异）；`tests/m25/` 契约/生命周期测试
矩阵；`tests/integration/working_context_host_test.cpp` 单系统闭环；
`examples/working_context_host_consumer.cpp` 参考宿主（CMake label
`consumer`）；文档同步。

非目标：**不实现 Loop 内自动化**——Loop 不发 W3 信号、不触发晋升、不调用
fork/merge（M23 §5/M24 §5 决策表与
[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md) 第 7
条冻结的宿主显式现状零触碰；自动化若未来立项须显式上位决策，不得实现期顺
手带入）；**不做模型介导语义合并**（DEC-044 第 5 条：后续加法增强、独立决
策；机械确定性合并口径不变）；**不做平台 Adapter 宿主接入**（范围钉死
Core 参考宿主 simulator/offline，Android 等另行立项）；**不做真实模型
canary**（受控凭据通道为本轮不需要的外部依赖，如需要按独立工作项经受控通
道另行冻结）；不修改 `IContextCurator`/`WorkingContextAutoCurator`/
`ContextMemorySupervisor`/store/commit 管线/W4 晋升映射/W5 fork-merge 契约
（本轮是既有契约的编排消费面，唯一新契约是供给缝本身）；不修改
`StandardContextManager` 与 Layer 0 既有语义（缝只消费
`context_items_from_working_context` 的既有转换产物，快照不自准入的裁决语
义不变）；不声明语义质量、token 收益或 continuation correctness，不跑
issue #48 的 A/B/C 三臂对照（RULE-10，无模型；对照指标归真实模型轮与
Stage E，`MNT-202609-27` 证据通道）；本阶段无真实模型网络调用。

## 3. 设计与决策依据

- [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)（接线
  形态：薄缝、注入语义、零自动化、消费场景、三层验收）——本文件 §4 是其
  契约面
- [DEC-035](../decisions/DEC-035-context-curator-working-context.md) 第 2 条
  （快照只经 Layer 0 候选转换进入模型请求）、
  [Context Curator 设计](../design/context_curator_design.md) §7（Layer 0
  准入路径）、§9（Curator 失败降级精神——供给失败的降级同源）
- [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 与
  [Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
  §4（W-06 不引入第二套动作执行通道；W-07 同 Session 单动作租约——Loop
  执行面零改动，缝是纯上下文供给）
- [M22](m22-working-context-stage-w3.md)（`on_signal`/`flush` 屏障语义：
  宿主须 await flush future 再置终态）、
  [M23](m23-memory-promotion-stage-w4.md)（`submit<T>` 泛型 Deferrable、
  不新增 Supervisor 方法）、[M24](m24-context-curator-stage-w5.md)（
  fork/delta/merge 契约与「父水位严格前进才可提交」操作序）
- 集成与 consumer 先例：`tests/integration/agent_harness_test.cpp`
  （MiraRuntime 会话 + executor.submit_auto 托管 Loop + scripted provider，
  `mira_add_test` label `integration`）、`examples/stateful_agent_consumer.cpp`
  与 `CMakeLists.txt` consumer 注册（label `consumer`、离线 `TIMEOUT 60`）
- `RULE-02`（Loop/编排工作由 Executor 管理，future 必消费）、`RULE-08`
  （渲染块有界）、`RULE-09`（快照条目 `UntrustedExternalData` 永不提升
  authority）、`RULE-10`（指标口径诚实）

## 4. 冻结的契约语义（跑前冻结，2026-09-23）

### 4.1 接线形态（承载 DEC-045，不重复裁决）

薄缝 = AgentLoop 的可选快照供给依赖 + `build_request` 注入；编排 = 宿主显
式（W3/W4/W5 面零自动化）。本节不新增语义；「示例非契约」——契约面 = 公开
头文件 + 本文件 §4 + API 手册条目 + 测试矩阵。

### 4.2 供给缝注入语义

```cpp
// agent_loop.hpp（加法，契约草案；未注入依赖时行为与现状逐字节一致）：
struct WorkingContextSeamOptions final {
    // 渲染上界（RULE-08，文档化默认值）：超出按 Layer 0 转换输出的固定
    // 顺序（section 声明序 + 条目序）截断并追加标注。
    std::size_t max_items = 32;
    std::size_t max_chars = 8'192;

    [[nodiscard]] Result<void> validate() const;  // bound > 0
};

// 只读供给回调：返回 nullopt = 当前会话无已提交快照（正常空态）；
// Result 错误 = 供给失败（降级语义，见下）。
using WorkingContextSupplier =
    std::function<Result<std::optional<WorkingContextSnapshot>>()>;

void set_working_context_supplier(WorkingContextSupplier supplier,
                                  WorkingContextSeamOptions options = {});
```

固定语义：

- **恰一次调用**：每步 `build_request` 装配时调用供给回调恰一次；Loop 不缓
  存快照内容跨步复用（每步重取，store 读是廉价只读操作且内部串行化）。
- **身份对齐门槛**（2026-09-23 修订：落在 Loop 实际持有的标识符上）：
  `AgentLoopSpec` 携带 `session_id`/`task_id`/`task_epoch`（
  `include/mira/agent_loop.hpp:93-99`），快照三项与当前 spec 完全一致才注
  入；不一致（其他任务、陈旧 task_epoch、父/子会话错配）则本步跳过注入并
  递增诊断计数（可见于既有事件/统计面），绝不注入其他任务帧的状态。
  **环境纪元边界（显式冻结）**：`AgentLoopSpec` 与 Loop 成员均不持有
  `environment_epoch`，Loop 不做环境纪元比较，M3 契约不为此扩面（DEC-002
  最小变更）；需要环境纪元门控的宿主在**供给回调内闭合**（回调可闭包当前
  环境纪元并在失配时返回 nullopt——零条目正常空态），快照条目的 epoch 标
  注照常随渲染进入请求（可审计）。
- **Layer 0 唯一转换**：注入内容 = `context_items_from_working_context(snapshot)`
  的产物（constraints → P1 `UserConstraint`、其余 section → P3
  `CheckpointSummary`、全部 `UntrustedExternalData`、条目携带源事件
  provenance 与 epoch 标注）；Loop 不发明第二套转换。
- **确定性渲染**：快照条目渲染为独立带标签块（provenance source
  `mira.agent-loop.working-context.v1`，与既有 `mira.agent-loop.system.v1`/
  `mira.agent-loop.context.v1` 同族；文本格式随实现冻结于契约四件套，跨进
  程字节一致），置于工具结果块之前、用户指令块之后；超出 `max_items`/
  `max_chars` 按转换输出固定顺序截断并追加 `[working context truncated]`
  标注。
- **降级语义**：供给回调返回错误 → 本步请求不含快照条目、发一次诊断事件
  （类型与载荷随实现冻结，语义 = 降级可见、不阻塞）、Loop 继续执行下一步；
  回调抛出异常按 Observer 回调纪律隔离转诊断（AGENTS.md），不得逃逸进请求
  装配或终止 Loop。空 store（nullopt）是正常空态，零条目、零诊断。
- **读侧无水位约束**：缝只读；写侧（curation/commit/merge）纪律全部归既
  有 §5.2 管线，缝不绕过、不豁免、不新增写路径（协议 §5）。

### 4.3 宿主编排时序（既有冻结语义的调用序，本文件只冻结「宿主在何处调用」）

- **W3 触发面**：宿主在会话事件推进点显式调用
  `WorkingContextAutoCurator::on_signal(session, input)`（返回的 shared
  future 必须消费）；任务边界显式调用 `flush(session, input)` 并**有界等待
  其 future 后才置 session/task 终态**（M22 §4.2 冻结语义）。
- **W4 晋升面**：宿主在任务终态边界显式调用
  `promote_working_context_to_memory`，经
  `ContextMemorySupervisor::submit<WorkingContextPromotionReport>(…,
  SupervisedOpClass::Deferrable, …)` 泛型路由执行（不新增 Supervisor 方
  法），future 必消费。
- **W5 fork/merge 面**：宿主操作序 = `fork_working_context`（父已提交快照 →
  子会话基线）→ 子会话自有链（可注入子 Loop 供给缝）→ 子返回事件进入父会
  话 → 父水位严格前进 → `working_context_delta_from_fork` +
  `merge_working_context_delta` → 既有 `commit_working_context` 提交；同水
  位异 digest `conflicting-watermark` fail-closed 不豁免（M24 §4.4 第 6 条
  的宿主侧调用序固化）。
- **零自动化断言面**：Loop 类型与实现全程不引用/不调用 Supervisor、
  AutoCurator、promotion 与 fork/merge 入口（测试以类型/符号级断言 + 行为
  断言双面冻结，HI-G3）。

### 4.4 生命周期与边界

- **Executor 路由与两个关闭面**（2026-09-23 修订：按组件 + API 显式拆
  分）：`run()` 沿既有 M3 形态由宿主在 Executor 管理的操作内驱动（集成测
  试以 `executor.submit_auto` 托管并消费 future）；缝的供给调用发生在请求
  装配的同步路径内（有界只读，非阻塞 I/O）。关闭序与断言归属：
  (i) **Supervisor 面**——`ContextMemorySupervisor::begin_shutdown()`
  （§17.2 序，`include/mira/context_memory_supervisor.hpp:180`）执行后，
  后续编排提交（`on_signal`/晋升 `submit`）被拒、在途 Deferrable resolve
  `Cancelled`（既有语义，W4-G5 同款；begin_shutdown 不关闭 store 与
  Executor，均归宿主）；
  (ii) **Executor 面**——Executor `shutdown(wait_for_tasks)`
  （`third_party/executor/include/executor/executor.hpp:102`）后，
  `submit_auto` 的新 `run()` 提交被拒（AGENTS.md 第 8 条：关闭中的提交转
  化为明确结果）；
  (iii) **Loop 面**——在途 `run()` 经 `OperationContext` 取消探针取消闭合
  （M3 既有语义），缝无独立关闭面（只读 + 同步有界，无悬挂点）。宿主关闭
  序 = 停编排生产者 → supervisor `begin_shutdown()` → executor
  `shutdown(true)` → 消费 future。
- **无隐藏线程**：缝不创建线程/定时器/后台循环（AGENTS.md 纪律）；编排的
  异步工作全部经既有 Supervisor 路由。
- **擦除**：`erase_session` 后供给回调自然返回 nullopt（快照投影已删），
  Loop 侧零特判；跨会话擦除编排归宿主（M23 §4.4 同款边界）。
- **恢复**：缝无自有状态（无缓存、无水位记录）——恢复 = store 按既有规则
  重建后供给自然恢复；Loop 重启后首步重取。

## 5. 阶段冻结协议必答八问（2026-09-23）

1. **行为**：AgentLoop 获得可选快照供给能力——未注入零漂移，注入后每步请
   求按 §4.2 携带已提交快照条目（身份对齐、有界渲染、失败降级）；宿主编排
   参考固化 W3/W4/W5 调用序。对应设计：DEC-045 全部条目；
   [Context Curator 设计](../design/context_curator_design.md) §7 的首个
   in-Loop 消费者（实现注记随交付回填）。
2. **所有者**：无新增可变状态——供给依赖由 Loop 持有（宿主注入的只读回
   调），缝无缓存无水位；快照 store 唯一写路径仍是 `commit_working_context`。
3. **契约**：最小公共契约面 = `agent_loop.hpp` 的 `WorkingContextSeamOptions`
   + `WorkingContextSupplier` + `set_working_context_supplier` 一项；错误与
   降级语义 §4.2；调用方为宿主与测试。契约四件套：公开头文件 + 本文件 §4 +
   API 手册 `docs/api/model-agent-loop.md` 条目（`docs/api/context-memory.md`
   交叉引用）+ `tests/m25/`。
4. **层与依赖**：变更入 `core` 模块（`include/mira` + `src/model`），仅依赖
   既有 `mira` 头与标准库；`tools/architecture-policy.json` 零差异（`core`
   requires 不变）；`examples` 模块既有 requires（core/workflow/state_store/
   executor/simulator_adapter）覆盖参考宿主所需，零差异。
5. **复用**：注入内容复用 `context_items_from_working_context`（Layer 0 唯
   一转换）；生命周期复用 W3 `on_signal`/`flush`、W4 `submit<T>` 泛型路
   由、W5 三纯函数与 §5.2 提交管线；setter 注入复用 Loop 既有
   `set_event_store`/`set_tool_registry` 先例。不新增平行转换/写路径/编排
   组件。
6. **时序**：每步恰一次供给调用；身份对齐门槛（spec `session_id`/
   `task_id`/`task_epoch` 三腿）拦截跨任务帧/跨会话/陈旧 task_epoch 快照，
   环境纪元门控归宿主回调；合并时序沿用 M24 §4.4（父水位严格前进才可提
   交，同水位冲突 fail-closed）；终态时序沿用 M22 §4.2（flush await 先于
   终态置位）；迟到/取消/关闭语义全部沿既有管线，缝不引入新竞态面（只读
   + 同步有界）。
7. **Executor 路由**：`run()` 由宿主经 `executor.submit_auto` 托管（集成
   测试先例），句柄宿主消费；编排的异步工作全部经既有 Supervisor Deferrable
   路由；关闭顺序 = 停编排生产者 → `ContextMemorySupervisor::begin_shutdown()`
   （§17.2 序，编排提交拒绝 + 在途取消）→ Executor `shutdown(wait_for_tasks)`
   （新 `run()` 提交拒绝）→ 消费 future；在途 `run()` 走 Loop 既有取消路
   径；无新路由、无裸线程。
8. **验证上下文**：评审者最小阅读集 = 本文件 §4、
   [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md)、
   `include/mira/agent_loop.hpp`（缝契约）、`tests/m25/` 契约与生命周期矩
   阵、`tests/integration/working_context_host_test.cpp`、
   `examples/working_context_host_consumer.cpp`。

### 决策记录表

| 决策点 | 选择 | 被否备选 | 依据 |
| --- | --- | --- | --- |
| 接线形态 | 薄缝：AgentLoop 可选快照供给依赖 + `build_request` 注入（DEC-045） | 示例先行（Loop 无注入点，接线名存实亡）；Loop 内建自动化（推翻 M23/M24 冻结 + 契约膨胀）；组合门面（送不进私有 `build_request`，投机面更大） | DEC-045 第 1 条与备选方案节 |
| 注入内容 | 仅已提交快照，经 `context_items_from_working_context` 既有转换渲染 | Loop 直读 store 拼装 section（第二套转换，破坏 Layer 0 唯一准入）；注入 checkpoint 条目（双投影并喂，宿主择一纪律交 Loop 裁决） | §4.2；DEC-035 第 2 条 |
| 注入门槛 | 身份对齐落在 Loop 实际持有标识符（spec `session_id`/`task_id`/`task_epoch`），不一致跳过 + 诊断；环境纪元不比较（Loop 不持有，M3 契约不扩面），门控归宿主回调闭包 | 引用 spec 上不存在的 `environment_epoch`（契约不可实现——2026-09-23 评审核验 `agent_loop.hpp:93-99` 后否决）；给 spec 加该字段（为一条门槛扩 M3 契约面）；无条件注入 `latest(session)`（跨任务帧状态污染当前请求）；注入前做 stale 改写（发明 Layer 0 之外的新陈旧语义） | §4.2；`agent_loop.hpp:93-99` 字段清单；W1-G4 epoch 标注语义（条目级标注照常随渲染可审计） |
| 渲染上界 | `WorkingContextSeamOptions`（32 条 / 8 KiB 文档化默认值，固定顺序截断 + 标注） | 无上界（违反 RULE-08）；复用 `WorkingContextMergeOptions`（语义错位：那是投影/合并的输入上界，非渲染上界） | §4.2；RULE-08 |
| 失败降级 | 供给错误 → 本步无快照条目 + 诊断事件，Loop 继续；回调异常隔离转诊断 | 供给失败终止 Loop（违反设计 §9 降级精神）；吞掉失败无诊断（协议 §5 拒绝形态） | §4.2；Context Curator 设计 §9 |
| 触发语义 | 零自动化：W3 信号/W4 晋升/W5 fork-merge 全部宿主显式，Loop 零 Supervisor 调用 | Loop 自动发信号/自动晋升/自动 fork-merge（推翻 M23 §5 决策表与 DEC-044 第 7 条，须上位决策）；示例中演示自动化（渗入实现即「顺手带入」） | §4.3；DEC-044 第 7 条；M24 §5 决策表 |
| 合并时序 | 子返回事件进入父会话 → 父水位严格前进 → merge → 既有 §5.2 提交 | 同水位直接提交（`conflicting-watermark` fail-closed 不豁免，M24 §4.4 第 6 条冻结）；为接线开提交例外（第二写路径） | §4.3；M24 §4.4；协议 §5 |
| 验收形态 | 三层：契约/生命周期矩阵 + 单系统闭环集成 + consumer 示例；全部确定性断言，无模型无语义声明 | 真实模型 canary（本轮不需要的外部依赖；如需要独立工作项经受控通道）；A/B/C 三臂对照（RULE-10 禁止——脚本 provider 度量的是脚本行为非语义质量） | §6；DEC-045 第 5 条；RULE-10 |
| 范围边界 | Core 参考宿主（simulator/offline、scripted provider） | 平台 Adapter 宿主接入（另行立项）；跨会话共享供给等新消费形态（超出 DEC-045 第 4 条授权） | §2；DEC-045 第 4/6 条 |

## 6. 测试与退出条件

门禁形态说明：本轮无模型调用（scripted provider），门禁落三层确定性断言，
由 Independent-Verification-Agent 独立编写、运行并复验（含 sanitizer 取
证）。

- [x] `HI-G1` 零漂移与注入契约（`tests/m25/m25_loop_seam_test.cpp`，label
  `integration;m25`）：未注入供给依赖 → 请求装配与现状逐字节一致（既有
  system/goal/follow-up/tool 块不变）；注入 + 身份对齐 → 快照八 section
  条目经 Layer 0 转换渲染进请求（kind 分区、`UntrustedExternalData`
  authority、`mira.agent-loop.working-context.v1` source 标签、确定性顺
  序、跨进程字节一致）；空 store → 零条目零诊断；身份不匹配三腿（spec
  `session_id`/`task_id`/`task_epoch` 任一与快照不一致，含父子会话错配）→
  跳过注入 + 诊断计数可见；宿主回调内环境纪元门控演示（回调闭包失配返回
  nullopt → 零条目，Loop 侧零特判）。
- [x] `HI-G2` 有界与降级（同文件）：超 `max_items`/`max_chars` 固定顺序截
  断 + `[working context truncated]` 标注；供给错误 → 本步请求无快照条目
  + 诊断事件 + Loop 继续且下一步恢复注入；回调异常隔离不逃逸；请求体量有
  界断言（RULE-08）；每步恰一次供给调用（回调计数断言）。
- [x] `HI-G3` 零自动化与宿主编排（`tests/m25/m25_host_orchestration_test.cpp`，
  label `integration;m25`）：Loop 全程零 Supervisor/AutoCurator/promotion/
  fork/merge 调用（类型级 + 行为级双断言）；宿主操作序冻结——`flush`
  future 有界等待先于终态置位；晋升经 `submit<WorkingContextPromotionReport>`
  Deferrable 且 future 被消费；fork/merge 按 M24 §4.4 序（合并仅在父水位
  严格前进时提交成功，同水位异 digest → `conflicting-watermark` 且 store
  不变）。
- [x] `HI-G4` 生命周期与两个关闭面（同文件）：`run()` 经 `executor.submit_auto`
  托管且 future 被消费；**Supervisor 面**——`begin_shutdown()` 后编排提交
  （on_signal/晋升 submit）被拒、在途 Deferrable resolve `Cancelled`（W4-G5
  同款语义）；**Executor 面**——`shutdown(wait_for_tasks)` 后 `submit_auto`
  的新 `run()` 提交被拒（AGENTS.md 第 8 条）；**Loop 面**——在途 `run()` 经
  `OperationContext` 取消 → `Cancelled` 且无悬挂，缝供给在取消/关闭路径无
  阻塞点；快照 store 并发读与 curation 写共存安全（store 既有串行化语义）。
- [x] `HI-G5` 单系统闭环（`tests/integration/working_context_host_test.cpp`，
  label `integration`，沿 `agent_harness_test.cpp` 的 MiraRuntime + scripted
  provider 形态）：主会话闭环——宿主信号 → curation 提交 → 后续请求携带
  快照条目 → 任务边界 flush → 终态 → 晋升，事件审计在案；子代理闭环——
  fork 基线 → 子 Loop 会话注入子链快照 → 子返回 → 父合并 → 父下一请求含
  合并条目；seam 降级场景在闭环中可见（诊断事件）。
- [x] `HI-G6` 参考宿主与文档（`examples/working_context_host_consumer.cpp`，
  CMake 注册 label `consumer`、离线 `TIMEOUT 60`，失败非零退出）：最小宿主
  端到端跑通（session + Loop + 供给缝 + supervisor + auto curator + store
  + fork/merge + 晋升，scripted provider）；文档同步完成——API 手册
  `docs/api/model-agent-loop.md` 供给缝条目、`docs/api/context-memory.md`
  交叉引用、README 能力表、总计划、[DEC-035](../decisions/DEC-035-context-curator-working-context.md)/[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md)
  关联回填、Context Curator 设计实现注记、术语表（如需新词条）。
- [x] 本地门禁：debug 全量 ctest 全绿（含新增 m25 两目标 + 集成目标 +
  consumer 目标）、ASAN/UBSAN/TSAN 新增目标零报告、`format-check`/
  `docs-check`/`platform-boundary-check`/`sbom-check`/`architecture-check`
  通过、clang-tidy 预检被改库源编译单元（`agent_loop.cpp` 等）零违例、本
  机 NDK 两 ABI 交叉编译预演通过；既有 m3（AgentLoop 既有套件）与 m20–m24
  套件零回归（缝为加法，未注入路径零漂移由 HI-G1 首项断言背书）。本地取
  证为针对性（debug 构建零警告、`docs-check` OK、`ctest -R consumer` 4/4、
  m25+集成 4/4、m3 两套件回归全绿；§9 第三次记录）；sanitizer、clang-tidy
  强门禁与 NDK 交叉编译面按冻结口径由 [PR #69](https://github.com/Linductor-alkaid/mira/pull/69)
  CI 管线全量复跑取证（见下条），本记录不另行声明本地 sanitizer/NDK 取证。
- [x] PR CI（Linux/Windows/Android/sanitizers/quality）全绿后回填验证记录
  并关闭本阶段。[PR #69](https://github.com/Linductor-alkaid/mira/pull/69)
  （head `4abf8ee`）CI 24/24 全部 SUCCESS：linux（gcc/clang × Debug/Release）、
  windows（Debug/Release）、android（arm64/x86_64 NDK 交叉）、sanitizers
  （ASAN/UBSAN/TSAN）与 quality 管线全绿。

## 7. 工作项

- [x] `M25-01` 阶段立项与本文件 §4/§5/§6 冻结 +
  [DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md) 交付
  （时间戳先于任何实现与测试）。
- [x] `M25-02` 供给缝契约与实现：`agent_loop.hpp`（`WorkingContextSeamOptions`
  + `WorkingContextSupplier` + setter）与 `agent_loop.cpp`（`build_request`
  注入、身份对齐门槛、有界渲染、降级路径）；未注入零漂移。
- [x] `M25-03` 契约/生命周期测试矩阵（§6 `HI-G1`–`HI-G4`，`tests/m25/`，
  CMake 注册 label `integration;m25`）——测试的编写、运行与 sanitizer 取
  证由 Independent-Verification-Agent 独立完成并复验。
- [x] `M25-04` 单系统闭环集成测试（§6 `HI-G5`，
  `tests/integration/working_context_host_test.cpp`，`mira_add_test` label
  `integration`）——同由 Independent-Verification-Agent 独立完成并复验。
- [x] `M25-05` 参考宿主示例（§6 `HI-G6`，
  `examples/working_context_host_consumer.cpp`，CMake 注册 label
  `consumer`、离线可跑）。
- [x] `M25-06` 文档同步（`M25-04`–`05` 清单）+ 本地全门禁与 PR CI 取证回
  填。

## 8. 风险与阻塞

- 风险：注入时序语义写错成为回归源（快照内容进错请求、跨任务帧污染）。处
  置：§4.2 逐条冻结（恰一次、身份对齐、有界、降级）并由 HI-G1/HI-G2 断言
  背书；未注入零漂移为 HI-G1 首项。
- 风险：示例/集成测试中的编排被误当契约，或编排自动化渗入 Loop 实现。处置：
  DEC-045 第 5 条「示例非契约」；HI-G3 类型级 + 行为级双断言冻结 Loop 零编
  排调用。
- 风险：供给缝被质疑为投机 API。处置：DEC-045 第 4 条列明两类消费方（主
  Loop + 子代理 Loop）且都进入 HI-G5 闭环验收；超出形态不在授权内。
- 风险：显式编排被误读为「显式 API 不可用」而催生不当自动化需求。处置：高
  质量参考宿主与 API 手册条目；宿主编排时点可作观察项记录（不作指标声明），
  供未来自动化上位决策论证动机。
- 风险：语义价值空窗——接线交付后仍无端到端价值声明。处置：诚实口径（RULE-10），
  价值验证锚定真实模型轮与 DEC-032 Stage E（`MNT-202609-27` 证据通道，
  In Progress 可追踪）。
- 外部：无（不依赖真机、凭据、外部语料或真实模型；scripted provider 离线
  可跑）。

## 9. 验证记录

2026-09-23：`M25-01` 立项，本文件 §4/§5/§6/§7 冻结（先于任何实现与测试），
[DEC-045](../decisions/DEC-045-agent-loop-working-context-seam.md) 同日交
付。进入门槛复核：Stage W4/W5 已关闭（[M23](m23-memory-promotion-stage-w4.md)
PR #67、[M24](m24-context-curator-stage-w5.md) PR #68）；M24 §2/§5 留痕的
「接线验收形态无任何已冻结文档」随 DEC-045 冻结满足——本立项即 M24 决策
表所指向的「宿主集成轮另行立项」。工作分支
`feat-m25-host-integration-round`。实现未开始；后续验证记录按日期追加。
立项前核验（本次实际执行）：`grep -n "Curator|curator|WorkingContext"
include/mira/agent_loop.hpp` → 仅命中无关行（Loop 现为零 Working Context
引用，接线确未发生）；`build_request`（`src/model/agent_loop.cpp:251-330`）
为直接请求装配、无上下文注入点；`set_event_store`/`set_tool_registry`
setter 注入先例（`include/mira/agent_loop.hpp:118-119`）；W5 交付契约面
`include/mira/context_working_context_fork.hpp:111-138`（三纯函数，候选经
既有 §5.2 管线提交）、`include/mira/context_working_context.hpp:300`
（Layer 0 转换）、`include/mira/context_working_context_auto.hpp:155-193`
（on_signal/flush/drain，flush 为终态前屏障）、
`include/mira/context_memory_supervisor.hpp:114/:153/:166/:212`（泛型
submit 与既有路由）；测试/示例基建 `tests/CMakeLists.txt:1-13`（
`mira_add_test(name, source, label)`）、`:62`（agent_harness_test 注册
label `integration`）、`CMakeLists.txt:400-424`（consumer 注册形态 label
`consumer` TIMEOUT 60）；`tools/architecture-policy.json`——`core`
requires `executor`、`examples` requires
`core/workflow/state_store/executor/simulator_adapter`（本轮零 policy 差
异）；`docs/api/model-agent-loop.md` 存在（API 手册条目落点）。
`python3 tools/check_docs.py .` → Markdown links and fences: OK。

2026-09-23（第二次）：独立计划评审发现两项阻塞缺陷，本次修订并补充核验。

- **B1 身份对齐门槛引用了 `AgentLoopSpec` 上不存在的字段**：原冻结「快照
  task_id / task_epoch / environment_epoch 与当前 AgentLoopSpec 任务帧一致
  才注入」（§1/§4.2/§5 第 6 问/决策表与 DEC-045 第 2 条）按原文不可实现：
  `AgentLoopSpec` 仅有 `task_id/session_id/task_epoch/goal/profile_id`（
  `include/mira/agent_loop.hpp:93-99`，`grep -n environment_epoch
  include/mira/agent_loop.hpp` 退出码 1），Loop 成员（`:143-152`）亦不携带
  该值，供给回调为零参闭包——实现者只能在临场扩 M3 契约、砍门槛腿或改回
  调签名之间自行抉择，均违反跑前冻结。修订裁决（三选一取「门槛落在 Loop
  实际持有标识符」）：门槛 = 快照 `session_id`/`task_id`/`task_epoch` 与
  spec 完全一致；**环境纪元不比较**——Loop 不持有该值、M3 契约不为此扩面
  （DEC-002 最小变更），环境纪元门控显式归宿主供给回调闭包（失配返回
  nullopt 正常空态），条目级 epoch 标注照常随渲染可审计；被否备选（给
  spec 加字段 / 保留不可实现命题）落决策记录表。修订落 §1/§4.2/§5 第 6
  问/决策表「注入门槛」行/HI-G1（不匹配三腿 + 宿主回调门控演示）。
- **B2 关闭断言挂错组件 API**：原 §4.4/HI-G4 把「`begin_shutdown` 后的
  run 提交被拒」写在 `executor.submit_auto` 托管语境下，但 `begin_shutdown`
  仅存在于 `ContextMemorySupervisor`（`include/mira/context_memory_supervisor.hpp:180`：
  §17.2 序、后续提交被拒、"Does not close stores or the Executor"），
  Executor 公开 API 是 `shutdown(bool wait_for_tasks)`
  （`third_party/executor/include/executor/executor.hpp:102`）——IVA 按
  原文必须在两个关闭面之间自行 reinterpret 已冻结门禁（协议 §6 禁止）。修
  订按「组件 + API」拆分为三面：Supervisor 面（`begin_shutdown()` 后编排
  提交被拒、在途 Deferrable `Cancelled`，W4-G5 同款）、Executor 面（
  `shutdown(wait_for_tasks)` 后新 `run()` 提交被拒，AGENTS.md 第 8 条）、
  Loop 面（在途 `run()` 经 `OperationContext` 取消，缝无独立关闭面）；宿
  主关闭序固化 = 停编排生产者 → supervisor `begin_shutdown()` → executor
  `shutdown(true)` → 消费 future。修订落 §4.4/§5 第 7 问/HI-G4。
- 两次核验均为本次实际执行（`sed -n '88,100p'` 与 `sed -n '143,153p'`
  include/mira/agent_loop.hpp、`grep -n environment_epoch
  include/mira/agent_loop.hpp`、`grep -n "begin_shutdown|shutdown"`
  include/mira/context_memory_supervisor.hpp 与 third_party/executor/
  include/executor/executor.hpp:102）；`python3 tools/check_docs.py .` →
  Markdown links and fences: OK。冻结状态、工作项与门禁数量不变（错误修正
  而非门禁放宽，协议 §6）。

2026-09-23（第三次）：实现交付与复验。`M25-02` 供给缝契约与实现落地——
`include/mira/agent_loop.hpp`（`WorkingContextSeamOptions` + `WorkingContextSupplier`
+ `set_working_context_supplier`，沿 setter 注入先例）与
`src/model/agent_loop.cpp`（`build_request` 在用户上下文块后、工具结果块前经
`context_items_from_working_context` 注入单一带标签块，来源标签
`mira.agent-loop.working-context.v1`；每步恰一次供给；身份对齐门槛三腿，
不匹配跳过 + `WorkingContextSeamSkipped` 诊断；环境纪元不比较；固定顺序截断 +
`[working context truncated]`；供给错误/异常隔离为恰一个
`WorkingContextSeamDegraded` 诊断且 Loop 继续；空 store 零条目零诊断；未注入
零漂移）。`M25-03`/`M25-04` 测试矩阵由 Independent-Verification-Agent 独立
交付；首轮门禁编译失败（seam API 未实现）由实现修复后，IVA 复验
`HI-G1`–`G5` 全绿（`tests/m25/` 2/2 目标 + 集成目标 1/1）。首轮复验暴露的
五处测试侧缺陷（跨实例随机 profile_id 比较、store 按快照会话键控致 session
腿不可达、stats 惰性记录需先 drain、同水位重信号走策略不触发路径、场景 2 空
store 断言注入）由 IVA 修正后复验通过，产品实现零改动。`M25-05` 参考宿主
`examples/working_context_host_consumer.cpp` 交付（自含 scripted provider 与
确定性 curator，W3 信号 → 缝消费 → W5 fork/子 Loop 缝/merge 于父水位 9 →
W3 flush 屏障 → 终态 → W4 晋升 → 关闭序全部宿主显式；请求审计断言
context-blind / 快照 / 合并条目三态；离线 `OK` 退出 0）；CMake 注册
`mira_working_context_host_consumer_test`（label `consumer`、`TIMEOUT 60`），
consumer label 4/4 全绿。`M25-06` 文档同步交付：API 手册
`docs/api/model-agent-loop.md` 供给缝节、`docs/api/context-memory.md` 宿主
集成轮消费面节与相关文档链接、README 能力表、总计划头注与 §4.1 第 18 条、
[DEC-035](../decisions/DEC-035-context-curator-working-context.md)/
[DEC-044](../decisions/DEC-044-multi-agent-context-fork-boundary.md) 关联
回填、Context Curator 设计 §7 实现注记、术语表「供给缝」词条。待办：
`HI-G6` 复验、本地全门禁（debug 全量 ctest、三 sanitizer、五检查、
clang-tidy、NDK 两 ABI）与 PR CI 取证回填（统一门禁跑）；`M25-05`/`M25-06`
复选框待复验后勾选。核验（本次实际执行）：
`ctest --preset debug -R consumer` → 4/4 全绿（含新增 consumer 目标）；
`mira_working_context_host_consumer` 独立运行输出 `working context host
example: OK`（退出码 0）。

2026-09-23（第四次，关闭）：`M25-06` 完成——
[PR #69](https://github.com/Linductor-alkaid/mira/pull/69)（head `4abf8ee`，
提交 `4afff9f` 立项冻结 + `4abf8ee` CI 修复）CI 24/24 全部 SUCCESS：linux
（gcc/clang × Debug/Release）、windows（Debug/Release）、android（arm64/
x86_64 NDK 交叉）、sanitizers（ASAN/UBSAN/TSAN）与 quality 管线全绿，双
push run（35819755432/35819758151）均 success，即 §6 本地门禁条目所列各面
（sanitizer、clang-tidy 强门禁、NDK 两 ABI 交叉、既有 m3 与 m20–m24 套件
回归）由 CI 全量复跑证实。首轮 CI 失败（linux clang Debug：`m25_loop_seam_test.cpp:359`
对 constexpr 变量的多余 lambda 捕获，clang `-Wall` 的
`-Wunused-lambda-capture` + `-Werror` 编译失败、gcc 无此检查）经升级裁决
授权对 IVA 测试文件做单行语义中性修复（捕获列表移除该 constexpr 变量，
CI job 107036229900 日志取证），复跑全绿。`HI-G1`–`G6` 与 `M25-01`–`M25-06`
全部勾选，里程碑转 `Completed`。交付面：`include/mira/agent_loop.hpp`/
`src/model/agent_loop.cpp` 供给缝加法扩展、`tests/m25/` + `tests/integration/
working_context_host_test.cpp` 门禁矩阵（IVA 所有）、
`examples/working_context_host_consumer.cpp` 参考宿主、契约四件套文档。
遗留（非本阶段范围，维持 §2 非目标留痕）：Loop 内自动化（W3 触发/W4 晋升/
W5 fork-merge 的自动化须各自上位决策）、平台 Adapter 宿主接入、真实模型
canary 与语义质量声明（归真实模型轮与 Stage E，`MNT-202609-27` 证据通道，
RULE-10）。
