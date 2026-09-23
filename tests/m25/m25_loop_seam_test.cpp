// M25 (DEC-045) host-integration round — AgentLoop Working Context snapshot
// supply seam, contract gates HI-G1 and HI-G2 over the frozen plan §4
// semantics (fixtures: tests/m25/m25_support.hpp):
//
//   HI-G1 zero drift and injection contract:
//     - no supplier attached -> request assembly byte-identical to the
//       pre-seam baseline (system/goal/follow-up/tool blocks unchanged);
//     - aligned committed snapshot -> eight-section Layer 0 conversion
//       rendered as labeled blocks (frozen source label, UntrustedExternal
//       discipline: never a System role, deterministic order, byte-identical
//       across loop instances for the same snapshot);
//     - empty store -> zero items, zero diagnostics (normal empty state);
//     - identity gate, three legs (session_id / task_id / task_epoch) ->
//       skip injection plus one visible diagnostic per skipped step;
//     - host-side environment-epoch gating demo: the supplier closure
//       returns nullopt on an epoch mismatch -> zero items, zero Loop-side
//       special-casing (no diagnostic).
//
//   HI-G2 bounded rendering and degradation:
//     - WorkingContextSeamOptions::validate bounds; max_items / max_chars
//       fixed-order truncation with the frozen "[working context truncated]"
//       marker (RULE-08);
//     - supplier error -> no seam items this step, one diagnostic, the loop
//       continues and the next step injects again;
//     - supplier exception (std and non-std) isolated, never escapes into
//       request assembly;
//     - exactly one supply call per assembled request.
//
// Frozen-surface note: the plan freezes the block text format "随实现冻结"
// (contract quadriad); this matrix pins the frozen semantics — the source
// label, position, Layer 0 conversion order, bounds, marker text, exactly-
// once, identity gate and degradation — and reads the diagnostic type as any
// WorkingContext-family event (see m25_support.hpp).

#include "m25_support.hpp"

#include <mira/agent_loop.hpp>
#include <mira/context_working_context.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace mira;
using namespace mira::testing;

// Fill all eight snapshot sections with distinct marker contents. Section
// declaration order (context_working_context.hpp): constraints, decisions,
// open_issues, active_tasks, verified_facts, failed_attempts, important_refs,
// next_actions.
void fill_eight_sections(WorkingContextSnapshot &snapshot, std::uint64_t event_seed_base) {
    snapshot.constraints.push_back(
        m25_item("m25 constraint: never deploy on fridays", event_seed_base + 1, 0.9));
    snapshot.decisions.push_back(
        m25_item("m25 decision: batch provider after the rate limit", event_seed_base + 2, 0.8));
    snapshot.open_issues.push_back(
        m25_item("m25 issue: tenant quota still unanswered", event_seed_base + 3, 0.7));
    snapshot.active_tasks.push_back(
        m25_item("m25 active task: finish the tenant onboarding", event_seed_base + 4, 0.7));
    snapshot.verified_facts.push_back(
        m25_item("m25 verified fact: staging accepts the v2 payload", event_seed_base + 5, 0.8));
    snapshot.failed_attempts.push_back(
        m25_item("m25 failed attempt: retry loop hit the limit", event_seed_base + 6, 0.6));
    snapshot.important_refs.push_back(
        m25_item("m25 ref: quota thread in ticket 482", event_seed_base + 7, 0.6));
    snapshot.next_actions.push_back(
        m25_item("m25 next action: ask about the deploy window", event_seed_base + 8, 0.7));
}

// Runs one two-step loop (tap, verified done) with the harness defaults and
// returns the assembled requests through `out`.
int run_two_step_loop(SeamLoopHarness &harness, AgentLoop &loop, ILoopVerifier &verifier,
                      std::vector<ModelRequest> &out) {
    harness.script(m25_tap_then_done_script());
    const auto context = harness.loop_context();
    const auto outcome = loop.run(harness.spec(), context, verifier);
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().outcome == LoopOutcome::Completed);
    MIRA_CHECK(harness.provider().requests().size() == 2);
    out = harness.provider().requests();
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G1: zero drift — no supplier attached, and supplier attached with an
// empty store, assemble byte-identical requests.
// ---------------------------------------------------------------------------

