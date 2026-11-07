# 每请求工具选择维护

> 状态：Completed
> 负责人：Mira Maintainers
> 更新日期：2026-11-08
> 所属计划：[总计划](mira-implementation-plan.md)
> 决策：[DEC-097](../decisions/DEC-097-per-request-tool-selection.md)

## 目标与范围

`MNT-202611-TS-01`：两类 loop 支持全量注册、每请求按需暴露；接活 profile 工具限额，
保留 canonical 安全上界，补齐失败语义、文档、示例和离线验收。不新增 Executor 路径。

## 工作项与退出条件

- [x] TS-G1：174 工具目录动态子集、空集、无 selector 兼容与两类 loop 通过。
- [x] TS-G2：未知/重复 ID、错误/异常 fail closed，未选工具不可执行，取消/重试通过。
- [x] TS-G3：profile 0/64/128 边界、129 配置拒绝、三方言及上传前限额拒绝通过。
- [x] TS-G4：M3 回归、格式/文档/架构/平台门禁通过，文档与证据同步。

## 风险与验证边界

默认 64 工具上限从死配置变为有效行为；迁移方式见 DEC-097。
真实 NapCat/OneBot 服务及 Windows/Android 不在本轮 Linux 离线验收范围；负责人为
部署宿主维护者，具备对应服务/工具链后固定版本和 profile 补跑集成与目标平台验证。

## 验证记录

2026-11-08：按用户授权选择 B，配合修复 profile 限额；实施与验证进行中。


2026-11-08 最终验收：基于 HEAD `6e55fed` 的当前工作树，Linux x86_64、GCC 13.3.0、
Debug、warnings-as-errors，kairo 使用仓库既有 pin。`MNT-202611-TS-01` 与 TS-G1–G4
按 Linux 离线范围 Completed；未修改 third_party，未新增并发路径或 Executor 缺口。

新增 `tests/m3/m3_tool_selection_test.cpp` 覆盖 174 工具全量目录、每轮选择/空集/顺序、
两类 loop、连续模式、缺省全量暴露、无注册表、未知/重复 ID、Error/异常脱敏与失败
事件、未选工具零执行、预取消、gateway 重试复用和 loop 恢复重选。成功事件校验 ID
数量与快照 digest；历史工具退出当前集合后，三方言同步/SSE 仍可编码，profile 的
零暴露限额不禁止工具历史，路由仍要求其工具协议能力。

数量覆盖默认 64、65、128、129、174；验证 profile 的 0/64/128 合法配置与 129 拒绝、
路由容量过滤/协议能力，以及三个 mapper 超限拒绝。直接 provider 配置上传路径与
计数器，超限时上传/HTTP 调用均为零；canonical 超限诊断包含实际数量与 128 上界。
API 示例、兼容性、设计、DEC-097 和总计划已同步。

可复现命令：

```bash
cmake --preset debug
cmake --build build/debug -j 8
ctest --test-dir build/debug -R '^mira_m3_tool_selection_test$' --output-on-failure
ctest --test-dir build/debug --output-on-failure -j 4
cmake --build build/debug --target docs-check architecture-check platform-boundary-check format-check
git diff --check
```

结果：全目标构建成功；专项 1/1 通过；Debug 全量 **157/157** 通过（38.11 秒，无跳过）；
四项静态门禁与 diff 空白检查通过，架构存量 baseline 保持 16 项。
未执行本轮 ASAN/UBSAN/TSAN、Windows/Android 与真实 NapCat/OneBot 集成；不据此
宣称 sanitizer、跨平台、性能或真实端点互操作保证。目标平台/桥侧补跑仍由部署宿主
维护者负责，具备对应工具链/服务时使用固定版本、profile 与同一选择策略验证。


2026-11-08 提交前独立基线验收：工具选择变更已从 M29 工作分支独立迁移到
`origin/master` 的 `d2533b9`，开发分支为 `feat/per-request-tool-selection`。
PR 仅含工具选择与 profile 限额变更，不包含 M29 尚未合并的实现；上文 157/157
属于原 M29 工作树的历史证据，不能代替当前 PR 基线验证。

在相同 Linux x86_64 / GCC 13.3.0 / Debug / warnings-as-errors 环境重新执行上文
configure、全目标 build、专项测试、全量 ctest 和四项静态门禁。结果：专项 **1/1**，
当前基线 Debug 全量 **146/146**（42.16 秒，无失败、无跳过）；docs、architecture、
platform-boundary、format 与 diff 空白检查通过，架构存量 baseline 仍为 16 项。
独立基线去除了 M29 的 11 个测试，工具选择新增 1 项；本次差异没有引入 M29 依赖。
本轮只提交并推送 PR，按维护者要求不检查或等待 CI、不进入合并流程；上述真实宿主、
目标平台与 sanitizer 验证限制保持有效。
