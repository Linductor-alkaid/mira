/* zcode-workflow
description: 沿 Mira 总计划持续开发：每轮评审计划→（需要时调研+立项冻结）→实现→独立测试取证（IVA）→本地门禁（debug 全量 + 三
  sanitizer + 五项检查 + NDK 预演）→PR→CI 全绿→文档收尾→合并 master
  并清理工作分支，循环直到计划完成或达到轮数上限。合并与分支清理已获维护者授权。
whenToUse: 要按 docs/plans/mira-implementation-plan.md 继续推进 Mira
  开发时运行；每轮产出一个经完整门禁与 CI 验证并合并的 PR。
args:
  androidNdkHome:
    type: string
    description: 本机 Android NDK 路径；留空则自动探测常见安装位置，探测失败时跳过本地 NDK 预演（由 CI 的 android job 兜底）
    required: false
    default: ""
  maxCycles:
    type: number
    description: 本次运行最多推进几个工作项到合并（1-5）
    required: false
    default: 2
*/
// Mira 持续开发工作流：沿总计划循环推进开发，直到计划内开发完成或达到循环上限。
// 每轮：评审计划 →（需要时调研+立项冻结）→ 准备分支 → 实现 → IVA 独立测试取证
//      → 本地门禁（debug 全量 + 三 sanitizer + 五项检查 + NDK 预演）→ PR → CI 全绿
//      → 文档收尾 → 复跑 CI → 合并 master 并删除工作分支（维护者已授权）。

const MAX_CYCLES = Math.min(5, Math.max(1, Math.floor(Number(args.maxCycles ?? 3))));
const NDK_OVERRIDE = String(args.androidNdkHome ?? "");
const ANDROID_TARGETS = [
  "mira_core", "mira_workflow", "mira_simulator_adapter", "mira_android_adapter",
  "mira_net_transport", "mira_mbedtls_transport", "mira_state_store", "mira_stateful_consumer",
];

interface PendingDecision {
  /** 待决议问题，一句话 */
  question: string;
  /** 背景与相关文档/代码线索 */
  context: string;
}

interface Assessment {
  /** actionable=有已冻结可直接推进的工作项；needs_planning=下一阶段需先立项冻结；complete=计划内开发全部完成 */
  status: "actionable" | "needs_planning" | "complete";
  /** 本轮工作项编号（actionable）或建议立项的阶段标识（needs_planning），如 "Stage W5" */
  workItemId: string;
  /** 一句话：这项工作交付什么 */
  summary: string;
  /** 建议提交 scope，如 "m24"（actionable 时必填） */
  scope: string;
  /** 建议分支名：小写字母数字与连字符，不带 auto/ 前缀（actionable 时必填） */
  branch: string;
  /** 判断依据：文件+行级引用，至少 3 条 */
  evidence: string[];
  /** 仅 needs_planning：立项前需要调研的待决议问题，可为空 */
  pendingDecisions: PendingDecision[];
}

interface Research {
  /** 调研的问题原句 */
  question: string;
  /** 候选方案，每条格式「方案名：优点/代价」 */
  options: string[];
  /** 明确推荐与理由（一段话） */
  recommendation: string;
  /** 主要风险与影响 */
  risks: string;
  /** 证据来源：文件路径与节号 */
  evidence: string[];
}

interface FreezeResult {
  /** 冻结的首个工作项编号，如 "M24-01" */
  workItemId: string;
  /** 建议分支名（小写字母数字连字符，不带 auto/ 前缀） */
  branch: string;
  /** 建议提交 scope，如 "m24" */
  scope: string;
  /** 本阶段一句话交付摘要（用于 PR 标题与汇报） */
  summary: string;
  /** 新建或修改的计划/决策文档路径 */
  touchedDocs: string[];
}

interface PlanReview {
  /** 阻塞性问题：不解决不得进入实施；无则空数组 */
  blocking: string[];
  /** 非阻塞改进建议 */
  suggestions: string[];
}

interface ImplSummary {
  /** 单行提交主题，含 scope 前缀（如 "feat(m24): ..."），≤72 字符 */
  subject: string;
  /** 交付说明：做了什么、设计依据、文档同步情况 */
  notes: string;
  /** 主要改动文件（工作区相对路径） */
  changedPaths: string[];
}

interface IvaVerdict {
  /** pass=门禁矩阵全部有测试覆盖且通过；fail=存在未通过门禁 */
  verdict: "pass" | "fail";
  /** 已覆盖的门禁编号与对应测试文件 */
  covered: string[];
  /** 未通过的门禁：编号+现象+输出摘录 */
  failures: string[];
  /** 执行过的关键命令与结果计数 */
  evidence: string[];
}

interface CycleOutcome {
  /** 循环序号（1 起） */
  cycle: number;
  /** 工作项编号 */
  workItemId: string;
  /** 进行中 | 已合并 | 受阻 */
  status: string;
  /** PR 编号，未创建为空串 */
  prNumber: string;
  /** PR 链接，未创建为空串 */
  prUrl: string;
  /** 本地门禁摘要 */
  gates: string;
  /** 一句话结论 */
  note: string;
}

