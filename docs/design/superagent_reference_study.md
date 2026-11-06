# DeepSeek Super-agent 参考研究

> 状态：研究输入（research note；不构成决策、设计规范或实现承诺）
> 版本：0.1
> 更新日期：2026-11-03
> 负责人：Mira Maintainers
> 适用范围：[M29](../plans/m29-interaction-coordinator-concurrency-and-recovery.md)、
> [M31](../plans/m31-capacity-admission-and-archival-recovery.md)、
> [M33](../plans/m33-autonomy-and-interruption-budget.md) 立项前的机制参考
> 关联决策：[DEC-004](../decisions/DEC-004-security-authority-confirmation.md)、
> [DEC-068](../decisions/DEC-068-interaction-coordinator-and-principal-input-entry.md)、
> [DEC-069](../decisions/DEC-069-bounded-capacity-and-forgetting-lifecycle.md)、
> [DEC-071](../decisions/DEC-071-attention-focus-and-safe-switch.md)、
> [DEC-072](../decisions/DEC-072-autonomy-and-interruption-budget.md)

## 1. 文档目的与效力

本文对 DeepSeek 于 2026-10-31 发布的 Super-agent 架构做机制级转述，并映射到 Mira
当前三条在途工作线：M29（执行中插话、并发归属、持久化与断线回补）、M31（容量准入与
归档回收）、M33（自主行为与打断预算）。

Super-agent 与 Mira 的产品形态不同：Super-agent 是**云端编码 Agent 的宿主运行时**，
面向单个 GitHub 仓库；Mira 是**端侧原生 AI Agent Runtime**，面向真实设备环境。因此本
文的价值不在"照搬"，而在三点：其一，它对"授权应当是任务级快照而非动作级自由裁量"的
论证方向；其二，它把"主动工作"的产品治理（何时接单、何时沉默）做成了可复现的机制；
其三，它公开了完整实现与领域模型文档，是少见的可以直接读到源码级细节的同类系统。

效力约定：

- 本文是对外部系统的转述与对 Mira 的建议，**不是规范**。任何采纳都必须经决策记录或
  专项设计显式冻结；未被引用的条目保持研究输入地位。
- 外部系统描述基于 2026-11-02 至 2026-11-03 对公开文章与公开仓库
  （`deepseek-ai/dsh-superagent-prototype`，MIT）的检索与源码阅读，可能随版本变化；
  引用其行为不表示 Mira 承诺兼容或对齐。
- 第 7 节的"有意分歧"与 `AGENTS.md`、`RULE-01` 至 `RULE-12` 冲突时，以后者为准。
- 本文不引入任何 Mira 公共概念。文中出现的 Super-agent 自有术语（Trigger、Thread、
  Eval、Mandate、Ticket、Assignment、Subscription、Source 等）均为**外部系统术语**，
  不进入[公共术语表](../project/glossary.md)；提到 Mira 侧概念时一律使用术语表
  的冻结用词。

## 2. 参考对象概览

