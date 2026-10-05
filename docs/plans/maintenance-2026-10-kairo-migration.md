# 维护计划：kairo v0.6.0 迁移轮（2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：[maintenance-2026-10-mirage-feedback-round2.md](maintenance-2026-10-mirage-feedback-round2.md)（PR #76 已合并）
> 建议发布点：依赖升级修订
> 更新日期：2026-10-05

## 1. 目标

将随附并发/生命周期库从 Executor v0.5.0（`2ae4fc8`）升级到 kairo v0.6.0
（`d9602ea`，上游更名 + 兼容层清理 + Scheduling Runtime 窗口），全树完成命名空间/
include/CMake/治理文件迁移，并借升级吸收上游 v0.5.2/v0.5.3 的评审修复
（[DEC-048](../decisions/DEC-048-kairo-0.6.0-migration.md)）。

## 2. 范围与非目标

### 2.1 范围

- submodule：`third_party/executor` → `third_party/kairo`（v0.6.0）；`.gitmodules`、
  `dependencies.lock.json`、`docs/supply-chain/direct-dependencies.md` 同步。
- 代码迁移：`<executor/...>` → `<kairo/...>`（含 77 个文件的 `executor::` 限定）、
  API 适配 6 处（`initialize_ex`→`initialize`×1、`submit_delayed_with_handle`→
  `submit_delayed`×4、`submit_delayed` 返回值 `.future` 消费×1、`ExecutorResult`
  显式 bool ×1）。
- 构建与治理：CMake 目标 `kairo::kairo`、`KAIRO_*` 选项、include 路径、header-check
  管线（`KAIRO_INCLUDE_DIR`）、`tools/architecture-policy.json` external module 与
  requires、根 `AGENTS.md` 集成路径、README/README_zh、活设计文档与 API 手册代码块。
- 台账：`EXE-20260922-001` 关联路径更新 + pin 迁移跟进行（缺陷在 v0.6.0 仍在，
  条目保持 Open）。

### 2.2 非目标

- 不接入 Scheduling Runtime（`IScheduler`/deadline/QoS/affinity/resource）；提交形态
  不变（DEC-048 备选方案）。
- 不修改 `third_party/kairo` 源码；MinGW 缺陷仍由台账跟踪（mira#75）。
- 不改写历史计划/决策/台账条目中的旧版本与旧路径记录。

## 3. 设计与决策依据

- [DEC-048](../decisions/DEC-048-kairo-0.6.0-migration.md)：本轮全部依赖形态变更
- [DEC-001](../decisions/DEC-001-runtime-executor-ownership.md)：所有权与关闭顺序约束不变
- 上游迁移指南：`third_party/kairo/docs/MIGRATION.md` §0.6.0

## 4. 工作项

- [x] `MNT-202610-06` submodule 替换与锁定信息同步（lock、direct-dependencies、
  .gitmodules）。
- [x] `MNT-202610-07` 全树命名空间/include/CMake/治理文件迁移与 6 处 API 适配，
  全树构建零警告零错误。
- [x] `MNT-202610-08` 全量回归：本地 ctest 103 项 + `architecture-check`/
  `docs-check`/header-check/SBOM 目标（Independent-Verification-Agent 执行）。
- [ ] `MNT-202610-09` CI 全矩阵绿（PR 验证）后回填证据。

## 5. 风险与阻塞

- kairo 源在 Windows/Android/sanitizer 组合下的编译差异由 CI matrix 覆盖；
  本机仅 Linux GCC Debug。
- 0.5.3 行为变化（定时器事件驱动、periodic 锚定）：Mira 无 `submit_periodic`；
  `submit_delayed` 路径由 m3/integration 定时器测试覆盖。
- EXE-20260922-001 不因升级关闭（v0.6.0 仍含缺陷），MinGW 门禁维持子集形态。

## 6. 测试与退出条件

- [ ] 本地全量 ctest 通过；`architecture-check`、`docs-check` 通过。
- [ ] CI 24 项检查全绿（含 Windows/Android/sanitizers/quality）。
- [ ] 台账、锁定信息、AGENTS.md、README 与实际依赖形态一致（docs-check 链接零缺失）。

## 7. 验证记录

2026-10-05：实现轮（分支 `auto/mnt-202610-kairo-migration`，工作树基于 master `3716dbf`）。

- 构建：`cmake --preset debug && cmake --build --preset debug -j 4` 全目标零警告零错误
  （迁移触碰 77 个源文件 + CMake/治理文件；编译期仅暴露 1 处 `ExecutorResult` 显式
  bool 转化，按 `.ok` 显式读取修复）。
- `architecture-check`（kairo external module 生效）、`docs-check` 通过。
- 测试与 CI：待 IV-Agent 与 PR 回填。

2026-10-05：回归轮（Independent-Verification-Agent 两轮执行）。

- 构建复核：clean-first 全量重编译通过；Mira 自有代码（src/include/adapters/tests/
  examples）零警告。kairo 上游源在 GCC 13 下自带 35 条非致命警告（`-Wshadow`×24 等，
  `third_party/kairo/src/...`），属上游问题不阻塞；残留引用扫描 `<executor/`、
  `executor::`、`initialize_ex` 命中为 0。
- 定时器/生命周期敏感面逐项通过（各复跑 3–5 次）：`m3_upload`（`submit_delayed` →
  `TimerSubmission.future` 消费）、`m3_gateway`（retry pacing）、
  `executor_lifecycle`（delayed 句柄取消 `CancelledBeforeDispatch`、`TaskCancelled`
  异常、realtime/blocking worker、`SerialExecutionContext`、`max_in_flight_tasks`
  容量拒绝、`shutdown(true)`）、`m1_core`/`stress`（关闭顺序）、
  `m3_conversation_loop`、`m7_tool_module_mcp`（`.ok` 适配）。
- 首轮全量 101/103：两个迁移尾巴（均非 kairo 行为差异）——
  1. `cmake/MiraConfig.cmake.in` 仍 `find_dependency(executor 0.4)` → 改为
     `find_dependency(kairo 0.6 CONFIG REQUIRED)`；复验又暴露消费者工程
     `tests/consumer/CMakeLists.txt` 半迁移（`find_package(executor 0.4)`、旧目标名），
     由验证代理按全仓惯例机械改名（`find_package(kairo 0.6)` + `kairo::kairo`）。
  2. `docs/supply-chain/sbom.cdx.json` 组件仍为 executor@2ae4fc8 → 更新为
     kairo@d9602ea（name/version/bom-ref/purl），`check_sbom.py` 比对通过。
- 修复后全量：**103/103 通过**（CTEST_EXIT=0，118.77s）；`architecture-check`、
  `docs-check`、`mira_public_headers_test`（header-check）、`mira_platform_boundary_test`
  通过。
- 未执行项：Windows/Android 构建与 sanitizer 变体（PR CI matrix 覆盖）；kairo 定时器
  事件驱动后的长时空转 CPU 独立测量（上游文档口径 ~0.024%，无 CI 目标）。
