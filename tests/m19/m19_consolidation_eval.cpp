// M19 (DEC-032 Stage D) consolidation eval harness. Builds the frozen
// synthetic conversation dataset (planted constraints/decisions/threads/
// preferences/marker entries), drives the full Layer 3 pipeline —
// ProviderSemanticConsolidator over a scripted deterministic IModelProvider,
// strict parsing, provenance binding, five-tuple commit — and asserts the
// pre-frozen gates D1-D5 from the milestone §4, printing a JSON report.
// Metrics measure pipeline behaviour, not semantic consolidation quality
// (RULE-10).

#include <mira/context_consolidation.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Frozen configuration (M19 §4, frozen before the first evaluation run)
// ---------------------------------------------------------------------------

struct FrozenConfig final {
    std::uint64_t seed = 0x4d49'5231'3953'4443ULL; // "MIR19SDC"
    std::size_t sessions = 12;
    std::size_t entries_per_session = 40;
    std::size_t planted_constraints = 6;
    std::size_t planted_decisions = 4;
    std::size_t planted_threads = 4;
    std::size_t planted_preferences = 3;
    std::size_t marker_entries = 2;
    std::uint64_t task_epoch = 3;
    std::uint64_t environment_epoch = 7;
    // Pinned after the first frozen run; the harness asserts equality so the
    // dataset is checked, not assumed (same discipline as M17/M18).
    std::string dataset_digest =
        "31758a94b646e96f4a2ea133c1dec9d4bc0d62541d09b20d8ed941ebd4fd9866";
};

