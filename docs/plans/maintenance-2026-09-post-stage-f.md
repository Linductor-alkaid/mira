# 阶段 F 后续：验收补齐与产品闭环

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 前置：M8–M13 已合入的实现；跨平台验收缺口见下文
> 建议发布点：先恢复 Workflow learning alpha 验收，再按 demo 证据定义下一发布点
> 更新日期：2026-09-09

## 1. 目标与边界

依据当前源码、测试、构建配置和仓库验收记录确认阶段 F 的实际状态，把遗留能力与证据缺口
转为有负责人、依赖和退出条件的任务。本轮交付状态审计与计划更新；后续实现由下列任务
跟踪。新增公开契约先冻结专项设计与 DEC，再创建对应里程碑，不预分配 M14 等编号。

依据：[项目管理规范](../project/project_management_and_documentation.md) §5/§10、
[双路径架构](../design/agent_harness_and_workflow_architecture.md)、
[Workflow 专项设计](../design/workflow_runtime_design.md) §11–14、
[DEC-011](../decisions/DEC-011-demo-first-external-validation.md)、
[DEC-029](../decisions/DEC-029-memory-domains-and-learning-contracts.md)、
[DEC-030](../decisions/DEC-030-learning-loop-runtime-semantics.md)。
M5/M6 保持 Cancelled，M7 保持 Blocked；本计划不批准恢复原范围或扩大 v1 平台承诺。

## 2. 状态核对与缺口

审计基线：`cd89384`，阶段 F 实现合并提交 `db2e814`。初始工作树干净。

| 范围 | 已有证据与实际边界 | 尚缺内容 / 跟踪项 |
| --- | --- | --- |
| 阶段 A–F | M8–M13 实现与测试已合入；历史记录为 Linux/Windows、sanitizer、quality 通过 | Android CI 漏编译 Workflow；重开六个里程碑的跨平台验收，`MNT-202609-22` |
| F 学习闭环 | 域映射、Episode/Lesson、失败查询与 `relevant_lessons` 已实现；M13 测试以三个 Run 验证记住、恢复、再次检索 | AgentLoop 未消费 continuation；测试由宿主直接 resume/record lesson，不能证明模型采纳有效，`MNT-202609-23/24` |
| 长期资产与恢复 | M4 有 SQLite Memory；M9 Library/Run 与 M12 App Model 为进程内投影 | M13 fixture 与 consumer 学习段均用内存后端；缺跨重启学习集成与事件重建逐字段/digest 验证，`MNT-202609-25/26` |
| 自动化资产复用 | M11 有轨迹捕获、编译、归纳、DryRun 入库；M12 有图与导航规划 | 缺 Procedure 索引消费者、按目标选 Workflow，以及真实 UI 到 `ScreenStateProvider` 的消费侧验证，`MNT-202609-27/31` |
| 真实平台与 Provider | Android ABI 文档已有 miracle P1 截图及 lease 路径证据；Provider 矩阵逐 capability 记录 | UI tree、转码后视觉闭环、决策修复、输入/权限/Takeover/宿主销毁完整矩阵仍有外部未结项，`MNT-202609-27` |
| 评估与发布 | 有单元/集成/consumer 测试与 M4 benchmark；BuiltIn 工具闭环已交付 | 缺 Workflow 任务级统一评估、学习增益/成本基线、soak；ToolModule 签名/协商/OOP 仍未交付，`MNT-202609-28/29/30` |
| 文档入口 | 总计划已记录 M13 关闭 | README 与架构总览仍只报 M0–M4 或 Workflow 未实现；本轮修正，`MNT-202609-21` |

### 2.1 BUG-20260909-001：Android CI 未覆盖 Workflow 目标

- 证据：[CI 配置](../../.github/workflows/ci.yml) 的 Android build 使用显式 `--target`
  列表，仅包含 Core、Simulator、Android Adapter、transport、state store、stateful consumer。
  [CMakeLists.txt](../../CMakeLists.txt) 中 `mira_workflow` 是独立目标；
  `mira_stateful_consumer` 仅链接 Core/state store/Executor，不会间接构建 Workflow。
  Android presets 同时关闭测试，因此 consumer tests 也不会补上这一覆盖。
