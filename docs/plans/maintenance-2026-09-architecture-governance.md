# 维护计划：架构治理落地（2026-09 第五轮）

> 状态：Completed（2026-09-21 关闭；PR CI 20/20 取证见验证记录）
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（维护轮，依据
> [DEC-043](../decisions/DEC-043-architecture-policy-and-baseline.md)）
> 前置：无（纯治理与文档轮，不改运行时行为）
> 建议发布点：不适用（无发布物）
> 更新日期：2026-09-21（PR #65 CI 取证回填，交付关闭）

## 1. 目标

依据维护者指令（参考 zai-org/ZCode 架构治理体系后的落地顺序），把根
`AGENTS.md` 的文字约束升级为机器可检查的门禁，并补齐术语、阶段冻结协议与
契约交付标准，使后续里程碑（M7 TR2 起）在冻结与实现时有可核对的协作基架。

## 2. 范围与非目标

### 2.1 范围

- 架构策略单一来源 `tools/architecture-policy.json` 与检查器
  `tools/check_architecture.py`，存量违规进 `tools/architecture-baseline.json`，
  接入 CMake `architecture-check` 目标与 CI `quality` job。
- [公共术语表](../project/glossary.md)（CONTEXT 模式，词条带 `_Avoid:_`）。
- [阶段冻结与决策协议](../project/stage_freeze_protocol.md)（必答八问、决策
  记录模板、golden 参照、单一 policy 源引用）。
- [架构治理](../project/architecture_governance.md)（规则目录、基线流程、
  契约四件套标准）。
- 文档同步：AGENTS.md 工程约束引用、M7 计划 TR2 冻结指针、总计划决策索引
  与质量门禁。

### 2.2 非目标

- 任何运行时行为、公共契约或构建产物变化。
- 一次性清零 17 个存量超预算文件（基线渐进治理，见 DEC-043 备选方案）。
- 收紧 `max_file_lines` 预算（保留为后续评审项）。
- Core 内部子目录（src/runtime、src/model 等）的层内方向治理（记为未来
  收紧项）。

## 3. 设计与决策依据

- [DEC-043](../decisions/DEC-043-architecture-policy-and-baseline.md)
- [架构治理](../project/architecture_governance.md)；
  [项目管理与文档规范](../project/project_management_and_documentation.md)

## 4. 工作项

- [x] `MNT-202609-35` 架构策略与检查门禁：policy JSON、检查器、基线生成、
  CMake 目标与 CI 接入（2026-09-21 交付；篡改场景验证由独立验证代理取证，
  见验证记录）。
- [x] `MNT-202609-36` 公共术语表 `docs/project/glossary.md`，词条定义与
  `_Avoid:_` 反义词，链接权威设计文档（2026-09-21 交付）。
- [x] `MNT-202609-37` 阶段冻结与决策协议 `docs/project/stage_freeze_protocol.md`，
  M7 计划 TR2 立项指针接入（2026-09-21 交付）。
- [x] `MNT-202609-38` 架构治理文档 `docs/project/architecture_governance.md`
  含契约四件套标准与 golden 参照（2026-09-21 交付）。

## 5. 风险与阻塞

- 检查器为静态 include 扫描，不解析预处理分支；误报风险由
  `external_modules` 登记与例外机制吸收，误报应在当期修复登记而非绕过。
- CI `quality` job 新增步骤失败会阻塞合并；基线已含全部存量违规，理论新增
  违规只来自本次变更之后的新代码。

## 6. 测试与退出条件

- [x] `python3 tools/check_architecture.py` 在干净工作树退出码 0，报告
  17 个基线抑制项。（2026-09-21 IVA 两轮取证）
- [x] 篡改场景（Core 引 Adapter 头、模块环、超预算新文件、未登记外部头、
  过期例外）各自使检查器以退出码 1 失败并输出对应规则与修复提示。
  （2026-09-21 IVA 于临时副本取证；含 deep-import、基线闭环/stale 不阻塞、
  非法 policy 退出码 2、最长前缀归属、违规键行号无关性共 10 场景）
- [x] `cmake --build build --target architecture-check` 构建可用。
  （2026-09-21 IVA 取证 configure + 目标构建通过）
- [x] 既有质量门禁（platform-boundary-check、docs-check）仍通过。
  （2026-09-21 IVA 取证；docs-check 首轮因治理文档一处逃逸链接失败，修复后
  通过）
- [x] 文档相对链接经 `docs-check` 校验通过。（2026-09-21 IVA 取证，含术语表
  与治理文档全部新增链接）
- [x] PR CI 全绿并回填取证。（2026-09-21：[PR #65](https://github.com/Linductor-alkaid/mira/pull/65)
  于提交 `2934f57` 触发 push 与 pull_request 两个运行共 20/20 检查通过，
  含 `quality` job 的 architecture-check 首次远端执行；运行
  35608096554 / 35608102909）

## 7. 验证记录

2026-09-21：实现完成于工作树（policy 8 模块、检查器、基线 17 项、CMake/
CI 接入、四份文档、DEC-043）。实现过程中的首跑取证：初版检查器暴露文件
归属 bug（重叠根重复扫描）与 policy 调研遗漏（net_transport→executor、
tests→openssl/platform_windows、C 标准头），均已修复并复跑通过。

2026-09-21：Independent-Verification-Agent 两轮取证（自包含篡改场景在
/tmp 临时副本执行，真实工作树零污染，前后 `git status` 一致）。第一轮
A1/A2、B2–B10、C1 部分、D 通过；暴露两个阻塞缺陷——(1) net 公共头映射
失效（`_adapter` 后缀模板对不上模块 id `net_transport`，core→net 反向
依赖不可检测）；(2) 治理文档一处 `../../../AGENTS.md` 逃逸链接击穿
docs-check。主循环修复（映射改为按 policy roots 反查；链接改
`../../AGENTS.md`；治理文档补记"刷新基线前先清过期例外"与"无法解析的
引号 include 被跳过"两条已知限制）后第二轮复验 R1–R4 全部 PASS，含
`core -> net_transport` 违规与 `core -> net_transport -> core` 环的正确
报告、17 条基线无回归、docs-check/CMake 目标通过。另按复验观察补
`.gitignore` 忽略 `__pycache__/`、`*.pyc`。

限制与补跑条件：GitHub Actions `quality` job 的远端端到端运行以本地等价
目标取证代替（本会话不推送）；合并 PR 后由维护者按仓库惯例回填 PR CI
证据并关闭本计划（负责人：Mira Maintainers）。

2026-09-21：CI 证据回填并关闭本计划。[PR #65](https://github.com/Linductor-alkaid/mira/pull/65)
（提交 `2934f57`）在 GitHub Actions 两个运行（push 35608096554 与
pull_request 35608102909）共 20/20 检查通过：linux gcc/clang × Debug/Release
8 项、windows Debug/Release 4 项、android arm64/x86_64 4 项、
ASAN/UBSAN/TSAN 6 项、quality 2 项（format/docs/sbom/platform-boundary/
architecture-check 首次远端执行）。此前"本地等价取证代替远端 CI"的限制
解除，本计划所有退出条件闭合。