[[nodiscard]] std::string digest_hex(const Sha256Digest &digest) {
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    for (const auto byte : digest.bytes) {
        std::ostringstream slot;
        slot << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<unsigned int>(byte);
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
// Dataset
// ---------------------------------------------------------------------------

enum class PlantedKind : std::uint8_t { Constraint, Decision, Thread, Preference, Noise };

struct PlantedItem final {
    PlantedKind kind = PlantedKind::Constraint;
    std::string content; // the exact statement the scripted supplier emits
    EventId origin;      // the entry the statement must cite
};

struct EvalSession final {
    SessionId session;
    TaskId task;
    std::vector<ConversationEntry> entries;
    std::vector<PlantedItem> planted;
    std::vector<EventId> entry_origins; // provenance ACL audit set
    std::string token;
};

struct Dataset final {
    std::vector<EvalSession> sessions;
    std::string digest_hex_text;
};

[[nodiscard]] std::string planted_prefix(PlantedKind kind) {
    switch (kind) {
    case PlantedKind::Constraint:
        return "constraint: ";
    case PlantedKind::Decision:
        return "decision: ";
    case PlantedKind::Thread:
        return "thread: ";
    case PlantedKind::Preference:
        return "preference: ";
    case PlantedKind::Noise:
        return "";
    }
    return "";
}

[[nodiscard]] Dataset build_dataset(const FrozenConfig &config) {
    SplitMix64 rng{config.seed};
    Dataset dataset;

    std::uint64_t sequence = 1;
    for (std::size_t session_index = 0; session_index < config.sessions; ++session_index) {
        EvalSession eval_session;
        eval_session.session = SessionId{id_from(rng, "session")};
        eval_session.task = TaskId{id_from(rng, "task")};
        eval_session.token = rare_token(rng);

        SplitMix64 local{rng.next()};
        // Deterministic entry texts: planted items carry kind markers, marker
        // entries carry secrets that must never surface, the rest is noise.
        std::vector<std::string> texts;
        std::vector<PlantedKind> kinds;
        const auto plant = [&](PlantedKind kind, std::size_t count) {
            for (std::size_t index = 0; index < count; ++index) {
                texts.push_back(planted_prefix(kind) + eval_session.token + " item" +
                                std::to_string(index));
                kinds.push_back(kind);
            }
        };
        plant(PlantedKind::Constraint, config.planted_constraints);
        plant(PlantedKind::Decision, config.planted_decisions);
        plant(PlantedKind::Thread, config.planted_threads);
        plant(PlantedKind::Preference, config.planted_preferences);
        for (std::size_t index = 0; index < config.marker_entries; ++index) {
            // Sensitive content in the conversation: legal as input (real
            // sessions contain pasted secrets), forbidden in the checkpoint.
            texts.push_back("paste credentials api_key=sk-" + eval_session.token +
                            "-secret and password=" + eval_session.token);
            kinds.push_back(PlantedKind::Noise);
        }
        while (texts.size() < config.entries_per_session) {
            texts.push_back("noise " + rare_token(local) + " " + rare_token(local));
            kinds.push_back(PlantedKind::Noise);
        }

        // Deterministic shuffle so planted positions vary across the prefix.
        for (std::size_t index = texts.size(); index > 1; --index) {
            const std::size_t swap = static_cast<std::size_t>(local.next() % index);
            std::swap(texts[index - 1], texts[swap]);
            std::swap(kinds[index - 1], kinds[swap]);
        }

        for (std::size_t index = 0; index < texts.size(); ++index) {
            ConversationEntry entry;
            entry.kind = ConversationEntry::Kind::UserMessage;
            entry.text = texts[index];
            entry.origin = EventId{id_from(local, "event")};
            entry.session_sequence = sequence++;
            eval_session.entries.push_back(entry);
            eval_session.entry_origins.push_back(entry.origin);
            if (kinds[index] != PlantedKind::Noise) {
                PlantedItem planted;
                planted.kind = kinds[index];
                planted.content = texts[index];
                planted.origin = entry.origin;
                eval_session.planted.push_back(std::move(planted));
            }
        }
        dataset.sessions.push_back(std::move(eval_session));
    }

    std::vector<std::string> sorted_texts;
    for (const auto &eval_session : dataset.sessions) {
        for (const auto &entry : eval_session.entries) {
            sorted_texts.push_back(entry.text);
        }
    }
    std::sort(sorted_texts.begin(), sorted_texts.end());
    std::string joined;
    for (const auto &text : sorted_texts) {
        joined += text;
        joined += '\n';
    }
    dataset.digest_hex_text = digest_hex(digest_string(joined));
    return dataset;
}

// ---------------------------------------------------------------------------
// Scripted consolidation supplier (deterministic "small model")
// ---------------------------------------------------------------------------

class ScriptedConsolidationProvider final : public IModelProvider {
  public:
    ScriptedConsolidationProvider() {
        auto profile = std::make_shared<ModelProfile>();
        profile->id = ModelProfileId{id_from(rng_, "profile")};
        profile->display_name = "scripted-consolidator";
        profile->version = SemanticVersion{1, 0, 0};
        profile->endpoint_origin = "https://consolidation.eval";
        profile->model_selector = "scripted-extractive-v1";
        profile_ = profile;
    }

    [[nodiscard]] const ModelProfile &profile() const override { return *profile_; }

    [[nodiscard]] Result<ModelResponse> infer(const ModelRequest &request,
                                              const OperationContext &,
                                              const ProviderInferOptions &) override {
        ++calls_;
        if (fail_) {
            Error unavailable;
            unavailable.code = ErrorCode::Unavailable;
            unavailable.domain = "eval";
            unavailable.safe_message = "scripted provider down";
            return unavailable;
        }
        // React to the request, not to the dataset: parse the numbered
        // transcript the consolidator built.
        const auto *transcript_part = std::get_if<TextPart>(&request.input[1].content[0]);
        if (transcript_part == nullptr) {
            Error invalid;
            invalid.code = ErrorCode::InvalidArgument;
            invalid.domain = "eval";
            invalid.safe_message = "no transcript input";
            return invalid;
        }
        std::string constraints;
        std::string decisions;
        std::string threads;
        std::string preferences;
        const auto add_statement = [](std::string &into, const std::string &index_text,
                                      const std::string &body) {
            if (!into.empty()) {
                into += ",";
            }
            into += "{\"content\":\"" + body + "\",\"sources\":[" + index_text +
                    "],\"confidence\":0.9}";
        };
        std::istringstream stream(transcript_part->text);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.size() < 2 || line.front() != '[') {
                continue;
            }
            const auto close = line.find(']');
            if (close == std::string::npos) {
                continue;
            }
            // "[<n>|seq=<s>] <body>" — the citation is the entry number only.
            std::string index_text = line.substr(1, close - 1);
            const auto pipe = index_text.find('|');
            if (pipe != std::string::npos) {
                index_text = index_text.substr(0, pipe);
            }
            // Entry lines carry the role prefix from the segment text
            // ("user: "/"loop: "); strip it before matching the planted
            // statement markers.
            std::string body = line.substr(close + 2);
            if (body.rfind("user: ", 0) == 0) {
                body = body.substr(6);
            } else if (body.rfind("loop: ", 0) == 0) {
                body = body.substr(6);
            }
            if (body.rfind("constraint: ", 0) == 0) {
                add_statement(constraints, index_text, body);
            } else if (body.rfind("decision: ", 0) == 0) {
                add_statement(decisions, index_text, body);
            } else if (body.rfind("thread: ", 0) == 0) {
                add_statement(threads, index_text, body);
            } else if (body.rfind("preference: ", 0) == 0) {
                add_statement(preferences, index_text, body);
            }
            // Marker and noise lines are never restated: the scripted
            // supplier models a well-behaved small model; the consolidator's
            // output filter is the second line of defence (D3).
        }
        std::string json = "{\"summary\":\"scripted consolidation\",\"confidence\":0.85";
        json += ",\"constraints\":[" + constraints + "]";
        json += ",\"decisions\":[" + decisions + "]";
        json += ",\"unresolved_threads\":[" + threads + "]";
        json += ",\"preferences\":[" + preferences + "]}";

        ModelResponse response;
        response.contract_version = SchemaVersion{1, 0};
        response.request_id = request.request_id;
        response.operation_id = request.operation_id;
        response.profile_id = profile_->id;
        response.requested_model = profile_->model_selector;
        response.status = ModelCompletionStatus::Completed;
        MessageOutput message;
        OutputTextPart text;
        text.text = std::move(json);
        message.content.emplace_back(std::move(text));
        response.output.emplace_back(std::move(message));
        return response;
    }

    bool fail_ = false;
    std::uint64_t calls_ = 0;

  private:
    SplitMix64 rng_{0x5343'4f4e'5345'4d31ULL};
    std::shared_ptr<const ModelProfile> profile_;
};

// ---------------------------------------------------------------------------
// Round plumbing
// ---------------------------------------------------------------------------

struct RoundMetrics final {
    std::size_t sessions = 0;
    std::size_t planted_constraints = 0;
    std::size_t planted_decisions = 0;
    std::size_t planted_threads = 0;
    std::size_t recalled_constraints = 0;
    std::size_t recalled_decisions = 0;
    std::size_t recalled_threads = 0;
    std::size_t statements = 0;
    std::size_t matched_statements = 0;
    std::size_t provenance_violations = 0;
    std::size_t marker_violations = 0;
    std::size_t cross_session_violations = 0;
    std::size_t committed = 0;
    std::size_t preference_candidates = 0;
    std::size_t consolidation_errors = 0;
    std::size_t stale_discarded = 0;
    std::size_t terminal_discarded = 0;
    std::size_t stale_committed = 0;
    std::uint64_t checkpoint_bytes = 0;
    std::uint64_t presented_bytes = 0;
    double compression_ratio = 0.0;

    [[nodiscard]] bool equals(const RoundMetrics &other) const {
        return sessions == other.sessions && planted_constraints == other.planted_constraints &&
               planted_decisions == other.planted_decisions &&
               planted_threads == other.planted_threads &&
               recalled_constraints == other.recalled_constraints &&
               recalled_decisions == other.recalled_decisions &&
               recalled_threads == other.recalled_threads && statements == other.statements &&
               matched_statements == other.matched_statements &&
               provenance_violations == other.provenance_violations &&
               marker_violations == other.marker_violations &&
               cross_session_violations == other.cross_session_violations &&
               committed == other.committed && preference_candidates == other.preference_candidates &&
               consolidation_errors == other.consolidation_errors &&
               stale_discarded == other.stale_discarded &&
               terminal_discarded == other.terminal_discarded &&
               stale_committed == other.stale_committed &&
               checkpoint_bytes == other.checkpoint_bytes &&
               presented_bytes == other.presented_bytes &&
               compression_ratio == other.compression_ratio;
    }

    [[nodiscard]] double constraint_recall() const {
        return planted_constraints == 0
                   ? 1.0
                   : static_cast<double>(recalled_constraints) /
                         static_cast<double>(planted_constraints);
    }
    [[nodiscard]] double decision_recall() const {
        return planted_decisions == 0
                   ? 1.0
                   : static_cast<double>(recalled_decisions) /
                         static_cast<double>(planted_decisions);
    }
    [[nodiscard]] double thread_recall() const {
        return planted_threads == 0
                   ? 1.0
                   : static_cast<double>(recalled_threads) / static_cast<double>(planted_threads);
    }
    [[nodiscard]] double precision() const {
        return statements == 0
                   ? 1.0
                   : static_cast<double>(matched_statements) / static_cast<double>(statements);
    }
    [[nodiscard]] double provenance_accuracy() const {
        return statements == 0
                   ? 1.0
                   : static_cast<double>(statements - provenance_violations) /
                         static_cast<double>(statements);
    }
};

// One full pipeline round: segment every session prefix, consolidate through
// the model-backed reference consolidator, commit with five-tuple validation,
// then audit the committed checkpoints against the planted ground truth. In
// the degraded mode the provider fails after one seeded good commit, so the
// round also proves "failure keeps the previous checkpoint".
struct RoundResult final {
    RoundMetrics metrics;
    std::vector<ConversationCheckpoint> committed_checkpoints;
    std::optional<Sha256Digest> seeded_digest;
    bool seeded_retained = false;
};

[[nodiscard]] ConsolidationOptions make_options_for(const EvalSession &eval_session,
                                                    const FrozenConfig &config) {
    ConsolidationOptions options;
    options.identity.task = eval_session.task;
    options.identity.task_epoch = config.task_epoch;
    options.identity.environment_epoch = config.environment_epoch;
    return options;
}

[[nodiscard]] RoundResult run_round(const Dataset &dataset, const FrozenConfig &config,
                                    bool provider_fails) {
    ScriptedConsolidationProvider provider;
    ProviderSemanticConsolidator consolidator(provider);
    InMemoryConversationCheckpointStore store;
    RoundResult result;
    result.metrics.sessions = dataset.sessions.size();

    for (std::size_t index = 0; index < dataset.sessions.size(); ++index) {
        const auto &eval_session = dataset.sessions[index];
        ConversationSegmentationOptions segmentation;
        segmentation.window_entries = eval_session.entries.size() + 1; // one prefix segment
        const auto segments =
            segment_conversation(eval_session.session, eval_session.entries, segmentation);
        if (!segments.has_value() || segments.value().size() != 1) {
            std::cerr << "segmentation failed\n";
            std::exit(2);
        }
        const ConversationSegment &segment = segments.value().front();
        result.metrics.presented_bytes += segment.text.size();

        // Degraded mode: seed one good commit, then fail every provider call.
        if (provider_fails && index == 0) {
            const auto seeded = consolidator.consolidate(segment, make_options_for(eval_session, config));
            if (!seeded.has_value()) {
                std::cerr << "seeding consolidation failed\n";
                std::exit(2);
            }
            ConversationCommitState live;
            live.session = eval_session.session;
            live.task = eval_session.task;
            live.task_epoch = config.task_epoch;
            live.environment_epoch = config.environment_epoch;
            const auto outcome = commit_conversation_checkpoint(store, seeded.value(), live);
            if (outcome.disposition != ConversationCommitDisposition::Committed) {
                std::cerr << "seeding commit failed\n";
                std::exit(2);
            }
            result.seeded_digest = seeded.value().projection_digest();
            provider.fail_ = true;
        }

        const auto consolidated = consolidator.consolidate(segment, make_options_for(eval_session, config));
        if (!consolidated.has_value()) {
            ++result.metrics.consolidation_errors;
            continue;
        }
        const ConversationCheckpoint &candidate = consolidated.value();

        ConversationCommitState live;
        live.session = eval_session.session;
        live.task = eval_session.task;
        live.task_epoch = config.task_epoch;
        live.environment_epoch = config.environment_epoch;
        const auto outcome = commit_conversation_checkpoint(store, candidate, live);
        if (outcome.disposition == ConversationCommitDisposition::Committed) {
            ++result.metrics.committed;
            result.committed_checkpoints.push_back(candidate);
        }

        // Recall per kind: a planted item is recalled when a committed
        // statement quotes its content and cites its origin.
        const auto recall_of = [&](PlantedKind kind) {
            std::size_t hits = 0;
            const auto &sections = kind == PlantedKind::Constraint
                                       ? candidate.constraints
                                       : (kind == PlantedKind::Decision ? candidate.decisions
                                                                        : candidate.unresolved_threads);
            for (const auto &planted : eval_session.planted) {
                if (planted.kind != kind) {
                    continue;
                }
                for (const auto &statement : sections) {
                    if (statement.content == planted.content &&
                        std::find(statement.source_events.begin(), statement.source_events.end(),
                                  planted.origin) != statement.source_events.end()) {
                        ++hits;
                        break;
                    }
                }
            }
            return hits;
        };
        result.metrics.recalled_constraints += recall_of(PlantedKind::Constraint);
        result.metrics.recalled_decisions += recall_of(PlantedKind::Decision);
        result.metrics.recalled_threads += recall_of(PlantedKind::Thread);

        // Statement-level audit: precision, provenance, markers, ACL.
        const auto audit = [&](const std::vector<ConversationStatement> &statements,
                               PlantedKind kind) {
            for (const auto &statement : statements) {
                ++result.metrics.statements;
                bool planted_match = false;
                for (const auto &planted : eval_session.planted) {
                    if (planted.kind != kind) {
                        continue;
                    }
                    if (statement.content == planted.content &&
                        std::find(statement.source_events.begin(), statement.source_events.end(),
                                  planted.origin) != statement.source_events.end()) {
                        planted_match = true;
                        break;
                    }
                }
                if (planted_match) {
                    ++result.metrics.matched_statements;
                }
                const bool provenance_ok = !statement.source_events.empty();
                bool in_session = true;
                for (const auto &event : statement.source_events) {
                    if (std::find(eval_session.entry_origins.begin(), eval_session.entry_origins.end(),
                                  event) == eval_session.entry_origins.end()) {
                        in_session = false;
                    }
                }
                if (!provenance_ok || !in_session) {
                    ++result.metrics.provenance_violations;
                    if (!in_session) {
                        ++result.metrics.cross_session_violations;
                    }
                }
                if (statement.content.find("api_key") != std::string::npos ||
                    statement.content.find("password=") != std::string::npos) {
                    ++result.metrics.marker_violations;
                }
                if (kind == PlantedKind::Preference) {
                    ++result.metrics.preference_candidates;
                }
                result.metrics.checkpoint_bytes += statement.content.size();
            }
        };
        audit(candidate.constraints, PlantedKind::Constraint);
        audit(candidate.decisions, PlantedKind::Decision);
        audit(candidate.unresolved_threads, PlantedKind::Thread);
        audit(candidate.preferences, PlantedKind::Preference);
    }
    result.metrics.planted_constraints = dataset.sessions.size() * config.planted_constraints;
    result.metrics.planted_decisions = dataset.sessions.size() * config.planted_decisions;
    result.metrics.planted_threads = dataset.sessions.size() * config.planted_threads;
    result.metrics.compression_ratio =
        result.metrics.presented_bytes == 0
            ? 0.0
            : static_cast<double>(result.metrics.checkpoint_bytes) /
                  static_cast<double>(result.metrics.presented_bytes);

    // Stale-replay and terminal discipline over the committed chain.
    if (!provider_fails) {
        for (const auto &checkpoint : result.committed_checkpoints) {
            ConversationCommitState live;
            live.session = checkpoint.session_id;
            live.task = checkpoint.task_id;
            live.task_epoch = checkpoint.task_epoch;
            live.environment_epoch = checkpoint.environment_epoch;
            if (checkpoint.through_event_sequence > 1) {
                auto stale = checkpoint;
                stale.through_event_sequence -= 1;
                stale.id = conversation_checkpoint_id_from_seed(
                    stale.session_id.to_string() + "|" + std::to_string(stale.through_event_sequence));
                const auto outcome = commit_conversation_checkpoint(store, stale, live);
                if (outcome.disposition == ConversationCommitDisposition::DiscardedStale) {
                    ++result.metrics.stale_discarded;
                } else if (outcome.disposition == ConversationCommitDisposition::Committed) {
                    ++result.metrics.stale_committed;
                }
            }
            auto terminal = live;
            terminal.task_terminal = true;
            const auto terminal_outcome =
                commit_conversation_checkpoint(store, checkpoint, terminal);
            if (terminal_outcome.disposition == ConversationCommitDisposition::DiscardedTerminal) {
                ++result.metrics.terminal_discarded;
            }
        }
    }

    // Degraded round: the seeded checkpoint must be untouched.
    if (provider_fails && result.seeded_digest.has_value()) {
        const auto latest = store.latest(dataset.sessions.front().session);
        if (latest.has_value() && latest.value().has_value() &&
            latest.value()->projection_digest() == *result.seeded_digest) {
            result.seeded_retained = true;
        }
    }
    return result;
}

[[nodiscard]] JsonValue round_json(const RoundMetrics &metrics) {
    JsonValue::Object object;
    object.emplace_back("sessions", static_cast<std::int64_t>(metrics.sessions));
    object.emplace_back("planted_constraints", static_cast<std::int64_t>(metrics.planted_constraints));
    object.emplace_back("planted_decisions", static_cast<std::int64_t>(metrics.planted_decisions));
    object.emplace_back("planted_threads", static_cast<std::int64_t>(metrics.planted_threads));
    object.emplace_back("recalled_constraints", static_cast<std::int64_t>(metrics.recalled_constraints));
    object.emplace_back("recalled_decisions", static_cast<std::int64_t>(metrics.recalled_decisions));
    object.emplace_back("recalled_threads", static_cast<std::int64_t>(metrics.recalled_threads));
    object.emplace_back("constraint_recall", metrics.constraint_recall());
    object.emplace_back("decision_recall", metrics.decision_recall());
    object.emplace_back("thread_recall", metrics.thread_recall());
    object.emplace_back("statements", static_cast<std::int64_t>(metrics.statements));
    object.emplace_back("matched_statements", static_cast<std::int64_t>(metrics.matched_statements));
    object.emplace_back("precision", metrics.precision());
    object.emplace_back("provenance_accuracy", metrics.provenance_accuracy());
    object.emplace_back("provenance_violations", static_cast<std::int64_t>(metrics.provenance_violations));
    object.emplace_back("marker_violations", static_cast<std::int64_t>(metrics.marker_violations));
    object.emplace_back("cross_session_violations",
                        static_cast<std::int64_t>(metrics.cross_session_violations));
    object.emplace_back("committed", static_cast<std::int64_t>(metrics.committed));
    object.emplace_back("preference_candidates",
                        static_cast<std::int64_t>(metrics.preference_candidates));
    object.emplace_back("consolidation_errors",
                        static_cast<std::int64_t>(metrics.consolidation_errors));
    object.emplace_back("stale_discarded", static_cast<std::int64_t>(metrics.stale_discarded));
    object.emplace_back("terminal_discarded", static_cast<std::int64_t>(metrics.terminal_discarded));
    object.emplace_back("stale_committed", static_cast<std::int64_t>(metrics.stale_committed));
    object.emplace_back("checkpoint_bytes", static_cast<std::int64_t>(metrics.checkpoint_bytes));
    object.emplace_back("presented_bytes", static_cast<std::int64_t>(metrics.presented_bytes));
    object.emplace_back("compression_ratio", metrics.compression_ratio);
    return JsonValue(std::move(object));
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

    const FrozenConfig config;
    const Dataset dataset = build_dataset(config);
    if (!config.dataset_digest.empty() && dataset.digest_hex_text != config.dataset_digest) {
        std::cerr << "dataset digest mismatch: got " << dataset.digest_hex_text << ", want "
                  << config.dataset_digest << "\n";
        return 2;
    }

    const RoundResult hybrid = run_round(dataset, config, false);
    const RoundResult degraded = run_round(dataset, config, true);
    const RoundResult repeat = run_round(dataset, config, false);

    bool gates_ok = true;
    std::vector<std::string> failures;
    // D1: pipeline recall and precision of the committed checkpoints.
    const bool d1 = hybrid.metrics.constraint_recall() == 1.0 &&
                    hybrid.metrics.decision_recall() == 1.0 &&
                    hybrid.metrics.thread_recall() == 1.0 && hybrid.metrics.precision() == 1.0;
    if (!d1) {
        gates_ok = false;
        failures.push_back("D1: recall/precision not preserved (constraint " +
                           std::to_string(hybrid.metrics.constraint_recall()) + ", decision " +
                           std::to_string(hybrid.metrics.decision_recall()) + ", thread " +
                           std::to_string(hybrid.metrics.thread_recall()) + ", precision " +
                           std::to_string(hybrid.metrics.precision()) + ")");
    }
    // D2: provenance accuracy and watermark binding.
    bool watermarks_bound = true;
    for (std::size_t index = 0; index < hybrid.committed_checkpoints.size(); ++index) {
        if (hybrid.committed_checkpoints[index].through_event_sequence == 0) {
            watermarks_bound = false;
        }
    }
    const bool d2 = hybrid.metrics.provenance_violations == 0 && watermarks_bound &&
                    hybrid.metrics.provenance_accuracy() == 1.0;
    if (!d2) {
        gates_ok = false;
        failures.push_back("D2: provenance violations " +
                           std::to_string(hybrid.metrics.provenance_violations));
    }
    // D3: zero marker and cross-session leakage in every committed statement.
    const bool d3 = hybrid.metrics.marker_violations == 0 &&
                    hybrid.metrics.cross_session_violations == 0 &&
                    degraded.metrics.marker_violations == 0 &&
                    degraded.metrics.cross_session_violations == 0;
    if (!d3) {
        gates_ok = false;
        failures.push_back("D3: marker or cross-session leakage observed");
    }
    // D4: commit discipline — failures commit nothing and keep the previous
    // checkpoint; stale and terminal replays are discarded, never committed.
    const bool d4 = degraded.metrics.consolidation_errors == dataset.sessions.size() &&
                    degraded.metrics.committed == 0 && degraded.seeded_retained &&
                    hybrid.metrics.stale_discarded == hybrid.committed_checkpoints.size() &&
                    hybrid.metrics.terminal_discarded == hybrid.committed_checkpoints.size() &&
                    hybrid.metrics.stale_committed == 0;
    if (!d4) {
        gates_ok = false;
        failures.push_back("D4: commit discipline violated (errors " +
                           std::to_string(degraded.metrics.consolidation_errors) + ", commits " +
                           std::to_string(degraded.metrics.committed) + ", retained " +
                           std::to_string(degraded.seeded_retained ? 1 : 0) + ", stale_committed " +
                           std::to_string(hybrid.metrics.stale_committed) + ")");
    }
    // D5: repeated in-process rounds agree on every aggregate.
    const bool d5 = repeat.metrics.equals(hybrid.metrics);
    if (!d5) {
        gates_ok = false;
        failures.push_back("D5: repeated evaluation diverged");
    }

    JsonValue::Object report;
    report.emplace_back("schema", "mira.m19.consolidation-eval.v1");
    report.emplace_back("dataset_digest", dataset.digest_hex_text);
    JsonValue::Object config_json;
    config_json.emplace_back("seed", static_cast<std::int64_t>(config.seed));
    config_json.emplace_back("sessions", static_cast<std::int64_t>(config.sessions));
    config_json.emplace_back("entries_per_session",
                             static_cast<std::int64_t>(config.entries_per_session));
    config_json.emplace_back("planted_constraints",
                             static_cast<std::int64_t>(config.planted_constraints));
    config_json.emplace_back("planted_decisions",
                             static_cast<std::int64_t>(config.planted_decisions));
    config_json.emplace_back("planted_threads", static_cast<std::int64_t>(config.planted_threads));
    config_json.emplace_back("planted_preferences",
                             static_cast<std::int64_t>(config.planted_preferences));
    config_json.emplace_back("marker_entries", static_cast<std::int64_t>(config.marker_entries));
    report.emplace_back("config", JsonValue(std::move(config_json)));
    report.emplace_back("hybrid_round", round_json(hybrid.metrics));
    report.emplace_back("degraded_round", round_json(degraded.metrics));
    report.emplace_back("degraded_seeded_retained", degraded.seeded_retained);
    JsonValue::Object gates_json;
    gates_json.emplace_back("d1_pipeline_recall_precision", d1);
    gates_json.emplace_back("d2_provenance_accuracy", d2);
    gates_json.emplace_back("d3_safety_zero_violations", d3);
    gates_json.emplace_back("d4_commit_discipline", d4);
    gates_json.emplace_back("d5_determinism", d5);
    report.emplace_back("gates", JsonValue(std::move(gates_json)));
    JsonValue::Array failure_list;
    for (const auto &message : failures) {
        failure_list.emplace_back(message);
    }
    report.emplace_back("gate_failures", JsonValue(std::move(failure_list)));
    report.emplace_back(
        "limitations",
        "scripted extractive supplier behind IModelProvider, not a real model: metrics measure "
        "pipeline behaviour (provenance binding, marker/ACL filtering, five-tuple commit "
        "discipline, determinism), not semantic consolidation quality; Constraint Recall/"
        "Precision, contradiction and hallucination claims against real content require a real "
        "small model via the supply-chain review channel (DEC-032 §5/§9, RULE-10)");

    const std::string json = to_json_string(JsonValue(std::move(report)));
    std::cout << json << '\n';
    if (!output_path.empty()) {
        std::ofstream file(output_path);
        file << json << '\n';
    }

    if (!gates_ok) {
        for (const auto &message : failures) {
            std::cerr << "gate failure: " << message << '\n';
        }
    }
    return gates_ok ? 0 : 1;
}
