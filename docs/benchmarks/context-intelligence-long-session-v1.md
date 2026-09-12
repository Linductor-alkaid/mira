# Context Intelligence long-session 基线 v1（Stage A 首轮）

> 状态：Active（首轮基线，登记于 2026-09-13）
> 负责人：Mira Maintainers
> 适用范围：[DEC-032](../decisions/DEC-032-context-intelligence-layered-context.md)
> Stage A（[M16](../plans/m16-context-intelligence-stage-a.md)）；Layer 0 确定性
> Reduce 的 long-session 行为基线，是 Stage B–F 的实验矩阵 A 列参考点
> 复现命令见文末

## 1. 目的与范围

这是 [M16](../plans/m16-context-intelligence-stage-a.md) 产出的 long-session 首轮
基线：100/500/1000 轮合成会话驱动现有 `StandardContextManager`（不引入任何模型或
新契约），全量历史每请求重 presented（宿主呈现纪律的最坏情形），验证并量化
「Model Request token 趋于有界而非线性增长」，同时固化选择/丢弃审计基线。本报告
只声明合成负载与保守 token 估计口径下的结构性结论，不外推为绝对 token 数、真实
Provider 计数或真机行为（miracle 派生负载回放归 `MNT-202609-27` 证据通道）。

## 2. 方法与环境

- Harness：`tests/m16/m16_long_session_baseline.cpp`（M16-02/03），单线程确定性
  纯函数驱动 `StandardContextManager::prepare`（`ConservativeTokenCounter` 默认
  配置，无 exact counter、无 usage 校准——所有 token 为保守上界）。无模型、无
  线程、无时钟；固定 splitmix64 种子与计数器派生 `Id128`。
- 冻结 profile（[M16 §4](../plans/m16-context-intelligence-stage-a.md)）：
  window 48,000 / reserved 8,192 / safety 1,024 → `input_budget` 38,784 tokens；
  trim 0.70 / checkpoint 0.85 / hard 0.95；内容七类混合（对话、截图+UI 树、工具
  对、Workflow 事件、恢复事件、用户纠正、检索记忆 + 动作/检查点）；测量采样
  t ≤ 100 每轮、之后每 10 轮；warm-up 32 轮。
- `dataset_digest`：
  `b8b082e7e26d2206505a6e54fafe495a551b4f49145d1ca2c2948f33a77c64a5`
  （冻结配置 canonical JSON 的 SHA-256；本轮基线锚点）。
- 环境：Ubuntu 24.04.4 x86_64、Intel Core Ultra 5 225H（14 线程）、GCC 13.3.0、
  CMake 3.28.3、CMake `debug` 预设（无优化）；全套件单轮运行 ~11 s。
- 门禁：G1 请求闭合 / G2 稳态有界带 [19,392, 32,966] / G3 有界与 N 无关 /
  G4 约束保持 / G5 确定性 / G6 审计完备——全部通过；同进程重复与跨进程重复
  报告字节级一致。

## 3. 基线结果（摘要）

**Token 趋势**（稳态段 = t > 32 的采样；`estimated_tokens` 为选择集保守上界）：

| N | 采样数 | 稳态 min / p50 / p95 / max | 稳态 utilization | 终末原始 presented |
| --- | --- | --- | --- | --- |
| 100 | 100 | 24,398 / 25,848 / 27,119 / 27,147 | 0.629–0.699 | 286,579 |
| 500 | 140 | 24,398 / 26,212 / 27,139 / 27,147 | 0.629–0.699 | 1,453,697 |
| 1000 | 190 | 24,398 / 27,053 / 27,145 / 27,147 | 0.629–0.699 | 2,917,016 |

- 原始历史线性增长（N=1000 为 N=100 的 10.2 倍，~2.9M tokens），选择 token 三个
  N 共用同一平台上界 27,147 < 32,966（checkpoint 水位界）；G2/G3 通过。
- 稳态落点 0.63–0.70（trim 水位 0.70 之下、checkpoint 推荐 0.85 从未触发）：trim
  阶段的引用替换足以闭合请求，checkpoint/硬水位动作在稳态空转。
- `checkpoint_recommended` 触发率 0/所有采样。

**终末请求 disposition 混合（N=1000，5,549 项 presented）**：

