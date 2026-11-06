# FileEventStore v2：持久化时间与旧日志兼容

> 状态：Active
> 版本：1.1
> 更新日期：2026-11-06
> 负责人：Mira Maintainers
> 依据：[DEC-087](../decisions/DEC-087-file-event-wall-time-persistence.md)、BUG-20261106-001

## 时间契约与 C++ 消费者

新 append 只采集一次 Timestamp，wall 作为有符号纳秒整数写入日志，同一次采集值
进入内存投影；重开不会重新赋当前时间。EventEnvelope 尾部增加 timestamp_origin：

| 来源 | wall | monotonic |
| --- | --- | --- |
| AppendProcess | 本次进程 append 采集 | 本次进程采集；只有适当的同进程时钟域内才可比较 |
| PersistedWall | 从 v2 精确恢复 | 不可用，默认值；不能算历史耗时/deadline |
| UnavailableLegacy | v1 未持久化，无法追回，默认值 | 不可用，默认值 |

即使 wall=0，也应以 origin 判断是否缺失：v2 可以精确表示 Unix epoch，不能只凭
整数零判断来源。wall 可回拨，严格排序/恢复仍依靠 session/task sequence。
这不是磁盘 commit 时钟，也不是语义验证证据。

公开 C++ 布局增加，**库与全部消费者必须一起重编译**，不保证新旧二进制对象混用。
EventEnvelope 的逻辑 schema 不因物理文件格式变化而升版；源代码聚合初始化的
尾字段有默认值，手动构造信封时仍由构造者负责正确时间与来源。

可编译的纯同步读取辅助（调度/I/O 由宿主另行管理）：

```cpp
#include <mira/event_store.hpp>

std::optional<std::int64_t> recorded_wall_ns(const mira::EventEnvelope &event) {
    if (event.timestamp_origin == mira::EventTimestampOrigin::UnavailableLegacy)
        return std::nullopt;
    return mira::wall_nanos(event.timestamp);
}
```

## 磁盘格式和旧 reader

首次新写入追加以下标记，随后每个物理行是 `E2 ` 加规范 JSON：

```text
MIRA_EVENT_LOG 2
MIRA_EVENT_RECORDS
E2 { ... }
```

所有新记录使用 binary stream 写入单字节 LF，避免 Windows 文本流自动转换
CRLF。读取保持 binary 并在解析前去除行末 CR，兼容旧 Windows CRLF 前缀；
截尾偏移按原始字节（包括 CR）计算，续写不重写历史前缀。

JSON 严格包含 schema、event/runtime/session/task ID、session/task sequence、cause、
classification、type/data、wall_ns、integrity、record_digest。sequence/cause 用
十进制字符串保存完整 uint64 范围，wall_ns 是 int64；type/data 中的换行、CR、NUL
正确转义。integrity 保持请求幂等摘要的既有含义；record_digest 还校验时间、序号
等元数据。二者是校验和，不是认证或篡改者不可伪造的签名。

v1 数值行可作为原样历史前缀读取，时间标为 UnavailableLegacy。新 append 在旧
前缀之后添加一次上述双标记及新行，不重写旧字节、不伪造历史时间；同 ID 重试
只返回既有位置，不改变时间或追加格式标记。没有结尾换行的完整旧/新记录会在
下一次 append 前补分隔符。新段之后不再接受旧行或第二次格式切换。

重复 ID 还会比较实际请求字段，不单靠旧分隔符摘要；例如 type=`a|b`, data=`c`
与 type=`a`, data=`b|c` 不能被误当成相同请求。旧摘要仍保留用于协议兼容。

双标记让既有 v1 reader 命中“中间的非数值行”并只读拒绝，不把唯一的新记录当
坏尾截断；即使尾部修复后只剩空的新段，两个标记仍可防止降级。真实旧 reader
探针使用 `4884605` 的头文件/Core 编译：纯 v2 与旧前缀混合文件都返回
ReadOnlyDiagnostic，文件 SHA-256 不变。契约测试提供 `--emit-v2 <新目录>` 和
`--emit-mixed <新目录>` 生成探针文件；旧 SDK 不自动识别新记录，应升级 reader。

单 owner 约束仍适用：升级前停止旧 writer，禁止两个已加载实例/进程并发写同一
路径。新 reader 不重写未来版本；只读诊断后 append/read 均拒绝，不伪造部分成功。
只读实例的后续 recover 不重新扫描；需要修复时由 owner 关闭后显式重开并选择策略。

## 损坏与恢复

时间/身份/序号/checksum 错误、语义错误、未知版本或字段、非法降级都保留文件并
拒绝写入。v2 已换行但语法错误的完整行也不自动截断，避免把提交后的损坏伪装成
未提交尾巴。只有最后未换行的残缺记录或未完成格式标记可在 repair_uncommitted_tail
开启时截断；残缺中间记录或禁用修复则只读保留。v1 语法尾部仍沿用原恢复策略。

耐久等级另依 [DEC-088](../decisions/DEC-088-event-store-durability-and-external-kill.md)：
FileEventStore 新 append 刷新/关闭成功后确认 ProcessCrash；请求 Buffered 也返回
该实际等级。PowerLoss append/flush 在写入前返回 UnsupportedCapability；重复
请求不能提升等级。MemoryEventStore 仅报告 Buffered，更强 flush 拒绝；接到
journal/事实发布时必须消费弱确认失败，不得由生产 wrapper 伪造强确认。

物理格式不提供断电保证。OS 外部强杀、断电、设备写回与同步语义仍按 02/03
分别取证，不能凭 checksum 或流 flush 认为断电已安全。

## 下游与旧缓存

MemoryConsolidator 会明确拒绝含 UnavailableLegacy flat fact/Completed episode 的
自动固化批次，避免零时间回填为 now；错误返回前不调用 Memory query/apply。
宿主可分离批次，或重新取得原生验证证据，按 VerifiedFactPublisher 新协议发布，
不能通过改 origin/补当前时间来迁移旧事实。已有带 schema/valid_from 的事实协议
仍使用 payload 验证时间，支持其位于旧日志前缀，参见
[事实协议迁移](verified-fact-v1-and-checkpoint-v2.md)。

Conversation view 继续按 sequence 组织；旧时间默认值不代表真实的 1970 年发生
时间，需回到源 EventEnvelope 检查 origin。旧缓存、MemoryRecord 或对外审计结果
不自动重写，历史原时间无法凭文件 mtime/重开时刻恢复。缓存治理由宿主按原
授权/保留策略核查，本次不自动删除或给旧数据补造时间。

契约测试为 [file_event_time_test.cpp](../../tests/contract/file_event_time_test.cpp)，
覆盖新旧混合、重复、纳秒/大整数/控制字符、未知版本、篡改、残缺与只读；Linux
和 sanitizer/旧 reader 的实际命令及范围见维护计划第十三轮。
