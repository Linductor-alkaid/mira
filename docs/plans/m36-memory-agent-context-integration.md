# M36：受管记忆代理与完整请求准入接线

> 状态：Completed（Linux 离线范围；目标平台/真实模型与长期验证不在本轮声明）
> 负责人：Mira Maintainers
> 所属计划：[总计划](mira-implementation-plan.md)
> 前置：M34/M35 的本地受管执行基础；现有 ContextManager、IMemory 与 consolidation
> 建议发布点：Linux 离线宿主集成增量
> 更新日期：2026-11-09

## 目标与范围

按[研究报告](../design/memory-agent-reference-study.md)与
[DEC-101](../decisions/DEC-101-memory-agent-context-admission.md)，交付可接入两种 Loop
的完整请求准入，以及宿主拥有的有界记忆服务：按工作召回、事件候选提取、模型介导整理、
历史压缩、修订/删除失效、主代理工具入口和可运行宿主示例。

不关闭 M29/M30：主体恢复、承诺实体、全部自然语言纠正/删除意图和容量/自主行为仍在
既有里程碑。Layer 4 token 压缩、长期性能保证、跨进程 worker 续跑不在本增量。

## 工作项与验收

- [x] `M36-01` 冻结 owner、版本、权限、完整请求准入与兼容性，决策/设计同步。
- [x] `M36-02` Gateway 最终准入：原始协议保留，候选低权限，完整计数和 per-profile fallback，错误可见。
- [x] `M36-03` 记忆服务：受管 job、scope 隔离、既有写入管线、版本/删除屏障、terminal 后提取。
- [x] `M36-04` 模型与主代理工具接线、非阻塞候选供给、历史压缩与保护尾验证。
- [x] `M36-05` 两工作切换、用户纠正、迟到候选的离线集成示例与测试矩阵。
- [x] `M36-06` API/设计/计划/术语同步，构建、定向及全量测试、sanitizer 和静态门禁取证。

## Executor 与关闭

模型/阻塞存储等待放在外部注入 Executor 的 blocking-I/O worker，job 持有合作取消探针、
deadline 和 promise/future；服务持有 worker 句柄。每工作一个在途 job，所有工作和
回执总量有界，拒绝显式返回。外部 owner 收拢服务后才关闭存储及 Executor。

## 测试与退出条件

- [x] 完整 envelope、图像、工具/输出 schema、fallback 按各 profile 准入；无超限派发。
- [x] 两工作不串线、修订和删除后的迟到结果不提交，显式写入状态可解释。
- [x] 历史前缀匹配、protected tail 与工具配对保留；压缩失效回退与最终容量失败可见。
- [x] 正常、异常、拒绝、执行中取消、timeout、shutdown、接管均有测试。
- [x] Linux Debug 全量、定向 sanitizer 与文档/架构/平台/格式检查通过。

## 风险与未执行验证

模型质量依赖真实模型和场景数据；本轮先验收离线 provider fixture。Windows/Android、
真实桥侧模型、长时间运行的性能/存储增长未运行前不得标为完成。负责人 Mira Maintainers，
补跑条件：目标工具链/设备、provider 凭据和固定数据集齐备，按本计划测试矩阵执行。

## 验证记录

2026-11-09：用户授权采纳研究建议并开发；基于 `62ad918`，分支
`feat/memory-agent-context-integration`，保留先前研究文档，开始实现本增量。


2026-11-09：实现与 Linux 离线验收完成（`62ad918` 加本工作树，未修改 third_party）。
交付 Gateway 全请求准入、MemoryAgentService 三模式、独立模型循环、前台委派回流、
协调层输入装饰器、压缩来源 Artifact 与耐久删除屏障。任务 epoch 允许 Runtime 合法初值 0。
引用造假、scope 越界、纠正/删除迟到结果、protected tail/工具配对、模型异常、队列/Executor
拒绝、取消/超时/接管、close 重试、服务重建与重复 Artifact 清理均有 fixture 覆盖。

