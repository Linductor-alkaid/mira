// M22 (DEC-035 Stage W3) Working Context auto-trigger eval harness. Builds
// the frozen synthetic signal dataset (SplitMix64, seed "MIR22W3C", 8
// sessions x 40 signals with 0-10 conversation advance and 0-12 reported
// execution events per signal, checkpoints minted at deterministic watermark
// markers, a zero-advance/high-event stretch and a checkpoint gap per
// session), drives the frozen pipeline (WorkingContextAutoCurator ->
// ProviderContextCurator + scripted deterministic provider ->
// schedule_working_context_curate -> commit_working_context) and asserts the
// pre-frozen gates W3-G1..G6 from milestone §4.3, printing a JSON report to
// argv[1] (stdout summary either way). Metrics measure deterministic pipeline
// behaviour (trigger fidelity against the pure function, coalescing, forced
// flush, failure fallback, previous selection, shutdown, cross-run
// determinism), never semantic curation quality (RULE-10). Everything the
// report contains is process-independent, so two runs compare byte for byte
// (`cmp`).
//
// Known implementation deviation kept red on purpose: fire/forced-flush
// shared futures reach the caller without associated state (double share()
// inside WorkingContextAutoCurator::Impl::issue), so caller-side consumption
// is recorded as caller_future_defects and folds into gate W3-G3 (and the
// failure-code checks of W3-G4) instead of aborting the run.

#include "../support/m3_support.hpp"
#include "../support/test.hpp"

#include <mira/context_consolidation.hpp>
#include <mira/context_curator.hpp>
#include <mira/context_memory_supervisor.hpp>
#include <mira/context_working_context.hpp>
#include <mira/context_working_context_auto.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>
#include <mira/model_provider.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Frozen configuration (M22 §4.3, frozen before the first evaluation run)
// ---------------------------------------------------------------------------

struct FrozenConfig final {
    std::uint64_t seed = 0x4d49'5232'3257'3343ULL; // "MIR22W3C"
    std::size_t sessions = 8;
    std::size_t signals_per_session = 40;
    std::uint64_t watermark_interval = 8;
    std::uint64_t event_count_interval = 16;
    std::size_t phase_length = 10; // 4 phases: normal / stall / gap / normal
    std::uint64_t mint_stride = 6; // checkpoint marker in the normal phases
    std::uint64_t task_epoch = 3;
    std::uint64_t environment_epoch = 7;
    // Pinned once the generator was frozen; the harness asserts equality so
    // the dataset is checked, never assumed (M17-M21 style).
    std::string dataset_digest = "6f2ab2e57b93e1d66846100f92b91563a83f5ffd1f05b5dbf6682dc5f4911535";
};

[[nodiscard]] std::string digest_hex(const Sha256Digest &digest) {
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    for (const auto byte : digest.bytes) {
        std::ostringstream slot;
        slot << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
        text += slot.str();
    }
    return text;
}

struct SplitMix64 final {
    std::uint64_t state = 0;
    [[nodiscard]] std::uint64_t next() {
        state += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t mixed = state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return mixed ^ (mixed >> 31U);
    }
};

[[nodiscard]] Id128 id_from(SplitMix64 &rng, std::string_view tag) {
    const std::uint64_t first = rng.next();
    const std::uint64_t second = rng.next();
    Id128::Bytes bytes{};
    std::memcpy(bytes.data(), &first, sizeof(first));
    std::memcpy(bytes.data() + sizeof(first), &second, sizeof(second));
    for (std::size_t index = 0; index < tag.size() && index < bytes.size(); ++index) {
        bytes[index] ^= static_cast<std::uint8_t>(tag[index]);
    }
    return Id128{bytes};
}

[[nodiscard]] std::string rare_token(SplitMix64 &rng) {
    static constexpr std::string_view kAlphabet = "abcdefghjkmnpqrstuvwxyz23456789";
    std::string token(6, 'a');
    for (auto &character : token) {
        character = kAlphabet[static_cast<std::size_t>(rng.next() % kAlphabet.size())];
    }
    return token;
}

// ---------------------------------------------------------------------------
// Frozen dataset: deterministic signal plans with minted checkpoint markers
// ---------------------------------------------------------------------------

struct RecentSpec final {
    std::string text;
    EventId origin;
    std::uint64_t sequence = 0;
};

struct SignalSpec final {
    std::uint64_t advance = 0;              // conversation advance at this signal
    std::uint64_t events = 0;               // reported execution-event delta
    std::uint64_t watermark = 0;            // cumulative conversation watermark
    bool mints = false;                     // a checkpoint is committed here
    std::uint64_t checkpoint_watermark = 0; // carried checkpoint watermark
    std::uint64_t checkpoint_revision = 0;  // revision the carried checkpoint was minted at
    std::vector<RecentSpec> recent;         // bounded tail carried with the checkpoint
};

struct EvalSession final {
    SessionId session;
    TaskId task;
    std::string token;
    std::vector<SignalSpec> signals;
};

struct Dataset final {
    std::vector<EvalSession> sessions;
    std::string digest_hex_text;
};

[[nodiscard]] Dataset build_dataset(const FrozenConfig &config) {
    SplitMix64 rng{config.seed};
    Dataset dataset;

    for (std::size_t session_index = 0; session_index < config.sessions; ++session_index) {
        EvalSession eval_session;
        eval_session.session = SessionId{id_from(rng, "session")};
        eval_session.task = TaskId{id_from(rng, "task")};
        eval_session.token = rare_token(rng);

        SplitMix64 local{rng.next()};
        // All local draws are explicitly sequenced scalars: expression-level
        // evaluation order of two rng calls is unspecified and would fork the
        // dataset between compilers (M19 lesson a2c4c0d).
        std::vector<EventId> events;
        events.reserve(config.phase_length * config.signals_per_session);
        for (std::size_t index = 0; index < config.signals_per_session * 2; ++index) {
            events.push_back(EventId{id_from(local, "event")});
        }
        std::vector<std::size_t> order(events.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        for (std::size_t bound = order.size(); bound > 1; --bound) {
            const std::size_t swap_index = static_cast<std::size_t>(local.next() % bound);
            std::swap(order[bound - 1], order[swap_index]);
        }

        std::uint64_t watermark = 0;
        std::uint64_t last_mint = 0;
        std::uint64_t last_mint_revision = 0;
        for (std::size_t signal_index = 0; signal_index < config.signals_per_session;
             ++signal_index) {
            const std::uint64_t base_advance = local.next() % 11;
            const std::uint64_t base_events = local.next() % 13;
            const std::size_t phase = signal_index / config.phase_length;
            std::uint64_t advance = base_advance;
            std::uint64_t events_delta = base_events;
            if (signal_index == 0) {
                advance = 4 + (base_advance % 7); // first signal always mintable
            }
            if (phase == 1) {
                if (signal_index <= 14) {
                    // Zero-advance, high-execution-event stretch.
                    advance = 0;
                    events_delta = 10 + (base_events % 3);
                } else if (signal_index <= 18) {
                    // Small mint steps under continued event pressure.
                    advance = 1 + (base_advance % 2);
                    events_delta = 10 + (base_events % 3);
                } else {
                    // Guaranteed settling fire that closes the stall.
                    advance = 1 + (base_advance % 2);
                    events_delta = 16;
                }
            }
            if (phase == 2) {
                // Checkpoint gap: the conversation advances but the host
                // commits nothing, so every signal carries the stale
                // checkpoint and the unsettled gate blocks every decision.
                advance = 1 + (base_advance % 10);
            }
            watermark += advance;
            bool mints = false;
            if (phase != 2 && watermark > last_mint &&
                (signal_index == 0 || watermark - last_mint >= config.mint_stride ||
                 (phase == 1 && signal_index >= 15))) {
                mints = true;
            }
            if (mints) {
                last_mint = watermark;
                last_mint_revision = signal_index + 1;
            }
            SignalSpec signal;
            signal.advance = advance;
            signal.events = events_delta;
            signal.watermark = watermark;
            signal.mints = mints;
            signal.checkpoint_watermark = last_mint;
            signal.checkpoint_revision = last_mint_revision;
            for (std::size_t slot = 0; slot < 2; ++slot) {
                const std::size_t position = order[(signal_index * 2 + slot) % order.size()];
                RecentSpec recent;
                recent.text = "event " + eval_session.token + " pos" + std::to_string(position);
                recent.origin = events[position];
                const std::uint64_t base =
                    signal.checkpoint_watermark >= 2 ? signal.checkpoint_watermark - 1 : 1;
                recent.sequence = base + slot;
                signal.recent.push_back(std::move(recent));
            }
            eval_session.signals.push_back(std::move(signal));
        }
        dataset.sessions.push_back(std::move(eval_session));
    }

    // Dataset digest over sorted structural lines (advance/events/mint
    // markers/recent texts and sequences; no raw id bytes so the anchor stays
    // endian-independent, M21 methodology).
    std::vector<std::string> lines;
    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        const auto &eval_session = dataset.sessions[session_index];
        for (std::size_t signal_index = 0; signal_index < eval_session.signals.size();
             ++signal_index) {
            const auto &signal = eval_session.signals[signal_index];
            std::string recent;
            for (const auto &entry : signal.recent) {
                recent += entry.text + "@" + std::to_string(entry.sequence) + ",";
            }
            lines.push_back(
                "s" + std::to_string(session_index) + "|n" + std::to_string(signal_index) +
                "|adv=" + std::to_string(signal.advance) + "|evt=" + std::to_string(signal.events) +
                "|ckpt=" + std::to_string(signal.checkpoint_watermark) +
                "|rev=" + std::to_string(signal.checkpoint_revision) +
                "|mint=" + (signal.mints ? "1" : "0") + "|recent=" + recent);
        }
    }
    std::sort(lines.begin(), lines.end());
    std::string joined;
    for (const auto &line : lines) {
        joined += line;
        joined += '\n';
    }
    dataset.digest_hex_text = digest_hex(digest_string(joined));
    return dataset;
}

// ---------------------------------------------------------------------------
// Deterministic builders for the eval rounds (two constraints, one decision,
// one thread, two recent entries — one scripted output policy covers every
// committing fire).
// ---------------------------------------------------------------------------

[[nodiscard]] ConversationStatement make_statement(const std::string &content,
                                                   const EventId &origin, std::uint64_t sequence) {
    ConversationStatement statement;
    statement.content = content;
    statement.source_events = {origin};
    statement.source_sequence = sequence;
    statement.confidence = 0.9;
    return statement;
}

