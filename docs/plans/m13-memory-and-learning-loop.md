# M13：Memory 与学习闭环（阶段 F）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：[M11](m11-trajectory-compilation-and-task-induction.md)、
> [M12](m12-app-model-and-navigation.md)（已完成；阶段 F 的直接前置是阶段 D/E）
> 建议发布点：Workflow learning alpha
> 更新日期：2026-09-09

## 1. 目标

依据 [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) 阶段 F、
[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md) 与
[DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)，交付 Workflow 双
路径的记忆端：Memory 四类域组织（`MemoryKind` 之上的确定性全映射与域级检索视图）、
Workflow Episode/RecoveryLearning 学习契约（版本化、fail closed、内容寻址、纯函数转换
到 `MemoryRecord`）、失败签名与失败检索查询构建，以及 Runtime 学习闭环（结算期
Episode 记录、失败驱动升级时的检索进入 `agent_continuation`、恢复经验
`record_recovery_lesson`），使 DEC-014 §14 的长期学习闭环在 Workflow 路径上闭合：
失败被记住、被检索、恢复经验被复用（`W-03`：事件流仍是事实源，学习记录是可重建
投影）。

选择新增 M13 而非扩展 M12 的理由：M12 已按其退出条件关闭（环境知识端）；学习闭环是
独立能力增量（记忆端），验收证据（域/契约/转换/查询/结算/升级/lesson/事件/端到端
矩阵）与 M12 测试不同源，按规范使用新里程碑文件承载。这是 DEC-014 §16 阶段表的最后
一个阶段，后续方向（真实平台验证、M7 重定义）按总计划另行立项。

## 2. 范围与非目标

### 2.1 范围

- 四类域组织：`MemoryDomain` 封闭集、`memory_domain_of` 全映射（与 DEC-014 §10.2
  逐行一致）、`memory_kinds_of_domain` 逆映射、name/parse；不新增存储分层。
- 学习契约：`WorkflowEpisodeRecord`、`WorkflowRecoveryLesson`、
  `WorkflowFailureSignature` 的 JSON 往返、fail-closed 校验（`WorkflowLearningLimits`
  上限、reason_code 消毒）、内容寻址 digest、`*_to_memory_record` 纯转换
  （Verified/1.0/事件 provenance）、`recovery_lesson_from_record` 往返解析、确定性
  ID 派生。
- 失败检索：`failure_retrieval_query` 纯函数（kinds 恒为 `{Episode, RecoveryLesson}`、
  exact_terms 签名标识、无 embedding 腿）。
- Runtime 集成：`set_learning_context`（scope 拒绝 User、配置 fail closed）、结算期
  Episode 记录（DryRun 跳过、写失败不影响终态、诊断计数器）、失败驱动升级的检索与
  `agent_continuation().relevant_lessons`（降级为空、条数有界）、
  `record_recovery_lesson`（准入、两形态派生、幂等、宿主专用）。
- 事件：`WorkflowEpisodeRecorded`/`WorkflowLessonRecorded` 两员入 v1 闭集（构建/解析、
  fail closed、离线回放无 IMemory 调用）；学习投影的重建配方（DEC-030 §5）。
- 端到端取证：Run A 失败升级（检索空）→ Episode 记录 → 恢复完成 → Lesson 记录 →
  Run B 同签名失败升级时 `relevant_lessons` 同时含 Episode 与 Lesson；
  installed-consumer 覆盖新公共面。

### 2.2 非目标

- Workflow/Skill 版本资产自动索引为 Procedure 记录（无检索消费者，DEC-029 备选方案
  显式推迟）。
- 检索向量腿与召回质量调优（`RULE-10`：exact+FTS 两腿为保守首期，无语义召回声明）。
- 对话式 `update_memory` 编排与 User Model 扩展（M4 既有规则不变，DEC-014 §10.5）。
- Episode TTL/compaction/retention 策略（宿主 IMemory 后端既有能力承载）。
- Agent 自动采纳/执行 lesson 的编排（`relevant_lessons` 只供数据，编排属 Agent
  Harness 侧）。