执行中纠正覆盖 ConversationLoop、AgentLoop 以及设备 schema repair：旧响应不执行，
同一活动身份在恢复预算内刷新，设备重新观察；未消费工具结果保留。schema repair 保留
原始任务框架并追加有界校验摘要。修复原 request codec 未恢复全部 provenance digest/version
的问题。DEC-059 既有导入/promotion 行为保留，只有新服务强制严格模型候选策略。

环境：Linux x86_64，GCC 13.3.0，C++20；SQLite 3.53.4，kairo pin `78c6f5575331`。
全部 provider 为离线 fixture，不使用真实模型凭据或网络。可复现命令与结果：

```bash
cmake --preset debug
cmake --build --preset debug -j 4
ctest --test-dir build/debug --output-on-failure -j 4
# 160/160，包括新增 4 个专项测试和两工作宿主示例。

cmake --preset asan -DMIRA_ENABLE_UBSAN=ON
cmake --build --preset asan --target mira_m36_context_admission_test mira_m36_memory_agent_test mira_m36_memory_agent_lifecycle_test mira_m36_memory_agent_model_test mira_memory_agent_host_consumer -j 4
ctest --test-dir build/asan -R 'm36_|memory_agent_host' --output-on-failure
# ASAN+UBSAN 5/5。

cmake --preset tsan
cmake --build --preset tsan --target mira_m36_context_admission_test mira_m36_memory_agent_test mira_m36_memory_agent_lifecycle_test mira_m36_memory_agent_model_test mira_memory_agent_host_consumer -j 4
setarch x86_64 -R ctest --test-dir build/tsan -R 'm36_|memory_agent_host' --output-on-failure
# TSAN 5/5；采用仓库 CI 相同的禁用 ASLR 方式。

python3 tools/check_docs.py .
python3 tools/check_architecture.py .
python3 tools/check_platform_boundary.py .
python3 tools/check_sbom.py .
cmake --build --preset debug --target format-check
git diff --check
# 全通过，架构基线 16 项未改变，无新增违规。
```

12 个受影响的生产编译单元分别运行 `clang-tidy -p build/debug <file> --warnings-as-errors='*'`
通过：model_context_admission、model_gateway、model_contracts、model_schema、agent_loop、
conversation_loop、memory_agent、memory_agent_jobs、memory_agent_model、memory_agent_seams、
memory_agent_artifacts、memory_consolidation。当前会话原始构建/测试摘要保留在
`/tmp/mira-m36-{full,asan,tsan}-*.log`，静态检查在 `/tmp/mira-m36-tidy-*.log`。

验证过程保留：首次全量 157/160，发现 epoch 0 误拒、codec 丢 selection_digest、费用估算
兼容性问题，修复后 160/160；费用继续使用原 DEC-084 估算器，新增诊断由价格宿主按事件类型
消费。曾全局收紧 namespaced 候选导致 M23 兼容测试失败，按 DEC-059 恢复默认兼容并在
新服务内强制严格策略。新增重建测试还发现已擦除 Artifact 在新实例的 live index 中返回
NotFound，现经 open 确认不可用后幂等完成。执行中纠正测试发现 schema repair 丢失任务
框架，已修复并加入回归。TSAN 首次因 ASLR 出现 unexpected memory mapping、未进入测试，
采用上述 setarch 方式后实际执行 5/5，不将原环境失败当作测试通过。

- [ ] Windows/Android：负责人 Mira Maintainers，目标工具链/设备齐备后执行构建、公开头文件与适用生命周期矩阵。
- [ ] 真实模型：负责人 Mira Maintainers，固定 profile、凭据与标注场景后测召回/压缩忠实度、拒答与约束保留。
- [ ] 长期运行：负责人 Mira Maintainers，目标部署档位和多任务数据集齐备后测跨窗口/重启的资源与积压增长。

上述未执行项不阻塞本次限定的 Linux fixture 交付，不构成对应平台、语义质量或长期性能保证。
同 scope 删除后的新世代接纳由 M30-09 跟踪；M29–M33 仍按各自退出条件推进，未标记完成。
