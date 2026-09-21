# Mira 架构治理

> 状态：Active
> 版本：1.0
> 更新日期：2026-09-21
> 适用范围：模块依赖方向、include 卫生、文件规模预算、公共契约交付标准
> 上位决策：[DEC-043](../decisions/DEC-043-architecture-policy-and-baseline.md)

## 1. 目的与效力

把根 [`AGENTS.md`](../../AGENTS.md) 中的分层与平台边界约束变成机器可检查的
门禁：依赖方向写反、跨模组私有头引用、未登记的第三方头和失控的文件规模在提交前
失败，而不是在评审中被人工发现。

规则的可执行单一来源是 [`tools/architecture-policy.json`](../../tools/architecture-policy.json)。
本文解释字段含义、规则目录和基线流程，**不复制规则取值**；修改约束时只改
policy 文件并同步本文的规则目录描述，禁止在 AGENTS.md、技能或计划文档中
复制具体规则，避免多处漂移。

## 2. 策略文件结构

`tools/architecture-policy.json` 包含四类字段：

| 字段 | 含义 |
| --- | --- |
| `modules` | 受治理模块：`id`、`roots`（源码根，最长前缀决定文件归属）、`requires`（允许依赖的目标模块或外部模块） |
| `external_modules` | 外部头文件族到外部模块名的映射（如 `executor/` → `executor`），消费方必须在 `requires` 中声明 |
| `global` | 全局预算：`max_file_lines`、`forbid_cycles` |
| `exceptions` | 带理由与可选 `expires`（ISO 日期）的例外；过期例外使检查失败 |

当前模块地图与依赖方向：

```
examples ──> core <── workflow          tests ──> 全部模块（验证面）
             ^  ^                        ^
             │  └── state_store          │
             └──── simulator_adapter     │
.android_adapter ─┘ .net_transport ──────┘
```

约束要点（由检查器强制）：

- Core（`include/mira` 顶层与 `src`）不得依赖任何 Adapter、Workflow、存储实现
  或平台头；平台头例外已由 `tools/check_platform_boundary.py` 单独把关。
- Adapter 与 Workflow 只能依赖 Core 的公共头（`include/mira/...`），不得用
  相对路径伸进其他模块的私有源码。
- 新增跨模块依赖前先在 policy 中登记 `requires`，再写 include。

## 3. 规则目录

| 规则 | 含义 | 典型修复 |
| --- | --- | --- |
| `module-dependency` | include 解析到的目标模块不在来源模块 `requires` 中 | 登记 `requires` 或移除该依赖 |
| `deep-import` | 引号 include 落进其他模块的私有源码 | 改为 `include/mira` 公共头 |
| `unregistered-include` | 尖括号 include 既非标准头也未在 `external_modules` 登记 | 登记 external 模块并声明 `requires`，或移除 |
| `cycle` | 模块级依赖图出现环 | 拆分所有者，或经 port 反转依赖 |
| `max-file-lines` | 文件超过 `max_file_lines` 预算 | 按职责拆分；例外需评审决策 |
| `expired-exception` | `exceptions` 中的例外已过 `expires` | 解决违规并删除过期例外 |

平台 SDK 头与预处理分支（JNI、`windows.h`、`__ANDROID__` 等）不在本检查器
范围内，由既有 `platform-boundary-check` 目标覆盖；两者互补，规则不重复。

## 4. 基线机制

存量违规记录在 [`tools/architecture-baseline.json`](../../tools/architecture-baseline.json)，
只用于历史债务渐进治理：

1. **新违规一律阻塞**；与基线的差异以违规键（`规则:文件:详情`，不含行号）计算。
2. 基线只允许人工在评审后刷新：`python3 tools/check_architecture.py --update-baseline`。
   CI 与 CMake 目标永不自动刷新；刷新前先清理 policy 中已过期的例外，
   否则被例外抑制的违规不会进入基线。
3. 报告中的 stale 条目（基线里有、当前已消失）应在下次触碰该文件时删除。
4. 收紧预算（如下调 `max_file_lines`）属于改变约束，须按第 6 节同步决策记录。

## 5. 运行方式

```bash
# 本地直接运行
python3 tools/check_architecture.py

# 经 CMake（与 format-check/docs-check 同族）
cmake --build build --target architecture-check
```

CI 的 `quality` job 在 `Format, docs, supply chain` 步骤中与其他检查一同执行。
退出码：`0` 通过；`1` 存在新违规或过期例外；`2` policy 文件非法。

已知限制（静态扫描的设计取舍）：检查器不解析预处理分支与编译器 include 路径；
无法在仓库内解析的引号 include 被静默跳过，因此经 `-I` 搜索目录绕过模块根的
深引用不受 `deep-import` 治理。引入此类 include 布局前先在评审中论证。

## 6. 与决策流程的关系

下列变化必须先建立或更新 `docs/decisions/` 记录，再改 policy：

- 新增或删除受治理模块、改变任何 `requires` 依赖方向。
- 调整 `max_file_lines` 预算或新增全局规则。
- 接受新的 `external_modules` 头文件族。
- 基线刷新中包含"放宽"方向的语义变化。

基线的纯机械刷新（跟随重命名、删除已修复条目）不需要决策记录，但提交说明
必须列出差异。

## 7. 公共契约四件套交付标准

面向 `include/mira` 新增或重大扩展公共契约的模块，交付时必须同时提供四件：

| 件 | 载体 | 要求 |
| --- | --- | --- |
| 契约头 | `include/mira/<契约>.hpp` | 只依赖 Core 公共类型；错误经 `Result<T>`；强类型 ID；不暴露平台/第三方类型 |
| 用法示例 | `examples/` 或 API 手册内嵌片段 | 可编译的最小消费路径；展示错误与取消处理，不展示裸成功路径 |
| 不变量文档 | [`docs/api/`](../api/index.md) 对应页面 | 记录类型表达不了的语义：状态转换表、幂等边界、失败语义、线程与 Executor 路由约束 |
| 契约测试 | `tests/contract/` | 冻结公共语义：状态转换合法性、终态幂等、fail-closed 校验，不测实现细节 |

四件不全的契约不得宣告完成（对应根 `AGENTS.md` 完成定义）。现有参照实现：
`environment.hpp`（契约头）+
[`environment-observation.md`](../api/environment-observation.md)（不变量文档）+
[`environment_contract_test.cpp`](../../tests/contract/environment_contract_test.cpp)
（契约测试）+ `examples/minimal_consumer.cpp`（用法示例）。

新模块进入 policy 时按第 6 节登记，并沿用本节标准交付首个契约。