interface Finding {
  /** 位置：路径或 PR/工作项编号 */
  where: string;
  /** 一句话说明 */
  what: string;
  /** 证据：命令输出或文件引用 */
  evidence: string;
  /** verified=命令或 CI 已证实；unconfirmed=未能复现确认 */
  status: "verified" | "unconfirmed";
  /** low/medium/high */
  severity: "low" | "medium" | "high";
}

interface WorkflowReport {
  /** 两三句话回答：本轮推进了什么、为何停止 */
  conclusion: string;
  findings: Finding[];
  /** 已验证的门禁与命令 */
  verified: string[];
  /** 未覆盖项及原因 */
  notCovered: string[];
}

artifact.board("progress", {
  title: "开发进度看板",
  key: "workItemId",
  status: "status",
  columns: ["进行中", "已合并", "受阻"],
  cardTitle: "workItemId",
  detail: [
    { field: "prUrl", label: "PR" },
    { field: "gates", label: "门禁" },
  ],
});

// ---------- 门禁命令（确定性执行，脚本只信退出码） ----------
async function debugGate(): Promise<string> {
  const cfg = await world.run("cmake", ["--preset", "debug"]);
  if (cfg.exitCode !== 0) return `cmake --preset debug 失败：\n${(cfg.stderr || cfg.stdout).slice(-3000)}`;
  const bld = await world.run("cmake", ["--build", "--preset", "debug"], { timeoutMs: 7_200_000 });
  if (bld.exitCode !== 0) return `cmake --build --preset debug 失败：\n${(bld.stderr || bld.stdout).slice(-3000)}`;
  const tst = await world.run("ctest", ["--preset", "debug"], { timeoutMs: 3_600_000 });
  if (tst.exitCode !== 0) return `ctest --preset debug 全量失败：\n${(tst.stderr || tst.stdout).slice(-3000)}`;
  return "";
}

async function sanitizerGate(preset: string): Promise<string> {
  const cfg = await world.run("cmake", ["--preset", preset]);
  if (cfg.exitCode !== 0) return `${preset} 配置失败：${(cfg.stderr || cfg.stdout).slice(-2000)}`;
  const bld = await world.run("cmake", ["--build", "--preset", preset], { timeoutMs: 7_200_000 });
  if (bld.exitCode !== 0) return `${preset} 构建失败：${(bld.stderr || bld.stdout).slice(-2500)}`;
  const tst = preset === "tsan"
    ? await world.run("setarch", ["x86_64", "-R", "ctest", "--preset", preset], { timeoutMs: 3_600_000 })
    : await world.run("ctest", ["--preset", preset], { timeoutMs: 3_600_000 });
  if (tst.exitCode !== 0) return `${preset} 测试失败：${(tst.stderr || tst.stdout).slice(-2500)}`;
  return "";
}

async function qualityGate(): Promise<string> {
  const cfg = await world.run("cmake", ["--preset", "static-analysis"]);
  if (cfg.exitCode !== 0) return `static-analysis 配置失败：${(cfg.stderr || cfg.stdout).slice(-1500)}`;
  const bld = await world.run("cmake", ["--build", "--preset", "static-analysis"], { timeoutMs: 10_800_000 });
  if (bld.exitCode !== 0) return `clang-tidy 构建失败：${(bld.stderr || bld.stdout).slice(-2500)}`;
  const qa = await world.run("cmake", ["--build", "--preset", "static-analysis", "--target", "format-check", "docs-check", "sbom-check", "platform-boundary-check", "architecture-check"], { timeoutMs: 3_600_000 });
  if (qa.exitCode !== 0) return `五项检查（format/docs/sbom/platform-boundary/architecture）失败：${(qa.stderr || qa.stdout).slice(-2500)}`;
  return "";
}

async function findNdk(): Promise<string> {
  if (NDK_OVERRIDE !== "") return NDK_OVERRIDE;
  const bases = ["/home/linductor/Android/Sdk/ndk", "/usr/local/lib/android/sdk/ndk"];
  for (const base of bases) {
    try {
      const ls = await world.run("ls", [base]);
      const first = ls.stdout.split("\n").map((s) => s.trim()).filter(Boolean)[0];
      if (first === undefined || first === "") continue;
      const cand = `${base}/${first}`;
      const probe = await world.run("test", ["-d", `${cand}/build/cmake/android.toolchain.cmake`]);
      if (probe.exitCode === 0) return cand;
    } catch {
      // 候选路径不可读，尝试下一个
    }
  }
  return "";
}

async function androidGate(ndk: string): Promise<string> {
  for (const p of ["android-arm64-release", "android-x86_64-release"]) {
    const cfg = await world.run("cmake", ["--preset", p, `-DCMAKE_ANDROID_NDK=${ndk}`]);
    if (cfg.exitCode !== 0) return `${p} 配置失败：${(cfg.stderr || cfg.stdout).slice(-1500)}`;
    const bld = await world.run("cmake", ["--build", "--preset", p, "--target", ...ANDROID_TARGETS], { timeoutMs: 7_200_000 });
    if (bld.exitCode !== 0) return `${p} 目标构建失败：${(bld.stderr || bld.stdout).slice(-2500)}`;
  }
  return "";
}

