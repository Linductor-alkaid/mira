# 维护计划：记忆抽取的模型供给面（2026-11）

> 状态：Completed
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M36 后维护轮）
> 前置：[MNT-202611-HRW 混合检索默认接线](maintenance-2026-11-hybrid-default-wiring.md)
>（同日交付，装配面先例）
> 建议发布点：Host contract alpha 修订（新公开契约加法，无默认行为翻转，
> 见 DEC-114 影响与风险）
> 更新日期：2026-11-18

## 1. 目标

补齐 `IConsolidationModel` 的零实现缺口：交付 `ProviderConsolidationModel`
适配器（事件 → 候选，镜像 ProviderSemanticConsolidator 模板）与一呼式供给
装配 `wire_extraction_supply`（consolidate 直驱 + MemoryAgentService
processor 两面一个装配）。无默认行为翻转：无模型路径原样保留且仍是缺省。

## 2. 范围与非目标

### 2.1 范围

- 新增 `include/mira/extraction_model_provider.hpp` /
  `src/context/extraction_model_provider.cpp`：`ExtractionModelOptions`、
  `ProviderConsolidationModel`、`ExtractionSupplyConfig`、
  `ExtractionSupplyStack`、`wire_extraction_supply`。
- 测试：新 `tests/m4/m4_extraction_supply_test.cpp`（六门）。
- 新增 `examples/extraction_supply_host_consumer.cpp`（离线 scripted 全链）。
- `CMakeLists.txt`（mira_core 源列表、示例目标）、`tests/CMakeLists.txt`、
  `tests/headers/CMakeLists.txt`。
- 文档：DEC-114（新增）、设计 §13.3 注记、API、release note 表述更新、
  术语表、README、总计划注记、本计划。

### 2.2 非目标（DEC-114 明示）

- 默认行为翻转；Compress 需求语义变更；检索面改动；JSON/TOML 配置解析面；
  真实供应商冒烟（另行登记）；经 ModelGateway 路由抽取调用。

## 3. 设计与决策依据

- 决策记录：[DEC-114](../decisions/DEC-114-extraction-model-supply.md)。
- 适配器模板：`ProviderSemanticConsolidator`
  （`src/context/context_consolidation.cpp:574-741`）——两条 ModelInputItem、
  `attach_output_contract`、单次 infer、fail-closed 解析、引用越界丢弃。
- 共享解析基建：`src/context/context_support.hpp`（consolidator/curator
  单源，本适配器加入同源）。
- 接线器形状：[DEC-113](../decisions/DEC-113-hybrid-retrieval-default-wiring.md)
  `hybrid_retrieval_wiring.hpp`（config validate 前置 + stack）。
- 降级先例：recall embedder（DEC-111）、checkpoint 摘要失败落确定性
  checkpoint。

## 4. 工作项（跑前冻结）

- [x] `MNT-202611-ME-01` 立项与跑前冻结：DEC-114 落成、本计划冻结。
- [x] `MNT-202611-ME-02` 适配器：`ProviderConsolidationModel`（转录/schema/
      fail-closed 解析/引用绑定/degrade-or-throw 双态）。
- [x] `MNT-202611-ME-03` 接线面：`ExtractionSupplyConfig`/`Stack`/
      `wire_extraction_supply`（profile 装配 + `agent_processor` 工厂）；
      CMake/headers 登记。
- [x] `MNT-202611-ME-04` 契约测试：六门（装配断言/转录与请求形状/happy
      path 端到端/fail-closed/degrade-throw 双态/processor 工厂冒烟）。
- [x] `MNT-202611-ME-05` 离线示例：wire → 种子事件 → consolidate →
      确定性 Applied + 模型 Preference PendingApproval → 降级轮。
- [x] `MNT-202611-ME-06` 文档同步：设计 §13.3、`docs/api/context-memory.md`、
      release note 表述、术语表词条、README、总计划注记。