- 学习记录专用持久化 schema（`RISK-2026-038` 沿袭；宿主后端承载 + 事件重建）。
- 训练数据导出（`RULE-12` 维持默认关闭）。
- 四类域的图级查询扩展（App Model 图查询已随 M12 交付；本阶段域查询只做 kind 过滤
  视图）。

## 3. 准入条件与设计依据

### 3.1 准入条件

- M11/M12 `Completed`（阶段 D/E 冻结：轨迹与资产语义、升级与续跑上下文、Run 结算
  路径）。PR #32/#33 CI 全绿。
- 本里程碑经维护者评审由 `Proposed` 转 `Planned`（用户指示依设计与计划推进下一步
  开发，与 M8–M12 同一授权模式）；专项设计与决策（DEC-029/030、设计 v0.6 §14）随本
  里程碑创建并冻结。

### 3.2 设计与决策依据

- [Workflow Runtime 设计](../design/workflow_runtime_design.md) §14（阶段 F 实施规范）
- [DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)
- [DEC-014](../decisions/DEC-014-agent-harness-workflow-dual-plane.md) §10/§14/§16、
  [Context 与 Memory 架构设计](../design/context_and_memory_design.md)（M4 事实层：
  scope/ACL、consolidation、检索、降级与路由）
- [DEC-023](../decisions/DEC-023-workflow-policy-set-runtime-semantics.md)（升级与
  续跑上下文）、[DEC-024](../decisions/DEC-024-conversation-patch-execution.md)（patch
  记录）、[DEC-019](../decisions/DEC-019-workflow-ir-contract.md) 至
  [DEC-028](../decisions/DEC-028-navigation-planner-and-navigate-resolution.md)

## 4. 工作项

### 4.1 四类域组织与学习契约

- [x] `M13-01` 冻结并实现 `MemoryDomain` 四类组织：`memory_domain_of` 对全部
  `MemoryKind` 的全映射（逐值与 DEC-014 §10.2 一致）、`memory_kinds_of_domain`
  逆映射、name/parse 封闭集、未知名字 fail closed。
- [x] `M13-02` 实现 `WorkflowEpisodeRecord`/`WorkflowRecoveryLesson`/
  `WorkflowFailureSignature` 契约：JSON 往返无损、未知字段/版本/枚举与上限 fail
  closed、reason_code 消毒（`[A-Za-z0-9._:-]`、≤128）、内容寻址 digest、
  `*_to_memory_record` 纯转换（Verified、confidence=1.0、事件 provenance、过
  `MemoryRecord::validate`）、`recovery_lesson_from_record` canonical 往返与非
  canonical fail closed、SHA-256 确定性 ID 派生。
- [x] `M13-03` 实现 `failure_retrieval_query` 纯函数：kinds 恒为 `{Episode,
  RecoveryLesson}`、exact_terms 为签名标识（step_id 缺席不进入）、无 embedding、
  同签名同查询、limits 负向。

### 4.2 Runtime 学习闭环

- [x] `M13-04` 实现学习上下文与结算期 Episode 记录：`set_learning_context`
  （null/非法 scope/非法配置 fail closed，User scope 拒绝；未安装 NoOp 回归）；非
  DryRun 三终态各记录一条；DryRun 跳过零记录；memory 写失败不影响终态与 Task 结算，
  递增诊断计数器并发射 `WorkflowEpisodeRecorded`（failed）；mutation-id 幂等。
- [x] `M13-05` 实现失败检索进入续跑上下文：失败驱动升级触发同步查询，
  `agent_continuation().relevant_lessons` 携带有界结果（kind、statement、confidence）；
  检查点让渡不检索；查询失败降级为空结果且升级照常；结果条数/token/deadline 受配置
  约束；锁外查询、锁内写入。
- [x] `M13-06` 实现 `record_recovery_lesson`：准入（Completed、非 DryRun、失败升级
  计数 > 0、学习上下文已安装）全负向；恢复 patch 与 `resumed_without_patch` 两形态
  派生（无参数值）；同 run 幂等；写失败对调用方可见；不注册为模型工具（`W-04`）；
  `WorkflowLessonRecorded` 审计。

