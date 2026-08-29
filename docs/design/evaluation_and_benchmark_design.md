# Mira 评估与基准体系设计

> 状态：Active  
> 版本：1.0  
> 更新日期：2026-08-30  
> 适用范围：Runtime 正确性、Agent 成功率、安全、成本、Memory、本地模型、实时控制和平台兼容性

## 1. 目的

Mira 的评估对象是完整闭环，不只是模型回答质量。本文定义可复现的场景、指标、数据切分、运行
manifest、基线、回归和发布门禁，使“更快、更省、更安全、跨平台”等声明有具体证据。

本文不在架构阶段写死所有数值阈值。每个里程碑和产品 profile 在目标平台数据上冻结 threshold，
并保留 baseline、置信区间和失败样本。

## 2. 评估层级

| 层级 | 评估对象 | 关键问题 |
| --- | --- | --- |
| L0 Contract | 类型、schema、Provider/Adapter | 是否遵守接口、取消、错误和 capability |
| L1 Component | Planner、OCR、detector、Memory、Controller | 独立正确性、延迟、资源和鲁棒性 |
| L2 Loop | Simulator 中 Observe→Verify | 状态机、恢复和副作用是否正确 |
| L3 Task | 任务场景/应用版本 | 目标是否完成、用了多少步骤/成本 |
| L4 Platform | Android/Linux/Windows/设备 | 真实权限、生命周期、输入和性能 |
| L5 Soak/Safety | 长期、故障、对抗、回放 | 是否泄漏、重复副作用、无法关闭或漂移 |

低层通过不能代替高层。例如 detector mAP 通过不证明任务成功；Simulator 成功不证明 Android 输入
可用。

## 3. EvalCase 与 RunManifest

```cpp
struct EvalCase {
    EvalCaseId id;
    SemanticVersion version;
    TaskSpec task;
    EnvironmentFixture environment;
    ExpectedOutcomeOracle oracle;
    SafetyExpectations safety;
    BudgetLimits budgets;
    Tags tags;
};

struct EvalRunManifest {
    EvalRunId id;
    GitRevision revision;
    BuildIdentity build;
    PlatformIdentity platform;
    ExecutorConfigDigest executor;
    RuntimeConfigDigest runtime;
    ProviderProfileDigests providers;
    ModelPackageDigests models;
    DatasetDigest dataset;
    SeedSet seeds;
    TimeRange time;
};
```

case、fixture、oracle 和数据版本 immutable。远端模型不能完全确定时，记录 provider/model、request
digest、temperature/seed（若支持）、日期和 response fixture；发布回归同时包含 recorded deterministic
模式与受控 live sample。

## 4. 场景集

### 4.1 基础闭环

- 纯视觉与 Screenshot+UI Tree 完成同一目标。
- 多步导航、输入、滚动、Tool 和完成验证。
- 无目标、目标已经完成、模型请求 Human、明确失败。

### 4.2 恢复与生命周期

- model/observe/tool timeout、schema error、429/5xx、queue full。
- Action 明确未执行与 `ExecutionUncertain`。
- pause/resume、cancel、Takeover、deadline 和 shutdown 在每个状态注入。
- process crash 在 Event/Artifact/side-effect 每个边界。
- permission revoke、rotation、host destroy 和 late completion。

### 4.3 安全

- 页面/OCR/Tool/Memory prompt injection。
- confirmation replay/substitution、跨 scope、SSRF、路径穿越、secret/log leak。
- malicious Event/Replay、model package/signature 和 Tool crash。

### 4.4 长上下文与 Memory

- 长 Task 多次 checkpoint、context overflow、Provider compaction loss。
- 多 Session 时间推理、事实更新/失效、abstention 和 Memory poisoning。
- erasure 后检索、context、Replay 和训练 export。

### 4.5 本地感知与控制

- 主题/语言/分辨率/应用版本/OOD/遮挡。
- local-only、VLM-only、hybrid 和 fallback。
- joystick/drag feedback loss、deadline miss、target overwrite、input release。

## 5. 指标

### 5.1 Task 与效率

- `TaskSuccessRate`：由确定性 oracle/独立 verifier 判定，不采用模型自报。
- `VerifiedCompletionRate`、`FalseCompletionRate`。
- steps/actions/model calls/tool calls、wall time。
- input/output/image tokens、estimated/actual cost。
- VLM fallback、Human request/intervention、recovery 次数。
- no-progress loop、max-step/deadline exhaustion。

### 5.2 副作用与安全

- duplicate side effects，目标必须为零。
- unsafe proposal/execution、Policy capture、confirmation bypass/replay。
- uncertain effect 数、正确恢复率、错误自动重试数。
- cancel/takeover/shutdown stop-to-release latency 和 unconfirmed release。
- secret/sensitive leakage、cross-scope retrieval、Replay live-call 数，目标均为零。

### 5.3 性能与资源

- observe/model/plan/persist/action/verify 分段 P50/P95/P99/max。
- queue wait、Executor admission rejection、control callback/jitter/deadline miss。
- CPU、RSS、allocation、I/O、network bytes、thermal/energy（可用时）。
- cold/warm model load/inference、Artifact throughput、Event durable latency。

### 5.4 Memory 与本地模型

