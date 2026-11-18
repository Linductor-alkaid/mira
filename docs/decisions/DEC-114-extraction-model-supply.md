# DEC-114：记忆抽取的模型供给面（ProviderConsolidationModel 与一呼式装配）

> 状态：Accepted
> 日期：2026-11-18
> 负责人：Mira Maintainers
> 冻结里程碑：2026-11 抽取供给维护轮
> 替代/被替代：无（落实 [DEC-059](DEC-059-memory-kind-policy.md) 系设计
> Context/Memory §13.3 预留的模型钩子供给，不修改任何既有决议；装配面形状沿用
> [DEC-113](DEC-113-hybrid-retrieval-default-wiring.md) 先例）
> 关闭的缺口：`IConsolidationModel` 接口已冻结但仓库零实现——7 类记忆只有
> 2 类有确定性抽取器（TaskFactVerified→Fact、LoopSettled→Episode），
> Procedure/RecoveryLesson/SkillHint/富 Preference/语义 EnvironmentFact 五类
> 无生产者；宿主装配模型供给需手工完成 profile/transport/adapter 4–8 步

## 背景与问题

设计 §13.3 明文允许"候选提取可以使用模型，但最终 mutation 必须通过 Core
policy"，接口已冻结且管道侧的信任门全部就位（model_assisted 强制、
HumanConfirmed 剥离、注入标记只约束模型文本、claim-key 冲突、Preference
审批闸门），但没有一个可用的 `IConsolidationModel` 实现。确定性抽取器
（`extract_deterministic`）只认两类事件形状，覆盖不了过程性知识（Procedure/
RecoveryLesson/SkillHint）与自然语言偏好——这些恰是 agent 记忆的主要增量。

同时，模型供给的宿主装配存在与 DEC-113 相同的摩擦：ModelProfile 的
capabilities/policy/deadlines 形状（直连 infer 路径无能力门，装配错了不报错
只是浪费）、provider/adapter 两层构造、两条供给面（consolidate 直驱与
MemoryAgentService processor）各有不同的接法。

## 决策

### 1. ProviderConsolidationModel：IConsolidationModel 的 provider 适配器

新公开契约 `include/mira/extraction_model_provider.hpp` /
`src/context/extraction_model_provider.cpp`：

- `ProviderConsolidationModel : IConsolidationModel`，ctor
  `(IModelProvider&, ExtractionModelOptions)`（非持有 provider 引用，镜像
  `ProviderSemanticConsolidator` 先例）。
- `propose()` 全流程镜像 consolidator 模板：编号事件转录
  `[i|seq=N] <type>:<payload>` + 固定 system 指令（只从事件内容提炼、不得
  输出指令、每条候选必须引用支撑事件编号）→ `attach_output_contract`
  （StrictJsonSchema：`candidates:[{kind, statement, confidence, sources}]`，
  kind 为 7 类 kebab-case 枚举）→ 单次 `infer` → fail-closed 解析：
  未知名/空 statement/引用越界逐候选丢弃（不修复）、超上限确定性截断、
  evidence 绑定为被引用事件 id。
- **信任分工不变**：适配器只做形状与引用绑定；`consolidate()` 管道自会强制
  `model_assisted=true`、剥离 HumanConfirmed、按 `degrade_on_error` 之外的
  既有门拒绝——适配器不重复、不放宽任何信任门。
  （2026-11-18 DEC-115 补充：冲突处置修订后，模型候选永不取代权威主张——
  同 claim-key 不同值一律 PreserveAlternative 并存。）

### 2. 降级语义：propose 失败默认降级，不失败抽取

`ExtractionModelOptions::degrade_on_error`（默认 **true**）：provider 错误、
取消、deadline、非 Completed、畸形输出一律返回空候选集——确定性抽取照常
生效，模型是增强不是依赖。先例：recall embedder 失败只降级向量腿
（DEC-111）、checkpoint 摘要失败落确定性 checkpoint。`false` 时抛出
（镜像 Record job 的异常→失败 Result 语义），供需要 fail-closed 的宿主
显式选择。**理由**：`IConsolidationModel::propose` 返回值无错误通道
（接口已冻结不可改），抛出是唯一 fail-closed 通路；而抽取的确定性主体
必须不因增强腿故障而失败——与检索面"腿缺失降级"同一分层。