### 4.3 事件与取证

- [x] `M13-07` 实现两员事件（构建/解析/闭集扩展）与离线回放无副作用；学习投影重建
  配方入文档。
- [x] `M13-08` 端到端取证：失败 → Episode → 恢复完成 → Lesson → 再失败检索同时命中
  两者的闭环用例；installed-consumer 用例（域映射、契约转换、检索查询直接使用 +
  Runtime 学习上下文安装与 lesson 记录）。
- [x] `M13-09` 测试矩阵取证与文档同步：本机 Release/Debug/ASAN/UBSAN（TSAN 按环境
  限制记录补跑条件）、quality 门禁、installed-consumer；总计划、设计文档（v0.6）、
  API 手册与 DEC 注记同步后关闭里程碑。

## 5. Executor 路由与关闭表（细化设计 §7/§14）

| 工作 | Executor 能力 | 句柄所有者 | 结算要求 |
| --- | --- | --- | --- |
| Episode 记录（结算期 `IMemory.apply`） | 结算线程同步门面（后端自路由 store worker，M4 §17） | 无新任务 | 失败转诊断计数器与审计事件，不影响终态 |
| 失败检索（升级期 `IMemory.query`） | 驱动线程同步门面、有界 deadline | 无新任务 | 失败/超时降级为空结果 + 诊断计数器 |
| Lesson 记录（`record_recovery_lesson`） | 调用线程同步门面 | 无新任务 | 失败对调用方可见（Result 错误）+ 审计事件 |

理由：阶段 F 不引入新任务类别、timer 或私有并发；学习 I/O 经 IMemory 同步门面由宿主
后端的 store worker 承载（M4 §16/§17 既有路由），WorkflowRuntime 不拥有存储 worker。
关闭顺序不变（M9 §5）：`WorkflowRuntime::shutdown()` -> `MiraRuntime` 关闭 -> 宿主非
worker 线程 `executor.shutdown(true)`；学习路径无待排空队列。

## 6. 风险与阻塞

- `RISK-2026-049`：结算热路径上的同步记忆写入受后端延迟影响（IMemory 门面同步）。
  Owner：Mira Maintainers。缓解：M4 后端有界请求通道 + 检索 deadline + 写失败不阻塞
  终态；API 手册披露。
- `RISK-2026-050`：失败检索按结构化签名召回，同 Workflow 不同根因可能一起命中（排序
  由 M4 规则承担）。Owner：Mira Maintainers。缓解：DEC-029 披露为保守取舍；调优等
  真实失败语料（`RULE-10`）。
- `RISK-2026-051`：`relevant_lessons` 是增量字段，旧消费者需确认无紧序列化假设。
  Owner：Mira Maintainers。缓解：installed-consumer 回归 + API 手册兼容性注记。
- `RISK-2026-038`（沿袭）：学习记录持久化载体是宿主 IMemory 后端；事件重建配方为
  恢复兜底。

## 7. 测试与退出条件

- [x] `M13-01` 至 `M13-08` 全部完成并有可复现验证记录。
- [x] 域矩阵：`memory_domain_of` 全部 `MemoryKind` 逐值断言；逆映射覆盖全域且元素
  完整；name/parse 正反向与未知名字负向。
- [x] 契约矩阵：Episode/Lesson/签名 JSON 往返无损；未知字段、未知版本、未知枚举、
  空 ID、负计数、超限字段、非法字符 reason_code fail closed；digest 确定性（同内容
  两次相等、一次改动即变）。
- [x] 转换矩阵：`to_memory_record` 产物 kind/verification/confidence/provenance/
  validity/scope 逐字段断言且通过 `MemoryRecord::validate` 与 `MemoryMutation`
  （Add 形态）校验；lesson canonical 往返相等、非 canonical statement fail closed；
  ID 派生确定性（同种子同 ID）。
