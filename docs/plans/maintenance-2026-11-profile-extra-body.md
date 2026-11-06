# Profile 厂商请求体扩展维护

> 状态：Completed
> 负责人：Mira Maintainers
> 更新日期：2026-11-06
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 决策：[DEC-090](../decisions/DEC-090-profile-extra-body.md)
> 设计：[LLM API 协议设计](../design/llm-api-protocol-design.md) §5.4

## 目标与范围

交付配置级 `ModelProfile::extra_body`，仅向 Chat Completions wire root 追加受控厂商
字段。覆盖校验、路由/digest、mapper/provider 和离线验证，不新增异步工作或 Executor
路径。不扩展 Responses/Messages，不修改 canonical thinking 语义，不声明真实厂商
互操作或性能。

## 工作项与退出条件

- [x] `MNT-202611-EB-01`：公开配置、校验、manifest/digest、同步/SSE 编码、其他方言
  拒绝；负向与 mock provider 测试通过，API、示例、设计、兼容性及验证证据同步。
- [x] EB-G1：扩展值原样进入 wire，mapper 自有字段保持原值，空配置兼容。
- [x] EB-G2：冲突（含未输出字段）、重复/空键、非法 JSON、非 Chat 方言在发送前拒绝。
- [x] EB-G3：配置变更影响 manifest/profile/wire digest，排序稳定，RouteDecision 绑定。
- [x] EB-G4：M3 回归及 format/docs/architecture/platform-boundary 门禁通过。

## 风险与验证边界

厂商可能通过未标准化字段影响生成或工具行为，宿主需审查固定配置；本地通道不提升
能力证据等级。Qwen/vLLM/SGLang 的真实互操作不属于本地门禁：当前未配置目标服务，
负责人为部署宿主维护者；需固定服务/模型版本、profile digest 后运行目标请求补验。
Windows/Android 未在本轮本机执行，不新增跨平台运行声明。

## 验证记录

2026-11-06：从工作树 HEAD `4f1780d` 开始；现有 M29 计划、总计划及 DEC-089 的用户
变更保留，扩展维护独立登记。最终命令与结果见下文。

本轮实现补充：`JsonValue::Object` 在公开 profile 末尾追加；内部共享配置门禁覆盖
Router、mapper 和上传前 provider。保留标准与 canonical 控制键，最终 wire 再防御
实际碰撞；配置错误只返回固定安全消息。完整推理 body 上限在 provider 出线前
执行，并清空每轮 trace/header，避免本地拒绝沿用上次远端写入诊断。JSON 数字
序列化调整范围检查顺序，避免大有限数在进入浮点输出前越界转整数。

本机环境：Linux x86_64、GCC 13.3.0、Debug、warnings-as-errors；ASAN+UBSAN
定向构建与 Debug 构建使用独立目录。未新增 Executor 路径、外部依赖或台账缺口，
无第三方源码变更。

2026-11-06 最终验收：上述工作树上的本地交付完成，EB-G1–G4 全部通过。
新增 `tests/m3/m3_extra_body_test.cpp` 覆盖同步/SSE、原样值/固定字段不变、空配置
兼容、所有保留键（含未输出字段）、重复/空键、非法 UTF-8、非有限数/大有限数、
深度/字符串/字节上限、直接 mapper 方言误用、路由拒绝与同 ID profile 重绑定、
profile/wire digest 排序稳定及值变化、mock HTTP 实际请求、上传前拒绝，以及完整
请求超限零推理传输与旧 trace 不继承。现有 M3/M4 覆盖 continuation/replay 回归。

准确命令与结果：

```bash
cmake --preset debug
cmake --build build/debug -j 2
ctest --test-dir build/debug -R '^(mira_m[34]_.*|mira_.*consumer.*|mira_unit_test|mira_contract_test)$' --output-on-failure -j 2
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMIRA_BUILD_TESTS=ON -DMIRA_WARNINGS_AS_ERRORS=ON -DMIRA_ENABLE_ASAN=ON -DMIRA_ENABLE_UBSAN=ON
cmake --build build/asan --target mira_m3_extra_body_test mira_m3_canonical_test mira_m3_dialect_test mira_m3_anthropic_test -j 2
ctest --test-dir build/asan -R '^mira_m3_(extra_body|canonical|dialect|anthropic)_test$' --output-on-failure
cmake --build build/debug --target format-check docs-check architecture-check platform-boundary-check
git diff --check
```

Debug 全目标构建成功；所选回归 **55/55**（37.66 秒，含 installed consumer、同步/
SSE/transport/取消/预算/上传/工具/loop/continuation/replay）；ASAN+UBSAN 定向
**4/4**（4.62 秒）。四项静态门禁通过；架构存量 baseline 16 项保持不变。未运行
其他测试不计为成功，不将本轮证据扩展为全仓测试或跨平台互操作结论。

同步：公开头文件、API 配置示例、协议设计 §5.4、兼容性矩阵、DEC-090 与总计划。
真实 vLLM/SGLang/Qwen 端点及 Windows/Android 运行仍属上文明确的验证边界，由
部署宿主维护者在具备相应服务/工具链后补跑；本轮只关闭 Linux 离线扩展通道范围。
