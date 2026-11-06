# DEC-082：事件/TLS 局部复用与公开契约保留

> 状态：Accepted
> 日期：2026-11-05
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-11（剩余局部复用及兼容性审计）
> 替代/被替代：无

## 决策与边界

- Workflow 的 20 个事件序列化函数只共享最终 EventPayload 构造。每个函数仍
  显式提供事件名、字段及插入顺序、schema、EventClass；Critical/State 无默认值。
  不合并专有字段，不改解析、append、durability 或错误语义。
- 重构前从当前编码器捕获 20 个固定输入的 wire 样本；契约测试对照固定 fixture
  比较类型、分类及 data 字节，测试不得自行刷新 fixture。固定输入含引号、反斜杠、
  换行和 optional 字段。既有解析/重放测试继续验证合法性和消费者行为。
- TLS 的公共错误工厂及 create 的 initialized/verify_peer 准入、通道构造/setup
  尾部在 net Adapter 私有头复用。保留错误码、retryable、domain、消息和检查顺序；
  未初始化优先于 verify_peer=false。后端通过延迟构造 lambda 保留其上下文参数。
  不合并 OpenSSL/Mbed TLS 的信任配置、SNI/IP、握手、I/O 错误或 BIO 映射，不放宽
  证书/主机名校验，不创建任务或改变 Executor 管理的 transport worker。
- tool_module 与 registry 的逐字段相同错误工厂放在 tool 私有头；不改变模块
  trust/registry 状态机、领域码或错误消息。其他错误工厂保留所属领域的映射职责，
  不为消除包装函数而引入全库通用可配置错误框架。
- 报告点名的 parser、诊断名、digest、坐标词汇及枚举按公开库契约保留。补齐宿主
  直接消费测试及用途说明；没有仓库调用不构成可删除依据，不隐式激活预留状态。
  具体处置见[公开契约审计](../compatibility/2026-11-public-contract-audit.md)。
- Context index 与 SQLite Memory 的评分/选择保留各自编排：ACL 主体、候选类型、
  claim/verification/expiry、去重与预算策略不同。已共享数值边界，不把外形相同
  的排序循环扩成暴露后端策略的公共模板。模型/HTTP/安全的大小写语义也不因 Context
  已共享 ASCII 原语而自动统一。

## 兼容性与验证

不新增公共 API、不改变字段、枚举编号、schema 或依赖方向。所有新 helper 均私有，
没有新的异步路径、Executor owner 或跨模块深引用；不修改第三方代码。
TLS 两个后端验证初始化/禁止关闭验证、真实 loopback 信任/不信任、代理、已有 SNI
及 portable fixture；Workflow 比较旧 wire 样本并运行 M8–M14 回归。公开保留项经
契约与安装消费者验证，其他平台的未执行项保留在维护计划，不能由 Linux 测试推导。