- [x] 查询矩阵：同签名同查询逐字段断言；kinds 恒为 `{Episode, RecoveryLesson}`；
  step_id 缺席不进 exact_terms；零/越界 limits 负向。
- [x] 结算矩阵：非 DryRun 三终态（Completed/Failed/Cancelled）各记录一条 Episode
  （字段逐值）；DryRun 完成零记录；未装学习上下文零记录（M12 行为回归）；memory 写
  失败不影响终态与 Task 结算、计数器与 failed 事件到位；同 Run 重复结算幂等。
- [x] 升级矩阵：失败驱动升级触发检索且 `relevant_lessons` 含既有 Episode/Lesson
  （含截断上限）；检查点让渡（AgentAssisted）不检索；后端查询失败降级为空且升级完成
  （诊断计数器递增）；无匹配结果为空列表。
- [x] Lesson 矩阵：准入全负向（非 Completed、DryRun、零升级、未装上下文）；恢复
  patch 形态（patch_id/digest/targets、无参数值）与 `resumed_without_patch` 形态派生
  正确；同 run 幂等重放不报错不重复；写失败对调用方可见且审计事件到位。
- [x] 事件矩阵：两员载荷往返、未知字段 fail closed、闭集扩展回归（既有事件类型解析
  不受影响）；离线回放无 IMemory 调用与无副作用。
- [x] 端到端：闭环用例（失败 → 检索空 → Episode → 恢复完成 → Lesson → 再失败检索
  同时命中两者）断言 `relevant_lessons` 内容与顺序边界。
- [x] 门禁：ASAN/UBSAN 全绿；TSAN 在本机限制下按 M9–M12 模式取证；quality
  （clang-format、docs、sbom、platform-boundary）通过；Windows/Android 构建组合与
  clang-tidy 由 PR CI 补验全绿；installed-consumer 覆盖新公共面。
- [x] 总计划第 4/5 节、`workflow_runtime_design` v0.6、API 手册与本文件同步。

## 8. 验证记录

2026-09-09：依据 M12 退出条件与总计划阶段 F 规则创建本里程碑（`Proposed`）：先完成
专项决策（DEC-029、DEC-030）与设计更新（`workflow_runtime_design` v0.6 §14），再创建
本文件。同日经维护者评审（用户指示依设计与计划推进下一步开发，与 M8–M12 同一授权
模式）转 `Planned` 并进入实施（`In Progress`）。

2026-09-09：实现与本地验证（Ubuntu 24.04，x86_64，g++ 13.3.0，CMake 3.28.3，Unix
Makefiles；本机无 clang/clang-tidy，由 PR CI quality job 补验；`clang-format` 使用
miniconda 发行版但本机 format 结论以 PR CI 为准）。

- 实现：`workflow_learning.hpp/cpp`（`MemoryDomain` 四类组织与 name/parse、
  `WorkflowEpisodeRecord`/`WorkflowRecoveryLesson`/`WorkflowFailureSignature` 的 JSON
  往返与 fail-closed 校验、内容寻址 digest、SHA-256 确定性 ID 派生、
  `episode_to_memory_record`/`recovery_lesson_to_memory_record` 纯转换与 canonical
  statement 解析、`failure_retrieval_query` 确定性查询构建）；`workflow_events` 新增
  学习两员（构建/解析/闭集扩展）；`WorkflowRuntime` 新增
  `set_learning_context`（scope 拒绝 User、limits fail closed）与
  `record_recovery_lesson`（准入、patch/`resumed_without_patch` 两形态派生、幂等、
  审计），`settle_terminal` 尾部结算期 Episode 记录（DryRun 与无事件锚点按设计跳过、
  写失败转诊断计数器与 failed 事件、不影响终态），`escalate_waiting_agent` 失败驱动
  检索（锁外查询、锁内有界写入、降级空结果），`agent_continuation` 增
  `relevant_lessons` 增量字段，`handle_step_failure` 维护消毒失败签名与升级 patch
  水位。