| kind | selected | 按引用 | 压缩 | dropped | 选择 tokens |
| --- | --- | --- | --- | --- | --- |
| system-policy / goal / task-limits | 各 1 | 0 | 0 | 0 | 124 |
| user-constraint | 66 | 0 | 0 | 0 | 3,665 |
| current-observation | 1 | 0 | 0 | 0 | 1,974 |
| uncertain-side-effect | 1 | 0 | 0 | 0 | 34 |
| checkpoint-summary | 1 | 0 | 0 | 0 | 165 |
| recent-action | 132 | 301 | 0 | 567 | 18,072 |
| recent-error | 6 | 12 | 0 | 22 | 814 |
| historical-payload（对话/旧观察/Workflow/恢复） | 0 | 0 | 0 | 3,187 | 0 |
| tool-call / tool-result（各 600） | 0 | 0 | 0 | 各 600 | 0 |
| retrieved-memory | 0 | 0 | 0 | 50 | 0 |

**最小执行集成本**：3,135（N=100）→ 4,759（N=500）→ 5,797（N=1000）tokens，随
用户纠正累积（6 → 33 → 66 条）线性增长；G4 下 66/66 全保留（reason=
`minimum_set`）。

## 4. 首轮发现

1. **有界性由 Layer 0 单独闭合**：全量重 presented 的最坏情形下，贪心装配 +
   trim 引用替换把每次请求压回 ~0.63–0.70 utilization，界与 N 无关。「Session
   生命周期 != Context 生命周期」在纯确定性管线下已成立——Stage B–F 的收益空间
   不在有界性，而在**连续性**（下条）。
2. **长会话连续性损失被量化**：稳态下选择集被 P3（recent-action/error，按
   newest-first 装配 433 项）与最小执行集占满，**全部对话历史、工具对与检索记忆
   被逐出**（N=1000 终末请求 historical-payload 3,187 项 / 2.70M 原始 tokens
   dropped）。纯优先级装配没有相关性概念——这是 Stage 1 检索（把相关 Cold
   History 重新带入）的直接量化动机，也是 Stage D 固化（约束之外决策/未决线索
   无 P1 保护）的对照起点。
3. **约束保持是精确且线性计费的**：P1 最小执行集语义保证 66/66 用户纠正零丢失，
   代价是最小集 token 线性增长（~55 tokens/条）。Stage D 的语义固化必须在约束
   召回上对照该精确基线，并证明其增量（决策/未决线索召回）物有所值。
4. **水位机器的稳态行为**：trim（0.70）是唯一实际工作的水位动作；checkpoint
   （0.85）推荐从未触发，hard（0.95）从未到达。Context Intelligence 的 compaction
   滞回（设计 §6.1，60–70% 预备 / 80–85% 提交）在该负载形态下不会自然进入提交带
   ——固化的触发不能只依赖 watermark，需结合轮次/事件节奏（Stage D 设计输入）。
5. **工作负载模型注记**：本模型中 P3 动作项不随年龄降格到 P5（呈现纪律声明的
   最坏情形），因此 P5 对话在稳态被 P3 完全挤出。宿主若做 P3→P5 老化，混合会向
   对话倾斜，但 token 有界性结论不变（由水位机制保证，与混合无关）。

## 5. 限制与补跑条件

- **合成负载**：内容七类混合按设计 §11 结构合成；miracle 派生负载回放未执行。
  补跑条件：`MNT-202609-27` 提供脱敏原始会话语料（设备/Provider/任务形态）后，
  以同 harness 的 replay 装载器立项执行（不修改冻结 profile 的合成腿）。
- **保守 token 上界**：`ConservativeTokenCounter`（4 bytes/token 等）系统性高于
  Provider 实计数；趋势与相对结构结论不受影响，绝对数值不可外推（`RULE-10`）。
- 单机单配置（debug 预设）；Windows/Android/sanitizer 复验由 CI 矩阵取证
  （Android 为编译级，无运行要求）。
- 学习增益、约束召回的语义指标不在本基线（无模型参与），归 Stage B+ 各自冻结。

## 6. 复现

```bash
cmake --preset debug
cmake --build --preset debug --target mira_m16_long_session_baseline
./build/debug/tests/mira_m16_long_session_baseline [report.json]
ctest --preset debug -R mira_m16
```

退出码非零即门禁失败；报告 JSON 含全部采样序列统计、终末 disposition 混合、
`dataset_digest` 与门禁明细。
