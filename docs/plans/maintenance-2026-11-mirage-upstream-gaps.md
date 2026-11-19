# Mirage 上游缺口复核与修复

> 状态：In Progress（本轮本地验收通过，进入 PR 评审）
> 负责人：Mira/Mirage Maintainers
> 所属计划：[Mira 实施总计划](mira-implementation-plan.md)
> 基线：Mira master `39db9bc`，与 Mirage 当前 Mira gitlink 和锁文件一致
> 契约依据：[DEC-118](../decisions/DEC-118-mirage-host-contract-followups.md)
> 更新日期：2026-11-19

本轮从主线核对 Mirage 依赖台账。兼容 API 保持正常调用行为，新增或改变的
可观察行为经各自契约测试验证。已交付能力与消费侧工作分别核对，不以源码修订
替代跨平台或端到端验收。

| 工作项 | Mirage 反馈 | 依据与退出条件 |
| --- | --- | --- |
| MNT-MRG-01 | MIRA-20261008-002、MIRA-20261013-001 | 核心时间 wire 表达不变，通过 GCC 严格头编译；错误一侧访问确定抛出并覆盖 void |
| MNT-MRG-02 | MIRA-20261012-002 | Model Gateway 按单探针冷却恢复，覆盖并发准入、失败回开、取消和 fallback |
| MNT-MRG-03 | MIRA-20261119-001 | M36/DEC-101/107/108。提供历史插入和终态工作释放，保持角色、工具群组及删除屏障 |
| MNT-MRG-04 | MIRA-20261112-002/003 | M14/DEC-031/117。恢复异常收口，固定逐次回执、排空和旧等待兼容 |
| MNT-MRG-05 | MIRA-20260922-001 | 复跑已锁 kairo 的 MinGW POSIX 构建，记录剩余阻塞及归属 |
| MNT-MRG-06 | MIRA-20261016-004 | 核对图像编码的宿主能力与可复用实现，验证 PNG artifact 兼容 |
| MNT-MRG-07 | MIRA-20261111-001 | DEC-038。由有界任务事件投影可配对、可验证的 Trajectory，明确证据缺失 |
| MNT-MRG-08 | MIRA-20261112-001、MIRA-20261027-001 | DEC-069、M29–M31。分别验证工件、事件与元数据准入和恢复的已交付范围 |
| MNT-MRG-09 | MIRA-20261016-001、MIRA-20261016-003 | 判断上游交付和产品采用触发条件，避免重复实现检索和调度 |

## 验证记录

2026-11-19：Mira `refs/heads/master`、Mirage 工作树 gitlink 和依赖锁均核对为
`39db9bc63adb27282c36a6a6cc1b25f476632926`。M14 异步任务继续通过 pinned Kairo
有限任务管理，宿主仍持有 Executor，接受的任务和异常由 future 结算。

Linux x86-64 GCC 13.3 Release 构建开启系统 libpng 1.6.43，CTest 195/195；
格式、文档、依赖锁和 SBOM、平台边界、架构门禁通过。改动实现的九个翻译单元
使用 clang-tidy 18.1.8 检查通过。独立 CMake 消费项目从安装前缀导入
`Mira::png_adapter` 并运行 1×1 RGBA 编码。跨平台 CI 由 PR 跟踪。

| 工作项 | 定向验证 |
| --- | --- |
| MNT-MRG-01 | GCC 消费头以 `-Wuseless-cast -Werror` 编译；`mira_contract_test` |
| MNT-MRG-02 | `mira_m3_supervisor_budget_test` 的探针独占、重开、代与取消 |
| MNT-MRG-03 | `mira_m36_context_admission_test` 的三方言、完整原入参与预算降级；`mira_m36_memory_agent_lifecycle_test` 的微配额、在途释放拒绝与擦除 |
| MNT-MRG-04 | `mira_m14_recovery_test` 的在途超时、逐次回执、异常、次数拒绝与关停 |
| MNT-MRG-05 | 当前 pin 的 Kairo `10e870bcb94b` 随 MinGW-w64 GCC 13 POSIX 构建 `kairo` 目标成功；原问题 [#75](https://github.com/Linductor-alkaid/mira/issues/75) 已关闭 |
| MNT-MRG-06 | `mira_png_adapter_test` 解码核对 Alpha/颜色、stride 和拒绝 |
| MNT-MRG-07 | `mira_m11_compile_test` 核对来源关联、参数变更拒绝、缺终态、截断和取消 |
| MNT-MRG-08 | `mira_m1_core_test` 核对严格微配额、重开、幂等、墓碑、未入账对象和 EventStore 写前拒绝 |
| MNT-MRG-09 | 核对 [DEC-111](../decisions/DEC-111-memory-embedding-producer-and-vector-identity.md)、[DEC-113](../decisions/DEC-113-hybrid-retrieval-default-wiring.md) 和 [DEC-110](../decisions/DEC-110-kairo-pin-v0.7.4-release.md) |

## 后续验收

- Mira 维护者按 [M31](m31-capacity-admission-and-archival-recovery.md) 推进共享引用分步
  删除、归档回收与跨存储预留。需先冻结主体份额、保护集合与部分操作结算协议，再补
  故障注入、迟到写入和断电边界验证。
- Mirage 维护者在合并 pin 后补跑产品的三方言会话、长期记忆生命周期、连续 Workflow
  恢复、真实历史轨迹，以及 Windows 包安装和 PNG 依赖验证。M28/M29 已交付的
  InteractionCoordinator 由 Mirage 对照会话路由及客户端订阅逐步接入。模型、检索
  质量及自己的桌面验证归产品验收。
- 上游 QoS 标注与 AdaptiveScheduler 保持 DEC-054 延期策略。根据生产调度压力/运行
  指标评估后再采用。MinGW POSIX 验证范围为 pinned Kairo，完整 Mirage Windows 产品和
  Android 设备运行仍按各自目标环境补跑。