- 实现中的语义修正（相对决策草案，随本轮冻结）：失败签名的 `reason_code` 取
  `Error.domain + ":" + domain_code` 的稳定标识符（charset 内构造保证，异常文本不进
  检索面）；Episode 记录的 provenance 锚点为 `WorkflowRunSettled` 事件 ID，未接事件
  存储的 Runtime 无可锚定记录、按设计跳过（与 M12 事件语义回归无冲突：测试 fixture
  与 installed-consumer 均接了事件存储）。
- 新增测试：`mira_m13_contract_test`（域映射逐值与逆映射、name/parse 负向、
  Episode/Lesson/签名往返与 fail closed 全负向、digest 与 ID 确定性、转换逐字段且过
  `MemoryRecord::validate` 与 mutation 校验、canonical-only 解析、查询确定性与
  limits 负向）、`mira_m13_learning_test`（上下文准入负向、三终态 Episode 记录逐值、
  DryRun/未装上下文零记录、写失败不影响结算且 failed 事件到位、升级检索进入
  `relevant_lessons`、查询失败降级、检查点让渡不检索、lesson 准入负向与幂等、
  patch 形态派生、端到端闭环（Run A 失败 Episode → Run B 恢复 Lesson → Run C 检索
  同时命中两者））、`mira_m13_events_test`（两员往返与 fail closed、闭集扩展、离线
  回放无副作用）。
- 安装包：`mira_installed_consumer` 增加 M13 用例（域映射与契约直接使用、学习上下文
  安装、失败 → Episode → 恢复完成 → Lesson → lesson 查询往返；执行器线程数按
  「驱动 + 监控 + 步派发 + 余量」提升到 4）。
- 结果：Release/Debug/ASAN/UBSAN 各 66/66 通过；TSAN（`setarch x86_64 -R`）65/65
  通过；`docs-check`、`sbom-check`、`platform-boundary-check`、`format-check`
  通过。
- 限制：Windows/Android 构建组合与 clang-tidy、format-check 的 CI 权威结论由 PR
  补验后随 `M13-09` 回填（见后续记录）；本机 TSAN 少 1 项为 installed-consumer
  组合限制（沿袭 M9–M12 模式）。
- 同步：DEC-029/030（本轮冻结）、`workflow_runtime_design`（v0.6 §14 与路由/事件/
  模块/测试表）、API 手册（workflow-contracts 增 M13 节与兼容性更新）、总计划
  （§4/§5）、本文件。

2026-09-09：PR [#34](https://github.com/Linductor-alkaid/mira/pull/34) CI 全绿（head
`bff2fe3`，push pipeline run
[`34310027223`](https://github.com/Linductor-alkaid/mira/actions/runs/34310027223)、
pull_request pipeline run
[`34310029920`](https://github.com/Linductor-alkaid/mira/actions/runs/34310029920)）：
Linux GCC/Clang（Debug/Release）、Windows MSVC（Debug/Release）、Android arm64-v8a 与
x86_64（NDK）、ASAN/UBSAN/TSAN 与 quality（clang-tidy 18 + clang-format + docs/sbom/
platform-boundary 检查）全部 24 项通过，补齐本机缺失的 clang-tidy、format 权威结论与
跨平台验证。两轮修复：Windows Debug 首轮报 MSVC `std::accumulate` 缺 `<numeric>`
（GCC 传递包含掩盖，commit `ef03e5f`）；quality 首轮报 clang-tidy
`clang-analyzer-optin.performance.Padding`——`RunRecord` 新增学习字段打乱对齐分组
（32 字节填充），按建议重排字段（commit `bff2fe3`）；修复后复验全绿，语义不变
（本机 m8–m13 套件复跑通过）。PR 已合并（merge `db2e814`）。`M13-01` 至 `M13-09`
全部完成，退出条件逐项满足，本里程碑关闭（`Completed`）。DEC-014 阶段 A–F 全部交付；
后续方向（M7 重定义与真实平台验证、阶段 F 非目标中的 Procedure 索引/失败检索向量腿/
Agent 采纳 lesson 编排）按证据另行立项。