[[nodiscard]] ConversationCheckpoint
make_eval_checkpoint(const SessionId &session, const TaskId &task, std::uint64_t watermark,
                     std::uint64_t revision, std::uint64_t environment_epoch) {
    ConversationCheckpoint checkpoint;
    checkpoint.id = conversation_checkpoint_id_from_seed(
        session.to_string() + "|" + std::to_string(watermark) + "|" + std::to_string(revision));
    checkpoint.session_id = session;
    checkpoint.task_id = task;
    checkpoint.task_epoch = 3;
    checkpoint.environment_epoch = environment_epoch;
    checkpoint.through_event_sequence = watermark;
    checkpoint.created_at = Timestamp::now();
    const std::uint64_t seed_base = watermark * 31 + revision;
    SplitMix64 constraint_rng{seed_base + 1};
    SplitMix64 constraint_rng2{seed_base + 2};
    SplitMix64 decision_rng{seed_base + 3};
    SplitMix64 thread_rng{seed_base + 4};
    checkpoint.constraints.push_back(
        make_statement("constraint r" + std::to_string(revision) + " confirm before sending",
                       EventId{id_from(constraint_rng, "ev")}, watermark - 2));
    checkpoint.constraints.push_back(
        make_statement("constraint r" + std::to_string(revision) + " keep the quota thread open",
                       EventId{id_from(constraint_rng2, "ev")}, watermark - 1));
    checkpoint.decisions.push_back(
        make_statement("decision r" + std::to_string(revision) + " use the batch provider",
                       EventId{id_from(decision_rng, "ev")}, watermark - 2));
    checkpoint.unresolved_threads.push_back(
        make_statement("thread r" + std::to_string(revision) + " waiting for the quota reply",
                       EventId{id_from(thread_rng, "ev")}, watermark - 1));
    checkpoint.summary = "revision " + std::to_string(revision);
    checkpoint.confidence = 0.9;
    for (const auto *statements :
         {&checkpoint.constraints, &checkpoint.decisions, &checkpoint.unresolved_threads}) {
        for (const auto &statement : *statements) {
            for (const auto &origin : statement.source_events) {
                if (std::find(checkpoint.source_events.begin(), checkpoint.source_events.end(),
                              origin) == checkpoint.source_events.end()) {
                    checkpoint.source_events.push_back(origin);
                }
            }
        }
    }
    return checkpoint;
}

[[nodiscard]] WorkingContextRefreshInput
make_refresh_input(const ConversationCheckpoint &checkpoint, const WorkingContextIdentity &identity,
                   const WorkingContextCommitState &live, std::uint64_t reported_events,
                   std::vector<ConversationSegmentEntry> recent) {
    WorkingContextRefreshInput input;
    input.checkpoint = checkpoint;
    input.identity = identity;
    input.live = live;
    input.reported_events = reported_events;
    input.recent_events = std::move(recent);
    return input;
}

[[nodiscard]] WorkingContextIdentity make_identity(const EvalSession &eval_session,
                                                   const FrozenConfig &config,
                                                   std::uint64_t environment_epoch) {
    WorkingContextIdentity identity;
    identity.task = eval_session.task;
    identity.task_epoch = config.task_epoch;
    identity.environment_epoch = environment_epoch;
    return identity;
}

[[nodiscard]] WorkingContextCommitState make_live(const EvalSession &eval_session,
                                                  const FrozenConfig &config,
                                                  std::uint64_t environment_epoch) {
    WorkingContextCommitState live;
    live.session = eval_session.session;
    live.task = eval_session.task;
    live.task_epoch = config.task_epoch;
    live.environment_epoch = environment_epoch;
    return live;
}

[[nodiscard]] std::vector<ConversationSegmentEntry> recent_entries(const SignalSpec &signal) {
    std::vector<ConversationSegmentEntry> entries;
    entries.reserve(signal.recent.size());
    for (const auto &spec : signal.recent) {
        ConversationSegmentEntry entry;
        entry.text = spec.text;
        entry.origin = spec.origin;
        entry.session_sequence = spec.sequence;
        entries.push_back(std::move(entry));
    }
    return entries;
}
// Two deterministic recent entries bounded by the given checkpoint watermark
// (sequences never run past it, so the curator's input pre-check passes on
// every round).
[[nodiscard]] std::vector<ConversationSegmentEntry> pair_recent(std::uint64_t watermark,
                                                                std::uint64_t salt) {
    std::vector<ConversationSegmentEntry> entries;
    entries.reserve(2);
    for (std::size_t slot = 0; slot < 2; ++slot) {
        SplitMix64 origin_rng{salt * 131 + slot + 1};
        ConversationSegmentEntry entry;
        entry.text = "round recent " + std::to_string(salt) + " slot" + std::to_string(slot);
        entry.origin = EventId{id_from(origin_rng, "recent")};
        const std::uint64_t base = watermark >= 2 ? watermark - 1 : 1;
        entry.session_sequence = base + slot;
        entries.push_back(std::move(entry));
    }
    return entries;
}

// ---------------------------------------------------------------------------
// Deterministic gate (absorb / coexistence / shutdown rounds): entry is
// observable, both sides block on one mutex/cv.
// ---------------------------------------------------------------------------

struct CuratorGate final {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t entered_count = 0;
    bool released = false;

    void wait_at_gate() {
        std::unique_lock lock(mutex);
        ++entered_count;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
    }

    void wait_entered(std::size_t count) {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this, count] { return entered_count >= count; });
    }

    void release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        cv.notify_all();
    }

    // Re-arms the gate for the next scenario iteration. Only legal when no
    // op is inside wait_at_gate anymore — every prior hold must have been
    // released and awaited, which the scenario rounds guarantee.
    void reset() {
        std::lock_guard lock(mutex);
        released = false;
    }
};

struct GateReleaser final {
    CuratorGate &gate;
    ~GateReleaser() { gate.release(); }
};

// ---------------------------------------------------------------------------
// Scripted deterministic provider: valid curated output from the frozen
// policy, deterministic gate/park choreography, and the five failure classes.
// ---------------------------------------------------------------------------

class ScriptedCuratorProvider final : public IModelProvider {
  public:
    enum class Behavior {
        Ok,
        ProviderError,
        MalformedJson,
        Refusal,
        SleepPastDeadline,
        ReturnCancelled,
        Gate,
    };

    ScriptedCuratorProvider()
        : profile_(std::make_shared<ModelProfile>(mira::testing::make_profile(
              ProtocolDialect::OpenAIResponsesV1, "https://m22-auto-eval.test"))) {}

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }

    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                              const OperationContext &context,
                                              const ProviderInferOptions &) override {
        calls_.fetch_add(1, std::memory_order_release);
        {
            std::lock_guard lock(transcript_mutex_);
            const auto *text = std::get_if<TextPart>(&request.input[1].content[0]);
            last_transcript_ = text != nullptr ? text->text : std::string{};
        }
        // Guard the numbering mirror: the transcript must present exactly the
        // number of entries the script computed for this call. Disabled
        // (expected_entries_ == 0) for the failure-injection behaviors.
        if (expected_entries_ != 0 &&
            last_transcript_.find("; " + std::to_string(expected_entries_) + " numbered entries") ==
                std::string::npos) {
            Error mismatch;
            mismatch.code = ErrorCode::Internal;
            mismatch.domain = "test";
            mismatch.safe_message = "scripted provider saw an unexpected transcript size";
            return mismatch;
        }
        if (behavior_ == Behavior::SleepPastDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        if (behavior_ == Behavior::Gate && gate_ != nullptr) {
            gate_->wait_at_gate();
            if (park_after_gate_) {
                // Cooperative cancellation hold: only the supervisor's stop
                // flag (or an explicit let-go) can end it, so begin_shutdown
                // always has a drainable path.
                while (!context.cancelled() && !let_go_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (context.cancelled()) {
                    Error cancelled;
                    cancelled.code = ErrorCode::Cancelled;
                    cancelled.domain = "test";
                    cancelled.safe_message = "scripted provider observed cancellation";
                    return cancelled;
                }
            }
        }
        if (behavior_ == Behavior::ProviderError) {
            Error unavailable;
            unavailable.code = ErrorCode::Unavailable;
            unavailable.domain = "test";
            unavailable.safe_message = "provider down";
            return unavailable;
        }
        if (behavior_ == Behavior::ReturnCancelled) {
            Error cancelled;
            cancelled.code = ErrorCode::Cancelled;
            cancelled.domain = "test";
            cancelled.safe_message = "provider observed cancellation";
            return cancelled;
        }
        ModelResponse response;
        response.contract_version = SchemaVersion{1, 0};
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = profile_->id;
        response.requested_model = profile_->model_selector;
        response.status = ModelCompletionStatus::Completed;
        MessageOutput message;
        if (behavior_ == Behavior::Refusal) {
            OutputRefusalPart refusal;
            refusal.safe_summary = "cannot help with that";
            message.content.emplace_back(std::move(refusal));
        } else {
            OutputTextPart text;
            text.text = behavior_ == Behavior::MalformedJson ? "this is not json" : response_;
            message.content.emplace_back(std::move(text));
        }
        response.output.emplace_back(std::move(message));
        return response;
    }

    void set_script(std::string response, std::size_t expected_entries) {
        response_ = std::move(response);
        expected_entries_ = expected_entries;
    }
    void set_behavior(Behavior behavior) { behavior_ = behavior; }
    void arm_gate(CuratorGate &gate) { gate_ = &gate; }
    void set_park_after_gate(bool park) { park_after_gate_ = park; }
    void let_go() { let_go_ = true; }

    [[nodiscard]] std::string last_transcript() const {
        std::lock_guard lock(transcript_mutex_);
        return last_transcript_;
    }
    [[nodiscard]] std::uint64_t calls() const { return calls_.load(std::memory_order_acquire); }

  private:
    std::shared_ptr<ModelProfile> profile_;
    Behavior behavior_ = Behavior::Ok;
    std::string response_ = "{}";
    std::size_t expected_entries_ = 0;
    CuratorGate *gate_ = nullptr;
    bool park_after_gate_ = false;
    std::atomic<bool> let_go_{false};
    mutable std::mutex transcript_mutex_;
    std::string last_transcript_;
    std::atomic<std::uint64_t> calls_{0};
};

// ---------------------------------------------------------------------------
// Transcript layout mirror (frozen M21 §4.1 three-block numbering) and the
// scripted output policy (retain / supersede / add, all citations legal).
// ---------------------------------------------------------------------------

constexpr std::size_t kSectionCount = 8;

[[nodiscard]] std::array<const std::vector<WorkingContextItem> *, kSectionCount>
previous_sections(const WorkingContextSnapshot &snapshot) {
    return {&snapshot.constraints,    &snapshot.decisions,      &snapshot.open_issues,
            &snapshot.active_tasks,   &snapshot.verified_facts, &snapshot.failed_attempts,
            &snapshot.important_refs, &snapshot.next_actions};
}

struct TranscriptLayout final {
    std::size_t prev_base[kSectionCount] = {};
    std::size_t prev_count[kSectionCount] = {};
    std::size_t prev_total = 0;
    std::size_t ckpt_base[3] = {};
    std::size_t ckpt_counts[3] = {};
    std::size_t event_base = 0;
    std::size_t total = 0;
};

[[nodiscard]] TranscriptLayout layout_for(const WorkingContextSnapshot *previous,
                                          const ConversationCheckpoint &checkpoint,
                                          std::size_t event_count) {
    TranscriptLayout layout;
    std::size_t cursor = 0;
    if (previous != nullptr) {
        const auto sections = previous_sections(*previous);
        for (std::size_t section = 0; section < kSectionCount; ++section) {
            layout.prev_base[section] = cursor;
            layout.prev_count[section] = sections[section]->size();
            cursor += sections[section]->size();
        }
    }
    layout.prev_total = cursor;
    layout.ckpt_counts[0] = checkpoint.constraints.size();
    layout.ckpt_counts[1] = checkpoint.decisions.size();
    layout.ckpt_counts[2] = checkpoint.unresolved_threads.size();
    layout.ckpt_base[0] = cursor;
    cursor += layout.ckpt_counts[0];
    layout.ckpt_base[1] = cursor;
    cursor += layout.ckpt_counts[1];
    layout.ckpt_base[2] = cursor;
    cursor += layout.ckpt_counts[2];
    layout.event_base = cursor;
    layout.total = cursor + event_count;
    return layout;
}

struct ScriptedItem final {
    std::string content;
    std::vector<std::size_t> sources;
    double confidence = 0.9;
};

[[nodiscard]] std::string items_json(const std::vector<ScriptedItem> &items) {
    std::string joined;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index > 0) {
            joined += ",";
        }
        std::string sources;
        for (std::size_t source = 0; source < items[index].sources.size(); ++source) {
            if (source > 0) {
                sources += ",";
            }
            sources += std::to_string(items[index].sources[source]);
        }
        joined += "{\"content\":\"" + items[index].content + "\",\"sources\":[" + sources +
                  "],\"confidence\":" + std::to_string(items[index].confidence) + "}";
    }
    return "[" + joined + "]";
}

