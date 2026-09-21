# DEC-043：机器可检查的架构策略、基线与契约交付标准

> 状态：Accepted
> 日期：2026-09-21
> 决策人：Mira Maintainers
> 需求来源：维护者指令（2026-09-21，依据对 zai-org/ZCode 架构治理体系的
> 参考研究落地）
> 上位决策：无（本决策落实根 `AGENTS.md` 既有强制约束，不改变架构方向）
> 关联决策：[DEC-001](DEC-001-runtime-executor-ownership.md)（Executor 所有权）、
> [DEC-002](DEC-002-public-contract-versioning.md)（公共契约版本化）、
> [DEC-009](DEC-009-tool-module-boundary.md)（模组边界）
> 关联文档：[架构治理](../project/architecture_governance.md)、
> [阶段冻结与决策协议](../project/stage_freeze_protocol.md)、
> [公共术语表](../project/glossary.md)、
> [`tools/architecture-policy.json`](../../tools/architecture-policy.json)

## 背景与问题

根 `AGENTS.md` 已规定"Core 依赖方向指向抽象、Platform Adapter 不得反向依赖、
平台类型不得渗入 Core"等强制约束，但这些约束只能靠评审人工把关：

1. 跨模块 include 方向写反（如 Core 引用 Adapter 头）无法在提交前被发现；
2. 第三方头（executor、sqlite、mbedtls、openssl、平台头）的消费范围没有登记
   面事实，新增依赖是否越界无客观判据；
3. 文件规模无预算，超大文件持续增长没有减速机制；
4. 公共契约交付物（契约头、示例、不变量文档、测试）依赖记忆，新人或代理协作
   时容易缺件。

平台 SDK 头已有 `tools/check_platform_boundary.py` 单项检查，但它只覆盖平台
符号，不覆盖模块间依赖方向与规模预算。

## 决策

引入机器可检查的架构治理，规则单一来源为
[`tools/architecture-policy.json`](../../tools/architecture-policy.json)，由
`tools/check_architecture.py` 在提交前与 CI（`quality` job 的
`architecture-check` CMake 目标）强制执行：

1. **模块依赖方向**：8 个受治理模块（core、workflow、state_store、三个
   adapter、examples、tests）各自声明 `requires`；文件按最长前缀归属唯一
   模块；违规规则为 `module-dependency`、`deep-import`、`unregistered-include`、
   `cycle`。
2. **规模预算**：`max_file_lines = 1200`；存量 17 个超标文件记入
   [`tools/architecture-baseline.json`](../../tools/architecture-baseline.json)，
   新违规一律阻塞。基线只允许评审后人工 `--update-baseline` 刷新，CI 永不
   自动刷新。
3. **例外机制**：policy `exceptions` 支持带理由与 `expires` 的限期例外，
   过期例外使检查失败。
4. **文档与流程**：新增
   [架构治理](../project/architecture_governance.md)（规则目录、基线流程、
   与决策记录的接口）、[公共术语表](../project/glossary.md)（词条带 `_Avoid:_`
   反义词，统一文档/代码/事件命名）、[阶段冻结与决策协议](../project/stage_freeze_protocol.md)
   （M7 TR2 起的阶段立项冻结必答八问、决策记录模板与 golden 参照）。
5. **契约四件套标准**：`include/mira` 新增或重大扩展公共契约时，契约头、
   用法示例、不变量文档（`docs/api/`）、契约测试（`tests/contract/`）四件
   必须齐备方可宣告完成。

AGENTS.md 与各文档只引用 policy，不复制具体规则取值，避免多处漂移。

## 备选方案

1. **维持纯文字约定**：不引入机器检查。否决——约束违反只能靠评审发现，
   与"完成定义"中"相关测试通过"的可验证性要求不符。
2. **在 CMake 目标层硬编码依赖方向**（`target_link_libraries` 即边界）：部分
   采用——CMake 已经表达链接边界，但 CMake 无法拒绝 include 方向（头文件
   依赖），两者互补；policy 检查源码层，CMake 保持现状。
3. **要求一次性清零存量违规**：否决——17 个存量超标文件与既有依赖需大规模
   重构，风险与收益不成比例；采用基线渐进治理，新违规阻塞即可阻止恶化。
4. **预算取 ZCode 的 400 行**：否决——Mira 存量分布（>600 行 28 个文件）下
   会产生大面积基线噪音；取 1200 行使基线保持在可管理规模，收紧路径在
   治理文档中声明。

## 影响与风险

- 所有后续变更的 CI 增加 `architecture-check` 门禁；触碰跨模块边界的工作
  需要先登记 policy（成本为一次 JSON 编辑）。
- policy JSON 无注释能力，规则语义解释由治理文档承担；字段级 `_doc` 说明
  指向文档。
- 静态 include 扫描不解析预处理分支与生成代码；无法解析的引号 include 被
  忽略（已知限制，记录在治理文档）。
- 术语表与冻结协议是协作约定，违反时不被机器检查；依赖评审执行。

## 验证方式

- `python3 tools/check_architecture.py` 在当前工作树通过（17 个存量违规进
  基线，退出码 0）；篡改测试（构造反向依赖、环、超预算文件）必须失败并由
  独立验证代理取证。
- `cmake --build build --target architecture-check` 可用并纳入 CI `quality`
  job。
- `platform-boundary-check` 与新检查并存，规则不重复。

## 关联文档和工作项

- 维护计划：[architecture-governance（2026-09 第五轮）](../plans/maintenance-2026-09-architecture-governance.md)
  （`MNT-202609-35`–`MNT-202609-38`）。
- 后续阶段冻结（M7 TR2 起）遵循
  [阶段冻结与决策协议](../project/stage_freeze_protocol.md)。