async function checksStarted(pr: string): Promise<boolean> {
  for (let i = 0; i < 30; i++) {
    const v = await world.run("gh", ["pr", "view", pr, "--json", "statusCheckRollup"]);
    if (v.exitCode === 0) {
      try {
        const parsed = JSON.parse(v.stdout) as { statusCheckRollup?: Array<{ status?: string; state?: string }> };
        if (Array.isArray(parsed.statusCheckRollup) && parsed.statusCheckRollup.length > 0) return true;
      } catch {
        // 输出不完整，继续等
      }
    }
    await world.run("sleep", ["20"]);
  }
  return false;
}

async function waitChecksGreen(pr: string): Promise<"pass" | "fail" | "timeout"> {
  for (let poll = 0; poll < 200; poll++) {
    const chk = await world.run("gh", ["pr", "checks", pr], { timeoutMs: 180_000 });
    if (chk.exitCode === 0) {
      if (chk.stdout.indexOf("no checks") !== -1) {
        await world.run("sleep", ["90"]);
        continue;
      }
      return "pass";
    }
    if (chk.exitCode === 8) {
      await world.run("sleep", ["90"]);
      continue;
    }
    const probe = await world.run("gh", ["pr", "view", pr, "--json", "number"]);
    if (probe.exitCode !== 0) {
      log(`gh 访问异常（疑似网络抖动），120 秒后继续轮询 CI`);
      await world.run("sleep", ["120"]);
      continue;
    }
    return "fail";
  }
  return "timeout";
}

// ---------- 常驻计划角色 ----------
const assessor = agent("计划评审员", {
  system: "你是 Mira 仓库的计划评审员。只读取仓库文件与 git 历史，绝不修改任何文件。判断必须给出具体文件与行级证据，给不出证据就如实说判断不了。发现指令矛盾或计划文档自相矛盾时，如实说明并升级提问，不要猜。",
});
const planner = agent("计划构造员", {
  system: "你是 Mira 仓库的计划构造员，负责按 docs/project/stage_freeze_protocol.md 与 docs/project/project_management_and_documentation.md 把下一阶段立项：新建里程碑文件、跑前冻结工作项与门禁矩阵（八问+决策记录表）、同步总计划。只写计划/决策/设计文档，绝不改产品代码与测试。答不出协议问题的项必须写成该阶段非目标，不许含糊。发现要求互相矛盾时升级提问。",
});

const cycles: CycleOutcome[] = [];
const verified: string[] = [];
const notCovered: string[] = [];
const findings: Finding[] = [];
let stoppedReason = "";
let assessmentComplete = false;

