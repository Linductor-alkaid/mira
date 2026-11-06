# DEC-077：检索共享向量有效性与余弦计算

> 状态：Accepted
> 日期：2026-11-05
> 负责人：Mira Maintainers
> 冻结里程碑：MNT-202611-09（复用治理维护轮）
> 替代/被替代：无

## 背景与问题

Context 与 SQLite Memory 各实现一次 cosine，零向量分别返回 0 与 -1。
两条检索腿都接受非负分数，因此 Context 把无有效相似度的零向量当作候选；
SQLite 路径还直接把 blob 指针当作 float 指针，依赖存储未承诺的对齐。

## 决策

新增 Core 公共纯函数 `embedding_cosine_similarity(span<float const>, span<float const>)`，
位于 `embedding_math.hpp`，实现只依赖标准库。Core 与 state_store 都依赖这一边界，
不互相引用私有头、不新增模块依赖。

- 空向量、维度不匹配、非有限值或零范数返回 nullopt，不能用 0/-1 代替无效结果。
- 有效余弦用 double 累计，结果限制在 [-1, 1]；负相关是有效值。两个调用方保留既有
  非负候选门槛，正交的有效 0 分与无效零向量明确区分。
- 无效比较计入既有 vector_errors；全部比较不可用时沿用降级到 exact/lexical 或
  exact/FTS 的质量语义。不改 ACL、profile/维度门槛、融合权重和预算截断。
- SQLite blob 先复制到对齐的 float 存储，再调用共享函数；不假定 blob 对齐。
- 不改变 embedding 写入/供给 API 的接受规则；索引中的零向量也不能经向量腿被召回。

## 备选方案

直接统一为返回 -1：混淆有效负相关与无效输入，不采用。
让 Context 引用 storage 私有助手：违反模块边界，不采用。
把全部排序与选择合并：两个调用方的 ACL 与资产模型不同，本轮不采用。

## 影响与风险

零向量曾经经 Context 向量腿进入结果，现在被排除；这是语义纠错。
正常有限非零向量的计算顺序保持一致，仅限制数值舍入越过端点的情况。
新增公共 C++ 函数属于源码加法，沿用 DEC-002 不承诺稳定 ABI 的约束。
SQLite 解码增加有界复制，维度/扫描上限不变，不宣称性能或真实召回质量提升。

## 验证方式

数值边界覆盖零/空/维度/非有限、极大与极小 float、同向/反向/正交；
两条检索路径证明无效向量不入选、关键词回退仍工作、正常排序/ACL/预算无回退。
运行既有检索评估与适用 sanitizer，不用 fixture 推导真实模型语义质量。

## 关联文档和工作项

- [维护计划](../plans/maintenance-2026-11-runtime-wiring-and-reuse.md)：09。
- [Context 与 Memory 设计](../design/context_and_memory_design.md)。