- 影响：M8–M13 的历史 Android job 成功不能证明这六阶段新增模块可交叉编译。现有功能与
  Linux/Windows 历史通过记录保留，但矩阵取证工作项及相应退出条件重新打开，状态恢复
  `In Progress`。依赖方复用已冻结契约与实现，跨平台关闭统一等待本缺口。
- Owner：Mira Maintainers。解除条件：两种 Android ABI 实际编译 Workflow，并保留准确
  commit、NDK、命令、日志与结果；按各里程碑退出条件复核后逐项关闭。属于 Mira CI
  覆盖问题，无证据表明是 Executor 能力缺口。

### 2.2 能力边界

`relevant_lessons` 是数据，`record_recovery_lesson` 是宿主专用写入门面；采纳经验、生成
修复、重新验证和决定是否沉淀资产仍缺 Harness 编排。学习记录默认还要求宿主安装
IMemory 与 EventStore；缺事件锚点的 Episode 会跳过（`record_episode`），并非开箱即持久学习。

M13 已按范围推迟 Procedure 自动索引、向量召回、User Model 扩展、TTL/retention、训练导出。
这些属于新增能力，不据此重开已完成的功能项。跨进程 Library/Run/App Model 持久化亦为
M9/M12 显式非目标；M4 恢复能力不能直接外推到它们。
DEC-030 §5 的重建配方目前缺少“仅凭持久事件恢复同一学习记录”的完整取证；本轮将其列为
待核实契约风险，不将事件可解析等同于已实现恢复工具。

## 3. 工作项与顺序

所有任务负责人均为 Mira Maintainers；外部验证由 Maintainers 汇总 miracle 宿主维护者证据。
`Planned` 只表示任务定义可执行，不表示依赖已满足；`Proposed` 实现项须先完成对应设计任务。

### 3.1 P0：恢复验收可信度

- [x] `MNT-202609-21`（Completed）完成阶段 F 状态审计、缺口归类及后续计划；同步总计划、
  M8–M13、M7 跟进入口、README、架构现状与平台证据边界，校验文档链接与结构。
- [ ] `MNT-202609-22`（Planned）修复 `BUG-20260909-001`：Android CI 显式编译
  `mira_workflow`，增加安装包 consumer 对 `Mira::workflow` 的交叉链接检查，防止只产出
  静态库而遗漏链接闭包。依赖：完整 pinned 子模块、NDK 26.3.11579264/API 24。
  验收：arm64-v8a/x86_64 两配置实际编译全部 Workflow 源文件并完成 consumer 链接；
  原有 Linux/Windows/quality 回归通过；回填六个里程碑与平台矩阵。设备运行单列，不能
  用交叉链接成功代替。

### 3.2 P1：让 Agent 使用经验并可验证地恢复

- [ ] `MNT-202609-23`（Planned）冻结 Harness 恢复编排设计与决策。依赖：DEC-023/024/030。
  验收产物明确 `WaitingAgent -> continuation -> 有界模型请求 -> 结构化 patch/决策 ->
  resume -> Observe/Verify -> 宿主记录 lesson` 的所有者、事件关联与失败出口；定义无
  lesson、拒绝采纳、失效 lesson、取消/Takeover/迟到响应、升级预算及敏感信息处理。
- [ ] `MNT-202609-24`（Proposed）实现上述编排与公共 consumer。依赖：22 的验收补齐、23
  冻结后正式立项。验收：recorded Provider 真正收到检索上下文并生成可验证修复；首次
  失败、恢复、再次命中全链路可追踪；恶意 lesson 不提升权限，取消/接管后无新增动作，
  终态不复活，正常/异常/拒绝/超时/shutdown 均有测试；真实 Provider 证据由 27 补齐。