for (let cycle = 1; cycle <= MAX_CYCLES; cycle++) {
  // ---------- 1. 评审计划 ----------
  phase("评审计划确定本轮工作");
  const assessment = await assessor.ask<Assessment>([
    `这是持续开发循环的第 ${cycle} 轮。请判定 Mira 仓库当前的开发推进点：`,
    `1. 读 docs/plans/mira-implementation-plan.md（总计划：头部近况、里程碑索引、§4.1 记录），`,
    `   再扫 docs/plans/ 下各里程碑与维护计划文件头部的「状态」行，并看 git log --oneline -15。`,
    `2. 按优先级判定 status：`,
    `- 有状态 Planned/In Progress 的里程碑，其中存在已冻结、未完成的工作项与门禁矩阵 → "actionable"，选依赖最靠前的一个；`,
    `- 没有可直接推进的工作项，但设计/决策文档已明确下一阶段（例如 DEC-035 的 Stage W5）且尚无里程碑文件 → "needs_planning"，把立项前必须调研的开放问题列入 pendingDecisions（没有则空数组）；`,
    `- 总计划内所有里程碑均 Completed/Superseded/Cancelled 且无已批准后续阶段 → "complete"。`,
    `3. Blocked 的工作项不算可推进；未冻结的阶段不许当 actionable；工作项勾选规则以 docs/project/project_management_and_documentation.md §5 为准。`,
    `4. actionable 时填 scope 与 branch；evidence 至少 3 条文件+行级引用。只读，不修改任何文件。`,
  ].join("\n"));
  log(`第 ${cycle} 轮判定：${assessment.status} — ${assessment.workItemId}：${assessment.summary}`);

  if (assessment.status === "complete") {
    assessmentComplete = true;
    stoppedReason = "总计划内已无待开发工作，全部里程碑关闭";
    break;
  }

  let workItemId = assessment.workItemId;
  let summary = assessment.summary;
  let scope = (assessment.scope || workItemId).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "");
  let branchName = (assessment.branch || scope).toLowerCase().replace(/[^a-z0-9-]+/g, "-").replace(/^-+|-+$/g, "");
  if (branchName === "") branchName = `dev-cycle-${cycle}`;

  // ---------- 2. 需要时：调研（只读，在 master 上进行） ----------
  let researchDigest = "";
  if (assessment.status === "needs_planning" && assessment.pendingDecisions.length > 0) {
    phase("调研待决议方案");
      const research = await Promise.all(assessment.pendingDecisions.map((d, i) =>
        agent(`方案调研员-${cycle}-${i + 1}`, {
          system: "你是 Mira 仓库的方案调研员。针对给定问题读相关设计、决策、计划文档与既有代码，给出候选方案对比与明确推荐。只调研，不修改任何文件。结论必须给出处；推测要显式标注为推测。发现问题超出可调研范围时如实说明并升级提问。",
        }).ask<Research>([
          `调研问题：${d.question}`,
          `背景与线索：${d.context}`,
          `要求：读 docs/design/ 与 docs/decisions/ 相关文档及对应代码；options 每条「方案名：优点/代价」；recommendation 给明确推荐与理由；risks 列主要风险；evidence 给文件路径与节号。`,
        ].join("\n"))));
      researchDigest = research
        .map((r, i) => `【问题 ${i + 1}】${r.question}\n候选：${r.options.join("；")}\n推荐：${r.recommendation}\n风险：${r.risks}\n证据：${r.evidence.join("、")}`)
        .join("\n\n");
      log(`完成 ${research.length} 个待决议问题的调研`);
  }

  // ---------- 3. 准备工作分支（此后所有写入只发生在分支上） ----------
  phase("准备工作分支");
  let pulled = false;
  let pullErr = "";
  for (let attempt = 1; attempt <= 6; attempt++) {
    const pull = await world.run("git", ["pull", "--ff-only"]);
    if (pull.exitCode === 0) {
      pulled = true;
      break;
    }
    pullErr = (pull.stderr || pull.stdout).slice(0, 300);
    log(`git pull 第 ${attempt}/6 次失败（可能为网络抖动），120 秒后重试`);
    if (attempt < 6) await world.run("sleep", ["120"]);
  }
  if (!pulled) {
    stoppedReason = `git pull --ff-only 重试 6 次仍失败（网络不可达），本轮中止：${pullErr}`;
    break;
  }
  const st = await git.status();
  const stray = st.untracked.filter((p) => !p.startsWith(".zcode/"));
  if (st.branch !== "master" || st.staged.length > 0 || st.unstaged.length > 0 || stray.length > 0) {
    stoppedReason = `工作区不是干净的 master（branch=${st.branch ?? "detached"}，staged=${st.staged.length}，unstaged=${st.unstaged.length}，未跟踪=${stray.join("、")}），中止以免把无关变更卷进 PR`;
    break;
  }
  const branch = `auto/${branchName}`;
  const exists = await world.run("git", ["rev-parse", "--verify", "--quiet", `refs/heads/${branch}`]);
  if (exists.exitCode === 0) {
    stoppedReason = `分支 ${branch} 已存在，中止（请先手工处理残留分支）`;
    break;
  }
  const co = await world.run("git", ["checkout", "-b", branch]);
  if (co.exitCode !== 0) {
    stoppedReason = `创建分支 ${branch} 失败：${co.stderr.slice(0, 500)}`;
    break;
  }
  log(`已切换到工作分支 ${branch}`);

  // ---------- 4. 需要时：立项冻结（写入发生在分支上，随 PR 一起提交） ----------
  if (assessment.status === "needs_planning") {
    phase("立项并冻结工作项与门禁");
    const freezeAsk = [
      `下一阶段需要立项冻结。阶段标识：${assessment.workItemId}。判定依据：${assessment.evidence.join("；")}`,
      researchDigest === "" ? "" : `前期调研结论如下，请采纳或给出更优选择并说明理由：\n${researchDigest}`,
      `请按 docs/project/stage_freeze_protocol.md 与 docs/project/project_management_and_documentation.md 立项：`,
      `1. 新建 docs/plans/ 下新的里程碑文件（编号顺延现有最大 M 编号；用规范 §7 模板，状态 Planned），冻结本阶段工作项（稳定编号）与门禁矩阵（G 编号、逐条可执行、指向具体测试文件），附协议八问回答与决策记录表；`,
      `2. 更新 docs/plans/mira-implementation-plan.md（头部近况一行 + 索引/§4.1 登记）；`,
      `3. 涉及「必须决策」的跨模块契约或边界变化时，按规范新增/更新 docs/decisions/ 的 DEC；否则记入里程碑文件决策记录表；`,
      `4. 门禁矩阵必须约定本阶段测试目录（tests/ 下）与测试文件路径；`,
      `5. 只写文档，不改代码与测试；不做 git 提交（脚本统一提交）；`,
      `6. 工作分支已创建：${branch}。FreezeResult.branch 必须填 "${branchName}"（与已建分支一致，不另起名）。`,
      `完成后返回 FreezeResult。`,
    ].filter(Boolean).join("\n");
    let freeze = await planner.ask<FreezeResult>(freezeAsk);
    for (let r = 0; r < 2; r++) {
      const review = await agent(`独立计划评审员-${cycle}-${r + 1}`, {
        system: "你是独立计划评审员，此前未参与这份冻结文档的撰写。读冻结后的里程碑文件、它引用的设计/决策文档与相关代码，找会让实施失败或返工的问题：范围歧义、违反 docs/project/stage_freeze_protocol.md §5 的形态、与 AGENTS.md 的 Executor 纪律冲突、门禁不可执行、依赖未识别。不修改任何文件；不复述优点，只报问题，每条给文件+行级依据。",
      }).ask<PlanReview>([
        `评审以下冻结文档（用 Read 读原文，不要只信摘要）：${freeze.touchedDocs.join("、")}`,
        `阶段交付目标：${freeze.summary}`,
        `blocking 只收「不解决就不能进入实施」的问题；suggestions 收改进建议。`,
      ].join("\n"));
      if (review.blocking.length === 0) {
        if (review.suggestions.length > 0) log(`计划评审通过，另有 ${review.suggestions.length} 条非阻塞建议`);
        break;
      }
      freeze = await planner.ask<FreezeResult>([
        `独立计划评审员发现阻塞性问题，请修订冻结文档后重新返回 FreezeResult：`,
        review.blocking.map((b, i) => `${i + 1}. ${b}`).join("\n"),
      ].join("\n"));
    }
    workItemId = freeze.workItemId;
    summary = freeze.summary;
    scope = (freeze.scope || scope).toLowerCase().replace(/[^a-z0-9]+/g, "-");
    log(`已立项冻结：${workItemId} — ${summary}（分支 ${branch}）`);
  }

  const developer = agent(`开发实现员-第${cycle}轮`, {
    system: "你是 Mira 仓库的开发实现员，在当前工作分支上实现已冻结的工作项。严格遵守仓库 AGENTS.md（Executor 纪律、分层与依赖方向、取消与关闭路径、文档同步矩阵）与冻结文档的范围/非目标边界。测试矩阵由独立测试验证员负责，你不要改动或删除它的测试文件。不要执行 git commit/push（脚本统一提交）。若门禁按现有设计不可能通过或指令矛盾，如实说明并升级提问，不要绕过或伪造结果。",
  });
  const verifier = agent(`独立测试验证员-第${cycle}轮`, {
    system: "你是 Mira 仓库的独立测试验证员（Independent-Verification-Agent 职责）：独立于实现者，为本阶段门禁矩阵编写并执行测试，只回报有命令证据的结论。你拥有 tests/ 下本阶段测试目录的所有权；绝不修改产品源码。失败就报失败并附输出摘录；环境跑不了就如实说缺什么；禁止为通过而放宽断言或绕过门禁。不执行 git commit/push。",
  });

  // ---------- 4. 实现 ----------
  phase("实现工作项");
  const impl = await developer.ask<ImplSummary>([
    `实现已冻结的工作项 ${workItemId}：${summary}`,
    `1. 先读冻结文档（docs/plans/ 下按 ${workItemId} 所属里程碑定位）及其引用的设计/决策文档；`,
    `2. 按冻结范围实现：严守 AGENTS.md 的 Executor 纪律与分层约束；「非目标」清单是硬边界；`,
    `3. 同步文档（docs/project/project_management_and_documentation.md §11 矩阵）：设计、API 手册、计划状态；开始实施时里程碑状态转 In Progress，工作项在验证完成前保持未勾选；`,
    `4. 不写 git commit/push；不动独立测试验证员的测试目录（冻结文档约定的 tests/ 下目录）；`,
    `5. 完成后自测：cmake --preset debug、cmake --build --preset debug、ctest --preset debug -R 只跑你的新增测试；失败继续修。`,
    `返回 ImplSummary：subject（单行提交主题，含前缀如 "feat(${scope}): ..."，≤72 字符）、notes（交付与文档同步说明）、changedPaths。`,
  ].join("\n"));
  report({ cycle, workItemId, status: "进行中", prNumber: "", prUrl: "", gates: "实现完成，待测试取证", note: impl.notes.slice(0, 200) }, "progress");

  // ---------- 5. IVA 独立测试取证 ----------
  phase("独立测试取证");
  let iva = await verifier.ask<IvaVerdict>([
    `请为工作项 ${workItemId}（分支 ${branch}）做独立测试取证：`,
    `1. 读冻结文档中的工作项与门禁矩阵；按其约定在 tests/ 下本阶段目录编写独立测试矩阵，覆盖每条门禁（正常、异常、边界；并发/生命周期路径按 AGENTS.md 要求覆盖适用项）；在 tests/CMakeLists.txt 登记新目录；`,
    `2. 构建并只运行你自己的测试：cmake --preset debug；cmake --build --preset debug；ctest --preset debug -R <你的测试标签>；`,
    `3. 全量门禁（ctest 全量、三 sanitizer、五项检查）由脚本统一执行，你不要重复跑；`,
    `4. verdict=pass 必须以你实际执行的命令输出为证；每条 failure 附现象与输出摘录；evidence 记录你跑过的命令与计数。`,
  ].join("\n"));
  let ivaRound = 1;
  while (iva.verdict === "fail" && ivaRound < 3) {
    log(`独立测试第 ${ivaRound} 轮取证发现失败，交回实现员修复`);
    await developer.ask<string>([
      `独立测试验证员发现以下失败，请修复产品代码（测试文件归验证员所有，不要改）：`,
      iva.failures.join("\n"),
      `修复后只做针对性构建自查，不要跑全量套件（脚本统一跑）。`,
    ].join("\n"));
    ivaRound += 1;
    iva = await verifier.ask<IvaVerdict>([
      `第 ${ivaRound} 轮复验：实现员已针对以下失败修复，请重新执行门禁矩阵测试并回报：`,
      iva.failures.join("\n"),
    ].join("\n"));
  }
  if (iva.verdict !== "pass") {
    report({ cycle, workItemId, status: "受阻", prNumber: "", prUrl: "", gates: `独立测试 ${ivaRound} 轮未过`, note: iva.failures.slice(0, 3).join("；").slice(0, 200) }, "progress");
    findings.push({ where: workItemId, what: `独立测试取证 ${ivaRound} 轮后仍有失败，未创建 PR`, evidence: iva.failures.slice(0, 3).join("；").slice(0, 500), status: "verified", severity: "high" });
    stoppedReason = `独立测试取证 ${ivaRound} 轮后仍有失败：${iva.failures.slice(0, 3).join("；").slice(0, 300)}`;
    break;
  }
  log(`独立测试取证通过（${ivaRound} 轮）：${iva.covered.length} 条门禁有测试覆盖`);
  if (ivaRound > 1) {
    findings.push({ where: workItemId, what: `独立测试第 1 轮发现缺陷，修复后 ${ivaRound} 轮复验通过`, evidence: iva.evidence.slice(0, 3).join("；").slice(0, 500), status: "verified", severity: "medium" });
  }

  // ---------- 6. 本地门禁 ----------
  phase("本地门禁验证");
  const gateNotes: string[] = [];
  let gateFail = "";
  for (let round = 1; round <= 3; round++) {
    gateFail = await debugGate();
    if (gateFail === "") {
      gateNotes.push("debug 全量 ctest 通过");
      break;
    }
    if (round < 3) {
      log(`debug 门禁第 ${round} 轮失败，交回实现员修复`);
      await developer.ask<string>(`debug 全量门禁失败（第 ${round} 轮），输出尾部：\n${gateFail}\n请修复；只做针对性自查，脚本会重跑全量。`);
    }
  }
  if (gateFail !== "") {
    report({ cycle, workItemId, status: "受阻", prNumber: "", prUrl: "", gates: "debug 门禁未过", note: gateFail.slice(0, 200) }, "progress");
    findings.push({ where: workItemId, what: "debug 全量门禁 3 轮修复后仍失败，未创建 PR", evidence: gateFail.slice(0, 500), status: "verified", severity: "high" });
    stoppedReason = `debug 全量门禁 3 轮后仍失败`;
    break;
  }

  // 强门禁组：三 sanitizer + static-analysis/五项检查 + NDK 预演，共享 3 轮修复
  let androidNote = "本机未定位到 NDK，跳过本地预演（CI 的 android job 覆盖）";
  let strongFail = "";
  for (let round = 1; round <= 3; round++) {
    const problems: string[] = [];
    for (const p of ["asan", "ubsan", "tsan"]) {
      const r = await sanitizerGate(p);
      if (r !== "") problems.push(r);
    }
    const q = await qualityGate();
    if (q !== "") problems.push(q);
    const ndk = await findNdk();
    if (ndk !== "") {
      const a = await androidGate(ndk);
      if (a !== "") problems.push(a);
      else androidNote = `NDK 预演通过（android-arm64/android-x86_64 目标构建，NDK=${ndk}）`;
    }
    if (problems.length === 0) {
      gateNotes.push("三 sanitizer（ASAN/UBSAN/TSAN）零报告 + 五项检查与 clang-tidy 零违例" + (androidNote.startsWith("NDK") ? " + NDK 两 ABI 预演" : ""));
      break;
    }
    strongFail = problems.join("\n");
    if (round < 3) {
      log(`强门禁组第 ${round} 轮有 ${problems.length} 个问题，交回实现员修复`);
      await developer.ask<string>(`强门禁失败（第 ${round} 轮）：\n${strongFail.slice(0, 5000)}\n请修复；脚本会重跑全组。`);
    }
  }
  if (strongFail !== "" && gateNotes.length < 2) {
    report({ cycle, workItemId, status: "受阻", prNumber: "", prUrl: "", gates: "强门禁组未过", note: strongFail.slice(0, 200) }, "progress");
    findings.push({ where: workItemId, what: "强门禁组（sanitizer/质量检查/NDK 预演）3 轮修复后仍失败，未创建 PR", evidence: strongFail.slice(0, 500), status: "verified", severity: "high" });
    stoppedReason = `强门禁组 3 轮后仍失败`;
    break;
  }
  if (androidNote.startsWith("本机")) notCovered.push(`${workItemId}：${androidNote}`);
  verified.push(`${workItemId}：${gateNotes.join("；")}`);

  // ---------- 7. 提交并创建 PR ----------
  phase("提交并创建 PR");
  await world.run("git", ["add", "-A"]);
  const cm = await world.run("git", ["commit", "-m", impl.subject]);
  if (cm.exitCode !== 0) {
    report({ cycle, workItemId, status: "受阻", prNumber: "", prUrl: "", gates: "无变更可提交", note: "实现与测试未产生任何变更" }, "progress");
    stoppedReason = `git commit 失败（无变更或错误）：${(cm.stderr || cm.stdout).slice(0, 300)}`;
    break;
  }
  let pushed = false;
  let pushErr = "";
  for (let attempt = 1; attempt <= 8; attempt++) {
    const push1 = await world.run("git", ["push", "-u", "origin", branch]);
    if (push1.exitCode === 0) {
      pushed = true;
      break;
    }
    pushErr = (push1.stderr || push1.stdout).slice(0, 500);
    log(`git push 第 ${attempt}/8 次失败（可能为网络抖动），120 秒后重试：${pushErr.slice(0, 160)}`);
    if (attempt < 8) await world.run("sleep", ["120"]);
  }
  if (!pushed) {
    stoppedReason = `git push 重试 8 次仍失败（本地分支 ${branch} 已保留全部提交）：${pushErr}`;
    break;
  }
  const prBody = [
    `## 工作项`,
    `${workItemId}：${summary}`,
    ``,
    `## 交付说明`,
    impl.notes,
    ``,
    `## 独立测试取证（IVA）`,
    `- 取证轮次：${ivaRound}`,
    `- 覆盖门禁：${iva.covered.join("、")}`,
    `- 证据：${iva.evidence.slice(0, 6).join("；")}`,
    ``,
    `## 本地门禁`,
    ...gateNotes.map((g) => `- ${g}`),
    `- ${androidNote}`,
    ``,
    `> 由持续开发工作流自动推进；PR 合并与工作分支清理已获维护者授权。`,
  ].join("\n");
  let prUrlLine = "";
  for (let attempt = 1; attempt <= 3 && prUrlLine === ""; attempt++) {
    const out = await world.run("gh", ["pr", "create", "--base", "master", "--head", branch, "--title", impl.subject.startsWith("docs(") ? `feat(${scope}): ${summary}` : impl.subject, "--body", prBody]);
    const m = /https:\/\/github\.com\/[^\s]+\/pull\/\d+/.exec(out.stdout + " " + out.stderr);
    if (m !== null) {
      prUrlLine = m[0];
    } else {
      log(`gh pr create 第 ${attempt}/3 次未成功：${(out.stderr || out.stdout).slice(0, 160)}`);
      if (attempt < 3) await world.run("sleep", ["60"]);
    }
  }
  if (prUrlLine === "") {
    stoppedReason = `gh pr create 重试后仍未创建成功（本地分支 ${branch} 已保留全部提交）`;
    break;
  }
  const prMatch = /\/pull\/(\d+)/.exec(prUrlLine);
  if (!prMatch) {
    stoppedReason = `PR 链接解析失败：${prUrlLine.slice(0, 300)}`;
    break;
  }
  const prNumber = prMatch[1];
  const prUrl = prUrlLine;
  log(`已创建 PR #${prNumber}`);

  // ---------- 8. CI 全绿 → 文档收尾 → 复跑 ----------
  phase("等待 CI 全绿");
  await checksStarted(prNumber);
  let readyToMerge = false;
  let backfilled = false;
  let ciFailInfo = "";
  for (let round = 1; round <= 3; round++) {
    await checksStarted(prNumber);
    const verdict = await waitChecksGreen(prNumber);
    if (verdict === "pass") {
      if (backfilled) {
        readyToMerge = true;
        break;
      }
      log(`PR #${prNumber} CI 首轮全绿，补全验证记录后复跑`);
      await developer.ask<string>([
        `PR #${prNumber} CI 已全绿（${prUrl}）。请只做文档收尾，不改代码与测试：`,
        `1. 里程碑文件：状态改 Completed，验证记录补上 PR ${prNumber}、CI 全绿证据与本轮 commit；`,
        `2. 总计划 docs/plans/mira-implementation-plan.md：头部近况与 §4.1 对应记录同步（引用 PR ${prNumber}）；`,
        `3. 其余受影响文档若前面有遗留待补项，一并补齐。`,
      ].join("\n"));
      await world.run("git", ["add", "-A"]);
      const docCm = await world.run("git", ["commit", "-m", `docs(${scope}): record PR #${prNumber} CI evidence and close ${workItemId}`]);
      if (docCm.exitCode !== 0) {
        readyToMerge = true;
        break;
      }
      backfilled = true;
      let docsPushed = false;
      for (let attempt = 1; attempt <= 5 && !docsPushed; attempt++) {
        const dp = await world.run("git", ["push"]);
        if (dp.exitCode === 0) {
          docsPushed = true;
        } else {
          log(`收尾推送第 ${attempt}/5 次失败（可能为网络抖动），60 秒后重试`);
          if (attempt < 5) await world.run("sleep", ["60"]);
        }
      }
      if (!docsPushed) {
        stoppedReason = `文档收尾推送失败（本地分支 ${branch} 保留收尾提交，未合并）`;
        break;
      }
      continue;
    }
    const list = await world.run("gh", ["pr", "checks", prNumber]);
    ciFailInfo = `${list.stdout}\n${list.stderr}`.slice(0, 4000);
    if (verdict === "timeout") {
      stoppedReason = `CI 轮询超过 5 小时未出结果，本地分支 ${branch} 保留全部提交`;
      break;
    }
    if (round === 3) break;
    log(`PR #${prNumber} CI 第 ${round} 轮有失败项，交回实现员修复后重推`);
    await developer.ask<string>(`PR CI 有失败项（第 ${round} 轮）：\n${ciFailInfo}\n请在当前分支修复；脚本负责提交、推送并重跑 CI。`);
    await world.run("git", ["add", "-A"]);
    await world.run("git", ["commit", "-m", `fix(${scope}): address CI failures (round ${round})`]);
    for (let attempt = 1; attempt <= 5; attempt++) {
      const p = await world.run("git", ["push"]);
      if (p.exitCode === 0) break;
      log(`修复推送第 ${attempt}/5 次失败（可能为网络抖动），60 秒后重试`);
      if (attempt < 5) await world.run("sleep", ["60"]);
    }
  }
  if (!readyToMerge) {
    report({ cycle, workItemId, status: "受阻", prNumber, prUrl, gates: "CI 未达全绿", note: ciFailInfo.slice(0, 200) }, "progress");
    findings.push({ where: `PR #${prNumber}`, what: `CI 3 轮后未达全绿（文档收尾${backfilled ? "后" : "前"}），未合并`, evidence: ciFailInfo.slice(0, 500), status: "verified", severity: "high" });
    stoppedReason = `PR #${prNumber} CI 未达全绿`;
    break;
  }
  verified.push(`${workItemId}：PR #${prNumber} CI 全绿（${prUrl}）`);

  // ---------- 9. 合并并清理 ----------
  phase("合并并清理分支");
  let merged = false;
  let mgErr = "";
  for (let attempt = 1; attempt <= 4 && !merged; attempt++) {
    const mg = await world.run("gh", ["pr", "merge", prNumber, "--merge", "--delete-branch"]);
    if (mg.exitCode === 0) {
      merged = true;
      break;
    }
    mgErr = (mg.stderr || mg.stdout).slice(0, 500);
    const pv = await world.run("gh", ["pr", "view", prNumber, "--json", "state"]);
    if (pv.exitCode === 0 && pv.stdout.indexOf("MERGED") !== -1) {
      log(`PR #${prNumber} 已确认为合并状态（前次调用已生效）`);
      merged = true;
      break;
    }
    log(`合并 PR #${prNumber} 第 ${attempt}/4 次未成功（可能为网络抖动），60 秒后重试：${mgErr.slice(0, 160)}`);
    if (attempt < 4) await world.run("sleep", ["60"]);
  }
  if (!merged) {
    report({ cycle, workItemId, status: "受阻", prNumber, prUrl, gates: "合并失败", note: mgErr.slice(0, 200) }, "progress");
    stoppedReason = `合并 PR #${prNumber} 失败：${mgErr}`;
    break;
  }
  const backToMaster = await world.run("git", ["checkout", "master"]);
  if (backToMaster.exitCode !== 0) log(`git checkout master 非零（可能已在 master），继续`);
  await world.run("git", ["pull", "--ff-only"]);
  const head = await world.run("git", ["rev-parse", "--short", "HEAD"]);
  const outcome: CycleOutcome = {
    cycle,
    workItemId,
    status: "已合并",
    prNumber,
    prUrl,
    gates: gateNotes.join("；"),
    note: `已合并到 master（${head.stdout.trim()}），分支 ${branch} 已删除`,
  };
  report(outcome, "progress");
  cycles.push(outcome);
  log(`第 ${cycle} 轮完成：${workItemId} 已合并（PR #${prNumber}，master @ ${head.stdout.trim()}）`);
}