int no_supplier_and_empty_store_are_zero_drift() {
    const auto session = m25_session_from_seed(500);
    const auto task = m25_task_from_seed(501);
    // One shared profile identity so the byte comparison includes the
    // profile field.
    const auto profile_id = ModelProfileId::generate();

    // Baseline: the seam dependency is never injected.
    SeamLoopHarness baseline(session, task, profile_id);
    auto baseline_loop = baseline.make_loop();
    ModelDoneVerifier verifier;
    std::vector<ModelRequest> baseline_requests;
    MIRA_CHECK(run_two_step_loop(baseline, *baseline_loop, verifier, baseline_requests) == 0);

    // Injected dependency, but the store holds nothing for the session: the
    // normal empty state. The assembly must not drift a byte.
    SeamLoopHarness injected(session, task, profile_id);
    InMemoryWorkingContextStore store;
    WorkingContextSupplierHarness supplier_harness(store, session);
    supplier_harness.set_script({WorkingContextSupplierHarness::Mode::Store});
    auto loop = injected.make_loop();
    loop->set_working_context_supplier(supplier_harness.supplier());
    ModelDoneVerifier injected_verifier;
    std::vector<ModelRequest> injected_requests;
    MIRA_CHECK(run_two_step_loop(injected, *loop, injected_verifier, injected_requests) == 0);

    MIRA_CHECK(baseline_requests.size() == injected_requests.size());
    for (std::size_t index = 0; index < baseline_requests.size(); ++index) {
        const auto left = m25_request_wire_bytes(baseline_requests[index]);
        const auto right = m25_request_wire_bytes(injected_requests[index]);
        if (left != right) {
            std::cerr << "zero-drift mismatch on request " << index << "\nbaseline: " << left
                      << "\ninjected: " << right << '\n';
            return 1;
        }
        MIRA_CHECK(m25_seam_item_indices(injected_requests[index]).empty());
    }
    // The supplier was consulted exactly once per assembled request even in
    // the empty state (plan §4.2 exactly-once), and the empty state produced
    // zero diagnostics.
    MIRA_CHECK(supplier_harness.calls() == injected_requests.size());
    MIRA_CHECK(m25_working_context_diagnostic_count(injected.events(), session) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G1: aligned committed snapshot renders through the Layer 0 conversion.
// ---------------------------------------------------------------------------

int aligned_snapshot_is_injected() {
    const auto session = m25_session_from_seed(510);
    const auto task = m25_task_from_seed(511);

    SeamLoopHarness harness(session, task);
    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 512);
    fill_eight_sections(snapshot, 900);
    MIRA_CHECK(store.put(snapshot).has_value());

    WorkingContextSupplierHarness supplier_harness(store, session);
    auto loop = harness.make_loop();
    loop->set_working_context_supplier(supplier_harness.supplier());
    ModelDoneVerifier verifier;
    std::vector<ModelRequest> requests;
    MIRA_CHECK(run_two_step_loop(harness, *loop, verifier, requests) == 0);

    for (const auto &request : requests) {
        const auto seam_indices = m25_seam_item_indices(request);
        MIRA_CHECK(seam_indices.size() == 1);
        // The frozen provenance source label (plan §4.2).
        MIRA_CHECK(request.input[seam_indices.front()].provenance.source ==
                   "mira.agent-loop.working-context.v1");
        // RULE-09: model-mediated derived projection is never promoted —
        // the seam blocks never ride as System authority.
        for (const auto seam_index : seam_indices) {
            MIRA_CHECK(request.input[seam_index].role != ModelRole::System);
        }
        // Position: after the user context block, and the user block itself
        // stays before any tool result block.
        const auto user_index = m25_find_item_source(request, "mira.agent-loop.context.v1");
        MIRA_CHECK(user_index.has_value());
        MIRA_CHECK(user_index.value() < seam_indices.front());

        // Layer 0 conversion order (section declaration order) is visible in
        // the rendered text, and every section statement is carried.
        const auto text = m25_seam_text(request);
        std::size_t position = 0;
        const char *expected_order[] = {
            "m25 constraint:",    "m25 decision:",       "m25 issue:", "m25 active task:",
            "m25 verified fact:", "m25 failed attempt:", "m25 ref:",   "m25 next action:"};
        for (const auto *marker : expected_order) {
            const auto found = text.find(marker, position);
            MIRA_CHECK(found != std::string::npos);
            position = found + 1;
        }
        MIRA_CHECK(text.find("never deploy on fridays") != std::string::npos);
        MIRA_CHECK(text.find("ask about the deploy window") != std::string::npos);
    }
    // Exactly one supply call per assembled request; no diagnostics on the
    // aligned path.
    MIRA_CHECK(supplier_harness.calls() == requests.size());
    MIRA_CHECK(m25_working_context_diagnostic_count(harness.events(), session) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G1: byte determinism — the same committed snapshot renders byte-equal
// seam blocks on independent loop instances.
// ---------------------------------------------------------------------------

int seam_rendering_is_byte_deterministic() {
    const auto session = m25_session_from_seed(520);
    const auto task = m25_task_from_seed(521);

    SeamLoopHarness first(session, task);
    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 522);
    fill_eight_sections(snapshot, 930);
    MIRA_CHECK(store.put(snapshot).has_value());

    WorkingContextSupplierHarness first_supplier(store, session);
    auto first_loop = first.make_loop();
    first_loop->set_working_context_supplier(first_supplier.supplier());
    ModelDoneVerifier first_verifier;
    std::vector<ModelRequest> first_requests;
    MIRA_CHECK(run_two_step_loop(first, *first_loop, first_verifier, first_requests) == 0);

    SeamLoopHarness second(session, task);
    WorkingContextSupplierHarness second_supplier(store, session);
    auto second_loop = second.make_loop();
    second_loop->set_working_context_supplier(second_supplier.supplier());
    ModelDoneVerifier second_verifier;
    std::vector<ModelRequest> second_requests;
    MIRA_CHECK(run_two_step_loop(second, *second_loop, second_verifier, second_requests) == 0);

    MIRA_CHECK(first_requests.size() == second_requests.size());
    for (std::size_t index = 0; index < first_requests.size(); ++index) {
        const auto left = m25_seam_text(first_requests[index]);
        const auto right = m25_seam_text(second_requests[index]);
        MIRA_CHECK(!left.empty());
        MIRA_CHECK(left == right);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G1: seam blocks stay before the tool result block (plan §4.2 position).
// ---------------------------------------------------------------------------

int seam_sits_between_user_and_tool_blocks() {
    const auto session = m25_session_from_seed(530);
    const auto task = m25_task_from_seed(531);

    SeamLoopHarness harness(session, task);
    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 532);
    fill_eight_sections(snapshot, 940);
    MIRA_CHECK(store.put(snapshot).has_value());

    auto registration = make_wait_tool();
    auto registry = std::make_shared<BuiltinToolRegistry>();
    MIRA_CHECK(registry->register_tool(registration.spec, registration.handler));

    harness.script({tool_call_response(registration.spec, R"json({"duration_ms": 1})json"),
                    text_response(R"json({"action":"done","reason":"goal reached"})json")});
    WorkingContextSupplierHarness supplier_harness(store, session);
    auto loop = harness.make_loop();
    loop->set_tool_registry(registry);
    loop->set_working_context_supplier(supplier_harness.supplier());
    ModelDoneVerifier verifier;
    const auto context = harness.loop_context();
    const auto outcome = loop->run(harness.spec(), context, verifier);
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().outcome == LoopOutcome::Completed);

    const auto requests = harness.provider().requests();
    MIRA_CHECK(requests.size() == 2);
    // The tool round request carries both the seam block and the tool result
    // block, with the seam between the user context block and the tool block.
    const auto &tool_round = requests.back();
    const auto user_index = m25_find_item_source(tool_round, "mira.agent-loop.context.v1");
    const auto tool_index = m25_find_item_source(tool_round, "mira.agent-loop.tool-result.v1");
    const auto seam_indices = m25_seam_item_indices(tool_round);
    MIRA_CHECK(user_index.has_value());
    MIRA_CHECK(tool_index.has_value());
    MIRA_CHECK(seam_indices.size() == 1);
    MIRA_CHECK(user_index.value() < seam_indices.front());
    MIRA_CHECK(seam_indices.front() < tool_index.value());
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G1: identity gate — each mismatched leg skips injection with a visible
// diagnostic per skipped step, and never renders the foreign frame.
// ---------------------------------------------------------------------------

int identity_gate_skips_mismatched_frames() {
    const auto session = m25_session_from_seed(540);
    const auto task = m25_task_from_seed(541);

    const struct {
        const char *name;
        int leg; // 0 = session, 1 = task, 2 = task_epoch
    } legs[] = {{"session", 0}, {"task", 1}, {"epoch", 2}};

    for (const auto &leg : legs) {
        SeamLoopHarness harness(session, task);
        InMemoryWorkingContextStore store;
        auto snapshot = m25_snapshot(session, task, 1, 542 + static_cast<std::uint64_t>(leg.leg));
        fill_eight_sections(snapshot, 950);
        switch (leg.leg) {
        case 0:
            snapshot.session_id = m25_session_from_seed(599); // foreign session frame
            break;
        case 1:
            snapshot.task_id = m25_task_from_seed(598); // foreign task frame
            break;
        default:
            snapshot.task_epoch = snapshot.task_epoch + 1; // stale epoch
            break;
        }
        MIRA_CHECK(store.put(snapshot).has_value());

        // The supplier reads the chain the snapshot was stored under (its
        // own session id); the identity gate then compares that snapshot
        // against the loop's task frame.
        WorkingContextSupplierHarness supplier_harness(store, snapshot.session_id);
        auto loop = harness.make_loop();
        loop->set_working_context_supplier(supplier_harness.supplier());
        ModelDoneVerifier verifier;
        std::vector<ModelRequest> requests;
        MIRA_CHECK(run_two_step_loop(harness, *loop, verifier, requests) == 0);
        for (const auto &request : requests) {
            MIRA_CHECK(m25_seam_item_indices(request).empty());
        }
        // Skip is counted and visible on the existing event surface: one
        // diagnostic per skipped step.
        MIRA_CHECK(m25_working_context_diagnostic_count(harness.events(), session) ==
                   requests.size());
        // The supplier was still consulted exactly once per step.
        MIRA_CHECK(supplier_harness.calls() == requests.size());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G1: host-side environment-epoch gating closes inside the supplier
// callback; a mismatch is the normal empty state — zero items, zero
// diagnostics (the Loop holds no environment epoch and does no comparison).
// ---------------------------------------------------------------------------

int environment_epoch_gate_closes_in_supplier() {
    const auto session = m25_session_from_seed(550);
    const auto task = m25_task_from_seed(551);

    SeamLoopHarness harness(session, task);
    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 552);
    fill_eight_sections(snapshot, 960);
    MIRA_CHECK(snapshot.environment_epoch == 7);
    MIRA_CHECK(store.put(snapshot).has_value());

    // The host closure pins the environment epoch it observed; on mismatch
    // it returns the empty state instead of the snapshot.
    constexpr std::uint64_t host_environment_epoch = 8;
    WorkingContextSupplier gated_supplier =
        [&store, session,
         host_environment_epoch]() -> Result<std::optional<WorkingContextSnapshot>> {
        auto latest = store.latest(session);
        if (!latest.has_value()) {
            return latest;
        }
        if (latest.value().has_value() &&
            latest.value()->environment_epoch != host_environment_epoch) {
            return Result<std::optional<WorkingContextSnapshot>>(
                std::optional<WorkingContextSnapshot>{});
        }
        return latest;
    };
    auto loop = harness.make_loop();
    loop->set_working_context_supplier(std::move(gated_supplier));
    ModelDoneVerifier verifier;
    std::vector<ModelRequest> requests;
    MIRA_CHECK(run_two_step_loop(harness, *loop, verifier, requests) == 0);
    for (const auto &request : requests) {
        MIRA_CHECK(m25_seam_item_indices(request).empty());
    }
    // Zero Loop-side special-casing: no diagnostic for the host-gated empty
    // state.
    MIRA_CHECK(m25_working_context_diagnostic_count(harness.events(), session) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G2: options bounds.
// ---------------------------------------------------------------------------

int seam_options_validate_bounds() {
    WorkingContextSeamOptions defaults;
    MIRA_CHECK(defaults.max_items == 32);
    MIRA_CHECK(defaults.max_chars == 8'192);
    MIRA_CHECK(defaults.validate().has_value());

    WorkingContextSeamOptions zero_items;
    zero_items.max_items = 0;
    MIRA_CHECK(!zero_items.validate().has_value());

    WorkingContextSeamOptions zero_chars;
    zero_chars.max_chars = 0;
    MIRA_CHECK(!zero_chars.validate().has_value());
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G2: max_items truncation keeps the fixed conversion order and appends
// the frozen marker.
// ---------------------------------------------------------------------------

int max_items_truncation_is_fixed_order() {
    const auto session = m25_session_from_seed(560);
    const auto task = m25_task_from_seed(561);

    SeamLoopHarness harness(session, task);
    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 562);
    for (std::uint64_t index = 1; index <= 5; ++index) {
        snapshot.constraints.push_back(
            m25_item("m25 bounded item " + std::to_string(index), 970 + index, 0.9));
    }
    MIRA_CHECK(store.put(snapshot).has_value());

    WorkingContextSeamOptions options;
    options.max_items = 3;
    MIRA_CHECK(options.validate().has_value());
    WorkingContextSupplierHarness supplier_harness(store, session);
    auto loop = harness.make_loop();
    loop->set_working_context_supplier(supplier_harness.supplier(), options);
    ModelDoneVerifier verifier;
    std::vector<ModelRequest> requests;
    MIRA_CHECK(run_two_step_loop(harness, *loop, verifier, requests) == 0);

    for (const auto &request : requests) {
        const auto text = m25_seam_text(request);
        // First three conversion-order entries survive, in order.
        std::size_t position = 0;
        for (const auto index : {1, 2, 3}) {
            const auto marker = "m25 bounded item " + std::to_string(index);
            const auto found = text.find(marker, position);
            MIRA_CHECK(found != std::string::npos);
            position = found + 1;
        }
        // Beyond the bound is dropped, and the frozen marker is appended.
        MIRA_CHECK(text.find("m25 bounded item 4") == std::string::npos);
        MIRA_CHECK(text.find("m25 bounded item 5") == std::string::npos);
        MIRA_CHECK(text.find("[working context truncated]") != std::string::npos);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G2: max_chars truncation bounds the rendered bytes; under the bound no
// marker appears.
// ---------------------------------------------------------------------------

int max_chars_truncation_bounds_rendering() {
    const auto session = m25_session_from_seed(570);
    const auto task = m25_task_from_seed(571);

    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 572);
    const std::string long_content(160, 'x');
    snapshot.constraints.push_back(m25_item("m25 long A " + long_content, 981, 0.9));
    snapshot.constraints.push_back(m25_item("m25 long B " + long_content, 982, 0.9));
    snapshot.constraints.push_back(m25_item("m25 long C " + long_content, 983, 0.9));

    // Under the default bound the marker never appears (negative control).
    {
        SeamLoopHarness harness(session, task);
        MIRA_CHECK(store.put(snapshot).has_value());
        WorkingContextSupplierHarness supplier_harness(store, session);
        auto loop = harness.make_loop();
        loop->set_working_context_supplier(supplier_harness.supplier());
        ModelDoneVerifier verifier;
        std::vector<ModelRequest> requests;
        MIRA_CHECK(run_two_step_loop(harness, *loop, verifier, requests) == 0);
        for (const auto &request : requests) {
            const auto text = m25_seam_text(request);
            MIRA_CHECK(text.find("[working context truncated]") == std::string::npos);
        }
        MIRA_CHECK(store.erase_session(session, "m25 negative-control reset").has_value());
    }

    // A tight bound truncates in fixed order and appends the marker.
    {
        SeamLoopHarness harness(session, task);
        MIRA_CHECK(store.put(snapshot).has_value());
        WorkingContextSeamOptions options;
        options.max_chars = 256;
        MIRA_CHECK(options.validate().has_value());
        WorkingContextSupplierHarness supplier_harness(store, session);
        auto loop = harness.make_loop();
        loop->set_working_context_supplier(supplier_harness.supplier(), options);
        ModelDoneVerifier verifier;
        std::vector<ModelRequest> requests;
        MIRA_CHECK(run_two_step_loop(harness, *loop, verifier, requests) == 0);
        for (const auto &request : requests) {
            const auto text = m25_seam_text(request);
            MIRA_CHECK(text.find("m25 long A") != std::string::npos);
            MIRA_CHECK(text.find("[working context truncated]") != std::string::npos);
            // The rendered seam stays at the byte bound plus the fixed
            // marker/label overhead (RULE-08 budget holds on the wire).
            MIRA_CHECK(text.size() <= options.max_chars + 512);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G2: supplier failure degrades — no seam items that step, exactly one
// diagnostic, the loop continues, and the next step injects again.
// ---------------------------------------------------------------------------

int supplier_error_degrades_and_recovers() {
    const auto session = m25_session_from_seed(580);
    const auto task = m25_task_from_seed(581);

    SeamLoopHarness harness(session, task);
    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 582);
    fill_eight_sections(snapshot, 990);
    MIRA_CHECK(store.put(snapshot).has_value());

    WorkingContextSupplierHarness supplier_harness(store, session);
    supplier_harness.set_script(
        {WorkingContextSupplierHarness::Mode::Error, WorkingContextSupplierHarness::Mode::Store});
    auto loop = harness.make_loop();
    loop->set_working_context_supplier(supplier_harness.supplier());
    ModelDoneVerifier verifier;
    std::vector<ModelRequest> requests;
    MIRA_CHECK(run_two_step_loop(harness, *loop, verifier, requests) == 0);

    MIRA_CHECK(m25_seam_item_indices(requests[0]).empty());
    MIRA_CHECK(m25_seam_item_indices(requests[1]).size() == 1);
    // Degradation is visible exactly once and never blocks the loop.
    MIRA_CHECK(m25_working_context_diagnostic_count(harness.events(), session) == 1);
    MIRA_CHECK(supplier_harness.calls() == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// HI-G2: supplier exceptions are isolated — neither a std exception nor a
// non-std throw escapes into request assembly or terminates the loop.
// ---------------------------------------------------------------------------

int supplier_exception_is_isolated() {
    const auto session = m25_session_from_seed(590);
    const auto task = m25_task_from_seed(591);

    SeamLoopHarness harness(session, task);
    InMemoryWorkingContextStore store;
    auto snapshot = m25_snapshot(session, task, 1, 592);
    fill_eight_sections(snapshot, 1000);
    MIRA_CHECK(store.put(snapshot).has_value());

    WorkingContextSupplierHarness supplier_harness(store, session);
    supplier_harness.set_script({WorkingContextSupplierHarness::Mode::ThrowStd,
                                 WorkingContextSupplierHarness::Mode::ThrowInt,
                                 WorkingContextSupplierHarness::Mode::Store});
    harness.script({text_response(decision_body("tap", 0.4, 0.4)),
                    text_response(decision_body("tap", 0.6, 0.6)),
                    text_response(R"json({"action":"done","reason":"goal reached"})json")});
    auto loop = harness.make_loop();
    loop->set_working_context_supplier(supplier_harness.supplier());
    ModelDoneVerifier verifier;
    const auto context = harness.loop_context();
    const auto outcome = loop->run(harness.spec(), context, verifier);
    MIRA_CHECK(outcome.has_value());
    MIRA_CHECK(outcome.value().outcome == LoopOutcome::Completed);

    const auto requests = harness.provider().requests();
    MIRA_CHECK(requests.size() == 3);
    MIRA_CHECK(m25_seam_item_indices(requests[0]).empty());
    MIRA_CHECK(m25_seam_item_indices(requests[1]).empty());
    MIRA_CHECK(m25_seam_item_indices(requests[2]).size() == 1);
    // One diagnostic per degraded step (both exception kinds), recovery on
    // the third.
    MIRA_CHECK(m25_working_context_diagnostic_count(harness.events(), session) == 2);
    MIRA_CHECK(supplier_harness.calls() == 3);
    return 0;
}

} // namespace

int main() {
    const struct {
        const char *name;
        int (*fn)();
    } cases[] = {
        {"no_supplier_and_empty_store_are_zero_drift", no_supplier_and_empty_store_are_zero_drift},
        {"aligned_snapshot_is_injected", aligned_snapshot_is_injected},
        {"seam_rendering_is_byte_deterministic", seam_rendering_is_byte_deterministic},
        {"seam_sits_between_user_and_tool_blocks", seam_sits_between_user_and_tool_blocks},
        {"identity_gate_skips_mismatched_frames", identity_gate_skips_mismatched_frames},
        {"environment_epoch_gate_closes_in_supplier", environment_epoch_gate_closes_in_supplier},
        {"seam_options_validate_bounds", seam_options_validate_bounds},
        {"max_items_truncation_is_fixed_order", max_items_truncation_is_fixed_order},
        {"max_chars_truncation_bounds_rendering", max_chars_truncation_bounds_rendering},
        {"supplier_error_degrades_and_recovers", supplier_error_degrades_and_recovers},
        {"supplier_exception_is_isolated", supplier_exception_is_isolated},
    };
    for (const auto &entry : cases) {
        if (const int code = entry.fn(); code != 0) {
            std::cerr << "m25_loop_seam_test: case failed: " << entry.name << '\n';
            return code;
        }
    }
    return 0;
}