- [ ] `MNT-202609-25`（Planned）验证学习持久化与事件重建配方，形成字段级证据及差异清单。
  依赖：现有 M4/M13 接口及可构建环境。验收：用真实 SQLite IMemory 写 Episode/Lesson，
  关闭并重建 owner 后同 scope 查询一致、跨 scope 不可读；从持久事件重建时逐字段比较
  ID、timestamp、failure signature、patch 摘要、provenance 与 digest；缺字段则登记
  实现缺陷或提出 DEC-030 修订，不标为成功。另覆盖慢/失败 store 对取消与 shutdown 的影响。
- [ ] `MNT-202609-26`（Proposed）交付有需求证据支持的 Workflow Library/Run/App Model
  持久化与恢复。依赖：25 的差异清单、27 的跨重启需求证据、专项存储/迁移 DEC。
  验收：旧版本 Run 固定原 digest、crash window 重建幂等、损坏/未知版本拒绝、未确认
  副作用先 Observe/Verify，OfflineReplay 不执行输入/网络/Tool；恢复工具与离线展示分离。

### 3.3 P1：真实任务证据与统一评估

- [ ] `MNT-202609-27`（Blocked）汇总 miracle 在固定 Mira 版本上的消费与需求报告。
  阻塞：本仓库相关外部验收项尚未回填，本轮未取得完整设备/宿主报告。
  补跑条件：宿主实现、受支持设备、受控 Provider profile 与脱敏记录可用。验收包括
  `mira.host.tree.v1`、转码后图像请求、决策编译修复同任务复验，以及旋转/前后台/
  权限撤销/Takeover 输入释放/宿主销毁；补充 A–F 组合任务与跨重启需求，记录 Mira 与
  miracle commit、设备/API、Provider/model、样本数、失败归因、成本和成功率。
  原未结项保留在 [Host ABI 维护计划](maintenance-2026-09-host-abi-feedback.md)、
  [图像传输维护计划](maintenance-2026-09-transport-and-image-media.md)、
  [决策修复维护计划](maintenance-2026-09-decision-compile-repair.md)，以其为验收源；
  此项统一索引，不重复计算完成数。
- [ ] `MNT-202609-28`（Planned）定义当前离散动作 + Workflow 范围的最小 Eval profile。
  依赖：M8–M13 契约、[评估设计](../design/evaluation_and_benchmark_design.md)。验收：
  固定 case/fixture/baseline digest、失败分类、重复样本方法与预算，覆盖直接 Agent、
  Strict Workflow、恢复编排、启用/禁用 lesson 四种对照；指标包含任务成功率、恢复率、
  模型调用/token/cost、尾延迟、人工介入、重复副作用、内存/句柄与 shutdown 时间。
  阈值在跑结果前冻结；不得沿用已取消的 ONNX/realtime 准入要求。
- [ ] `MNT-202609-29`（Proposed）实现并运行最小任务评估与 soak。依赖：28 冻结；恢复
  组依赖 24，真实平台组依赖 27。验收：公共 API 驱动、recorded 回归确定、live canary
  单独报告、所有适用 fault/cancel/Takeover/rejection/shutdown 场景有结果；学习收益
  依据对照与分布报告，不能凭命中一条 lesson 宣称成功率或成本改善。
- [ ] `MNT-202609-30`（Planned）产出 M7 重定义提案与任务迁移映射。依赖：27 的需求报告、
  28 的 profile。验收：逐项映射 `M7-01`–`M7-28` 到保留/缩减/推迟的建议及证据，明确
  release profile、ToolModule/OOP 是否必要、平台等级与退出条件；提交专项 DEC 后再
  修改 M7 范围，原项在批准迁移前保持未勾选。

### 3.4 P2：由消费者与失败语料驱动的扩展

- [ ] `MNT-202609-31`（Proposed）建立 Procedure 资产索引及目标到 Workflow 的检索消费者。
  依赖：27 证明复用需求、资产版本/删除/撤销契约冻结。验收：只索引已验证版本，绑定
  workflow ID/digest/scope；失效版本拒绝、删除可传播，检索命中后重新校验准入与 Verify。