[[nodiscard]] std::string output_json(const std::vector<ScriptedItem> (&sections)[kSectionCount]) {
    static constexpr const char *kOutputKeys[kSectionCount] = {
        "constraints",    "decisions",       "open_issues",    "active_tasks",
        "verified_facts", "failed_attempts", "important_refs", "next_actions"};
    std::string json = "{\"confidence\":0.95";
    for (std::size_t section = 0; section < kSectionCount; ++section) {
        json += std::string(",\"") + kOutputKeys[section] + "\":" + items_json(sections[section]);
    }
    json += "}";
    return json;
}

// The frozen scripted policy. Fresh chain: every checkpoint statement is
// carried with its own citation. Incremental: retain all previous constraints,
// supersede the previous decision citing old + new, keep the previous first
// thread beside the new one, and fill the Curator sections from
// checkpoint/event citations only (retain/supersede/add merge relations all
// visible, M21 §4.3).
[[nodiscard]] std::string plan_output(const WorkingContextSnapshot *previous,
                                      const ConversationCheckpoint &checkpoint,
                                      std::size_t event_count, const std::string &tag,
                                      TranscriptLayout &layout_out) {
    const TranscriptLayout layout = layout_for(previous, checkpoint, event_count);
    layout_out = layout;
    std::vector<ScriptedItem> sections[kSectionCount];
    if (previous == nullptr) {
        for (std::size_t index = 0; index < checkpoint.constraints.size(); ++index) {
            sections[0].push_back(
                {checkpoint.constraints[index].content, {layout.ckpt_base[0] + index}, 0.9});
        }
        for (std::size_t index = 0; index < checkpoint.decisions.size(); ++index) {
            sections[1].push_back(
                {checkpoint.decisions[index].content, {layout.ckpt_base[1] + index}, 0.9});
        }
        for (std::size_t index = 0; index < checkpoint.unresolved_threads.size(); ++index) {
            sections[2].push_back(
                {checkpoint.unresolved_threads[index].content, {layout.ckpt_base[2] + index}, 0.9});
        }
    } else {
        const auto previous_all = previous_sections(*previous);
        for (std::size_t index = 0; index < previous_all[0]->size(); ++index) {
            sections[0].push_back({(*previous_all[0])[index].content,
                                   {layout.prev_base[0] + index},
                                   (*previous_all[0])[index].confidence});
        }
        sections[0].push_back({checkpoint.constraints.front().content, {layout.ckpt_base[0]}, 0.9});
        sections[1].push_back({"decision superseded " + tag + " (replaces the prior decision)",
                               {layout.prev_base[1], layout.ckpt_base[1]},
                               0.9});
        sections[2].push_back({(*previous_all[2]).front().content,
                               {layout.prev_base[2]},
                               (*previous_all[2]).front().confidence});
        sections[2].push_back(
            {checkpoint.unresolved_threads.front().content, {layout.ckpt_base[2]}, 0.9});
    }
    sections[3].push_back({"active task " + tag, {layout.ckpt_base[1]}, 0.9});
    sections[4].push_back({"verified fact " + tag, {layout.ckpt_base[0] + 1}, 0.9});
    sections[5].push_back({"failed attempt " + tag, {layout.event_base}, 0.9});
    sections[6].push_back({"important ref " + tag, {layout.ckpt_base[2]}, 0.9});
    sections[7].push_back({"next action alpha " + tag, {layout.event_base + 1}, 0.9});
    sections[7].push_back({"next action beta " + tag, {layout.ckpt_base[0]}, 0.9});
    return output_json(sections);
}

// ---------------------------------------------------------------------------
// Coordinator-side helpers: caller-future defect accounting and deterministic
// settlement waits (no sleeps; the supervised work always settles).
// ---------------------------------------------------------------------------

using AutoFuture = std::shared_future<Result<WorkingContextCommitOutcome>>;

[[nodiscard]] Result<WorkingContextCommitOutcome> consume_caller_future(const AutoFuture &future,
                                                                        std::uint64_t &defects) {
    try {
        return future.get();
    } catch (const std::future_error &) {
        ++defects;
        Error broken;
        broken.code = ErrorCode::Internal;
        broken.domain = "test.m22.eval";
        broken.safe_message = "caller shared_future has no associated state";
        return Result<WorkingContextCommitOutcome>(broken);
    }
}

