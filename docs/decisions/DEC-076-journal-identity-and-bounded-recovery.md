# DEC-076：动作日志身份绑定与有界恢复

> 状态：Accepted
> 日期：2026-11-05
> 负责人：Mira Maintainers
> 冻结里程碑：2026-11 运行时接线维护轮（MNT-202611-03）
> 替代/被替代：无；补齐 DEC-003、DEC-074、DEC-075 的通用 journal 边界

## 背景与问题

ActionJournal 的本地缓存仅按 ActionId 命中，未检查完整 intent；恢复只读取一页，
且 checkpoint 与 journal 对同一 pipe 字段使用不同解析规则。身份冲突、整数溢出和
分页中断可能使恢复得到不完整或错误的动作状态。

## 决策

1. 所有 journal 公开操作先校验非 nil 身份、runtime 与 owner 一致、目标字段有效。
   本地 ActionId 绑定完整 runtime/session/task、两个 epoch、摘要与 target，冲突返回
   InvalidArgument，不读写存储、不返回另一 intent 的缓存。恢复中同 ID 的持久化记录
   与 intent 冲突返回 DataLoss，不静默忽略。
2. 保留八字段 pipe 格式；journal 与 checkpoint 共用内部解析器。128 位 ID 必须符合现有 32 位十六进制表示且非 nil，
   digest 为 64 位十六进制，epoch 为完整可表示的无符号十进制整数。
   target 三字段各 1–1024 字节，禁止 `|`、ASCII 控制字符与 DEL；摘要尾部至多
   4096 字节，允许 `|` 作为普通尾部文本，禁止控制字符。总载荷上限 16384 字节。
   符合上述边界的旧载荷不重写；不符合约束的写入返回 InvalidArgument，读回返回 DataLoss，
   reducer 计 malformed，不将其当作已验证事实。尾部不会再被截断在第一个 `|`。
   合法 ActionPrepared 由 reducer 记录身份/epoch 元数据，不能误记为 malformed，
   也不能据此设置 verification_pending 或声称输入已发生。
3. 新增 ActionJournal::RecoveryPolicy，原双参数构造保留；默认 page_size=1024、
   max_events_scanned=1000000。计入扫描的所有会话事件（包括其他任务），逐页严格
   递增 session sequence，拒绝无进展/越界/跨会话页。耗尽上限返回 ResourceExhausted，
   不把部分扫描当作完整恢复。空页且 has_more 为 true 返回 DataLoss。
   RecoveryPlanner 同样按所有会话事件计数并校验分页进展，修复只累计匹配任务、
   且末页未检查上限的漏洞；不改变其 checkpoint/终态裁决顺序。
4. 仅四种 journal 事件影响恢复；其他同前缀事件不影响状态。支持旧的 start-first
   记录，但 receipt/uncertain 无 start、dispatch 后又出现 prepare 属 DataLoss；
   重复 start 不使 Receipt/Uncertain 降级。recover 为只读查询，不重新授权动作，
   不把重启后的 prepare 当作跨进程幂等重发 API。
5. append 回执须绑定本次 event_id，session/task sequence 必须有效，且满足 DEC-074
   的 achieved 门槛。存储 append/read 抛出的异常转为固定摘要的 Internal 错误，
   不能回显异常原文或绕过派发器的输入收敛路径。

## 备选方案

- 遇到身份/分页冲突直接跳过：会形成成功但不完整的恢复结果，不采用。
- 新建另一种 wire schema：本轮不需要；通过严格解析与兼容尾部规则保留既有格式。
- 在每次新动作 prepare 前扫描整个会话：开销随历史增长，本轮不采用；新 Dispatcher
  使用新 ActionId，重启必须先走恢复决策，不能靠重发 prepare 恢复动作。

## 影响与风险

以前接受的空目标、分隔符目标、控制字符、溢出 epoch 和冲突 ID 现在明确拒绝；
这是错误输入语义的收紧。合法旧八字段和 JSON reducer 词汇保持兼容。
历史超长/不合规载荷进入受限诊断，需宿主核查和显式迁移，不自动截断、清空或改写日志。
原双参数构造保持源码兼容；C++ 宿主需重新编译，沿用 DEC-002 不承诺稳定 C++ ABI 的边界。
恢复超限需要宿主明确提高扫描预算或先归档，不静默截断。存储仍负责完整性和
真实耐久性；此校验不能证明一个伪造回执的实现可靠。无新增并发或生命周期设施。

## 验证方式

覆盖完整 intent 的缓存绑定冲突、nil/分隔符/控制字符/长度边界、uint64 极值和溢出、
多页混合任务与无进展页、扫描上限、同 ID 持久化冲突、未知事件前缀、坏回执及异常。
checkpoint 对同一非法 pipe 拒绝投影。独立子进程在 prepare/start/平台执行/receipt
边界直接退出，由第二进程恢复，验证零自动重发与终态幂等。
突然退出不等价于断电；平台与 sanitizer 结果按实际执行记录，不据此宣称跨平台保证。

## 关联文档和工作项

- [维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)：02/03。
- [输入派发设计](../design/journaled_input_dispatch_design.md)。
- [核心 API](../api/core-runtime.md)。