- [ ] `MNT-202609-32`（Proposed）评估失败检索向量腿与 retention 扩展。依赖：25/29 的
  持久化与召回基线、真实失败语料。先测 exact+FTS 的漏召回/误召回、成本与延迟，再决策
  是否实现；任何实现需验证 ACL、版本漂移、删除传播和回退。无收益证据时保留现有方案。

建议先执行 22；23、25、28 可独立准备，27 持续回收外部证据。随后按证据推进 24/26/29，
由 30 收敛 M7。P2 不阻塞验收补齐。该顺序是本维护计划的任务优先级，不替代 DEC-011 的
产品范围决策，也不把缺少外部证据的任务置为已就绪。

## 4. Executor、风险与退出条件

本轮仅改文档，无新增执行路径。后续模型/有限编排任务使用 Executor 并消费 future；
持久化由后端受管 store worker 承载；设备事件通过 Adapter 外部循环协调；评估 owner
管理所有任务句柄。停止生产者、请求取消、回收模型/存储/平台工作、结算有限任务，最后
由非 worker 外部 owner `shutdown(true)`。具体容量、deadline、解阻塞与拒绝语义必须在
23/26/28 的设计中写明。若有 Executor 缺口，先核对公开接口并登记反馈台账。

- 风险：慢 IMemory 同步门面可能拖延驱动/结算（沿用 M13 `RISK-2026-049`）；由 25 取证。
- 风险：内存 fixture、事件解析与真实恢复语义不等价；由 25/26 检验，不扩大现有承诺。
- 外部阻塞负责人：Mira Maintainers；设备与 Provider 补跑条件见 27。

- [x] 本轮审计事实、计划与文档入口一致，历史失败及验收记录保留。
- [ ] 22 完成并恢复 M8–M13 的适用跨平台验收。
- [ ] 23/25/28 产物齐全，后续实现已获正式里程碑或有理由的延期记录。
- [ ] 27 外部证据归档；30 完成 M7 范围决策及任务映射。
- [ ] 本计划内 Proposed 实现项已正式迁移或通过决策明确取消/推迟，不能因文档更新完成
  而将整个维护计划关闭。

## 5. 验证记录

2026-09-09：`MNT-202609-21` 审计基于 `cd89384`。静态核对 `CMakeLists.txt`、
`.github/workflows/ci.yml`、Workflow/AgentLoop 公共 API 与实现、M13 三组测试及
installed-consumer，发现 `BUG-20260909-001`。CI 历史结果引用原里程碑记录，本轮未重新
查询远端 CI 或调用真实 Provider，不作为新的运行证据。

本轮环境 Linux x86_64、Ubuntu GCC 11.4.0、CMake 4.4.0。尝试
`cmake --build --preset debug -j2` 因无 Makefile 失败；随后 `cmake --preset debug`
因 `third_party/executor`、`third_party/mbedtls` 缺 CMakeLists 失败。
`git submodule status` 确认二者未初始化（分别固定 `4fd8e609`、`068ff080`）。
故 C++ 构建、CTest、sanitizer 本轮未执行成功，不沿用旧计数作为本轮结果。
Owner：Mira Maintainers；补跑条件：初始化 pinned 子模块、满足项目 C++20 工具链后，执行
configure/build/CTest；Android 按 22 使用 NDK 与两 ABI 取证。

文档验证：本轮 12 个新增/修改 Markdown 的相对链接、唯一一级标题、标题层级与代码围栏
经逐文件脚本检查通过；`git diff --check` 通过。全仓 `python3 tools/check_docs.py .`
仍失败，四处均为既有文档指向未初始化子模块中的 Executor API/集成指南/LICENSE 与
Mbed TLS LICENSE；未修改这些引用或将缺失伪造为通过。负责人 Mira Maintainers，补跑
条件为初始化上述 pinned 子模块后重新执行该命令。