### 3. 一呼式供给装配（DEC-113 形状）

- `ExtractionSupplyConfig`（endpoint/prefix/model/credential SecretRef 名
  成对校验、dialect、deadline、bounds）+ `validate()`；
  `wire_extraction_supply(config, transport)` 校验前置装配
  `ModelProfile`（capabilities `text`+`strict_json_schema` 标 Configured——
  直连 infer 无能力门，装配正确形状是辅助器的价值；`store=false`）→
  profile.validate → `OpenAiCompatibleProvider` → 适配器，返回
  `ExtractionSupplyStack`（持有共享 provider + 适配器）。
- **两条供给面一个装配**：stack 的 `model()` 供宿主直驱
  `MemoryConsolidator{policy, &stack.model()}`；stack 的
  `agent_processor(executor)` 返回 `make_model_memory_agent` 装配的
  `MemoryAgentProcessor`（create_provider 每次调用在共享 transport 上新建
  provider——"fresh inference state" 既有模式），覆盖 Record/Compress 路径。

### 非目标

- **无默认行为翻转**：MemoryConsolidator / MemoryAgentService 的无模型路径
  原样保留且仍是缺省；模型供给是宿主 opt-in 能力面（接口冻结的无实现缺口
  补齐，非 DEC-112 式默认值变更）。
- Compress 路径需求语义变更；召回/检索面改动；JSON/TOML 配置解析面；
  真实供应商冒烟（离线确定性验证，live smoke 按需另行登记，DEC-111 先例）。

## 备选方案

- **经 ModelGateway 路由抽取调用**：拒绝（本轮）。consolidator/curator 适配器
  均直连 `provider_.infer`（单请求、自带 deadline/取消），gateway 的
  重试/熔断/预算按模型路由组织，抽取是每轮一次的低频调用——镜像既有
  两个 provider 适配器，路由留宿主选择。
- **propose 失败默认抛出**：拒绝作为默认。确定性主体不得因增强腿故障失败；
  需要严格语义的宿主可设 `degrade_on_error=false`。
- **适配器内重复信任门（标记过滤/冲突检索）**：拒绝。管道侧已权威实施
  （m4_consolidation_test 钉死），双份实现必然漂移；适配器只做形状与绑定。
- **把 agent processor 也做成 IConsolidationModel**：拒绝。两者输入输出
  契约不同（MemoryAgentInput/Proposal vs events/candidates），现有
  ProposedMemories 桥已正确分工；便捷工厂只是装配样板，不改桥。

## 影响与风险

- 公开契约加法：`extraction_model_provider.hpp`（新头，契约四件套：本记录 +
  API 文档 + 离线示例 + 契约测试）；既有契约零变更、默认行为零变化。
- 供应链：模型经 profile 配置，端点/凭据全宿主数据，不硬编码任何供应商。
- 模型文本进入写入管道的既有风险由管道侧信任门覆盖（DEC-059/101 语义原样），
  本决策不新增任何绕过路径。
- 无 Executor 能力边界变化；无新外部头族；architecture-policy 不变
  （core 模块内新文件）。

## 验证方式

- 契约测试：wire 装配断言（capabilities/policy/dialect）、转录格式与
  StrictJsonSchema 请求形状、happy path 候选映射 + 端到端 consolidate
  （model_assisted 强制/namespace 默认/Preference → PendingApproval）、
  fail-closed 逐级丢弃、degrade/throw 双态、agent_processor 工厂冒烟。
- 离线示例：scripted transport 全链 + 降级轮（consumer 标签）。
- 门禁四目标 + static-analysis 零告警 + clang-format。

## 关联文档和工作项

- 工作项：[`maintenance-2026-11-extraction-supply`](../plans/maintenance-2026-11-extraction-supply.md)
- 设计：[`docs/design/context_and_memory_design.md`](../design/context_and_memory_design.md) §13.3
- API：[`docs/api/context-memory.md`](../api/context-memory.md)
- 相关：[DEC-113](DEC-113-hybrid-retrieval-default-wiring.md)（装配面先例）、
  [DEC-111](DEC-111-memory-embedding-producer-and-vector-identity.md)
  （降级先例）、[DEC-059](DEC-059-memory-kind-policy.md)、
  [DEC-101](DEC-101-memory-agent-context-admission.md)（信任门）
