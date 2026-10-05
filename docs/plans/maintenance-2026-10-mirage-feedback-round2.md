# 维护计划：Mirage 反馈第二轮上游消化（2026-10）

> 状态：In Progress
> 负责人：Mira Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)（M27 后维护轮）
> 前置：M25（Completed）、M27（Completed）
> 建议发布点：Host contract alpha 修订
> 更新日期：2026-10-05

## 1. 目标

消化 Mirage 桌面 Host 第二轮依赖反馈的 4 个 GitHub issues（#72–#75，编号
MIRA-20260922-001 / MIRA-20260927-001 / MIRA-20261004-001 / MIRA-20261004-002）：
修复 OpenSSL TLS adapter 的 SNI 缺失（#72）、workflow 版本解析的同 digest 草稿遮蔽
（#74），交付无观察对话循环与规范工具回填契约（#73，[DEC-047](../decisions/DEC-047-conversational-loop-and-canonical-tool-parts.md)），
并按台账流程登记 Executor 的 MinGW 可移植性缺陷（#75，不改 pinned 依赖）。

## 2. 范围与非目标

### 2.1 范围

- `adapters/net/openssl_tls.cpp`（#72 / MIRA-20261004-002）：`setup()` 在 connect 前为
  DNS 主机名设置 TLS `server_name`（SNI）；IP 字面量按 RFC 6066 §3 跳过；系统信任库、
  链验证与主机名验证语义不变。
- `src/workflow/workflow_versioning.cpp`（#74 / MIRA-20260927-001）：
  `resolve_workflow_version` 对同 content digest 多记录取**最新**匹配（与
  `latest_runnable_workflow_version` 的 rbegin 方向一致，语义为"该内容的当前验证状态"），
  消除 draft→publish 同内容流中 NotValidated 草稿遮蔽 DryRunPassed 记录的缺陷。
- 公开契约（#73 / MIRA-20261004-001，DEC-047）：
  - `ModelContentPart` 新增 `ToolCallPart` / `ToolResultPart`；`make_tool_call_item()` /
    `make_tool_result_item()`；校验、JSON 往返、canonical digest 与两 dialect 原生线格式
    （Responses `function_call`/`function_call_output`；Chat `tool_calls`/`role: tool`）。
  - `parse_decision()` 在 `OutputMode::Text` 下把工具调用解析为 `ToolProposals`。
  - 新增 `ConversationLoop`（`include/mira/conversation_loop.hpp`）：无观察
    model → tool → result → model 循环，文本回答即终态。
- `docs/executor_feedback/ledger.md`（#75 / MIRA-20260922-001）：登记
  `EXE-20260922-001`（blocking_io_executor 的 native_handle cast 阻塞 MinGW-w64 posix
  构建），转交上游流程；不修改 `third_party/executor`。

### 2.2 非目标

- 不修改 `third_party/executor`（#75 的修复属上游 executor 仓库；Mira 侧仅台账登记）。
- 不改变 `AgentLoop` 的设备闭环语义（文本回填路径保持冻结，见 DEC-047 备选方案）。
- 不在 Mira 侧对 SNI 缺失做开关或降级（无绕行；MiniMax 类端点的补验属 Mirage 侧 pin
  升级后的动作）。
- 不新增 `ModelRole::Tool`（dialect 差异不进规范 role 集，见 DEC-047）。

## 3. 设计与决策依据

- [DEC-047](../decisions/DEC-047-conversational-loop-and-canonical-tool-parts.md)：本轮全部
  公开契约新增（规范工具 part、Text 模式工具提案语义、ConversationLoop）。
- [DEC-015](../decisions/DEC-015-builtin-tool-execution-boundary.md)：工具执行边界不变，
  ConversationLoop 复用。
- [DEC-010](../decisions/DEC-010-cross-platform-tls-proxy-upload.md) /
  [DEC-013](../decisions/DEC-013-transport-export-and-image-media.md)：TLS 传输栈边界；
  SNI 是缺陷修复，不是能力开关。
- [DEC-020](../decisions/DEC-020-workflow-run-lifecycle.md)（W-03/W-04）：按 digest 解析与
  runnable 门禁语义；#74 是该语义下的实现缺陷修复。
- 台账流程：根 `AGENTS.md` §Executor 能力缺口与反馈台账、
  [executor_feedback/ledger.md](../executor_feedback/ledger.md)。

## 4. 工作项

- [x] `MNT-202610-01`（#72 / MIRA-20261004-002）OpenSSL TLS adapter 在 connect 前为 DNS
  主机名设置 SNI，IP 字面量跳过；验证与信任链语义不变。验收：ClientHello 携带
  server_name 扩展（DNS 主机名），IP 字面量不携带；证书验证失败仍 fail closed。
- [x] `MNT-202610-02`（#74 / MIRA-20260927-001）`resolve_workflow_version` 同 digest 取
  最新记录。验收：save（NotValidated）→ publish（DryRunPassed，同 digest）后按该 digest
  解析为可运行记录；不同 digest 的旧版本解析不受影响；未知 digest 仍 NotFound。
- [x] `MNT-202610-03`（#73 / MIRA-20261004-001）规范工具回填契约：`ToolCallPart` /
  `ToolResultPart` + 构造 helper + 校验/往返/digest + 两 dialect 线格式 + `parse_decision`
  Text 模式工具提案语义（DEC-047）。验收：往返与失败路径测试；混排 fail closed；Text 模式
  工具调用解析为 ToolProposals，非 Text 模式混排仍 Ambiguous。