| 项 | 内容 |
| --- | --- |
| 发布方 | DeepSeek Harness 团队 |
| 时间 | 2026-10-31 |
| 文章 | 《Super-agent 设计探索：迈向更自主的 Agent 系统》（微信公众号技术文章） |
| 原型 | `github.com/deepseek-ai/dsh-superagent-prototype`，MIT，TypeScript，v0.1.0 |
| 宿主 | 以 [DeepSeek Harness](https://www.deepseek.com/en/harness/) 插件 bundle 形态运行 |
| 规模 | 177 个 TS/TSX 源文件，约 30182 行；9 篇编号设计文档（`docs/000`–`docs/008`） |
| 定位 | README 自述为"研究原型，仅供参考，非 DeepSeek 正式产品"，且明示"不是生产就绪软件，可能含缺陷" |

DeepSeek Harness 本身是一个开源 agent harness，采用 Cordis 的"一切皆插件"架构，
以 Session 为执行容器，支持工具、技能与界面扩展。Super-agent 是它的一个插件 bundle。

## 3. 设计文章层机制提炼

以下为文章层机制，**不等于**原型实现；实现落差见第 4.1 节。

### 3.1 核心论点与四大特征

文章的核心论点是：单任务上限取决于模型能力，**多任务上限取决于人的注意力**（一人
通常只能跟进 3–5 个活跃会话）。现行 Agent 以互相独立的 Session 为运行单元，用户
并行工作时须自行切换会话并保管上下文。文章提出"**Session 应是系统的实现细节，而非
用户的认知负担**"，由系统承担调度与执行，人只做目标设定与结果验收。

四大特征：**连续**（跨 IM、GitHub 评论等渠道延续同一身份与上下文）、**高效**（前台
快速响应并接收下一条指令，后台多任务并行）、**主动**（持续监听环境事件，不值得处理
时保持安静）、**持久**（持续接入真实工作环境，异常后恢复执行）。

文章明确声明这些机制是**借用操作系统与分布式系统的既有思想**（进程调度、缓存、
快慢路径、投机执行、存算分离、看门狗），而非新理论。

### 3.2 双层架构（Router / Worker）

- **Router**：常驻，承接所有外部入口的人机交互；汇总跨任务信息、决策与记忆，是全局
  协调点与全局顺序基准；只负责交互与派发，不承担具体执行，实现上可不提供 Bash 工具；
  任务通常不复杂，轻量模型往往够用。积压事件合并到下一次调用中，长输入短输出使
  Prefill 可并行处理，故请求耗时增长慢于批处理规模。规模扩大后可引入多层过滤或水平
  拆分，但分片越细吞吐越高、跨分片记忆与上下文连续性越弱。
- **Worker**：每个 Worker 拥有独立上下文，挂载完整执行工具，负责单一任务；任务结果与
  代码质量通常要求更强模型；**不直接向用户输出**，推理与工具调用被隔离，状态与关键
  结论交 Router 统一回复。同层 Worker 一般不互相通信，交集时订阅同一 Thread 利用
  相同事件发现冲突，少数跨任务协作由 Router 转达。Worker 内部可再创建 Subagent 或
  组织 Agent Team，对 Router 保持透明。

### 3.3 事件驱动与两级过滤

半结构化环境事件统称 **Trigger**（群消息、邮件、日程提醒、代码评审评论、CI 结果）；
属于同一 PR、IM 话题或邮件往来的连续事件构成一条 **Thread**。事件处理分四步：过滤
噪声 → 路由至 Worker → 组织并注入上下文 → 在后台提前尝试处理。

- **Fast Path**：以确定性代码按来源、群组、用户或机器人白名单等条件准入，可直接响应
  明确请求、保留为候选或忽略。
- **Slow Path**：只有通过程序准入的事件才交给 LLM，由模型结合提示词与上下文判断是否
  行动；策略也可通过提示词扩展。

### 3.4 上下文机制（Inline & Spill / Chain of Recall）

- **Inline & Spill**：把上下文窗口视作缓存。近期可能使用的信息直接放入当前窗口
  （Inline），避免额外工具读取；超长或低频内容留在窗口外（Spill），只保留引用、摘要
  或续读入口。初始注入只提供当前对象 ID 与摘要，随后依据局部性加入关联对象 ID、
  相邻消息、被回复内容、代码评审邻近评论等；长内容可保留前 50 字并把余下部分 Spill。
- **Chain of Recall**：为 Trigger、Thread、Worker Session 分配唯一 ID 使对象形成
  引用图；每次召回结果同时携带下一跳对象 ID 与摘要，模型可沿关系逐步展开旧任务与
  相关上下文。

### 3.5 投机执行

在尚未确定结果是否被采用时提前消耗后台算力，允许尝试失败并丢弃结果。示例：收到
会议邀请后后台检查日程冲突、聚合背景材料并起草回复草稿；研发场景配置"静默复现并
修复"策略——群里出现缺陷讨论时后台拉环境复现、定位并尝试修复，**只有确认问题存在
且找到简单修复方案才交付（提 PR），否则全程静默**。

### 3.6 异步多信道交互与异步 ReAct Loop

- 多信道差异：IM 强调低打扰，只发决策请求与简短结论（文章给出的硬性示例是单次回复
  不超过 100 词）；GitHub、邮件与文档等异步渠道采用结构化高密度交付（完整问题背景、
  改动说明、验证结果）。
- 异步 ReAct Loop：经典 ReAct Loop 中模型发起同步 `run()` 工具调用后循环阻塞，会话
  无法响应新输入。文章提出把耗时命令与子任务工具调用改为异步接口：`start()` 启动后
  立即返回启动确认而不等待；后台完成后结果作为**新事件消息**投递回会话；`stop()` 可
  终止运行时间过长的后台命令。新用户输入在当前推理步结束后即被处理。文章声明该机制
  已在 DeepSeek Harness 中实现，但**尚未默认开启**。

### 3.7 存算分离与 BYOM

分两步解耦：① Agent Loop 的**计算与持久会话状态**分离（计算上云、会话状态入库、
Harness 无状态）→ ② 云端 Agent Loop 与**重状态执行环境**分离（仓库、文件、本地工具链
与凭据留在原位，本地只跑轻量 daemon 收命令回结果）。文章注明这与 Anthropic 的
"脑手分离"思路一致但场景不同（Anthropic 面向托管服务与 VPC，此处面向用户间歇在线的
个人机器）。

由此支持 **BYOM**（Bring Your Own Machine，接入用户日常开发机）、接入云端托管机或
按需创建的计算环境、在不同设备间切换执行环境；个人设备重新上线后由云端 Agent 补做
相关任务。

### 3.8 外部 Watchdog

"外部 Watchdog"是主系统**之外**的独立 Session，由**定时消息**驱动形成 Control Loop，
同时监控两层：基础设施存活、接口报错与状态恢复失败；以及任务语义——由模型判断各
Worker 是否有实质进展、是否陷入无效循环、是否偏离整体目标。发现异常后主动介入修复，
遇外部偶发故障持续复查直至恢复。维护者可用自然语言询问故障原因，也可让它修改提示词、
增删触发规则或修正主系统行为。

## 4. 原型层机制提炼

### 4.1 文章与原型的落差（阅读前提）

对原型全部 177 个 TS/TSX 源文件与 9 篇设计文档做关键词检索，以下文章机制**在原型
中没有任何实现**：

| 文章机制 | 原型状态 |
| --- | --- |
| 外部 Watchdog | 无该角色。相近机制是进程内 `HealthBoard`、重启恢复路径、进程外 tmux 服务脚本与另一 profile 里的定时 agent 检查（`watchdog` / `Watchdog` 命中 0） |
| 投机执行 | 无该术语与对应语义。`speculative` 命中 0；最接近的是"仅记录 would-accept 的推断模式"与"带 reason 的 hand-back 重入 Eval"（`speculative` 命中 0） |
| Inline & Spill | 无该机制（`spill` 命中 0） |
| Chain of Recall | 无该机制（`Chain of Recall` 命中 0） |
| 异步 ReAct Loop 的 `start()`/`stop()` 工具接口 | 无同名接口；相近的是前台 shell 命令超时后转后台 job 的做法 |
| Router / Worker 术语 | 原型用 Main Session / Executor Session，Router / Worker 仅作对照出现在提示词与 preset 命名中 |

原型 README 自述"文章中的部分设计是未来方向，未在本原型实现"，与上述检索结果一致。
因此本文对 Super-agent 的引用应区分两个证据层级：**文章层机制**（设计意图，未验证）
与**原型层机制**（可读源码验证）。本节 4.2–4.6 属后者。

### 4.2 领域模型

原型未沿用文章术语，而是自建一套**授权导向**的领域模型（`docs/001-domain.md`）：

```
Source ──Trigger──▶ Thread ──订阅扇出──▶ Ticket ──Assignment──▶ Executor
                      │                                     │
                      └──Eval（Gate + Assessment）──▶ Mandate└──▶ Channel（回复）
```

| 外部术语 | 定义与关键不变量 |
| --- | --- |
| Source | 事件流。kind 是可复用实现，instance 是一种配置。持有读位置与自己的 bot 身份；丢弃 bot 自己的消息与监听范围外的 Trigger；重启不得重放事件 |
| Trigger | 一个不可变事件。属于 1..n 个 Thread；**同一 Trigger 被接受两次只产生一个 Ticket**（幂等单元）；`meta` 归 Source 所有，Core 不解释 |
| Thread | 按时间排序的 Trigger 序列。**同一时刻最多一个 Eval** 在跑；可挂多个 Ticket；持有 Subscriptions、运行中的 Eval 与 Decision 历史 |
| Eval | 对一批 Trigger 的判断，**两层：gate + assessment**。**无状态**；输入 = Trigger 批 + Thread 快照 + 该 Thread 全部 Ticket（含已结束）+ 当前 Policy；输出 = Decision |
| Mandate | accept 授予了什么：Policy 版本、scope、strength、命中规则、发件人身份、Trigger 的接收方式。**不可变**；scope 变宽即新 Ticket；**Ticket 结束不结束 Mandate**；只有人的 stop 撤销，或 Policy 不再成立时撤销；恢复前须按当前 Policy **revalidate** |
| Ticket | 一个被授权的工作单元。**只有 Eval 能创建**；open ⇄ ended；人的消息可把它从 ended 拉回 open（除非被人 stop 过）；**定义自己的状态，不跟随 Executor 的状态** |
| Assignment | Ticket 与某个 Executor 实例的绑定。pending → running → ended，**独立于 Ticket 生命周期**；只有它的最终 report 才能在 Executor 侧结束 Ticket |
| Subscription | Ticket 与 Thread 的多对多关系（由 Thread 持有）。匹配 Trigger 扇出给每个 live 订阅 Ticket，**不检查发送者**；人的消息经此路径抵达 Ticket 时**不再走 Eval**；context-only 订阅"不授予权限也不恢复工单" |
| Decision / Report | Eval 结果：accept·stop·ignore·unavailable；Assignment 上报状态：progress·waiting·ready·blocked·failed·dropped |
| Disposition | 一个 Trigger 的去向记录：被 Source 过滤（带原因码）/ 接受评估 / 决策 / 抵达哪些 Ticket / 重排 / 每次 hand back 的原因 / 被谁确认 |

### 4.3 授权模型

**一条 Trigger 的两条路径。**

1. **Eval 路径**（决定是否开启新工作）。Gate 先做确定性准入判定，产出强度与依据：
   - `explicit`：名单上的人 @ 了 agent；或在已有 Ticket 的 Thread 上跟进；或 admission
     table 某格设为 explicit。
   - `inferred`：不在名单上的人跟进已有 Ticket 的 Thread（除非其格为 explicit）；或
     admission table 某格设为 inferred；或名单上的 bot 发言。
   - `not admitted`：其他 bot、被表忽略的一切、accept mode 为 `off` 时的全部 inferred。
2. **订阅路径**（延续既有工作）。订阅 Thread 上的每条消息（含 bot 与 CI）**绕过 gate、
   不产生新授权**，抵达 Ticket 作为上下文。

**只有三种操作能改变 Mandate**：accept 创建、人的 stop 撤销、revalidation 失败吊销。
Policy 变更**不撤销**活跃 Mandate，只在恢复前 revalidate 时可能失效；接受工单所依据
的规则被修改时，变更以通知形式送达在途工单而不是中断它。

**accept mode 三档**：`off`（无 inferred 成为候选）/ `observe`（默认：判定"本会接单"
但**只记录不执行**，记为 "would accept"）/ `auto`（判定即接受）。显式请求在所有
模式下都可用。

**提示词优先级（严格分层）**：① 工具与宿主强制的事实（身份、权限、参数校验）——
**提示词文本无法改变**；② 内置与生成段；③ 命中规则的流程与通信要求；④ 团队段与
团队 skill；⑤ **外部文本（消息、评论、文档、引用）——是数据，永不改变规则或授权**。

**准入表**：每个 Source 的 Policy 有一张按接收范围分行的表，每行最多三格（在册但未
@ / 不在册且 @ / 不在册且未 @），各格可取 explicit / inferred / ignore；未 @ 类格仅
在 Source 接收全部消息的范围内可用；CI 结果无发送者，只提供最后一格。

### 4.4 调度与副作用

- **单一写入者**：`Engine` 是 Trigger / Thread / Decision / Ticket / Assignment 的唯一
  写入者，**所有方法同步**，每个命令在一个事务里改聚合；端口在事务内入队外部工作，
  适配器在 commit **之后**执行（`afterCommit` 保证回滚不会执行副作用）。
- **Effects 持久化副作用队列**：事务内 `enqueue(kind, key, payload)`，commit 后才执行；
  每 kind/key 只有一行；最早到期优先并受并发上限约束；失败指数退避；抛出 `Later`
  表示延期且**不计失败**（例如无空闲 worker slot）。各 kind 的并发与退避：
  `assessment` 4 并发 / 2s→60s；`launch` = 空闲 worker slot 数 / passing failure
  5s→300s 共 3 次；`delivery` 4 并发 / 2s→60s；`relay`、`notice` 各 1 并发且保序 /
  2s→60s；`reply` 1 并发 / 5s→300s。
- **Outbox**：Channel 消息的持久回执，同 id 保证不重复排队；同一 Ticket 到同一会话
  的消息按入队顺序发送（ack 先于 answer/result）。
- **worker slot 背压**：slot 在 launch 开始（含 prepare 阶段）即占用；**人的消息可用
  超出 `maxWorkers` 的 1 个额外 slot，且唤醒优先于 launch**——"新工作不能妨碍人继续
  已有工单"。无空位时 launch 停在 `working / queued` 等待。
- **读取层裁剪**：Executor Session 只能读自己的 Ticket、自己的 Thread 与抵达过它的
  Trigger；**分页结果里剔除它无权读取对象的 id**。
- **上下文与体积上限**：评估输入的 Trigger 文本 ≤ 4000 字符、关联工单 ≤ 8 条、Thread
  窗口为触发前后各 5 条；分页上限 4000 UTF-16 单元；路由决策一次接受 1–20 条；聊天
  消息硬上限 100 词；工单报告摘要 ≤ 600 字符。
- **异步化技巧**：前台 shell 命令 5 秒后自动转后台 job，使 worker 能在下一个 step
  接收新消息而不必等长命令结束；Router 侧有 `noWait` 守卫，拒绝 `sleep`、>30 秒
  shell timeout、>5 秒 job 等待。
- **周期驱动**：每 2 秒跑一轮 pass，先处理到期 effects，再处理到期 Eval。

### 4.5 恢复与终态

- **已送达 vs 已读**：`inputsDelivered`（抵达 Session，等一个 step 取用）与
  `inputsRead`（被 Session 纳入历史）分离；被取消或错误打断且未写入历史的，算**未读**。
- **重启恢复**：启动时按**最老的 Ticket 优先**恢复被打断的 Assignment，并附 recovery
  note，要求 Executor 先检查目录、git 历史与远端状态再重复任何外部动作；Session、
  目录、订阅、待处理消息与已存储的副作用跨重启保留。
- **失败分类**：可通过的失败（模型供应商或网络的临时失败、HTTP 408/429/5xx、连接被
  拒或重置或超时）先重试 3 次并指数退避；不可通过的失败直接把工单标为失败并停止
  自动投递输入，但工具仍可用，人可直接在会话里继续。
- **终态幂等**：工单终态后工具调用被拒；已结束工单保留订阅，人的消息可在同一
  Mandate 下恢复它（无需新的 Eval 或 Decision），而 bot 与 CI 消息不能唤醒它。

### 4.6 可观测性

- **HealthBoard**：每个部件一个健康条目，由唯一写入者认领；连续三次失败才报 error
  （`delivery` 例外，报在工单上）；成功即清零。
- **状态脚本**：`scripts/dsh-superagent status` 以退出码区分四种情形——0 正常、1 运行
  时错或无响应（重启可能修好）、2 某部件报错（重启修不好）、3 等待 setup。
- **审计存储**：单 SQLite，表含 `trigger`、`disposition`、`trigger_thread`、`thread`、
  `ticket`、`decision`、`effect`、`outbox`、`kv`；`disposition` 记录每个 Trigger 的完整
  去向。领域事件 12 个（`trigger.received`、`eval.started`、`decision.recorded`、
  `ticket.created`、`ticket.resumed`、`ticket.ended`、`assignment.started`、
  `assignment.ended`、`thread.subscribed`、`thread.unsubscribed`、`trigger.requeued`、
  `policy.changed`），其中仅 3 个有消费者。
- **面板**：只回答三个问题——现在有什么需要我、系统是否健康且在做正确的事、一件工作
  从触发到工单的历史是什么；健康状态不占空间。

### 4.7 原型自述的局限与已知问题

| 项 | 说明 |
| --- | --- |
| 无进程隔离 | Router 与 Worker 都以**运行者 OS 用户身份**、**`danger-full-access`** 运行；Worker 可读操作者 home 目录、可按绝对路径执行程序。文档自述目录与包装器"是默认值，不是安全边界" |
| 管理 API 权限 | 本地 HTTP API 可改执行环境与 setup 账号字段，权限高于 agent 工具；仅靠来源头与回环地址限制 |
| 提示词注入面 | Agent 读取群聊、评审评论与邮件并据此行动；设计上靠"外部文本是数据、不能改变授权"缓解 |
| 存储无界增长 | 文档明写"尚无删除旧记录的机制"，触发、决策、已结束工单、回执与每个提示词版本都无界增长 |
| 领域事件未全消费 | 12 个领域事件中仅 3 个有消费者 |
| Source 专属耦合残留 | 若干通用模块仍直接引用 GitHub / 飞书；setup 只探测这两个来源的账号字段 |
| 进程内监督 | HealthBoard 与"后台工作 30 分钟上限"都在同一宿主进程内，不是文章所述的进程外 Watchdog |

## 5. 量化数据与证据等级

以下数字**全部转述自 DeepSeek 自述的合成负载评测，未经独立验证**，且评测使用简化环境、
不评估真实编码质量。按[项目管理与文档规范](../project/project_management_and_documentation.md)
§10 的证据分级，本文只把它们记为"厂商自述的对照实验结果"，不作为任何性能、实时性或
质量结论的依据。

| 指标 | Super-agent | 对照：单 Session + Subagent（中性提示） | 证据等级 |
| --- | --- | --- | --- |
| 任务完成时间 p50 | 12.4 分钟 | — | 厂商自述，合成负载 |
| 任务完成时间 p95 | 15.7 分钟 | 29.5 分钟 | 厂商自述，合成负载 |
| 去掉订阅直通后 Router 决策数 | +约 30% | — | 厂商自述，合成负载 |
| 去掉订阅直通后读缓存 token | 2323 万 → 3177 万（+约 37%） | — | 厂商自述，合成负载 |
| Router 到达率 1 → 40 条/分钟时单请求耗时 | 2.5s → 3.9s | — | 厂商自述，压测 |
| Router 单决定分摊推理时间 | 6.4s → 1.6s | — | 厂商自述，压测 |

评测设置（约 300 条消息中约 20 个工作请求，每配置跑 5 次）细节：中性提示下 5 次均
未调用 Subagent；改为优先委派后 5 次中 4 次委派但仍更慢，剩余 1 次因模型自判任务较小
而自行执行，形成长尾。

**可提炼的方法论观察**（不依赖上述数字成立）：去掉订阅直通后**端到端完成时间基本
不变**，但 Router 决策数与缓存 token 显著上升。这说明订阅直通的收益在**路由开销与
上下文成本**，不在响应延迟。

## 6. 映射到 Mira 的相关工作项

本节只列"Super-agent 提供了一个 Mira 尚无对应表达的机制形态"或"Mira 已有更强表达、
Super-agent 提供了佐证"之处。凡 Mira 已冻结且更强的，不再重复。

### 6.1 M29：执行中插话、并发归属与恢复

| Super-agent 机制 | 与 Mira 的关系 | 涉及工作项 |
| --- | --- | --- |
| 输入分 `delivery` / `relay` / `notice` 三条独立通道，且 relay 与 notice **保序**、delivery 允许并行 | 与 Mira 的 `User Input Sink`（协调层把输入投给已在运行的 Loop 的注册面）方向一致。Mira 侧尚未区分"投递在途工作"与"注入新指令"两类通道的排序要求 | `M29-02` |
| `inputsDelivered` 与 `inputsRead` 分离；被取消/错误打断且未写入历史的算**未读**，重启后重投 | 与 Mira 的"未决副作用**先重观测/验证**，不因恢复流程重复执行"（`RULE-05`、`W-02`）同族。Mira 侧需明确"已送达未取用"的输入在重启后的归属与去重 | `M29-05`、`M29-06` |
| **worker slot 的人道例外**：人的消息可用超出上限的 1 个额外 slot，唤醒优先于 launch | Mira 已用更强的表达冻结同义约束：`TaskPriority` 中"用户发起任务的优先级**始终不低于**自主任务"，以及 `High` 不等于可抢占。Super-agent 的 slot 例外是同一诉求的实现样本 | `M29-04` |
| Ticket **定义自己的状态而不跟随 Executor**；Assignment 独立于 Ticket 生命周期 | 与 Mira 的 `Task`（挂 Session 下、有终态幂等）与 `Session` 分离同向，但**基数不同**：Mira 是 1 Session : N Task，Super-agent 是 1 Executor Session : 1 Ticket。Mira 不应引入 Ticket 概念 | `M29-01` 跑前冻结 |
| 恢复按**最老的 Ticket 优先**，被打断的工单附 recovery note 要求先重检环境再重复外部动作 | 与 Mira 的重启恢复要求方向一致；Mira 的 `EnvironmentEpoch` 提供了比"检查 git 历史"更强的通用等价物（环境不连续即递增并使旧观察失效） | `M29-05` |

### 6.2 M31：容量准入与回收

| Super-agent 机制 | 与 Mira 的关系 | 涉及工作项 |
| --- | --- | --- |
| Effects 队列的**按 kind 并发上限 + 指数退避 + 提交拒绝**，且"无可用 worker slot"用延期语义而非失败表达 | 与 Mira 的 `Capacity Budget`（总量、按主体份额、软硬水位）同向，但**执行机制不可照搬**：Mira 的所有异步、延时、周期任务必须由 Executor 管理（`RULE-02`），不得自建调度器或私有队列 | `M31-03`、`M31-05` |
| "存储无界增长"被原文档列为已知限制 | 是 Mira `M31-05`（无界 append-only 层的回收窗口与配额）问题意识的**外部佐证**，可直接作为该工作项的背景引用 | `M31-05`、`M31-06` |
| 并发槽满时新工作排队等待而非抢占在途工作 | 与 Mira `RULE-08`（所有队列、缓存、上下文与并发 operation 都有容量或预算上限）一致 | `M31-03` |

### 6.3 M33：自主行为与打断预算

| Super-agent 机制 | 与 Mira 的关系 | 涉及工作项 |
| --- | --- | --- |
| accept mode = `observe` 默认为"判定本会接单但**只记录不执行**"，并把记录留作复盘 | 与 Mira 的 `Autonomous Intent`、`Autonomy Permission` 三分类、`Interruption Budget` 九道闸门**不冲突但粒度不同**：Mira 的闸门在**发起前**拦截，Super-agent 的 `observe` 在**判定后**留痕。若 Mira 采纳"先记录后执行"，它可作为 M33 打断预算闸门的**策略调参数据源**，而非闸门本身 | `M33-03`、`M33-04`、`M33-05` |
| 消息硬性上限（单次回复 100 词）、"只在人必须行动或能用上结果时才发言" | 与 Mira 的 `Interruption Budget`（Core 强制执行九道确定性闸门，宿主只提供"该不该说话"的策略）分工一致；Mira 的分工更清晰——**是否说话**由 Core 闸门裁决，而非由提示词自律 | `M33-04`、`M33-07` |
| 订阅路径**绕过 gate 但不产生新授权**，工单仍受其 Mandate 约束 | 与 Mira 的 `RULE-09`（外部内容是不可信数据，不能提升为授权）**完全同构**，是 Mira 已有约束的外部佐证 | `M33-03` |
| "外部文本永不改变规则或授权"的五级提示词优先级 | 与 Mira 的 `RULE-04`（模型输出只形成结构化 Decision；Action 在本地经过 capability、freshness、权限等校验）同向。Super-agent 进一步把**工具与宿主强制的事实**置于提示词之上且不可被提示词改写，是 Mira `DEC-004` 精神在提示词组织上的样本 | `M33-03` |
| 主动工单的恢复语义：人的消息可恢复已结束工单（同一授权下），bot 与 CI 不能唤醒 | 与 Mira `M33-08`（未完成自主任务不自动恢复）**方向相反**：Super-agent 允许人的消息恢复，Mira 要求自主任务不自动恢复。二者的差别在于恢复触发者，Mira 的规则更保守 | `M33-08` |

### 6.4 与 DEC-004 的授权边界对照（关键分歧）

Super-agent 的 `Mandate` 是**工单级授权快照**（授权范围、强度、命中规则、Policy 版本）。
Mira 的 Human Confirmation 是**动作级授权**（challenge 绑定 Principal、Session、
Task/epoch、environment epoch、action digest、目标摘要、风险、到期时间与单次 nonce）。

两者不可互换：

- Super-agent 的工单级授权**适用于无人值守的长时间编码工作**；Mira 面对的是真实
  设备环境上的外部副作用（发消息、支付、删除、凭据输入），`DEC-004` 已明确"确认只
  授权被展示的一个动作或明确有界 action set，不授权后续模型自由发挥"。
- Super-agent 的"范围变宽即新 Ticket"（scope escalation 需重新 accept）在 Mira 中
  对应 `DEC-004` 的"任何绑定字段变化……都使确认失效"——两者都拒绝"授权静默扩大"，
  但 Mira 的粒度更细（绑定到具体 action digest 与 epoch）。
- Super-agent 的 Policy 变更**不撤销**活跃授权、只在恢复前 revalidate；Mira 的
  confirmation 在 Takeover、取消、屏幕变化、过期或消费后立即失效——Mira 更严格。

**结论**：Mira 不应引入工单级授权。Super-agent 的 `Mandate` 值得借鉴的是**授权快照 +
恢复前重校验**这一形态，可用于 Mira 自主行为路径（`DEC-072` 的 `Autonomy Permission`
在任务级的投影），而**不是**用于替代动作级 Human Confirmation。

## 7. 有意分歧（不采纳清单）

| 参考立场 | Mira 决定 | 依据 |
| --- | --- | --- |
| Super-agent：自建 Effects 持久化副作用队列 + 每 2 秒一轮的 pass 循环 + 按 kind 并发上限与指数退避 | 不采纳执行机制。语义（事务内入队、提交后执行、并发上限、退避、拒绝显式化）可参考，但**所有异步、延时、周期与阻塞任务必须经 Executor 管理**，不得自建调度器或私有队列。若 kairo 现有公开能力确有缺口，按 `EXE-YYYYMMDD-NNN` 登记 | `RULE-02`、`RULE-11`、AGENTS.md |
| Super-agent：Router 与 Worker 均以 `danger-full-access` 运行在操作者 OS 身份下，目录与包装器"不是安全边界" | 不采纳。Mira 驱动真实设备环境，Core 不直接依赖平台 API，平台能力只经 Adapter 注入；越权边界由 `DEC-004` 与 Human Takeover 裁决 | `RULE-01`、`DEC-004` |
| Super-agent：工单级 `Mandate` 授权范围 | 不采纳。Mira 的授权保持动作级 Human Confirmation；工单级授权仅可能作为自主行为路径的**任务级投影**，且不得放宽动作级要求 | `DEC-004`、`DEC-072` |
| Super-agent：Slow Path 由模型研判后可直接 accept（`auto` 模式） | 不采纳。Mira 的归属裁决是**确定性**的 `Routing Decision`，模型只能提出建议；自主行为的准入由 `Interruption Budget` 与 `Autonomy Permission` 的确定性闸门裁决 | `DEC-068`、`DEC-072` |
| Super-agent：已结束工单可被人的消息恢复（同一 Mandate 下） | 部分分歧。Mira 要求未完成自主任务**不自动恢复**（`M33-08`）；恢复必须经显式宿主动作与重新观察 | `M33-08` |
| Super-agent：本地 daemon + 云端 Agent Loop 的"脑手分离"部署形态 | 不采纳部署形态。Mira 是端侧原生 Runtime；"状态与执行环境解耦、离线保留任务状态"的**语义**已有对应物（EventStore、Checkpoint Store、Replay），无需引入云端 daemon 分层 | `DEC-003`、Mira Runtime 设计 |
| Super-agent：文章层的外部 Watchdog（进程外独立 Session + 定时消息驱动） | 不采纳为独立角色。Mira 已有 `RULE-06` 的 watchdog 语义（连续控制的最大时长与 `Up/Cancel` 安全收敛）；若需要跨任务监督，走既有 Executor 承载路径，不新增平行生命周期 | `RULE-06`、`RULE-11` |
| Super-agent：存储无界增长被列为已知限制 | 不采纳为现状。Mira 的 `Capacity Budget` 与 `Forgetting Layer` 要求对无界 append-only 层给出回收窗口与配额 | `DEC-069` |
| Super-agent：外部文本与规则严格分层（外部文本永不改变授权） | **采纳方向**。与 Mira `RULE-09`、`RULE-04` 同向，作为既有约束的外部佐证，不改变现行约束 | `RULE-04`、`RULE-09` |

## 8. 参考来源

- DeepSeek Harness 团队，《Super-agent 设计探索：迈向更自主的 Agent 系统》（微信公众号
  技术文章，2026-10-31）。转述渠道：量子位
  <https://www.qbitai.com/2026/10/506904.html>、新浪科技
  <https://finance.sina.cn/tech/csj/2026-11-01/detail-inixepty4335109.d.html>、
  腾讯新闻 <https://news.qq.com/rain/a/20261101A00ZIL00>、证券时报
  <https://www.stcn.com/article/detail/4316179.html>
- 原型仓库与设计文档：`github.com/deepseek-ai/dsh-superagent-prototype`（MIT）。
  阅读范围：README、SETUP.md、`docs/000-philosophy.md` 至 `docs/008-cookbook.md`、
  全部 177 个 TS/TSX 源文件（2026-11-03 检索）
- DeepSeek Harness 产品页与仓库：<https://www.deepseek.com/en/harness/>、
  `github.com/deepseek-ai/deepseek-harness`
- 行业解读（仅作背景）：ai-bot.cn
  <https://ai-bot.cn/super-agent>（含与 OpenAI Codex cloud 的对照）、
  虎嗅《当 Agent 能同时干活，谁来守住人的注意力》
  <https://m.huxiu.com/article/4902022.html>（对授权边界的质疑）

## 9. 后续

- 本研究的采纳情况由决策记录逐条引用并冻结；未被引用的条目保持研究输入地位。
- 2026-11-03 首版：覆盖文章层与原型层两个证据层，完成第 4.1 节的实现落差核对
  （源码与文档全文关键词检索）、第 6 节对 M29 / M31 / M33 的机制映射与第 7 节的
  不采纳清单。**未执行**：任何构建、测试与平台验证——本文不含实现变更，不需要也不
  产生测试证据；原文引用的量化数据未经独立验证，按第 5 节的分级只作参考。
- 后续若 M29 / M31 / M33 的跑前冻结需要引入本文条目，应以决策记录形式登记，并同步
  更新[公共术语表](../project/glossary.md)与对应里程碑文档。
