# DEC-118 Mirage 宿主契约补齐

> 状态：Accepted
> 日期：2026-11-19
> 负责人：Mira Maintainers
> 工作项：[Mirage 上游缺口复核](../plans/maintenance-2026-11-mirage-upstream-gaps.md)

- `Result<T>` 按活动分支访问。错误分支误用按 `std::bad_optional_access` 确定失败。
  时间 wire 类型继续为有符号 64 位纳秒。
- ProviderCircuit 以单调时钟在请求入场时判定冷却。半开探针只有一个在途，成功达到
  所设阈值后关闭，失败重开，取消释放槽。Gateway 按代区分迟到回执，无后台计时器。
- 召回历史可在请求的宿主指令前缀之后、当前 User 之前装配。仅显式保留原生 wire
  role 的候选可进入该位置，完整 Layer 0 审计与工具群组顺序继续生效。
  `release_work` 释放已终结且作业已停的绑定，宿主保留不复用 Task ID 的职责。
- 异步恢复使用调用方持有的 receipt 区分每次结算。审计 EventId 可直接关联。
  排空与异常不扩大桌面权限。原始异常正文不作为安全错误返回。
- FileArtifactStore 的可选目录预算覆盖其独占目录中普通文件逻辑长度，包括临时文件、
  manifest 和墓碑。探测接口按计划 descriptor 报告归档是否进入索引。
  宿主明确重试时，以同一 writer 校验已发布对象后补写账本。
  FileEventStore 同时检查索引条目和可选日志字节上限，耗尽后保留读取与幂等重放。
  跨存储保护、物理块回收和自动孤儿清除继续由 M31 管理。
- 已授权的 ConversationLoop 事件可以与宿主验证证据配对形成编译草稿。
  调用方提供能对上已执行参数 digest 的获准参数，缺失证据拒绝。编译和发布仍走现有门禁。
- PNG 是宿主适配职责。可选 libpng Adapter 提供有界 RGBA8 编码，Core 与默认构建
  不链接 PNG，安装后的启用配置依赖目标环境提供 libpng。保留 Alpha，调用方选择脱敏和
  颜色空间。无默认下载、捆绑第三方源码或镜像变更。

采用可选、兼容的宿主接口。测试分别核对首次和后续 Tool 请求装配、在途行为、文件容量、
选择稳定性及脚本故障。执行记录见工作项。