// ---------- 汇总 ----------
const md = [
  `# Mira 持续开发运行报告`,
  ``,
  `**停止原因**：${assessmentComplete ? "总计划内已无待开发工作" : stoppedReason === "" ? `达到循环上限（完成 ${cycles.length}/${MAX_CYCLES} 轮）` : stoppedReason}`,
  ``,
  `## 各轮结果`,
  ...(cycles.length > 0
    ? cycles.map((c) => `- 第 ${c.cycle} 轮 \`${c.workItemId}\` — ${c.status} — [PR #${c.prNumber}](${c.prUrl}) — ${c.note}`)
    : ["- 本轮没有工作项推进到合并"]),
  ``,
  `## 发现`,
  ...(findings.length > 0
    ? findings.map((f) => `- [${f.status}/${f.severity}] ${f.where}：${f.what}`)
    : ["- 无"]),
  ``,
  `## 已验证`,
  ...verified.map((v) => `- ${v}`),
  ``,
  `## 未覆盖`,
  ...(notCovered.length > 0 ? notCovered.map((v) => `- ${v}`) : ["- 无"]),
].join("\n");
await artifact.markdown("report", md, {
  title: "持续开发运行报告",
  description: `本轮共推进 ${cycles.length} 个工作项到合并`,
  primary: true,
});

const result: WorkflowReport = {
  conclusion: assessmentComplete
    ? `总计划内开发已全部完成${cycles.length > 0 ? `（本次运行合并了 ${cycles.length} 个工作项：${cycles.map((c) => c.workItemId).join("、")}）` : ""}。`
    : stoppedReason !== ""
      ? `运行在第 ${cycles.length + 1} 轮中止：${stoppedReason}${cycles.length > 0 ? `此前已合并：${cycles.map((c) => `${c.workItemId}(PR #${c.prNumber})`).join("、")}` : ""}。`
      : `达到循环上限，本次共合并 ${cycles.length} 轮：${cycles.map((c) => `${c.workItemId}(PR #${c.prNumber})`).join("、") || "无"}。`,
  findings,
  verified,
  notCovered,
};
return result;