[[nodiscard]] bool await_settlement(WorkingContextAutoCurator &curator, const SessionId &session) {
    for (int guard = 0; guard < 100'000'000; ++guard) {
        curator.drain(session);
        const auto view = curator.session_view(session);
        if (!view.has_value() || !view->in_flight) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

[[nodiscard]] std::string normalized_json(const WorkingContextSnapshot &snapshot) {
    WorkingContextSnapshot copy = snapshot;
    copy.created_at = Timestamp{};
    copy.generated_by = ModelProfileId{};
    return to_json_string(working_context_to_json(copy));
}

[[nodiscard]] std::uint64_t store_latest_watermark(IWorkingContextStore &store,
                                                   const SessionId &session) {
    const auto latest = store.latest(session);
    if (!latest.has_value() || !latest.value().has_value()) {
        return 0;
    }
    return latest.value()->through_event_sequence;
}
// Sets the real deterministic script for one committing fire (the transcript
// guard is active, so the plan must match the previous snapshot the
// coordinator will pass), fires, consumes the caller future and settles.
// Returns the committed snapshot (nullopt when the fire did not commit).
[[nodiscard]] std::optional<WorkingContextSnapshot>
scripted_fire(WorkingContextAutoCurator &auto_curator, ScriptedCuratorProvider &provider,
              IWorkingContextStore &store, const SessionId &session,
              const ConversationCheckpoint &checkpoint, const WorkingContextIdentity &identity,
              const WorkingContextCommitState &live, std::uint64_t reported_events,
              const std::string &tag, std::uint64_t &defects) {
    const auto latest = store.latest(session);
    const WorkingContextSnapshot *previous_pointer =
        latest.has_value() && latest.value().has_value() ? &latest.value().value() : nullptr;
    const std::vector<ConversationSegmentEntry> entries =
        pair_recent(checkpoint.through_event_sequence,
                    checkpoint.through_event_sequence + checkpoint.id.value.to_string().size());
    TranscriptLayout layout;
    provider.set_script(plan_output(previous_pointer, checkpoint, entries.size(), tag, layout),
                        layout.total);
    provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
    auto result = auto_curator.on_signal(
        session, make_refresh_input(checkpoint, identity, live, reported_events, entries));
    if (!result.has_value()) {
        return std::nullopt;
    }
    (void)consume_caller_future(result.value(), defects);
    if (!await_settlement(auto_curator, session)) {
        return std::nullopt;
    }
    const auto committed = store.latest(session);
    if (!committed.has_value() || !committed.value().has_value()) {
        return std::nullopt;
    }
    return committed.value();
}

// ---------------------------------------------------------------------------
// Counters (one group per gate, M22 §4.3)
// ---------------------------------------------------------------------------

struct Counters final {
    // W3-G1 trigger fidelity.
    std::uint64_t pure_table_cases = 0;
    std::uint64_t pure_table_failures = 0;
    std::uint64_t chain_signals = 0;
    std::uint64_t chain_fires = 0;
    std::uint64_t fires_watermark = 0;
    std::uint64_t fires_event_count = 0;
    std::uint64_t catchup_fires = 0;
    std::uint64_t fire_prediction_mismatches = 0;
    std::uint64_t fire_kind_mismatches = 0;
    std::uint64_t nonfire_schedules = 0;
    std::uint64_t gated_decisions = 0;
    std::uint64_t missed_catchups = 0;
    std::uint64_t covered_checkpoint_fires = 0;
    std::uint64_t chain_disposition_mismatches = 0;
    // W3-G2 coalescing.
    std::uint64_t absorb_sessions = 0;
    std::uint64_t absorbed_signals = 0;
    std::uint64_t schedules_while_blocked = 0;
    std::uint64_t refire_missing = 0;
    std::uint64_t refire_stale_input = 0;
    std::uint64_t coexistence_final_mismatches = 0;
    std::uint64_t coexistence_conflicts = 0;
    // W3-G3 forced flush and future ownership.
    std::uint64_t forced_fires = 0;
    std::uint64_t forced_not_committed = 0;
    std::uint64_t boundary_noops = 0;
    std::uint64_t noop_wrong_reason = 0;
    std::uint64_t noop_wrong_committed = 0;
    std::uint64_t noop_curator_calls = 0;
    std::uint64_t terminal_late_not_discarded = 0;
    std::uint64_t terminal_store_moves = 0;
    std::uint64_t caller_future_defects = 0;
    // W3-G4 failure fallback.
    std::uint64_t failure_groups = 0;
    std::uint64_t failure_wrong_code = 0;
    std::uint64_t failure_store_moves = 0;
    std::uint64_t tight_retries = 0;
    std::uint64_t retry_misses = 0;
    std::uint64_t retry_streak_not_reset = 0;
    std::uint64_t failing_flush_noops = 0;
    std::vector<std::uint64_t> retry_intervals;
    // W3-G5 commit discipline and previous selection.
    std::uint64_t previous_fresh_ok = 0;
    std::uint64_t previous_same_chain_ok = 0;
    std::uint64_t previous_epoch_not_null = 0;
    std::uint64_t previous_same_chain_wrong = 0;
    std::uint64_t rejection_missing = 0;
    std::uint64_t capacity_rejection_missing = 0;
    // W3-G6 shutdown and determinism.
    std::uint64_t shutdown_sessions = 0;
    std::uint64_t shutdown_cancel_wrong = 0;
    std::uint64_t shutdown_post_close_schedules = 0;
    std::uint64_t shutdown_post_close_accepted = 0;
    std::uint64_t replay_pairs = 0;
    std::uint64_t replay_identical = 0;
    std::uint64_t replay_trajectory_mismatches = 0;
};

[[nodiscard]] JsonValue counters_json(const Counters &counters) {
    JsonValue::Object object;
    const auto put = [&object](const char *key, std::uint64_t value) {
        object.emplace_back(key, static_cast<std::int64_t>(value));
    };
    put("pure_table_cases", counters.pure_table_cases);
    put("pure_table_failures", counters.pure_table_failures);
    put("chain_signals", counters.chain_signals);
    put("chain_fires", counters.chain_fires);
    put("fires_watermark", counters.fires_watermark);
    put("fires_event_count", counters.fires_event_count);
    put("catchup_fires", counters.catchup_fires);
    put("fire_prediction_mismatches", counters.fire_prediction_mismatches);
    put("fire_kind_mismatches", counters.fire_kind_mismatches);
    put("nonfire_schedules", counters.nonfire_schedules);
    put("gated_decisions", counters.gated_decisions);
    put("missed_catchups", counters.missed_catchups);
    put("covered_checkpoint_fires", counters.covered_checkpoint_fires);
    put("chain_disposition_mismatches", counters.chain_disposition_mismatches);
    put("absorb_sessions", counters.absorb_sessions);
    put("absorbed_signals", counters.absorbed_signals);
    put("schedules_while_blocked", counters.schedules_while_blocked);
    put("refire_missing", counters.refire_missing);
    put("refire_stale_input", counters.refire_stale_input);
    put("coexistence_final_mismatches", counters.coexistence_final_mismatches);
    put("coexistence_conflicts", counters.coexistence_conflicts);
    put("forced_fires", counters.forced_fires);
    put("forced_not_committed", counters.forced_not_committed);
    put("boundary_noops", counters.boundary_noops);
    put("noop_wrong_reason", counters.noop_wrong_reason);
    put("noop_wrong_committed", counters.noop_wrong_committed);
    put("noop_curator_calls", counters.noop_curator_calls);
    put("terminal_late_not_discarded", counters.terminal_late_not_discarded);
    put("terminal_store_moves", counters.terminal_store_moves);
    put("caller_future_defects", counters.caller_future_defects);
    put("failure_groups", counters.failure_groups);
    put("failure_wrong_code", counters.failure_wrong_code);
    put("failure_store_moves", counters.failure_store_moves);
    put("tight_retries", counters.tight_retries);
    put("retry_misses", counters.retry_misses);
    put("retry_streak_not_reset", counters.retry_streak_not_reset);
    put("failing_flush_noops", counters.failing_flush_noops);
    put("previous_fresh_ok", counters.previous_fresh_ok);
    put("previous_same_chain_ok", counters.previous_same_chain_ok);
    put("previous_epoch_not_null", counters.previous_epoch_not_null);
    put("previous_same_chain_wrong", counters.previous_same_chain_wrong);
    put("rejection_missing", counters.rejection_missing);
    put("capacity_rejection_missing", counters.capacity_rejection_missing);
    put("shutdown_sessions", counters.shutdown_sessions);
    put("shutdown_cancel_wrong", counters.shutdown_cancel_wrong);
    put("shutdown_post_close_schedules", counters.shutdown_post_close_schedules);
    put("shutdown_post_close_accepted", counters.shutdown_post_close_accepted);
    put("replay_pairs", counters.replay_pairs);
    put("replay_identical", counters.replay_identical);
    put("replay_trajectory_mismatches", counters.replay_trajectory_mismatches);
    JsonValue::Array intervals;
    for (const std::uint64_t interval : counters.retry_intervals) {
        intervals.emplace_back(static_cast<std::int64_t>(interval));
    }
    object.emplace_back("retry_intervals", JsonValue(std::move(intervals)));
    return JsonValue(std::move(object));
}

// ---------------------------------------------------------------------------
// W3-G1 frozen pure-function boundary table
// ---------------------------------------------------------------------------

void run_pure_table(const FrozenConfig &config, Counters &counters) {
    struct Case final {
        std::uint64_t last_attempt;
        std::uint64_t events;
        std::uint64_t current;
        bool refresh;
        WorkingContextTriggerKind kind;
    };
    const WorkingContextTriggerPolicy policy;
    const Case cases[] = {
        {0, 0, config.watermark_interval - 1, false, WorkingContextTriggerKind::None},
        {0, 0, config.watermark_interval, true, WorkingContextTriggerKind::Watermark},
        {config.watermark_interval, 0, 2 * config.watermark_interval - 1, false,
         WorkingContextTriggerKind::None},
        {config.watermark_interval, 0, 2 * config.watermark_interval, true,
         WorkingContextTriggerKind::Watermark},
        {0, config.event_count_interval - 1, 0, false, WorkingContextTriggerKind::None},
        {0, config.event_count_interval, 0, true, WorkingContextTriggerKind::EventCount},
        {0, 3 * config.event_count_interval, config.watermark_interval, true,
         WorkingContextTriggerKind::Watermark},
        {config.watermark_interval, config.event_count_interval, 2 * config.watermark_interval,
         true, WorkingContextTriggerKind::Watermark},
        {3 * config.watermark_interval, 0, config.watermark_interval, false,
         WorkingContextTriggerKind::None},
        {3 * config.watermark_interval, config.event_count_interval, 0, true,
         WorkingContextTriggerKind::EventCount},
        {40, 15, 47, false, WorkingContextTriggerKind::None},
        {40, 15, 48, true, WorkingContextTriggerKind::Watermark},
        {40, 16, 44, true, WorkingContextTriggerKind::EventCount},
    };
    for (const Case &item : cases) {
        ++counters.pure_table_cases;
        const auto decision =
            evaluate_working_context_trigger(policy, item.last_attempt, item.events, item.current);
        if (decision.refresh != item.refresh || decision.kind != item.kind) {
            ++counters.pure_table_failures;
        }
    }
}

// ---------------------------------------------------------------------------
// Scenario: the 40-signal automatic chain over all sessions (W3-G1 evidence
// and the determinism anchors). Fully sequential: every refresh settles and
// is drained before the next signal, so an independent oracle mirroring M22
// §4.2 predicts every signal exactly.
// ---------------------------------------------------------------------------

struct ChainRun final {
    std::vector<std::vector<std::string>> committed_json;
    std::vector<std::vector<std::uint64_t>> settled_trajectory;
    std::vector<std::string> chain_digests;
};

[[nodiscard]] ChainRun run_auto_chain(const Dataset &dataset, const FrozenConfig &config,
                                      ScriptedCuratorProvider &provider, executor::Executor &exec,
                                      Counters &counters) {
    ChainRun run;
    run.committed_json.resize(dataset.sessions.size());
    run.settled_trajectory.resize(dataset.sessions.size());
    run.chain_digests.resize(dataset.sessions.size());

    const WorkingContextTriggerPolicy policy;
    InMemoryWorkingContextStore store;
    ProviderContextCurator curator(provider);
    ContextMemorySupervisor supervisor(exec);
    WorkingContextAutoCurator auto_curator(supervisor, curator, store, policy,
                                           ContextCurationOptions{});

    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        const auto &eval_session = dataset.sessions[session_index];
        // The oracle mirror of the M22 §4.2 per-session-chain state.
        std::uint64_t mirror_last_attempt = 0;
        std::uint64_t mirror_events = 0;
        std::uint64_t mirror_settled = 0;
        std::uint64_t gated_since_fire = 0;
        std::optional<WorkingContextSnapshot> previous;
        for (std::size_t signal_index = 0; signal_index < eval_session.signals.size();
             ++signal_index) {
            const auto &signal = eval_session.signals[signal_index];
            ++counters.chain_signals;
            mirror_events += signal.events;
            const auto checkpoint = make_eval_checkpoint(
                eval_session.session, eval_session.task, signal.checkpoint_watermark,
                signal.checkpoint_revision, config.environment_epoch);
            const auto entries = recent_entries(signal);
            const auto identity = make_identity(eval_session, config, config.environment_epoch);
            const auto live = make_live(eval_session, config, config.environment_epoch);

            const auto decision = evaluate_working_context_trigger(
                policy, mirror_last_attempt, mirror_events, signal.checkpoint_watermark);
            const bool predicted_fire =
                decision.refresh && signal.checkpoint_watermark > mirror_settled;
            if (decision.refresh && !predicted_fire) {
                ++counters.gated_decisions;
                ++gated_since_fire;
            }
            // A gap-closing checkpoint with a standing gated decision must
            // catch up on the first signal that carries it.
            if (signal.mints && gated_since_fire > 0 && !predicted_fire) {
                ++counters.missed_catchups;
            }

            if (predicted_fire) {
                TranscriptLayout layout;
                WorkingContextSnapshot *previous_pointer =
                    previous.has_value() ? &previous.value() : nullptr;
                provider.set_script(plan_output(previous_pointer, checkpoint, entries.size(),
                                                "f" + std::to_string(signal_index), layout),
                                    layout.total);
                provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
            }
            const auto calls_before = provider.calls();
            const auto stats_before = auto_curator.stats();

            auto result = auto_curator.on_signal(
                eval_session.session,
                make_refresh_input(checkpoint, identity, live, signal.events, entries));
            const bool fired = result.has_value();
            if (fired != predicted_fire) {
                ++counters.fire_prediction_mismatches;
            }
            if (!fired) {
                if (provider.calls() != calls_before) {
                    ++counters.nonfire_schedules;
                }
                continue;
            }
            ++counters.chain_fires;
            if (gated_since_fire > 0) {
                ++counters.catchup_fires;
                gated_since_fire = 0;
            }
            if (signal.checkpoint_watermark <= mirror_settled) {
                ++counters.covered_checkpoint_fires;
            }
            (void)consume_caller_future(result.value(), counters.caller_future_defects);
            if (!await_settlement(auto_curator, eval_session.session)) {
                ++counters.chain_disposition_mismatches;
                continue;
            }
            const WorkingContextAutoStats stats = auto_curator.stats();
            const bool watermark_kind =
                stats.fires_watermark - stats_before.fires_watermark == 1 &&
                stats.fires_event_count - stats_before.fires_event_count == 0;
            const bool event_kind = stats.fires_event_count - stats_before.fires_event_count == 1 &&
                                    stats.fires_watermark - stats_before.fires_watermark == 0;
            const bool kind_matches =
                (watermark_kind && decision.kind == WorkingContextTriggerKind::Watermark) ||
                (event_kind && decision.kind == WorkingContextTriggerKind::EventCount);
            if (!kind_matches) {
                ++counters.fire_kind_mismatches;
            }
            if (stats.committed - stats_before.committed != 1) {
                ++counters.chain_disposition_mismatches;
                continue;
            }
            const auto latest = store.latest(eval_session.session);
            if (!latest.has_value() || !latest.value().has_value() ||
                latest.value()->through_event_sequence != signal.checkpoint_watermark) {
                ++counters.chain_disposition_mismatches;
                continue;
            }
            previous = latest.value();
            run.committed_json[session_index].push_back(normalized_json(previous.value()));
            run.settled_trajectory[session_index].push_back(signal.checkpoint_watermark);
            mirror_last_attempt = signal.checkpoint_watermark;
            mirror_events = 0;
            mirror_settled = signal.checkpoint_watermark;
        }
        std::string joined;
        for (const auto &payload : run.committed_json[session_index]) {
            joined += payload;
            joined += '\n';
        }
        run.chain_digests[session_index] = digest_hex(digest_string(joined));
    }
    supervisor.begin_shutdown();
    return run;
}

// ---------------------------------------------------------------------------
// Scenario: coalescing (W3-G2) — a gate-blocked refresh absorbs every
// intermediate signal; the release refires exactly once on the latest input.
// ---------------------------------------------------------------------------

void run_absorb_round(const Dataset &dataset, const FrozenConfig &config,
                      ScriptedCuratorProvider &provider, executor::Executor &exec,
                      Counters &counters) {
    CuratorGate gate;
    provider.arm_gate(gate);
    GateReleaser releaser{gate};
    InMemoryWorkingContextStore store;
    ProviderContextCurator curator(provider);
    ContextMemorySupervisor supervisor(exec);
    WorkingContextAutoCurator auto_curator(supervisor, curator, store);

    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        const auto &eval_session = dataset.sessions[session_index];
        const auto identity = make_identity(eval_session, config, config.environment_epoch);
        const auto live = make_live(eval_session, config, config.environment_epoch);
        const auto first = make_eval_checkpoint(eval_session.session, eval_session.task, 8, 1,
                                                config.environment_epoch);
        const auto entries = pair_recent(8, session_index + 1);
        ++counters.absorb_sessions;

        // The fire blocks inside the provider gate; the transcript guard
        // needs the fresh-chain plan (five entries: 4 checkpoint + 2 events).
        TranscriptLayout layout;
        provider.set_script(plan_output(nullptr, first, entries.size(), "absorb", layout),
                            layout.total);
        // The previous session's holds were released and awaited, so the
        // gate can be re-armed for this session's fire.
        gate.reset();
        provider.set_behavior(ScriptedCuratorProvider::Behavior::Gate);
        auto result = auto_curator.on_signal(eval_session.session,
                                             make_refresh_input(first, identity, live, 0, entries));
        if (!result.has_value()) {
            ++counters.refire_missing;
            continue;
        }
        gate.wait_entered(session_index + 1);
        const auto calls_blocked = provider.calls();
        const auto absorbed_before = auto_curator.stats().absorbed;

        // Three signals while the refresh is in flight: all absorbed, none
        // queues a duplicate curator call.
        for (std::uint64_t index = 0; index < 3; ++index) {
            const auto carried =
                make_eval_checkpoint(eval_session.session, eval_session.task, 8 + index, 2 + index,
                                     config.environment_epoch);
            auto absorbed = auto_curator.on_signal(
                eval_session.session, make_refresh_input(carried, identity, live, 6, {}));
            if (absorbed.has_value()) {
                ++counters.schedules_while_blocked;
            }
        }
        counters.absorbed_signals += auto_curator.stats().absorbed - absorbed_before;
        if (provider.calls() != calls_blocked) {
            ++counters.schedules_while_blocked;
        }

        gate.release();
        if (!await_settlement(auto_curator, eval_session.session)) {
            ++counters.refire_missing;
            continue;
        }

        // The next threshold-crossing signal refires exactly once, carrying
        // the latest checkpoint (16), never an absorbed intermediate input.
        const auto latest = make_eval_checkpoint(eval_session.session, eval_session.task, 16, 9,
                                                 config.environment_epoch);
        const auto refire_calls = provider.calls();
        provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
        const auto committed =
            scripted_fire(auto_curator, provider, store, eval_session.session, latest, identity,
                          live, 0, "refire", counters.caller_future_defects);
        if (provider.calls() - refire_calls != 1 || !committed.has_value() ||
            committed->through_event_sequence != 16) {
            ++counters.refire_missing;
        }
        if (store_latest_watermark(store, eval_session.session) != 16) {
            ++counters.refire_stale_input;
        }
    }
    provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
    supervisor.begin_shutdown();
}