- [x] `MNT-202610-04`（#73 / MIRA-20261004-001）公开 `ConversationLoop`：无观察对话循环，
  文本回答终态；预算/取消/准入/工具预算沿用既有契约；事件面 `ToolExecuted` /
  `ConversationSettled`。验收：无 screen provider 环境完成问答与工具往返；非法提案、
  拒绝、取消、预算耗尽、空回答恢复路径全部有测试且无环境副作用。
- [x] `MNT-202610-05`（#75 / MIRA-20260922-001）登记 `EXE-20260922-001` 台账条目并转交
  上游流程；引用 mira#75；不改 `third_party/executor`、不改 pin。

## 5. 风险与阻塞

- #75 属上游 executor 仓库职责，Mira 侧无法闭环；跟踪责任在台账（Open），executor 侧
  issue 待提出。M4-06 评估时以该条目状态为依据。
- SNI 行为的端到端（真实 SNI 路由端点握手）证据属 Mirage 侧 pin 升级后补验；Mira 侧
  验收以 ClientHello 解析为准（无网络依赖）。
- `parse_decision` Text 模式语义是行为变更，依赖全量回归确认无既有消费者受损。

## 6. 测试与退出条件

- [x] `MNT-202610-01`：TLS 测试覆盖 DNS 主机名 SNI 存在、IP 字面量 SNI 缺席、
  hostname verification 仍生效。
- [x] `MNT-202610-02`：版本化测试覆盖同 digest draft→publish 解析、不同 digest 旧版本
  解析、未知 digest 拒绝。
- [x] `MNT-202610-03`：契约测试覆盖 part 校验（含 digest 失配、混排、超限载荷）、JSON
  往返、两 dialect 线格式、`parse_decision` Text/非 Text 模式行为。
- [x] `MNT-202610-04`：循环测试覆盖文本终态、工具往返、无注册表 fail closed、预算
  耗尽、取消、空回答恢复、拒绝路径。
- [x] 全量 ctest 通过（Linux x86_64 GCC Debug）与 `architecture-check`、`docs-check`
  通过。

## 7. 验证记录

2026-10-05：实现轮（工作树 = master `472e430` + 本轮变更，未提交）。环境：Ubuntu 24.04
x86_64，GCC 13.3.0，CMake debug 预设。

- 构建：`cmake --preset debug && cmake --build --preset debug -j 4` 全目标通过，0 警告
  0 错误（含新增 `src/model/conversation_loop.cpp`、`src/model/model_tool_wire.cpp` 与
  `adapters/net/openssl_tls.cpp`、`src/workflow/workflow_versioning.cpp`、
  `src/model/model_gateway.cpp` 修改）。
- 测试（Independent-Verification-Agent 两轮执行）：`ctest --test-dir build/debug
  --output-on-failure` 全量 **103/103 通过**。新增/扩展：
  - `mira_m3_conversation_loop_test`（新，17 场景）：文本一次定答；默认 config × 默认
    profile 限额（16384）可路由回归 pin；工具往返以规范 ToolCallPart/ToolResultPart
    进入第二请求（角色/digest/参数逐项断言）；无注册表 fail closed（含 "tool registry"
    诊断且仅 1 次模型调用）；工具预算耗尽；Refused；预取消与 admission 拒绝（零模型
    请求）；空回答恢复（feedback 进入下一请求）与预算耗尽；文本型/工具型 MaxTurns；空
    goal；可恢复 infer 错误两分支；`parse_decision` Text/混排/非 Text 三态；契约构造与
    5 条校验拒绝路径；JSON 往返；双 dialect 线格式（含混排与能力位 fail closed）。
  - `mira_m8_versioning_test`：同 digest draft(NotValidated)→publish(DryRunPassed) 解析
    取最新且 runnable；仅 draft 存在时仍 NotValidated；不同 digest 各自解析不变。
  - `mira_m3_tls_test`（仅 OpenSSL 变体，SNI 探针按 winsock 可移植宏编写）：DNS 主机名
    ClientHello 携带 server_name 且解码值精确等于主机名；IPv4/IPv6 字面量无该扩展。
    mbedtls 变体（`mira_m3_mbedtls_test`）回归通过（其 `mbedtls_ssl_set_hostname` 原生
    含 SNI，本轮未改）。
- 检查：`architecture-check`（modules 8，基线抑制 17，stale 0，clean——新增文件均在
  预算内：`model_tool_wire.cpp` 100 行、`conversation_loop.cpp` <360 行、
  `model_dialect.cpp` 1156/1200）与 `docs-check` 通过。
- 缺陷迭代记录（保留）：首轮验证发现 3 个实现缺陷并由主循环修复后复验通过——
  F1 `max_request_sensitivity` 对新 part 的空指针解引用（工具往返必崩，改为逐类型分支，
  工具 part 贡献 `Internal` 敏感度）；F2 默认预算组合超出默认 profile 限额导致路由
  拒绝（默认改为 12 轮 × 1024 token，信封 15360 ≤ 16384，头注释写明约束）；F3 无注册表
  时工具提案走不到显式分支（网关把未暴露工具的调用降级为 Ambiguous，循环新增
  fail-closed 分支给出 "tool registry" 诊断，不再空烧恢复预算）。
- 限制与补跑：SNI 对真实 SNI 路由端点（如 MiniMax）的端到端互操作与 MinGW-w64 复现
  未在本机执行（原因：无外部端点凭据与 MinGW 工具链；负责人：Mira/Mirage 维护者；
  补跑条件：Mirage pin 升级后按 mira#72/#75 复验）。Windows 平台编译路径未在本机执行
  （m3_tls_test.cpp 既有主体为 POSIX 风格；补跑条件：PR CI windows matrix）。CI 证据
  待提交 PR 后回填。
