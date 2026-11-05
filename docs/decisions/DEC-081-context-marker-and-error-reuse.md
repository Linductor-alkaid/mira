# DEC-081：Context/Memory marker 与局部错误构造复用

> 状态：Accepted
> 日期：2026-11-05
> 负责人：Mira Maintainers
> 冻结工作项：MNT-202611-11（lowercase/marker、Context Error 工厂子项）
> 替代/被替代：无

## 背景

MemoryConsolidator 与 Context 共用相同的默认禁止内容/注入词表，但公共配置头中
重复声明词表。Context 的扫描折叠正文和 marker 的 ASCII 大小写，Memory 仅折叠正文，
使自定义大写 marker 漏拦；空 marker 在 Memory 匹配任何正文，而其他配置已拒绝它。
Memory 的候选 key 另有一份相同的 ASCII 小写函数。六处 Context 局部 ErrorCode
错误工厂仅在 domain 不同，其构造语义一致。

## 决策

- 新增 Core 公共 `content_markers.hpp` 的两个默认词表工厂，四类 policy 默认值从
  同一来源生成独立 vector。既有词条、顺序、配置字段类型和可定制能力不变。
  工厂同步、无外部副作用、不启动任务；返回值由调用方拥有，允许分配异常。
- Context 私有 `context_text.hpp` 共享确定性 ASCII 小写及子串扫描。正文和 marker
  都只折叠 A–Z；非 ASCII 字节与嵌入 NUL 保持原样，不引入 locale 或 Unicode 归一化。
  Memory 候选 key 也使用同一个小写函数；claim key 的独立归一化规则不变。
- ConsolidationPolicy 与其他配置一样拒绝空 marker，返回 memory 域 InvalidMutation；
  在模型提案、存储查询和写入前验证，不把空 marker 静默忽略。空 vector 仍是合法的
  显式配置。默认合法配置行为保持；自定义大写 marker 现在正确拦截，空 marker
  从逐候选拒绝改为配置失败，属于明确的配置契约收紧。
- 保留各消费面的准入职责：禁止内容过滤所有 Memory 候选，注入过滤仅约束其
  model_assisted 候选；Context consolidation/curation 仍逐语句过滤。共享词表不
  等于共享审批、scope/ACL、可信度或验证等级策略，更不构成完整敏感内容检测保证。
- Context 私有 `context_error.hpp` 共用六处简单 ErrorCode 工厂的字段构造。
  domain 仍在调用方绑定，domain_code=0、retryable=false 与空可选字段保持。
  不合并 make_context_error / make_memory_error 的领域码映射，也不更改其他模块错误域。

## 备选方案与影响

仅抽小写、不统一词表，无法阻止默认值继续漂移；仅直接调用 Context marker helper
却不拒绝空配置，会把 Memory 的拒绝误改为允许，均不采用。
不把 Memory 拉进模型上下文 helper 的大依赖面，轻量文本/错误头保持私有。
新公共工厂按契约头、API 示例、不变量和契约测试四件套交付；没有新 Executor 生命周期、
并发任务、存储 schema 或跨模块私有依赖。

## 验证

固定两份默认词表逐项内容/顺序、四类配置一致性及返回值独立性；验证空 marker
配置失败、空 vector 可用、自定义大小写 marker 在 Memory/检索侧拦截，注入与
人工审批边界不变；核对候选 key 的 ASCII/非 ASCII 兼容。通过公共入口固定六个
错误域的 code、domain_code、retryable、消息及可选字段。运行对应调用方回归、
公开头/安装消费者与 Linux sanitizer，不从 fixture 推导其他平台或真实语义召回保证。

关联：[维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)、
[Context/Memory 设计](../design/context_and_memory_design.md)。