// ---------------------------------------------------------------------------
// Scenario: forced flush (W3-G3) — below-threshold force, covered-boundary
// no-op short circuit, terminal lateness, and monotonic coexistence.
// ---------------------------------------------------------------------------

void run_flush_round(const Dataset &dataset, const FrozenConfig &config,
                     ScriptedCuratorProvider &provider, executor::Executor &exec,
                     Counters &counters) {
    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        const auto &eval_session = dataset.sessions[session_index];
        const auto identity = make_identity(eval_session, config, config.environment_epoch);
        const auto live = make_live(eval_session, config, config.environment_epoch);
        InMemoryWorkingContextStore store;
        ProviderContextCurator curator(provider);
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);

        // (a) Forced fire below every threshold at the task boundary.
        const auto boundary = make_eval_checkpoint(eval_session.session, eval_session.task, 4, 1,
                                                   config.environment_epoch);
        {
            TranscriptLayout layout;
            provider.set_script(plan_output(nullptr, boundary, 2, "forced", layout), layout.total);
            provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
        }
        const auto committed_before = auto_curator.stats().committed;
        auto future = auto_curator.flush(
            eval_session.session,
            make_refresh_input(boundary, identity, live, 0, pair_recent(4, session_index + 1)));
        ++counters.forced_fires;
        const auto forced_outcome = consume_caller_future(future, counters.caller_future_defects);
        if (!await_settlement(auto_curator, eval_session.session)) {
            ++counters.forced_not_committed;
        }
        if (auto_curator.stats().committed - committed_before != 1 ||
            store_latest_watermark(store, eval_session.session) != 4) {
            ++counters.forced_not_committed;
        }
        if (forced_outcome.has_value() &&
            forced_outcome.value().disposition != WorkingContextCommitDisposition::Committed) {
            ++counters.forced_not_committed;
        }

        const auto calls_before = provider.calls();
        auto noop_future = auto_curator.flush(eval_session.session,
                                              make_refresh_input(boundary, identity, live, 0, {}));
        const bool immediate =
            noop_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        const auto noop = consume_caller_future(noop_future, counters.caller_future_defects);
        ++counters.boundary_noops;
        if (!immediate || provider.calls() != calls_before) {
            ++counters.noop_curator_calls;
        }
        if (!noop.has_value() ||
            noop.value().disposition != WorkingContextCommitDisposition::IdempotentNoOp ||
            noop.value().reason_code != "auto-refresh-current") {
            ++counters.noop_wrong_reason;
        } else {
            const auto latest_after = store.latest(eval_session.session);
            const bool committed_matches_latest =
                noop.value().committed.has_value() && latest_after.has_value() &&
                latest_after.value().has_value() &&
                noop.value().committed->id == latest_after.value()->id;
            if (!committed_matches_latest) {
                ++counters.noop_wrong_committed;
            }
        }

        // (c) Terminal lateness: a threshold-crossing policy refresh whose
        // live state went terminal is discarded and the store keeps the
        // forced snapshot.
        const auto before = store.latest(eval_session.session);
        const std::string before_json = before.has_value() && before.value().has_value()
                                            ? normalized_json(before.value().value())
                                            : std::string("<none>");
        WorkingContextCommitState terminal_live = live;
        terminal_live.session_terminal = true;
        const auto late = make_eval_checkpoint(eval_session.session, eval_session.task, 12, 2,
                                               config.environment_epoch);
        {
            TranscriptLayout layout;
            const WorkingContextSnapshot *previous_pointer =
                before.has_value() && before.value().has_value() ? &before.value().value()
                                                                 : nullptr;
            provider.set_script(plan_output(previous_pointer, late, 2, "late", layout),
                                layout.total);
            provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
        }
        const auto terminal_before = auto_curator.stats().discarded_terminal;
        auto late_result = auto_curator.on_signal(
            eval_session.session,
            make_refresh_input(late, identity, terminal_live, 0, pair_recent(12, 100)));
        if (late_result.has_value()) {
            (void)consume_caller_future(late_result.value(), counters.caller_future_defects);
            (void)await_settlement(auto_curator, eval_session.session);
        }
        if (auto_curator.stats().discarded_terminal - terminal_before != 1) {
            ++counters.terminal_late_not_discarded;
        }
        const auto latest_after_late = store.latest(eval_session.session);
        const std::string after_json =
            latest_after_late.has_value() && latest_after_late.value().has_value()
                ? normalized_json(latest_after_late.value().value())
                : std::string("<none>");
        if (after_json != before_json) {
            ++counters.terminal_store_moves;
        }
        supervisor.begin_shutdown();
    }

    // (d) Coexistence: the barrier bounded-drains the in-flight refresh, the
    // forced refresh fires beside it, and the monotonic commit discipline
    // lands the store exactly on the flush watermark with no conflict. One
    // coordinator per session keeps the scenario self-contained.
    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        const auto &eval_session = dataset.sessions[session_index];
        const auto identity = make_identity(eval_session, config, config.environment_epoch);
        const auto live = make_live(eval_session, config, config.environment_epoch);
        CuratorGate gate;
        provider.arm_gate(gate);
        GateReleaser releaser{gate};
        InMemoryWorkingContextStore store;
        ProviderContextCurator curator(provider);
        ContextMemorySupervisor supervisor(exec);
        ContextCurationOptions options;
        options.deadline = std::chrono::milliseconds(50);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store,
                                               WorkingContextTriggerPolicy{}, options);
        const auto policy_checkpoint = make_eval_checkpoint(eval_session.session, eval_session.task,
                                                            8, 1, config.environment_epoch);
        const auto boundary = make_eval_checkpoint(eval_session.session, eval_session.task, 16, 2,
                                                   config.environment_epoch);
        // The policy refresh blocks in the gate; the store stays empty, so
        // the forced refresh's script is a fresh-chain plan.
        TranscriptLayout layout;
        provider.set_script(plan_output(nullptr, policy_checkpoint, 2, "coexist", layout),
                            layout.total);
        provider.set_behavior(ScriptedCuratorProvider::Behavior::Gate);
        auto policy_result = auto_curator.on_signal(
            eval_session.session,
            make_refresh_input(policy_checkpoint, identity, live, 0, pair_recent(8, 200)));
        if (!policy_result.has_value()) {
            ++counters.coexistence_final_mismatches;
            continue;
        }
        gate.wait_entered(1);
        provider.set_script(plan_output(nullptr, boundary, 2, "coexist-flush", layout),
                            layout.total);
        auto flush_future =
            auto_curator.flush(eval_session.session, make_refresh_input(boundary, identity, live, 0,
                                                                        pair_recent(16, 300)));
        ++counters.forced_fires;
        gate.wait_entered(2);
        gate.release();
        (void)consume_caller_future(policy_result.value(), counters.caller_future_defects);
        const auto flush_outcome =
            consume_caller_future(flush_future, counters.caller_future_defects);
        for (int spin = 0; spin < 100'000'000; ++spin) {
            if (store_latest_watermark(store, eval_session.session) == 16) {
                break;
            }
            std::this_thread::yield();
        }
        if (store_latest_watermark(store, eval_session.session) != 16) {
            ++counters.coexistence_final_mismatches;
        }
        if (flush_outcome.has_value() &&
            flush_outcome.value().disposition != WorkingContextCommitDisposition::Committed) {
            ++counters.coexistence_final_mismatches;
        }
        if (auto_curator.stats().discarded_stale > 1) {
            ++counters.coexistence_conflicts;
        }
        supervisor.begin_shutdown();
    }
    provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
}

// ---------------------------------------------------------------------------
// Scenario: failure fallback (W3-G4) — five frozen classes, eight groups
// each: error future, byte-identical store, no tight retry, a real forced
// retry at the failed boundary, and a successful policy retry on the next
// crossing that resets the failure streak.
// ---------------------------------------------------------------------------