- retrieval recall/precision、temporal correctness、update/abstention。
- Goal/constraint/uncertain-effect checkpoint fidelity。
- OCR CER/WER、detector mAP/recall、state F1/ECE/OOD、locator clickability。
- Agent success/cost 相对 no-memory、VLM-only 和 previous model baseline 的差值。

平均值不能代替 tail、安全错误和按场景分层结果。

## 6. Oracle 与判定

优先顺序：Simulator ground truth/平台结构化事实 -> 独立确定性 predicate -> 人工盲评 -> 与被评系统
不同的受控 verifier。不能让产生 Action 的同一个模型单独判定自己成功。

Oracle 有版本和适用范围。人工评估记录 rubric、双人一致性和仲裁；VLM judge 记录 bias/calibration，
只用于难以自动化的辅助指标，不替代高风险安全 oracle。

## 7. 数据切分与污染防护

- train/eval/benchmark 按 user/session/task family/application version 分组切分。
- 公共 benchmark case 不进入任务模型训练或 prompt tuning；使用时记录 contamination risk。
- failure corpus 分 active regression 与 sealed holdout。
- live user data 只有授权、脱敏和治理后进入 eval。
- 每次 run 固定 dataset digest，缺失/删除 case 生成新版本并保留变更说明。

## 8. 基线与实验

至少保留：

- previous release。
- VLM-only、local-only、hybrid。
- no-memory 与 configured-memory。
- CPU reference EP 与目标 accelerated EP。
- Simulator deterministic 与目标平台。

A/B 只改变一个声明变量，其他 config/dataset/seed 尽量固定。远端模型漂移时使用 recorded response
隔离 Runtime 回归，并另跑 live canary 衡量供应商变化。

## 9. 统计与报告

- 报告样本量、成功/失败计数、分层和缺失/skip。
- 比例给置信区间；延迟给 percentile/bootstrap interval；配对 case 优先报告 paired delta。
- 多次 seed/重复运行，不只选最佳值。
- 性能 warm-up、计时范围和 outlier policy 预先定义。
- 失败样本保留 Event/Artifact refs（受权限/retention），报告能追到根因类别。
- 环境无法运行的门禁保持未完成，记录 owner 和补跑条件。

## 10. Benchmark Harness

Harness 通过 Mira 公共 API 和 Simulator/真实 Adapter 运行，不直接调用内部函数伪装端到端。它：

- 生成 RunManifest、每 case event log、metrics、result 和 safe diagnostic refs。
- 支持 recorded Provider、fault injection、fake clock 和真实 monotonic mode。
- 通过 Executor 管理并发 case；每 case 有独立 Session/namespace/配额。
- 禁止 benchmark harness 自建线程池或绕过 Runtime shutdown。
- 默认禁止真实副作用；平台 suite 使用隔离测试设备/账号和明确 capability。

结果格式版本化，原始机器可读数据放 `docs/benchmarks/` 所引用的 artifact/CI storage，仓库内报告
包含方法、环境、命令、摘要和限制。

## 11. Fault Injection

可控注入点：Executor reject/timeout/exception、Provider 网络阶段、Store write/fsync、Artifact publish、
Adapter callback、Environment epoch、Tool crash、ONNX deadline、Controller channel/watchdog 和 Host
lifecycle。注入 ID 写入 Event，使预期错误与意外错误可区分。

故障测试验证 invariant，而不仅是“进程没崩”：终态不复活、future/handle 被消费、不重复副作用、
输入释放、Critical Event durability、Replay 无副作用、shutdown report 准确。

## 12. 发布门禁

每个发布 profile 单独冻结阈值。通用硬门禁：

- duplicate external side effect = 0。
- confirmation bypass/replay、cross-tenant read、Replay live effect、secret leak = 0。
- 所有终态/取消/shutdown invariant 通过。
- 新行为的 contract/integration/fault tests 通过。
- 性能/任务指标未超过批准 regression budget；超过需 DEC/风险接受。
- 目标平台必需 case 实际运行，skip 不算通过。
- 模型/依赖/数据的 digest、许可、SBOM、评估和回滚信息完整。

## 13. CI 分层

- 每提交：unit、schema/golden、deterministic Simulator、文档链接。
- 每 PR/合并：integration、fault subset、ASAN/UBSAN、recorded agent eval。
- 定期/夜间：TSAN、stress/soak、full replay corpus、本地模型与 performance smoke。
- 候选发布：目标 Android/平台矩阵、live Provider canary、安全 suite、完整 benchmark、供应链。

CI runner 不支持真机、网络、sanitizer 或权限时，job 明确 skip/fail 原因，并由对应目标环境 gate
补跑；不能在通用 job 中把环境缺失转 green release evidence。

## 14. 回归归因

失败按 Runtime/Provider/Model/Adapter/Tool/Environment/Test/Infrastructure 分类并保留证据。远端模型
变化不能自动归因给 Runtime，也不能因此忽略任务失败。重复出现的 failure 转稳定 regression case，
涉及用户数据时先治理/最小化。

## 15. 关联文档

- [项目管理与文档规范](../project/project_management_and_documentation.md)
- [核心公共契约与状态机](core_contracts_and_state_machine.md)
- [本地感知与任务模型](local_perception_and_task_models.md)
- [实时控制层](realtime_control_design.md)
- [Context 与 Memory](context_and_memory_design.md)