- [x] `MNT-202611-ME-07` 全量验证：构建 + `ctest --preset debug` 全过 +
      门禁四目标 + static-analysis 零告警 + clang-format；证据回填。

### 跑前冻结八问

1. **行为**：DEC-114 决策 1–3；无模型路径零变化（缺省仍确定性）；设计
   §13.3 增供给注记。
2. **所有者**：宿主拥有 config/profile 数据、transport、凭据解析与
   Executor；Core 拥有适配器与装配面。
3. **契约面**：`extraction_model_provider.hpp`（新头）。既有契约零变更。
4. **层与依赖**：core 新文件仅 include std/`<mira/...>`/内部
   `context_support.hpp`；无新外部头族；policy 不变。
5. **复用**：context_support 单源解析基建、ProviderSemanticConsolidator
   模板、make_model_memory_agent、DEC-113 接线器形状、m19/m36 测试固定
   装置；不新增平行 HTTP、解析或评分路径。
6. **时序幂等**：`wire_extraction_supply` 校验前置（无不可逆注入点——
   consolidator/processor 均为构造参数）；`propose` 无隐藏状态；降级路径
   确定性（空候选集）。
7. **Executor 路由**：适配器为同步 facade；`propose` 由调用方在
   Executor 管理的 worker 上驱动（Record job 既有 blocking worker 路径）；
   `agent_processor` 经 `make_model_memory_agent` 既有 gateway 路由。
8. **验证上下文**：离线契约测试 + 全量 ctest + 四门禁 + tidy；CI 覆盖
   平台矩阵（合并前 PR）。

## 5. 风险与阻塞

- 模型文本进写入管道：由既有管道侧信任门覆盖（model_assisted 强制、
  HumanConfirmed 剥离、注入标记、审批闸门——m4_consolidation_test 钉死），
  本轮不新增绕过路径；适配器只做形状与引用绑定。
- `degrade_on_error=true` 的静默降级可能掩盖配置错误：事件与报告可见性靠
  调用方（Record job 已有失败/降级事件面）；fail-closed 宿主可设 false。
- 转录体积：`max_transcript_events` 确定性截断为上界，超界事件不进模型
  转录（确定性抽取不受影响）。
- 无 Executor、平台阻塞；本轮无真实供应商依赖。

## 6. 测试与退出条件

- [x] m4 契约测试六门全过。
- [x] 全量 `ctest --preset debug` 通过（192+ 项）。
- [x] `architecture-check`、`platform-boundary-check`、`docs-check`、
      `format-check`、static-analysis 改动文件零告警。
- [x] `extraction_supply_host_consumer` 正常退出（0），含降级轮。
- [x] 文档同步完成（设计/API/release note/术语表/README/总计划）。

## 7. 验证记录

### 2026-11-18：立项与跑前冻结

- DEC-114 落成（接口冻结缺口的供给面，无默认翻转）；本计划冻结。
- 未执行项：ME-02…07。

### 2026-11-18：实现、测试与文档同步（ME-02…07）

- **范围**：新公开契约 `extraction_model_provider.hpp` /
  `src/context/extraction_model_provider.cpp`（`ExtractionModelOptions`、
  `ProviderConsolidationModel : IConsolidationModel`、`ExtractionSupplyConfig`、
  `ExtractionSupplyStack`、`wire_extraction_supply` + `agent_processor` 工厂）；
  `tests/m4/m4_extraction_supply_test.cpp` 六门契约测试；
  `examples/extraction_supply_host_consumer.cpp` 离线全链 consumer；文档七处同步
  （设计 §13.3 注记、API 新节、release note 表述更新、术语表「抽取供给」词条、
  README、总计划）。
- **依据**：[DEC-114](../decisions/DEC-114-extraction-model-supply.md)；适配器模板
  ProviderSemanticConsolidator（m19）；共享解析基建 `src/context/context_support.hpp`
  （加入 consolidator/curator 单源）；接线器形状 DEC-113。
