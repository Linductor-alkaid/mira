# DEC-088：存储实际耐久等级与外部强杀验证

> 状态：Accepted
> 日期：2026-11-06
> 负责人：Mira Maintainers
> 工作项：MNT-202611-02 / MNT-202611-03
> 关联：DEC-003、DEC-074、DEC-087

## 背景

FileEventStore 的流刷新可将数据送达操作系统，不能证明断电安全；既有实现却将
request.required 原样作为 achieved，重复请求也如此。MemoryEventStore 同样错误
回显请求等级。Journal 的强确认检查无法识别这些虚报。现有九窗口 `_Exit` 测试
跳过析构，但没有外部进程终止证据。

## 决定

- MemoryEventStore append（含重复）只报告 Buffered；flush 只接受 Buffered。
  更强 flush 返回 UnsupportedCapability，非法枚举返回 InvalidArgument。
  append 请求更强等级仍可存下事件并返回较弱回执，调用方必须校验 achieved。
- FileEventStore 每次新 append 完成流刷新和关闭错误检查，实际报告 ProcessCrash，
  即使请求 Buffered 也不降低已经完成的等级。重复 append 返回既有事件的
  ProcessCrash 确认。PowerLoss 请求和 flush 在改动文件前明确返回
  UnsupportedCapability；非法枚举返回 InvalidArgument。flush 与 append/recover
  使用同一锁和只读门禁，不能在只读诊断后创建/修改文件。
- 本轮不新增 fsync/FlushFileBuffers 或平台头依赖，不把同步能力渗入 Core。
  若后续交付 PowerLoss，必须另行冻结文件/目录同步、创建/截断恢复、设备写回和
  故障语义，再经 Platform Adapter 边界实现并验证。实际掉电测试继续开放。
- 需要模拟强确认的单元测试使用显式 test-only 回执替身；裸内存 store 接入
  Dispatcher/VerifiedFactPublisher 必须拒绝，不执行输入或发布可信事实。
  示例中的真实 journal 使用 FileEventStore，不提供生产绕过开关。
- 外部强杀测试由测试 driver 启动 writer；writer 在指定窗口通过独立 marker 文件握手并
  阻塞等待，driver 收到后用操作系统进程终止能力杀死并 wait，再由独立 verifier
  重开日志。禁止靠时间猜窗口。覆盖原九窗口与重复恢复，不将进程强杀等同断电。
  测试进程编排由 CTest/Python subprocess 管理；应用不新增异步任务、线程或
  Executor owner，进程握手不承载 Runtime 调度。

## 验证与限制

验证等级矩阵、非法值、重复请求、拒绝零写入、只读 flush、Memory journal 零输入、
原 journal/事实/宿主回归，九窗口强杀以及 TSAN。Windows 的相同 driver 使用
subprocess.kill 对应的系统终止机制；Linux 运行证据不代替 Windows 实测。
Android 真机输入释放/Takeover、真实掉电、可写工具及连续控制接线保持原计划范围。

关联：[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)、
[文件格式](../compatibility/file-event-log-v2.md)、
[事件设计](../design/event_artifact_crash_consistency.md)。
