# ConversationLoop 宿主 system 模板维护

> 状态：Completed（Linux 离线契约范围）
> 负责人：Mira Maintainers
> 更新日期：2026-11-09
> 所属计划：[总计划](mira-implementation-plan.md)
> 决策：[DEC-098](../decisions/DEC-098-conversation-host-system-prompt.md)、[DEC-097](../decisions/DEC-097-per-request-tool-selection.md)

## 目标与范围

修复 `BUG-20261109-001`。采用 B 完整 system 覆盖与可审计模板身份/digest；
复用逐请求工具选择，验证宿主工具发声配置和动态暴露的组合。不新增并发路径，
不修改第三方代码，不改变文本终态、工具权限、历史、准入与取消语义。

## 工作项与退出条件

- [x] `MNT-202611-SP-01`：完整替换、自动/显式 digest、默认兼容与配置拒绝通过。
- [x] `MNT-202611-SP-02`：动态工具集合、三方言、历史、连续/有限、恢复/取消与审计通过。
- [x] `MNT-202611-SP-03`：相关构建/回归、文档、格式、架构和平台边界检查通过。

## 风险与验证边界

本轮验收 Linux 离线契约；模板不能保证模型服从或代表发声已送达。

- [ ] 真实 mira-bridge/Qwen3.5-9B 发声效果未执行：本轮无对应服务/设备，负责人为
  部署宿主维护者；升级 SDK、迁移 system 配置并接入动态工具选择后，保存事件与
  journal 对照，补跑工具发声、沉默、工具失败及恢复。
- [ ] Windows/Android 本轮未执行：负责人 Mira Maintainers；对应 CI/工具链可用后
  补跑构建和契约测试，设备行为另在真机复验。

## 验证记录

2026-11-09：基于 `edef847` 开始实施。动态暴露已由 DEC-097 交付，本轮补齐完整
system 缝与组合验收，不重复建立选择器或调度设施。

2026-11-09：基于 `edef847` 的 `fix/conversation-host-system-prompt` 工作树完成本地验收。
Linux x86_64、GCC 13.3.0、C++20、Debug、warnings-as-errors；kairo 保持既有 pin。
全目标构建成功，CTest **148/148** 通过（37.66 秒，无失败或跳过），包含公共头独立
包含与安装消费者。新增专项及既有工具选择定向 **2/2** 通过（新增发现流程随后纳入
全量验收）。未修改第三方代码、架构 baseline、模块依赖或 Executor 生命周期。

专项覆盖：默认有/无工具的文本、来源及旧 digest；完整替换且唯一 Internal System；
自动/宿主声明 digest 的 canonical 往返与审计元数据；有限/持续模式工具往返、历史
留存与工具退出暴露集合；发现工具结果后下一轮新增发送工具、再收回暴露；空白文本/
身份及全零摘要在回调/infer 前拒绝；三方言同步/stream 编码；loop 恢复重组、gateway
重试复用、预取消/回调后取消与未选工具零执行。测试不把 echo/send fixture 的成功
当作真实平台送达，也不据此宣称模型服从率。

```bash
cmake --preset debug
cmake --build build/debug -j 6
ctest --test-dir build/debug -R '^mira_m3_(conversation_system_prompt|tool_selection)_test$' --output-on-failure
ctest --test-dir build/debug --output-on-failure -j 4
cmake --build build/debug --target format-check
python3 tools/check_docs.py .
python3 tools/check_architecture.py
python3 tools/check_platform_boundary.py
git diff --check
```

格式（319 文件）、文档链接/围栏、架构（存量 baseline 16 项）、Core 平台边界及
空白检查通过。首次文档检查将示例 lambda 的 `](...)` 误识别为 Markdown 链接，
调整示例空格后通过。API 示例、设计、DEC-098 与总计划入口已同步。
本轮未执行 sanitizer、Windows/Android、真实 Qwen3.5-9B/桥侧验证；目标平台与
部署补跑的负责人和条件保留在风险节，不以离线测试替代。