- **环境**：Linux 7.0.0-43-generic x86_64；gcc 13.3.0；clang-tidy 18（miniconda，
  static-analysis preset）；基线 = master `69bf4eb`（PR #129 回填后）。
- **命令与结果**：

  | 命令 | 结果 |
  | --- | --- |
  | `cmake --build --preset debug -j 14` | 通过，无告警 |
  | `ctest --preset debug -j 14` | **191/191 通过**（189 项既有回归零失败；新增 m4 extraction supply 契约测试 + consumer） |
  | architecture / platform-boundary / docs / format 四门禁 | 全部通过（format 395 文件） |
  | static-analysis（clang-tidy 18） | 改动文件零告警（3 处 `performance-move-const-arg`：`ExtractionModelOptions` 平凡可拷贝与 `runtime_error(const string&)` 形参） |
  | `mira_m4_extraction_supply_test` | 六门全过（装配断言/转录与请求形状/happy path 端到端/fail-closed/degrade-throw 双态/processor 工厂冒烟） |
  | `mira_extraction_supply_host_consumer` | 退出 0：wire → consolidate（确定性 Applied + 模型 procedure Applied + 模型 Preference PendingApproval）→ apply_pending → 降级轮 |

- **实现期发现（后续轮次参考）**：
  1. `to_json_string(JsonValue(text))` 已产出**完整的带引号字符串 token**——拼接
     responses wire body 时前后缀都不要再补引号（双引号会让 provider 报
     "response body is not a json object"）。
  2. strict 输出契约的模型输出必须是 `{"candidates":[...]}` 对象——**裸数组**
     会被 fail-closed 降级为空候选集（示例首轮踩中；契约行为正确，脚本要守约）。
  3. `MemoryDataPolicy.store` 是 `optional<bool>`：`!policy.store` 测的是 engaged
     而非值——断言要写 `has_value() && !*store`。
  4. 模型候选经命名空间导入契约可达 per-kind scope 允许表之外的 kind（默认
     `allow_model_import_exemption=true` 时 `consolidation-model` 命名空间跳过
     kind 闸）：示例中 Environment scope 的模型 Procedure 候选按导入契约 Applied；
     DEC-101 的 agent 路径强制 exemption=false，行为分叉是设计预期。
  5. `IConsolidationModel::propose` 无错误通道（接口冻结）——fail-closed 语义
     只能经异常表达；`degrade_on_error` 双态是本轮补的显式契约。
- **限制与未执行项**：
  - ASAN/UBSAN/TSAN 与 Windows/Android 未本地执行，随 PR CI；按测试分层，
    发版前全量独立验证另行委派。
  - 真实供应商抽取冒烟未执行（离线确定性验证；沿 DEC-111 先例按需登记补跑条件）。
  - `max_transcript_events` 取前 N 的截断策略是文档化的确定性保守选择；按时间
    相关性选事件的智能截断留后续轮。
- **同步**：`docs/design/context_and_memory_design.md`（§13.3 注记）、
  `docs/api/context-memory.md`（extraction_model_provider 节）、
  `docs/releases/stateful-agent-beta.md`（供给面表述更新）、
  `docs/project/glossary.md`（抽取供给词条）、`README.md`（状态增补）、
  `docs/plans/mira-implementation-plan.md`（顶部注记）、
  `docs/decisions/DEC-114-…`（新增）、本计划。

### 2026-11-18：PR CI 验收（回填）

- PR [#130](https://github.com/Linductor-alkaid/mira/pull/130)（与 DEC-115 写入面
  解耦同分支合入，squash master `080f182`）：head `797ea36` 的 push +
  pull_request 双管线 **24/24 job 全绿**（run `44275055881` / `44275116747`，
  含 ASAN/UBSAN/TSAN、Windows Debug/Release 与 Android 双 ABI），**首轮一次
  通过，无修复轮**。
- master 合并提交 `080f182` 的 push 管线 **12/12 job 全绿**（run
  `44280466524`）。
- 本计划 §6 全部退出条件达成，计划状态转 Completed。
