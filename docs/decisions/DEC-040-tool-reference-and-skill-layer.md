# DEC-040：Tool 稳定引用、兼容状态与 Skill 层级——能力身份的连续抽象

> 状态：Accepted（方向冻结；实现未开始）
> 日期：2026-09-15
> 决策人：Mira Maintainers
> 需求来源：[Issue #56](https://github.com/Linductor-alkaid/mira/issues/56)
> （Tool Registry 与稳定引用、与 Skill 的关系）、
> [Issue #55](https://github.com/Linductor-alkaid/mira/issues/55)
> （Tool / Skill 层级缺口）
> 上位决策：[DEC-014](DEC-014-agent-harness-workflow-dual-plane.md)
> 关联决策：[DEC-009](DEC-009-tool-module-boundary.md)、
> [DEC-015](DEC-015-builtin-tool-execution-boundary.md)、
> [DEC-019](DEC-019-workflow-ir-contract.md)、
> [DEC-021](DEC-021-workflow-tool-channel.md)、
> [DEC-025](DEC-025-success-trajectory-compilation-and-publish-gate.md)、
> [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
> [DEC-038](DEC-038-unified-behavior-trace.md)、
> [DEC-039](DEC-039-mcp-tool-module-admission.md)

## 背景与问题

DEC-019 的 `ToolCall` 步骤以实参树内保留成员 `"tool"`（wire 名字符串）引用被调用
工具；DEC-015 提供了执行期身份一致性校验（执行前 wire 名/ToolId/版本/副作用声明
与暴露快照一致，否则拒绝）。这条链路对「当下执行」是闭合的，但对「长期保存」有两
个缺口：

1. **稳定引用缺失**：Workflow 可能保存数月，而工具实现会演进（schema 变更、版本
   退役、工具消失、MCP server 下线）。裸 wire 名引用无法表达「钉住哪个版本」或
   「跟随哪个演进线」，Registry 也无法在准入期回答「这个 Workflow 现在还能不能跑」，
   只能在执行期失败。
2. **Skill 层级无契约**：架构设计已定位 Skill（参数化、可复用的动作组合，注册与
   暴露沿用 Tool 通道），DEC-019 写明 `ToolCall` 可调用 Primitive Tool 或 Skill，
   DEC-029 推迟了 Workflow/Skill 资产索引为 Procedure 记录；但 Skill 的身份、发布、
   版本绑定与兼容语义没有任何契约冻结。Issue #55 与 #56 都把 Capability → Tool →
   Workflow → Skill 的连续抽象层列为缺口。

## 决策

1. **能力层级收敛为连续抽象，不建平行体系**：

   ```text
   Capability（环境事实，DEC-009 能力协商）
     → Tool（原子能力，统一 descriptor，BuiltIn/Host/MCP 同一引用空间）
       → Workflow（结构化过程，DEC-019 IR，版本化资产）
         → Skill（已验证、参数化、可独立调用的复用单元）
   ```

   四层共享同一套版本化、门禁与事件语义；任何一层都不新建第二套注册、执行或
   存储体系（W-03）。
2. **Skill 是「暴露为 Tool 的 Workflow」**：一个已通过 `publish_validated` 门禁
   （DEC-025）的 Workflow，可经宿主**显式发布动作**在 Tool 通道获得 wire 身份，
   成为 `ToolCall` 步骤与 Agent 动态调用的对象。Skill 调用在执行语义上就是子
   Workflow 调用经 Tool 通道（DEC-019 备选方案预留的引用方式），不引入新的执行
   通道或权限语义（W-06 不变）；其内部步骤仍走各自既有的校验与确认路径。编译
   产物不自动暴露为 Skill——自动暴露会绕过「宿主知道系统对外承诺了什么能力」
   这条产品边界。
3. **稳定逻辑引用**：Workflow 保存稳定能力身份，运行时由 Registry 解析到当前
   实现。最低要求（具体 URI 形态与版本规则在立项里程碑冻结）：
   - 引用不内联运行时对象或单次会话句柄；
   - 支持**版本钉住**（digest 或版本约束）与**跟随最新可用版本**两种模式，模式
     在引用中显式声明；钉住内容以内容寻址 digest 关联，不复制大内容（RULE-07）；
   - BuiltIn/HostProvided/MCP 来源处于同一引用命名空间，跨源冲突在注册期
     fail closed（DEC-009 既有规则）。
4. **兼容状态是投影**：Registry 演进必须可被 Workflow 资产检测。为 Workflow
   版本引入兼容状态维度——`Runnable` / `Degraded`（引用工具的 schema 演进但参数
   仍可绑定）/ `Invalid`（引用无法解析或 schema 不兼容）。状态是**从版本记录 +
   当前 Registry 快照确定性重算的投影**（RULE-07），不是新的存储事实；重算不读
   时钟。`Invalid` Workflow 在 Run 准入时 fail closed 拒绝启动；`Degraded` 允许
   启动但事件留痕。执行期身份一致性校验（DEC-015）保持不变——兼容状态是准入期
   前置检查，不是执行期校验的替代。
5. **Skill 版本绑定是显式的**：Skill descriptor 携带源 Workflow id 与
   `ir_digest`（钉住版本）；源 Workflow 产生新版本不自动改变已发布 Skill 的绑定，
   升级是显式动作并留下版本记录（对齐 DEC-019 版本化与 DEC-025 幂等语义）。
   Skill 的撤销/降级经 Tool 通道既有生命周期承载。
6. **Authority 不变**：Skill 与普通 Tool 经过完全相同的门禁——exposure 时
   `SafetyPolicy`、执行时 DEC-015 校验、逐副作用 DEC-004 判定、Workflow 路径
   DEC-021 §4 挂钩点。本决策不新增任何豁免；「Workflow 已被验证过」不等于其内部
   工具获得持久授权。
7. **Memory 衔接（承接 DEC-029 推迟项）**：Workflow/Skill 版本资产索引为
   `Procedure` 记录的方向由本决策承接；索引是投影（RULE-07），实现随首阶段
   里程碑冻结，仍遵守 M4 的 scope/ACL 与审批规则。

## 非目标

- 不改变 DEC-019 IR v1 的 schema：引用与兼容状态在 IR 中的表达是**加法演进**
  （新 schema 版本，DEC-002），随首个消费者里程碑冻结；v1 文档继续按既有
  fail-closed 规则解码。
- 不做自动 Skill 发现、推荐或自动发布；不做跨进程 Workflow 库（RISK-2026-038
  既有边界）。
- 不冻结引用 URI 的具体语法与版本约束语法（立项时冻结后即成公开兼容承诺）。
- 不改变 DEC-031 恢复编排与 DEC-030 学习闭环的既有语义。

## 备选方案

- **维持裸 wire 名 + 执行期校验**：长期保存的 Workflow 在工具演进后只能执行期
  失败，无显式 `Degraded`/`Invalid` 语义，Issue #56 的缺口不闭合。不采用。
- **Skill 作为独立于 Workflow 的第二类资产**（独立 IR、独立门禁、独立存储）：
  两套版本化与验证体系，违背连续抽象层目标，且 Skill 与源 Workflow 的同步问题
  无解。不采用；Skill 复用 Workflow 的版本化本体（DEC-029 §1 已把 Workflow 库
  定为 Procedural 域的版本化本体）。
- **Workflow 内联工具 schema 快照作为引用**：无法表达「跟随最新」模式，且把工具
  演进的检测责任转嫁给每份 Workflow 文档；digest 钉住 + Registry 解析已覆盖同一
  需求。不采用。
- **兼容状态写入库版本记录（成为存储事实）**：状态随 Registry 演进随时变化，
  落库会造成大量只反映「当时快照」的记录；投影 + 事件留痕已覆盖审计诉求。
  不采用。
- **编译产物自动暴露为 Skill**：绕过宿主对系统能力面的控制，与「模型不可直达
  发布」（DEC-025 §3）同一立场的反面。不采用。

## 影响与风险

- 引用 scheme 冻结后即公开兼容承诺（DEC-002 major.minor 纪律），冻结前必须以
  真实工具演进样例验证表达力。
- 兼容状态全量重算是 Workflow 数 × 引用数的规模（受 RULE-08 上限约束）；增量
  重算策略（Registry 变更事件驱动的脏标记）随立项冻结，避免大库下的重算风暴。
- 「Skill = Workflow as Tool」可能被误读为任意 Workflow 自动可调用：显式发布
  动作 + descriptor 钉住 + 宿主门禁是防线路，API 命名与文档必须显式。
- MCP 来源（DEC-039）的引用稳定性弱于 BuiltIn（server 可下线）：兼容状态机对
  该场景的价值最大，但也意味着 `Invalid` 判定会更频繁；宿主需要在部署层管理
  server 可用性，Runtime 只负责显式暴露状态。
- 与 DEC-038 的衔接：Trace L1 的工具调用引用应使用同一引用词表，避免轨迹层与
  资产层各自表述。

## 验证方式

- 方向以本决策为准；立项里程碑内冻结：引用解析矩阵（钉住/跟随 × 工具存在/消失/
  schema 演进/跨源冲突）、兼容状态投影的确定性重算（同输入同状态）、
  `Invalid` 准入拒绝与 `Degraded` 事件留痕、Skill 发布/升级/撤销生命周期与
  `ir_digest` 钉住、与 DEC-015 执行期校验的组合负向、Procedure 索引投影的可
  重建测试。

## 关联文档和工作项

- [Issue #55](https://github.com/Linductor-alkaid/mira/issues/55)、
  [Issue #56](https://github.com/Linductor-alkaid/mira/issues/56)
- [DEC-009](DEC-009-tool-module-boundary.md)、
  [DEC-015](DEC-015-builtin-tool-execution-boundary.md)、
  [DEC-019](DEC-019-workflow-ir-contract.md)、
  [DEC-021](DEC-021-workflow-tool-channel.md)、
  [DEC-025](DEC-025-success-trajectory-compilation-and-publish-gate.md)、
  [DEC-029](DEC-029-memory-domains-and-learning-contracts.md)、
  [DEC-038](DEC-038-unified-behavior-trace.md)、
  [DEC-039](DEC-039-mcp-tool-module-admission.md)
- [Agent Harness 与 Workflow 架构设计](../design/agent_harness_and_workflow_architecture.md)
  （Skill 定位、W-03/W-04/W-06）
- [工具模组设计](../design/tool_module_design.md)
- 专项设计与里程碑文件随首阶段立项交付（引用语法冻结是该阶段的退出条件之一）。
