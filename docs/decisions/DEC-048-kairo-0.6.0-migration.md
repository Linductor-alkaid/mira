# DEC-048：随附并发库升级 kairo v0.6.0（Executor 更名与兼容层清理窗口）

> 状态：Accepted
> 日期：2026-10-05
> 负责人：Mira Maintainers
> 冻结里程碑：维护轮（2026-10，kairo 迁移）
> 替代/被替代：无（细化 [DEC-001](DEC-001-runtime-executor-ownership.md) 的依赖承载；DEC-001 的所有权与关闭顺序约束不变）

## 背景与问题

Mira 随附的并发/生命周期库（原 Executor）pin 于 v0.5.0（`2ae4fc8`）。上游随后发布
v0.5.2/v0.5.3（2026-09-30 全量代码评审修复：9 项 P0 内存安全/挂死/数据竞争、24 项 P1
正确性、热路径与定时器性能优化）与 v0.6.0（项目更名 kairo、兼容层全面清理、Scheduling
Runtime）。继续停留 v0.5.0 意味着放弃评审修复窗口，且后续升级只会积累更大的破坏面。

v0.6.0 的破坏性变更：命名空间 `executor::` → `kairo::`、include `<executor/...>` →
`<kairo/...>`、CMake `executor::executor` → `kairo::kairo`、`EXECUTOR_*` 选项 →
`KAIRO_*`；历史弱 API 删除（`initialize_ex`→`initialize`、`wait_for_completion`、
注册类 `_ex` 族去后缀、字符串定时器 ID 体系删除、`push_task` 返回 ExecutorResult 且
`operator bool` 显式化、4 参 `submit_auto`、comm `is_lock_free` 别名、
`memory_locked` 字段）。迁移指南：`third_party/kairo/docs/MIGRATION.md` §0.6.0。

## 决策

1. **升级 pin 到 kairo v0.6.0**（`d9602ea6762806be320b9543e3b36f27f7dd5b1a`，MIT 不变），
   submodule 路径 `third_party/executor` → `third_party/kairo`，来源
   `https://github.com/Linductor-alkaid/kairo.git`；`dependencies.lock.json`、
   `docs/supply-chain/direct-dependencies.md` 同步。
2. **全树改名**：`<kairo/...>` include、`kairo::` 限定、CMake 目标/选项/include 路径、
   header-check 管线（`KAIRO_INCLUDE_DIR`）、`tools/architecture-policy.json` 的
   external module `executor` → `kairo`（头族 `kairo/`）与各模块 requires。Mira 自身
   概念性命名保留（如 `mira.tool_executor` 错误域、"executor shutdown" 诊断文本——
   指概念而非库名）。
3. **API 适配仅限实际使用面**：`initialize_ex`→`initialize`（消费 `.ok`）、
   `submit_delayed_with_handle`→`submit_delayed`（返回 `TimerSubmission{.handle,.future}`，
   与 0.5.x `_with_handle` 形态一致）、2 参 `submit_delayed` 返回值改经 `.future` 消费、
   `ExecutorResult` 显式 bool 化处显式读 `.ok`。Mira 未使用被删的字符串定时器体系、
   `push_task`、`is_lock_free`、`memory_locked`、4 参 `submit_auto` 与 GPU 注册族；
   保留面 `submit_on_with_handle`/`submit_with_handle`/`shutdown(bool)`/
   `SerialExecutionContext`/`max_in_flight_tasks`/`set_cancellation_registry_capacity`
   逐一核对无变化。
4. **Scheduling Runtime 暂不接入**：`IScheduler`/deadline/QoS/affinity/resource 为新增
   非破坏能力，`submit_auto(task(...))` 行为不变；Mira 不在本轮改任何提交形态，后续
   如需 EDF/QoS 语义另立决策。
5. **EXE-20260922-001（MinGW native_handle cast，mira#75）不随升级关闭**：v0.6.0 同一
   表达式仍在 `src/kairo/blocking_io_executor.cpp:157`，台账保持 Open 并更新关联路径。

## 备选方案

- **停留 v0.5.0 并仅升 v0.5.3**：可得评审修复但躲不开更名窗口，且 0.5.x 已非上游主线；
  仅推迟成本；否决。
- **vendor 一层 `executor::` 兼容别名**：在 Mira 内自造兼容层与上游"清理弱 API"方向
  相悖，且 AGENTS.md 禁止为绕开依赖形态自建设施；否决。
- **顺带接入 Scheduling Runtime**：与本轮"依赖形态迁移"目标正交，扩大回归面；否决，
  留待独立决策。

## 影响与风险

- 构建/CI：全部目标与 24 项 CI 检查必须重验（Windows/Android/sanitizers 覆盖 kairo
  源的编译差异）。
- 行为面：0.5.3 的定时器线程改事件驱动（空闲 CPU 显著下降）、periodic 锚定变化——
  Mira 无 `submit_periodic` 使用，定时器路径仅 `submit_delayed` 族；P0/P1 修复预期
  只收紧正确性。
- 历史 `EXE-20260830-*` 台账条目引用的旧路径/版本为历史记录，不改写。
- MinGW 门禁形态不变（EXE-20260922-001 仍 Open）。

## 验证方式

- 全树构建零警告零错误；全量 ctest（103 项）通过；`architecture-check`、`docs-check`、
  header-check、SBOM 目标通过。
- CI 全矩阵（Linux GCC/Clang、Windows、Android arm64/x86_64、ASAN/UBSAN/TSAN、
  quality）绿。

## 关联文档和工作项

- [DEC-001](DEC-001-runtime-executor-ownership.md)：Runtime-Executor 所有权（不变）
- 维护计划：[maintenance-2026-10-kairo-migration.md](../plans/maintenance-2026-10-kairo-migration.md)
- 上游：[kairo v0.6.0 release](https://github.com/Linductor-alkaid/kairo/releases/tag/v0.6.0)、
  `third_party/kairo/docs/MIGRATION.md`
- 台账：`EXE-20260922-001`（保持 Open）

> 2026-10-05 更新：pin 由 v0.6.0 tag（`d9602ea`）前移至 master `ef821dc`
> （`v0.6.0-12-gef821dc`），以承接 [kairo#215](https://github.com/Linductor-alkaid/kairo/pull/215)
> 对 `EXE-20260922-001`（MinGW `native_handle` cast）的修复；同一 0.6.x 源线，无新增
> 公开 API 消费，决策语义不变。