void run_failure_round(const Dataset &dataset, const FrozenConfig &config,
                       ScriptedCuratorProvider &provider, executor::Executor &exec,
                       Counters &counters) {
    struct ClassCase final {
        const char *name;
        ScriptedCuratorProvider::Behavior behavior;
        ErrorCode expected;
        std::chrono::milliseconds deadline;
    };
    const ClassCase classes[] = {
        {"provider-error", ScriptedCuratorProvider::Behavior::ProviderError, ErrorCode::Unavailable,
         std::chrono::milliseconds(10'000)},
        {"malformed-json", ScriptedCuratorProvider::Behavior::MalformedJson,
         ErrorCode::InvalidModelOutput, std::chrono::milliseconds(10'000)},
        {"refusal", ScriptedCuratorProvider::Behavior::Refusal, ErrorCode::InvalidModelOutput,
         std::chrono::milliseconds(10'000)},
        {"deadline", ScriptedCuratorProvider::Behavior::SleepPastDeadline,
         ErrorCode::DeadlineExceeded, std::chrono::milliseconds(20)},
        {"cancel", ScriptedCuratorProvider::Behavior::ReturnCancelled, ErrorCode::Cancelled,
         std::chrono::milliseconds(10'000)},
    };

    provider.set_script("{}", 0);
    for (const ClassCase &failure : classes) {
        ContextCurationOptions round_options;
        round_options.deadline = failure.deadline;
        for (std::size_t session_index = 0; session_index < dataset.sessions.size();
             ++session_index) {
            const auto &eval_session = dataset.sessions[session_index];
            const auto identity = make_identity(eval_session, config, config.environment_epoch);
            const auto live = make_live(eval_session, config, config.environment_epoch);
            InMemoryWorkingContextStore store;
            ProviderContextCurator curator(provider);
            ContextMemorySupervisor supervisor(exec);
            WorkingContextAutoCurator auto_curator(supervisor, curator, store,
                                                   WorkingContextTriggerPolicy{}, round_options);
            ++counters.failure_groups;
            provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);

            // Baseline commit at watermark 8.
            const auto baseline = make_eval_checkpoint(eval_session.session, eval_session.task, 8,
                                                       1, config.environment_epoch);
            const auto baseline_committed =
                scripted_fire(auto_curator, provider, store, eval_session.session, baseline,
                              identity, live, 0, "baseline", counters.caller_future_defects);
            if (!baseline_committed.has_value()) {
                ++counters.failure_store_moves;
                supervisor.begin_shutdown();
                continue;
            }
            const std::string before = normalized_json(baseline_committed.value());

            // Failing fire at watermark 16: the real script keeps the
            // transcript guard happy; the behavior class fails the run.
            const auto failing = make_eval_checkpoint(eval_session.session, eval_session.task, 16,
                                                      2, config.environment_epoch);
            {
                TranscriptLayout layout;
                provider.set_script(
                    plan_output(&baseline_committed.value(), failing, 2, "failing", layout),
                    layout.total);
            }
            provider.set_behavior(failure.behavior);
            auto result = auto_curator.on_signal(
                eval_session.session,
                make_refresh_input(failing, identity, live, 0, pair_recent(16, 400)));
            if (!result.has_value()) {
                ++counters.failure_wrong_code;
            } else {
                const auto error_outcome =
                    consume_caller_future(result.value(), counters.caller_future_defects);
                if (error_outcome.has_value() || error_outcome.error().code != failure.expected) {
                    ++counters.failure_wrong_code;
                }
            }
            if (!await_settlement(auto_curator, eval_session.session)) {
                ++counters.failure_store_moves;
            }
            auto_curator.drain(eval_session.session);
            const auto after_failure = auto_curator.stats();
            const auto current_latest = store.latest(eval_session.session);
            const std::string after =
                current_latest.has_value() && current_latest.value().has_value()
                    ? normalized_json(current_latest.value().value())
                    : std::string("<none>");
            if (after != before) {
                ++counters.failure_store_moves;
            }
            if (after_failure.consecutive_failures != 1) {
                ++counters.retry_streak_not_reset;
            }

            // Failures re-arm: below-threshold signals stay silent (no tight
            // retry) while the streak stands.
            provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
            std::uint64_t signals_before_retry = 0;
            result = auto_curator.on_signal(
                eval_session.session,
                make_refresh_input(failing, identity, live, 5, pair_recent(16, 500)));
            ++signals_before_retry;
            if (result.has_value()) {
                ++counters.tight_retries;
            }
            const auto small_step = make_eval_checkpoint(eval_session.session, eval_session.task,
                                                         18, 3, config.environment_epoch);
            result = auto_curator.on_signal(
                eval_session.session,
                make_refresh_input(small_step, identity, live, 0, pair_recent(18, 600)));
            ++signals_before_retry;
            if (result.has_value()) {
                ++counters.tight_retries;
            }

            // The failed boundary flush is a real retry (forced, not a no-op).
            const auto forced_before = auto_curator.stats().forced_flushes;
            auto flush_future = auto_curator.flush(
                eval_session.session,
                make_refresh_input(failing, identity, live, 0, pair_recent(16, 700)));
            const auto flush_outcome =
                consume_caller_future(flush_future, counters.caller_future_defects);
            if (!await_settlement(auto_curator, eval_session.session)) {
                ++counters.failing_flush_noops;
            }
            const WorkingContextAutoStats after_forced = auto_curator.stats();
            if (after_forced.forced_flushes - forced_before != 1 ||
                auto_curator.session_view(eval_session.session)->settled_watermark != 16) {
                ++counters.failing_flush_noops;
            }
            if (flush_outcome.has_value() &&
                flush_outcome.value().disposition != WorkingContextCommitDisposition::Committed) {
                ++counters.failing_flush_noops;
            }

            // The next threshold crossing retries through the policy path and
            // succeeds, keeping the streak at zero.
            const auto retry = make_eval_checkpoint(eval_session.session, eval_session.task, 24, 4,
                                                    config.environment_epoch);
            const auto retry_committed =
                scripted_fire(auto_curator, provider, store, eval_session.session, retry, identity,
                              live, 0, "retry", counters.caller_future_defects);
            if (!retry_committed.has_value()) {
                ++counters.retry_misses;
            }
            const WorkingContextAutoStats final_stats = auto_curator.stats();
            if (final_stats.consecutive_failures != 0) {
                ++counters.retry_streak_not_reset;
            }
            if (auto_curator.session_view(eval_session.session)->settled_watermark != 24) {
                ++counters.retry_misses;
            }
            counters.retry_intervals.push_back(signals_before_retry + 1);
            supervisor.begin_shutdown();
        }
    }
}

// ---------------------------------------------------------------------------
// Scenario: previous selection (W3-G5) — fresh chain passes nullptr, the same
// chain passes the store latest, an epoch bump passes nullptr and resets the
// settled watermark; observed through the transcript and the committed chains.
// ---------------------------------------------------------------------------

void run_previous_round(const Dataset &dataset, const FrozenConfig &config,
                        ScriptedCuratorProvider &provider, executor::Executor &exec,
                        Counters &counters) {
    InMemoryWorkingContextStore store;
    ProviderContextCurator curator(provider);
    ContextMemorySupervisor supervisor(exec);
    WorkingContextAutoCurator auto_curator(supervisor, curator, store);

    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        const auto &eval_session = dataset.sessions[session_index];
        const auto identity = make_identity(eval_session, config, config.environment_epoch);
        const auto live = make_live(eval_session, config, config.environment_epoch);
        const auto first = make_eval_checkpoint(eval_session.session, eval_session.task, 8, 1,
                                                config.environment_epoch);

        // Fresh chain: the transcript renders no prev: entries.
        const auto fresh_committed =
            scripted_fire(auto_curator, provider, store, eval_session.session, first, identity,
                          live, 0, "prev0", counters.caller_future_defects);
        if (!fresh_committed.has_value()) {
            ++counters.previous_epoch_not_null;
            continue;
        }
        if (provider.last_transcript().find("prev:") == std::string::npos) {
            ++counters.previous_fresh_ok;
        }

        // Same chain: the transcript cites previous entries and the committed
        // chain accumulates both checkpoints.
        const auto second = make_eval_checkpoint(eval_session.session, eval_session.task, 16, 2,
                                                 config.environment_epoch);
        const auto same_committed =
            scripted_fire(auto_curator, provider, store, eval_session.session, second, identity,
                          live, 0, "prev1", counters.caller_future_defects);
        if (provider.last_transcript().find("prev:") != std::string::npos &&
            same_committed.has_value() && same_committed->source_checkpoints.size() == 2) {
            ++counters.previous_same_chain_ok;
        } else {
            ++counters.previous_same_chain_wrong;
        }

        // Epoch bump on the same watermark with only the event axis
        // re-crossed: the settled reset must unblock the fire and the new
        // chain starts without a previous snapshot.
        const WorkingContextIdentity bumped_identity = make_identity(eval_session, config, 8);
        const WorkingContextCommitState bumped_live = make_live(eval_session, config, 8);
        const auto bumped = make_eval_checkpoint(eval_session.session, eval_session.task, 16, 3, 8);
        TranscriptLayout layout;
        provider.set_script(plan_output(nullptr, bumped, 2, "prev2", layout), layout.total);
        provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
        auto result = auto_curator.on_signal(
            eval_session.session,
            make_refresh_input(bumped, bumped_identity, bumped_live, 16, pair_recent(16, 800)));
        if (!result.has_value()) {
            // Without the settled reset the covered watermark would have
            // swallowed the signal entirely.
            ++counters.previous_epoch_not_null;
            continue;
        }
        (void)consume_caller_future(result.value(), counters.caller_future_defects);
        if (!await_settlement(auto_curator, eval_session.session)) {
            ++counters.previous_epoch_not_null;
            continue;
        }
        if (provider.last_transcript().find("prev:") == std::string::npos) {
            ++counters.previous_fresh_ok;
        } else {
            ++counters.previous_epoch_not_null;
        }
        const auto bumped_latest = store.latest(eval_session.session);
        if (bumped_latest.has_value() && bumped_latest.value().has_value() &&
            bumped_latest.value()->environment_epoch == 8 &&
            bumped_latest.value()->source_checkpoints.size() == 1) {
            ++counters.previous_fresh_ok;
        }
    }
    supervisor.begin_shutdown();
}

// ---------------------------------------------------------------------------
// Scenario: shutdown (W3-G6) — the in-flight refresh resolves Cancelled, the
// drain records it, and post-close signals/flushes resolve rejections with
// zero curator calls.
// ---------------------------------------------------------------------------

void run_shutdown_round(const Dataset &dataset, const FrozenConfig &config,
                        ScriptedCuratorProvider &provider, executor::Executor &exec,
                        Counters &counters) {
    CuratorGate gate;
    provider.arm_gate(gate);
    GateReleaser releaser{gate};
    provider.set_script("{}", 0);
    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        const auto &eval_session = dataset.sessions[session_index];
        const auto identity = make_identity(eval_session, config, config.environment_epoch);
        const auto live = make_live(eval_session, config, config.environment_epoch);
        InMemoryWorkingContextStore store;
        ProviderContextCurator curator(provider);
        ContextMemorySupervisor supervisor(exec);
        WorkingContextAutoCurator auto_curator(supervisor, curator, store);
        ++counters.shutdown_sessions;

        // The previous session's parked op has fully settled, so the gate
        // can be re-armed.
        gate.reset();
        provider.set_behavior(ScriptedCuratorProvider::Behavior::Gate);
        provider.set_park_after_gate(true);
        const auto first = make_eval_checkpoint(eval_session.session, eval_session.task, 8, 1,
                                                config.environment_epoch);
        auto result = auto_curator.on_signal(
            eval_session.session,
            make_refresh_input(first, identity, live, 0, pair_recent(8, 900)));
        if (!result.has_value()) {
            ++counters.shutdown_cancel_wrong;
            continue;
        }
        gate.wait_entered(session_index + 1);
        // The provider leaves the gate and parks on the supervisor's stop
        // flag — the only holding pattern begin_shutdown can drain.
        gate.release();
        const auto report = supervisor.begin_shutdown();
        if (!report.critical_drain_complete) {
            ++counters.shutdown_cancel_wrong;
        }
        const auto cancelled =
            consume_caller_future(result.value(), counters.caller_future_defects);
        if (cancelled.has_value() || cancelled.error().code != ErrorCode::Cancelled) {
            ++counters.shutdown_cancel_wrong;
        }
        provider.set_park_after_gate(false);

        // The explicit drain records the cancellation; the settled watermark
        // does not move and the store stays empty.
        auto_curator.drain(eval_session.session);
        const auto after_cancel = auto_curator.stats();
        if (after_cancel.errors == 0 ||
            auto_curator.session_view(eval_session.session)->settled_watermark != 0) {
            ++counters.shutdown_cancel_wrong;
        }

        // Post-close signals resolve the supervisor's rejection with zero new
        // curator calls.
        provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
        const auto calls_before = provider.calls();
        const auto next = make_eval_checkpoint(eval_session.session, eval_session.task, 16, 2,
                                               config.environment_epoch);
        auto rejected = auto_curator.on_signal(eval_session.session,
                                               make_refresh_input(next, identity, live, 0, {}));
        if (rejected.has_value()) {
            const auto rejection =
                consume_caller_future(rejected.value(), counters.caller_future_defects);
            if (rejection.has_value()) {
                ++counters.shutdown_post_close_accepted;
            }
        } else {
            ++counters.shutdown_post_close_accepted;
        }
        auto flush_rejected = auto_curator.flush(eval_session.session,
                                                 make_refresh_input(next, identity, live, 0, {}));
        const auto flush_rejection =
            consume_caller_future(flush_rejected, counters.caller_future_defects);
        if (flush_rejection.has_value()) {
            ++counters.shutdown_post_close_accepted;
        }
        if (provider.calls() - calls_before != 0) {
            ++counters.shutdown_post_close_schedules;
        }
        auto_curator.drain(eval_session.session);
        const auto final_latest = store.latest(eval_session.session);
        if (final_latest.has_value() && final_latest.value().has_value()) {
            ++counters.shutdown_post_close_accepted;
        }
    }
    provider.set_park_after_gate(false);
    provider.set_behavior(ScriptedCuratorProvider::Behavior::Ok);
}

// ---------------------------------------------------------------------------
// Scenario: rejections (W3-G5) — session mismatch and tracking capacity.
// ---------------------------------------------------------------------------

void run_rejection_round(const Dataset &dataset, const FrozenConfig &config,
                         ScriptedCuratorProvider &provider, executor::Executor &exec,
                         Counters &counters) {
    InMemoryWorkingContextStore store;
    ProviderContextCurator curator(provider);
    ContextMemorySupervisor supervisor(exec);
    WorkingContextTriggerPolicy tight;
    tight.max_tracked_sessions = 2;
    WorkingContextAutoCurator auto_curator(supervisor, curator, store, tight,
                                           ContextCurationOptions{});

    // Fill the two tracking slots with committed fires.
    for (std::size_t session_index = 0; session_index < 2; ++session_index) {
        const auto &eval_session = dataset.sessions[session_index];
        const auto checkpoint = make_eval_checkpoint(eval_session.session, eval_session.task, 8, 1,
                                                     config.environment_epoch);
        const auto committed =
            scripted_fire(auto_curator, provider, store, eval_session.session, checkpoint,
                          make_identity(eval_session, config, config.environment_epoch),
                          make_live(eval_session, config, config.environment_epoch), 0, "fill",
                          counters.caller_future_defects);
        if (!committed.has_value()) {
            ++counters.capacity_rejection_missing;
        }
    }

    // A third session exceeds the tracking bound: ResourceExhausted, no
    // curator call, no chain state.
    const auto &third_session = dataset.sessions[2];
    const auto third_identity = make_identity(third_session, config, config.environment_epoch);
    const auto third_live = make_live(third_session, config, config.environment_epoch);
    const auto third_checkpoint = make_eval_checkpoint(third_session.session, third_session.task, 8,
                                                       1, config.environment_epoch);
    const auto calls_before = provider.calls();
    auto over_capacity = auto_curator.on_signal(
        third_session.session,
        make_refresh_input(third_checkpoint, third_identity, third_live, 0, {}));
    if (!over_capacity.has_value()) {
        ++counters.capacity_rejection_missing;
    } else {
        const auto rejection =
            consume_caller_future(over_capacity.value(), counters.caller_future_defects);
        if (rejection.has_value() || rejection.error().code != ErrorCode::ResourceExhausted ||
            auto_curator.session_view(third_session.session).has_value()) {
            ++counters.capacity_rejection_missing;
        }
    }
    auto flush_over =
        auto_curator.flush(third_session.session,
                           make_refresh_input(third_checkpoint, third_identity, third_live, 0, {}));
    const auto flush_rejection = consume_caller_future(flush_over, counters.caller_future_defects);
    if (flush_rejection.has_value() ||
        flush_rejection.error().code != ErrorCode::ResourceExhausted) {
        ++counters.capacity_rejection_missing;
    }
    if (provider.calls() != calls_before) {
        ++counters.capacity_rejection_missing;
    }

    // Session mismatch: the signal's checkpoint belongs to another session.
    const auto &first_session = dataset.sessions.front();
    const auto mismatched = make_eval_checkpoint(third_session.session, third_session.task, 8, 1,
                                                 config.environment_epoch);
    auto mismatch_result = auto_curator.on_signal(
        first_session.session,
        make_refresh_input(mismatched,
                           make_identity(first_session, config, config.environment_epoch),
                           make_live(first_session, config, config.environment_epoch), 0, {}));
    if (!mismatch_result.has_value()) {
        ++counters.rejection_missing;
    } else {
        const auto rejection =
            consume_caller_future(mismatch_result.value(), counters.caller_future_defects);
        if (rejection.has_value() || rejection.error().code != ErrorCode::InvalidArgument) {
            ++counters.rejection_missing;
        }
    }
    supervisor.begin_shutdown();
}

} // namespace

int main(int argc, char **argv) {
    std::string output_path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (output_path.empty()) {
            output_path = argument;
        }
    }

    FrozenConfig config;
    const Dataset dataset = build_dataset(config);
    if (dataset.digest_hex_text != config.dataset_digest) {
        std::cerr << "dataset digest mismatch: got " << dataset.digest_hex_text << ", want "
                  << config.dataset_digest << "\n";
        return 2;
    }

    // Structural guarantees of the frozen dataset (M22 §4.3): both trigger
    // kinds observable well past the gate bound, and per session one
    // zero-advance/high-event stretch plus one checkpoint gap closed by a
    // minted catch-up checkpoint.
    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        const auto &signals = dataset.sessions[session_index].signals;
        std::uint64_t zero_advance = 0;
        for (std::size_t index = 10; index <= 14; ++index) {
            if (signals[index].advance == 0 && signals[index].events >= 10) {
                ++zero_advance;
            }
        }
        if (zero_advance != 5) {
            std::cerr << "dataset session " << session_index << " lost the zero-advance stretch\n";
            return 2;
        }
        for (std::size_t index = 20; index <= 29; ++index) {
            if (signals[index].mints || signals[index].advance == 0) {
                std::cerr << "dataset session " << session_index << " lost the checkpoint gap\n";
                return 2;
            }
        }
        if (!signals[30].mints) {
            std::cerr << "dataset session " << session_index
                      << " lost the gap-closing checkpoint\n";
            return 2;
        }
    }

    executor::Executor exec;
    if (!exec.initialize(executor::ExecutorConfig{})) {
        std::cerr << "executor initialization failed\n";
        return 2;
    }

    ScriptedCuratorProvider provider;
    Counters counters;

    run_pure_table(config, counters);
    const ChainRun chain = run_auto_chain(dataset, config, provider, exec, counters);
    const ChainRun replay = run_auto_chain(dataset, config, provider, exec, counters);

    run_absorb_round(dataset, config, provider, exec, counters);
    run_flush_round(dataset, config, provider, exec, counters);
    run_failure_round(dataset, config, provider, exec, counters);
    run_previous_round(dataset, config, provider, exec, counters);
    run_shutdown_round(dataset, config, provider, exec, counters);
    run_rejection_round(dataset, config, provider, exec, counters);

    // In-process determinism (W3-G6): the replayed chain must reproduce every
    // committed payload byte for byte and every settled watermark.
    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        const auto &first_rounds = chain.committed_json[session_index];
        const auto &second_rounds = replay.committed_json[session_index];
        if (first_rounds.size() != second_rounds.size()) {
            counters.replay_pairs += first_rounds.size() + second_rounds.size();
            ++counters.replay_trajectory_mismatches;
            continue;
        }
        for (std::size_t fire_index = 0; fire_index < first_rounds.size(); ++fire_index) {
            ++counters.replay_pairs;
            if (first_rounds[fire_index] == second_rounds[fire_index]) {
                ++counters.replay_identical;
            }
        }
        if (chain.settled_trajectory[session_index] != replay.settled_trajectory[session_index]) {
            ++counters.replay_trajectory_mismatches;
        }
        if (chain.chain_digests[session_index] != replay.chain_digests[session_index]) {
            ++counters.replay_trajectory_mismatches;
        }
    }

    // Report metrics (only-reported, not judged): fire counts by kind come
    // from the oracle the chain was checked against, split by kind; the
    // settled trajectories anchor the per-session chains.
    {
        const WorkingContextTriggerPolicy policy;
        for (const auto &eval_session : dataset.sessions) {
            std::uint64_t last_attempt = 0;
            std::uint64_t events = 0;
            std::uint64_t settled = 0;
            for (const auto &signal : eval_session.signals) {
                events += signal.events;
                const auto decision = evaluate_working_context_trigger(policy, last_attempt, events,
                                                                       signal.checkpoint_watermark);
                if (decision.refresh && signal.checkpoint_watermark > settled) {
                    if (decision.kind == WorkingContextTriggerKind::Watermark) {
                        ++counters.fires_watermark;
                    } else {
                        ++counters.fires_event_count;
                    }
                    last_attempt = signal.checkpoint_watermark;
                    events = 0;
                    settled = signal.checkpoint_watermark;
                }
            }
        }
    }

    bool gates_ok = true;
    std::vector<std::string> failures;
    const auto record = [&](bool ok, std::string message) {
        if (!ok) {
            gates_ok = false;
            failures.push_back(std::move(message));
        }
        return ok;
    };

    // W3-G1: pure-function table 100% correct; per-fire kind and inputs equal
    // the prediction; non-fire signals schedule nothing; checkpoint gaps
    // catch up; covered checkpoints never fire.
    const bool g1 = record(
        counters.pure_table_failures == 0 && counters.fire_prediction_mismatches == 0 &&
            counters.fire_kind_mismatches == 0 && counters.nonfire_schedules == 0 &&
            counters.missed_catchups == 0 && counters.covered_checkpoint_fires == 0 &&
            counters.chain_disposition_mismatches == 0 && counters.fires_watermark >= 12 &&
            counters.fires_event_count >= 12,
        "G1: trigger fidelity violated (table " + std::to_string(counters.pure_table_failures) +
            "/" + std::to_string(counters.pure_table_cases) + ", prediction mismatches " +
            std::to_string(counters.fire_prediction_mismatches) + ", kind mismatches " +
            std::to_string(counters.fire_kind_mismatches) + ", non-fire schedules " +
            std::to_string(counters.nonfire_schedules) + ", missed catchups " +
            std::to_string(counters.missed_catchups) + ", covered fires " +
            std::to_string(counters.covered_checkpoint_fires) + ", dispositions " +
            std::to_string(counters.chain_disposition_mismatches) + ")");
    // W3-G2: coalescing absorbs N/N with exactly one schedule; the release
    // refires once on the latest input; flush/policy coexistence stays
    // monotonic on the flush watermark.
    const bool g2 =
        record(counters.absorb_sessions == dataset.sessions.size() &&
                   counters.absorbed_signals == 3 * dataset.sessions.size() &&
                   counters.schedules_while_blocked == 0 && counters.refire_missing == 0 &&
                   counters.refire_stale_input == 0 && counters.coexistence_final_mismatches == 0 &&
                   counters.coexistence_conflicts == 0,
               "G2: coalescing violated (absorbed " + std::to_string(counters.absorbed_signals) +
                   "/" + std::to_string(3 * dataset.sessions.size()) + ", blocked schedules " +
                   std::to_string(counters.schedules_while_blocked) + ", refire missing " +
                   std::to_string(counters.refire_missing) + ", stale refires " +
                   std::to_string(counters.refire_stale_input) + ", coexistence mismatches " +
                   std::to_string(counters.coexistence_final_mismatches) + ")");
    // W3-G3: forced flush fires below threshold and commits; the covered
    // boundary short-circuits with zero curator calls; terminal lateness is
    // discarded without moving the store; every returned future is
    // consumable.
    const bool g3 = record(
        counters.forced_fires == 2 * dataset.sessions.size() &&
            counters.forced_not_committed == 0 &&
            counters.boundary_noops == dataset.sessions.size() && counters.noop_wrong_reason == 0 &&
            counters.noop_wrong_committed == 0 && counters.noop_curator_calls == 0 &&
            counters.terminal_late_not_discarded == 0 && counters.terminal_store_moves == 0 &&
            counters.caller_future_defects == 0,
        "G3: forced flush/future ownership violated (forced " +
            std::to_string(counters.forced_fires) + ", not committed " +
            std::to_string(counters.forced_not_committed) + ", noops " +
            std::to_string(counters.boundary_noops) + ", noop reasons " +
            std::to_string(counters.noop_wrong_reason) + ", noop committed " +
            std::to_string(counters.noop_wrong_committed) + ", noop curator calls " +
            std::to_string(counters.noop_curator_calls) + ", terminal not discarded " +
            std::to_string(counters.terminal_late_not_discarded) + ", terminal store moves " +
            std::to_string(counters.terminal_store_moves) + ", caller future defects " +
            std::to_string(counters.caller_future_defects) + ")");
    // W3-G4: every failure group resolves with the correct error, keeps the
    // store byte-identical, never tight-retries, retries on the next crossing
    // and resets the streak; a failing boundary flush is a real retry.
    const bool g4 =
        record(counters.failure_groups == 5 * dataset.sessions.size() &&
                   counters.failure_wrong_code == 0 && counters.failure_store_moves == 0 &&
                   counters.tight_retries == 0 && counters.retry_misses == 0 &&
                   counters.retry_streak_not_reset == 0 && counters.failing_flush_noops == 0 &&
                   counters.retry_intervals.size() == 5 * dataset.sessions.size(),
               "G4: failure fallback violated (groups " + std::to_string(counters.failure_groups) +
                   ", wrong codes " + std::to_string(counters.failure_wrong_code) +
                   ", store moves " + std::to_string(counters.failure_store_moves) +
                   ", tight retries " + std::to_string(counters.tight_retries) + ", retry misses " +
                   std::to_string(counters.retry_misses) + ", streak not reset " +
                   std::to_string(counters.retry_streak_not_reset) + ", flush noops " +
                   std::to_string(counters.failing_flush_noops) + ")");
    // W3-G5: the disposition matrix, previous selection and rejections.
    // Three fresh-chain observations per session: the first fire, the epoch
    // bump's transcript and the epoch bump's committed chain.
    const bool g5 = record(
        counters.previous_fresh_ok == 3 * dataset.sessions.size() &&
            counters.previous_same_chain_ok == dataset.sessions.size() &&
            counters.previous_epoch_not_null == 0 && counters.previous_same_chain_wrong == 0 &&
            counters.rejection_missing == 0 && counters.capacity_rejection_missing == 0,
        "G5: previous selection/rejections violated (fresh ok " +
            std::to_string(counters.previous_fresh_ok) + ", same-chain ok " +
            std::to_string(counters.previous_same_chain_ok) + ", epoch previous non-null " +
            std::to_string(counters.previous_epoch_not_null) + ", same-chain wrong " +
            std::to_string(counters.previous_same_chain_wrong) + ", rejection missing " +
            std::to_string(counters.rejection_missing) + ", capacity missing " +
            std::to_string(counters.capacity_rejection_missing) + ")");
    // W3-G6: shutdown cancellation and post-close rejection with zero
    // schedules; full-chain replay is snapshot-identical.
    const bool g6 = record(
        counters.shutdown_sessions == dataset.sessions.size() &&
            counters.shutdown_cancel_wrong == 0 && counters.shutdown_post_close_schedules == 0 &&
            counters.shutdown_post_close_accepted == 0 &&
            counters.replay_identical == counters.replay_pairs &&
            counters.replay_trajectory_mismatches == 0,
        "G6: shutdown/determinism violated (cancel wrong " +
            std::to_string(counters.shutdown_cancel_wrong) + ", post-close schedules " +
            std::to_string(counters.shutdown_post_close_schedules) + ", post-close accepted " +
            std::to_string(counters.shutdown_post_close_accepted) + ", replay identical " +
            std::to_string(counters.replay_identical) + "/" +
            std::to_string(counters.replay_pairs) + ", trajectory mismatches " +
            std::to_string(counters.replay_trajectory_mismatches) + ")");

    JsonValue::Object report;
    report.emplace_back("schema", std::string("mira.m22.working-context-auto-trigger-eval.v1"));
    JsonValue::Object environment;
    environment.emplace_back(
        "trigger_policy", JsonValue(JsonValue::Object{{"watermark_interval", std::int64_t{8}},
                                                      {"event_count_interval", std::int64_t{16}},
                                                      {"max_tracked_sessions", std::int64_t{64}}}));
    environment.emplace_back("pipeline", std::string("auto-curator->supervisor-deferrable->"
                                                     "provider-curator->commit"));
    report.emplace_back("environment", JsonValue(std::move(environment)));
    report.emplace_back("dataset_digest", dataset.digest_hex_text);
    JsonValue::Object config_json;
    config_json.emplace_back("seed", static_cast<std::int64_t>(config.seed));
    config_json.emplace_back("sessions", static_cast<std::int64_t>(config.sessions));
    config_json.emplace_back("signals_per_session",
                             static_cast<std::int64_t>(config.signals_per_session));
    config_json.emplace_back("watermark_interval",
                             static_cast<std::int64_t>(config.watermark_interval));
    config_json.emplace_back("event_count_interval",
                             static_cast<std::int64_t>(config.event_count_interval));
    config_json.emplace_back("phase_length", static_cast<std::int64_t>(config.phase_length));
    config_json.emplace_back("mint_stride", static_cast<std::int64_t>(config.mint_stride));
    report.emplace_back("config", JsonValue(std::move(config_json)));
    report.emplace_back("counters", counters_json(counters));
    JsonValue::Array session_ids;
    for (const auto &eval_session : dataset.sessions) {
        session_ids.emplace_back(eval_session.session.to_string());
    }
    report.emplace_back("session_ids", JsonValue(std::move(session_ids)));
    JsonValue::Array digests;
    for (const auto &digest : replay.chain_digests) {
        digests.emplace_back(digest);
    }
    report.emplace_back("chain_digests", JsonValue(std::move(digests)));
    JsonValue::Object trajectories;
    for (std::size_t session_index = 0; session_index < dataset.sessions.size(); ++session_index) {
        JsonValue::Array trajectory;
        for (const std::uint64_t watermark : replay.settled_trajectory[session_index]) {
            trajectory.emplace_back(static_cast<std::int64_t>(watermark));
        }
        trajectories.emplace_back("s" + std::to_string(session_index),
                                  JsonValue(std::move(trajectory)));
    }
    report.emplace_back("settled_trajectories", JsonValue(std::move(trajectories)));
    report.emplace_back("curator_calls_total", static_cast<std::int64_t>(provider.calls()));
    JsonValue::Object gates_json;
    gates_json.emplace_back("w3_g1_trigger_policy", g1);
    gates_json.emplace_back("w3_g2_coalescing", g2);
    gates_json.emplace_back("w3_g3_forced_flush_future_ownership", g3);
    gates_json.emplace_back("w3_g4_failure_fallback", g4);
    gates_json.emplace_back("w3_g5_commit_discipline_previous", g5);
    gates_json.emplace_back("w3_g6_shutdown_determinism", g6);
    report.emplace_back("gates", JsonValue(std::move(gates_json)));
    JsonValue::Array failure_list;
    for (const auto &message : failures) {
        failure_list.emplace_back(message);
    }
    report.emplace_back("gate_failures", JsonValue(std::move(failure_list)));
    report.emplace_back(
        "limitations",
        "scripted deterministic provider over a frozen synthetic signal dataset: metrics "
        "measure deterministic pipeline behaviour (trigger fidelity vs the pure function, "
        "coalescing, forced flush, failure fallback, previous selection, shutdown, cross-run "
        "determinism), not semantic curation quality, token savings or continuation "
        "correctness; no real model, network or credentials are involved (RULE-10)");

    const std::string json = to_json_string(JsonValue(std::move(report)));
    std::cout << json << '\n';
    if (!output_path.empty()) {
        std::ofstream file(output_path);
        file << json << '\n';
    }

    MIRA_CHECK(exec.shutdown(true) == executor::ShutdownResult::Completed);
    if (!gates_ok) {
        for (const auto &message : failures) {
            std::cerr << "gate failure: " << message << '\n';
        }
    }
    return gates_ok ? 0 : 1;
}
