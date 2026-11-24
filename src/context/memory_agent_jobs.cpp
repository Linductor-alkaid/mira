#include "context_text.hpp"
#include "memory_agent_support.hpp"

#include <mira/context_retrieval.hpp>

#include <algorithm>
#include <iterator>
#include <set>
#include <span>
#include <string_view>

namespace mira {
using memory_agent_support::error;
using memory_agent_support::history_json;
namespace {
// Recall facts merge into one framed user-role block (DEC-1xx): bare
// statements as individual user messages read as live user speech, and
// per-record speaker labels would only re-spend tokens. Verbatim conversation
// replay never goes through here — that path expands speaker-tagged turns
// into native roles via `context_items_from_candidate`.
constexpr std::string_view kRecallHeader = "[recalled memory v1|count=";
constexpr std::string_view kRecallSeparator = "\n- ";
constexpr const char *kMemoryAuditPrefix = "mira.memory-agent.";
// The group cap is at most 64 records, so the count and closing bracket use
// at most three bytes.
constexpr std::size_t kRecallHeaderMaxBytes = kRecallHeader.size() + 3;

ContextItem memory_block(const std::vector<MemoryRecord> &records, const OperationId &operation,
                         std::optional<std::size_t> group_index) {
    ContextItem item;
    auto seed =
        "mira.memory.recall|" + operation.to_string() + "|" + std::to_string(records.size());
    if (group_index)
        seed += "|" + std::to_string(*group_index);
    item.id = ContextItemId{context_asset_id_from_seed(seed).value};
    item.kind = ContextItemKind::RetrievedMemory;
    item.authority = ContextAuthority::RetrievedMemory;
    item.sequence = group_index.value_or(0);
    Sensitivity authority = Sensitivity::Public;
    std::string text = std::string{kRecallHeader} + std::to_string(records.size()) + "]";
    for (const auto &record : records) {
        text += kRecallSeparator;
        text += record.statement;
        authority = std::max(authority, record.sensitivity);
        item.provenance.insert(item.provenance.end(), record.provenance.begin(),
                               record.provenance.end());
    }
    item.content.emplace_back(TextPart{std::move(text), authority});
    return item;
}
bool in_scopes(const MemoryRecord &record, const std::vector<MemoryScope> &scopes) {
    return std::find(scopes.begin(), scopes.end(), record.scope) != scopes.end();
}
bool in_group(const MemoryRecord &record, const MemoryRecallGroup &group) {
    return group.kinds.empty() ||
           std::find(group.kinds.begin(), group.kinds.end(), record.kind) != group.kinds.end();
}
bool group_uses_embedding(const MemoryRecallGroup &group) {
    return std::all_of(group.kinds.begin(), group.kinds.end(), [](MemoryKind kind) {
        const auto &legs = memory_kind_policy(kind).retrieval_legs;
        return std::find(legs.begin(), legs.end(), MemoryRetrievalLeg::Semantic) != legs.end();
    });
}
class ProposedMemories final : public IConsolidationModel {
  public:
    std::vector<MemoryCandidate> candidates;
    std::vector<MemoryCandidate> propose(std::span<const EventEnvelope>,
                                         const MemoryScope &) override {
        return candidates;
    }
};
} // namespace

Result<void> MemoryAgentService::State::emit(const Job &job, const char *type,
                                             const MemoryAgentResult *result) {
    JsonValue::Object detail{{"operation", job.id.to_string()},
                             {"revision", std::to_string(job.revision)},
                             {"mode", static_cast<std::int64_t>(job.request.mode)},
                             {"source_digest", prompt_digest(job.request.history).to_string()}};
    if (result) {
        detail.emplace_back("candidates", static_cast<std::int64_t>(result->candidates.size()));
        detail.emplace_back("mutations",
                            static_cast<std::int64_t>(result->consolidation.entries.size()));
        detail.emplace_back("compressed_items",
                            static_cast<std::int64_t>(result->compressed_items));
        detail.emplace_back("through_sequence", std::to_string(result->through_event_sequence));
        if (result->projection_artifact) {
            detail.emplace_back("projection_artifact", result->projection_artifact->id.to_string());
            detail.emplace_back("projection_digest",
                                result->projection_artifact->digest.to_string());
        }
    } else if (job.request.mode == MemoryAgentMode::Record) {
        detail.emplace_back("through_sequence", std::to_string(job.after_sequence));
    }
    AppendRequest append;
    append.event_id = EventId::generate();
    append.runtime_id = runtime;
    append.session_id = job.binding.work.session_id;
    append.task_id = job.request.task;
    append.payload = {type, to_json_string(JsonValue{std::move(detail)}), EventClass::Diagnostic};
    // Durability of normal projection diagnostics is not a source guarantee.
    // Erasure barriers have a separate mandatory ProcessCrash ack.
    append.required = Durability::Buffered;
    try {
        auto receipt = events->append(append);
        if (!receipt)
            return error(ErrorCode::DataLoss,
                         "memory agent audit failed; mutation may have committed");
    } catch (...) {
        return error(ErrorCode::DataLoss, "memory agent audit raised an exception");
    }
    return {};
}

Result<MemoryAgentHandle> MemoryAgentService::State::submit(MemoryAgentRequest request,
                                                            OperationContext context) {
    if (request.mode != MemoryAgentMode::Recall && request.mode != MemoryAgentMode::Record &&
        request.mode != MemoryAgentMode::Compress)
        return error(ErrorCode::InvalidArgument, "unknown memory agent mode");
    if (request.record_page_limit == 0 || request.record_page_limit > kMaxRecordPagesPerJob ||
        (request.mode != MemoryAgentMode::Record && request.record_page_limit != 1))
        return error(ErrorCode::InvalidArgument, "invalid memory record page limit");
    if (request.query.size() > 4 * 1024 || request.history.size() > config.max_history_items)
        return error(ErrorCode::ResourceExhausted, "memory job input capacity exceeded");
    if (request.mode == MemoryAgentMode::Compress &&
        (!processor || request.protected_items == 0 ||
         request.protected_items >= request.history.size() ||
         !memory_agent_support::complete_history(request.history)))
        return error(ErrorCode::InvalidArgument, "invalid memory compression source");
    if (request.mode != MemoryAgentMode::Compress && !request.history.empty())
        return error(ErrorCode::InvalidArgument, "history is only accepted by compression");
    if (request.mode == MemoryAgentMode::Compress) {
        request.history.resize(request.history.size() - request.protected_items);
        if (!memory_agent_support::complete_history(request.history))
            return error(ErrorCode::InvalidArgument,
                         "compression cut splits a native tool round trip");
        if (to_json_string(history_json(request.history)).size() > config.max_source_bytes)
            return error(ErrorCode::ResourceExhausted,
                         "memory compression source exceeds byte capacity");
    }
    const std::lock_guard lock(mutex);
    const auto found = works.find(request.task);
    if (!initialized || closing || found == works.end() || found->second.revoked ||
        found->second.input_pending ||
        (request.mode != MemoryAgentMode::Record &&
         (!found->second.active || !memory_agent_support::request_current(found->second.binding))))
        return error(ErrorCode::InvalidState, "memory work is not active");
    if (!memory_agent_support::same_owner(context, found->second.binding.work))
        return error(ErrorCode::PermissionDenied, "memory job context does not own the work");
    if (request.expected_revision && *request.expected_revision != found->second.revision)
        return error(ErrorCode::InvalidState, "captured memory source revision is stale");
    if (context.cancelled_or_expired(Timestamp::now()))
        return error(context.cancelled() ? ErrorCode::Cancelled : ErrorCode::DeadlineExceeded,
                     "memory job caller is interrupted");
    if (request.mode == MemoryAgentMode::Record &&
        (!request.write_scope ||
         std::find(found->second.binding.scopes.begin(), found->second.binding.scopes.end(),
                   *request.write_scope) == found->second.binding.scopes.end()))
        return error(ErrorCode::PermissionDenied, "memory write scope is not authorized");
    if (jobs.size() >= config.max_jobs ||
        std::any_of(jobs.begin(), jobs.end(), [&](const auto &entry) {
            return !entry.second->done && entry.second->request.task == request.task;
        }))
        return error(ErrorCode::ResourceExhausted, "memory job capacity is exhausted");
    auto job = std::make_shared<Job>();
    job->request = std::move(request);
    job->binding = found->second.binding;
    job->revision = found->second.revision;
    job->after_sequence = found->second.recorded_through;
    job->record_snapshot = found->second.record_snapshot;
    job->context = std::move(context);
    const auto deadline = std::chrono::steady_clock::now() + config.timeout;
    if (!job->context.deadline || deadline < *job->context.deadline)
        job->context.deadline = deadline;
    if (auto logged = emit(*job, "mira.memory-agent.started.v1"); !logged)
        return logged.error();
    jobs.emplace(job->id, job);
    kairo::BlockingWorkerSpec worker;
    worker.name = "mira-memory-" + job->id.to_string();
    worker.config.thread_name = worker.name;
    const auto weak = weak_from_this();
    worker.worker =
        std::make_unique<memory_agent_support::Worker>([weak, job](kairo::StopToken stop) {
            if (const auto state = weak.lock())
                state->run(job, stop);
        });
    try {
        job->worker = executor.start_worker(std::move(worker));
    } catch (...) {
        job->worker.reset();
    }
    if (!job->worker || !job->worker->started()) {
        jobs.erase(job->id);
        return error(ErrorCode::ResourceExhausted, "Executor rejected memory worker");
    }
    return MemoryAgentHandle{job->id, job->future};
}

void MemoryAgentService::State::run(const std::shared_ptr<Job> &job, kairo::StopToken stop) {
    {
        const std::lock_guard lock(mutex);
        job->worker_id = std::this_thread::get_id();
    }
    auto context = job->context;
    const auto inherited = context.cancellation_requested;
    context.cancellation_requested = [job, inherited, stop] {
        return job->cancelled.load() || stop.stop_requested() || (inherited && inherited());
    };
    Result<MemoryAgentResult> outcome = error(ErrorCode::Internal, "memory job failed");
    try {
        outcome = execute(job, context);
        for (std::size_t pages = 1; pages < job->request.record_page_limit && outcome &&
                                    outcome.value().more_pending && job->record_snapshot;
             ++pages) {
            auto next = execute(job, context);
            if (!next) {
                outcome = next.error();
                break;
            }
            auto &combined = outcome.value();
            auto &page = next.value();
            combined.revision = page.revision;
            combined.through_event_sequence = page.through_event_sequence;
            combined.more_pending = page.more_pending;
            combined.degraded = combined.degraded || page.degraded;
            auto &report = combined.consolidation;
            report.candidates_examined += page.consolidation.candidates_examined;
            report.candidates_truncated += page.consolidation.candidates_truncated;
            std::ranges::move(page.consolidation.entries, std::back_inserter(report.entries));
            std::ranges::move(page.consolidation.pending_approval,
                              std::back_inserter(report.pending_approval));
        }
    } catch (...) {
        outcome = error(ErrorCode::Internal, "memory job raised an exception");
    }
    {
        const std::lock_guard lock(mutex);
        if (auto logged = emit(
                *job, outcome ? "mira.memory-agent.completed.v1" : "mira.memory-agent.failed.v1",
                outcome ? &outcome.value() : nullptr);
            !logged) {
            audit_error = logged.error();
            if (auto work = works.find(job->request.task); work != works.end())
                invalidate(work->second);
            outcome = logged.error();
        }
        // Drop potentially sensitive captured source before reporting cleanup.
        job->request.history.clear();
        job->done = true;
        job->promise.set_value(std::move(outcome));
    }
    cv.notify_all();
}

Result<MemoryAgentResult> MemoryAgentService::State::execute(const std::shared_ptr<Job> &job,
                                                             const OperationContext &context) {
    const auto interrupted = [&]() -> Result<void> {
        if (context.cancelled_or_expired(Timestamp::now()))
            return error(context.cancelled() ? ErrorCode::Cancelled : ErrorCode::DeadlineExceeded,
                         "memory job was interrupted");
        const std::lock_guard lock(mutex);
        if (!current(*job))
            return error(ErrorCode::InvalidState, "memory job version is stale");
        return {};
    };
    if (auto valid = interrupted(); !valid)
        return valid.error();
    MemoryAgentInput input;
    input.mode = job->request.mode;
    input.work = job->binding.work;
    input.user_constraints = job->binding.user_constraints;
    MemoryAgentResult result;
    std::optional<SessionSequence> pending_record_snapshot;
    auto cache_until = std::chrono::system_clock::now() + config.cache_ttl;
    result.operation = job->id;
    result.mode = input.mode;
    result.revision = job->revision;
    // DEC-111: the recall query carries the optional semantic prior. An
    // embed failure only drops the vector leg; the lexical legs answer and
    // the result is marked degraded. The call is bounded by the producer's
    // transport deadlines; cancellation of the job is honored at the next
    // store query.
    const auto embedder = [&]() -> std::shared_ptr<IEmbeddingProvider> {
        const std::lock_guard lock(mutex);
        return recall_embedder;
    }();
    const bool grouped = !config.recall_groups.empty();
    const MemoryRecallGroup default_group{{}, config.max_records, config.max_result_bytes};
    const auto groups = grouped ? std::span<const MemoryRecallGroup>{config.recall_groups}
                                : std::span<const MemoryRecallGroup>{&default_group, 1};
    const auto query_records = [&](const std::string &text, const auto &consume) -> Result<void> {
        if (auto valid = interrupted(); !valid)
            return valid.error();
        MemoryQuery query;
        query.scopes = job->binding.scopes;
        query.text = text;
        query.max_sensitivity = Sensitivity::Sensitive;
        query.deadline = std::min(config.timeout, std::chrono::milliseconds{250});
        // DEC-112 wiring: the work's label prefilter narrows the recall
        // universe before any leg runs (bounds validated at bind time).
        if (!job->binding.labels.empty()) {
            query.labels = job->binding.labels;
        }
        MemoryEmbedding embedding;
        if (embedder && std::any_of(groups.begin(), groups.end(), group_uses_embedding)) {
            auto embedded = embedder->embed(text);
            if (embedded) {
                embedding = MemoryEmbedding{std::move(embedded).value(), embedder->embedder_id()};
            } else {
                result.degraded = true;
            }
        }
        const auto cutoff =
            std::min(std::chrono::steady_clock::now() + query.deadline, *context.deadline);
        for (std::size_t index = 0; index < groups.size(); ++index) {
            if (auto valid = interrupted(); !valid)
                return valid.error();
            const auto &group = groups[index];
            if (grouped) {
                query.deadline = std::chrono::duration_cast<std::chrono::milliseconds>(
                    cutoff - std::chrono::steady_clock::now());
                if (query.deadline.count() <= 0) {
                    result.degraded = true;
                    break;
                }
                query.kinds = group.kinds;
            }
            query.max_results = group.max_records;
            query.token_budget = group.max_bytes / 4;
            query.query_embedding = group_uses_embedding(group) ? embedding : MemoryEmbedding{};
            auto queried = memory->query(query);
            if (!queried)
                return queried.error();
            if (queried.value().records.size() > group.max_records)
                return error(ErrorCode::ResourceExhausted, "memory store exceeded result capacity");
            for (const auto &record : queried.value().records) {
                if (!in_scopes(record, job->binding.scopes) || !in_group(record, group) ||
                    record.status != MemoryStatus::Active ||
                    record.sensitivity == Sensitivity::Secret)
                    return error(ErrorCode::PermissionDenied,
                                 "memory store returned unauthorized data");
                if (record.expires_at)
                    cache_until = std::min(cache_until, *record.expires_at);
                if (record.validity.valid_until)
                    cache_until = std::min(cache_until, *record.validity.valid_until);
            }
            result.degraded |= queried.value().quality.degraded ||
                               (grouped && std::chrono::steady_clock::now() >= cutoff);
            consume(std::move(queried.value().records));
        }
        return interrupted();
    };
    if (input.mode == MemoryAgentMode::Recall) {
        auto recalled = query_records(
            job->request.query.empty() ? input.work.goal : job->request.query,
            [&](std::vector<MemoryRecord> records) {
                input.records.insert(input.records.end(), std::make_move_iterator(records.begin()),
                                     std::make_move_iterator(records.end()));
            });
        if (!recalled)
            return recalled.error();
    } else if (input.mode == MemoryAgentMode::Record) {
        EventSliceQuery query;
        query.session_id = input.work.session_id;
        query.task_id = input.work.task_id;
        query.excluded_type_prefix = kMemoryAuditPrefix;
        query.after_sequence = job->after_sequence;
        query.through_sequence = job->record_snapshot;
        query.limit = config.max_events;
        query.byte_limit = config.max_source_bytes;
        auto slice = events->read_slice(query);
        if (slice) {
            auto &page = slice.value();
            if (page.scanned_through < query.after_sequence ||
                page.snapshot_through < page.scanned_through ||
                (query.through_sequence && page.snapshot_through != *query.through_sequence) ||
                page.has_more != (page.scanned_through < page.snapshot_through) ||
                (page.has_more && page.scanned_through == query.after_sequence) ||
                page.events.size() > query.limit)
                return error(ErrorCode::DataLoss, "event slice returned an invalid watermark");
            auto previous = query.after_sequence;
            for (const auto &event : page.events) {
                if (event.session_id != query.session_id || event.task_id != query.task_id ||
                    event.session_sequence <= previous ||
                    event.session_sequence > page.scanned_through ||
                    event.payload.type.starts_with(kMemoryAuditPrefix))
                    return error(ErrorCode::DataLoss, "event slice returned invalid task evidence");
                previous = event.session_sequence;
            }
            result.through_event_sequence = page.scanned_through;
            result.more_pending = page.has_more;
            if (page.has_more)
                pending_record_snapshot = page.snapshot_through;
            input.events = std::move(page.events);
        } else {
            if (slice.error().code != ErrorCode::UnsupportedCapability || query.through_sequence)
                return slice.error();
            auto page =
                events->read({input.work.session_id, job->after_sequence, config.max_events});
            if (!page)
                return page.error();
            result.more_pending = page.value().has_more;
            for (auto &event : page.value().events) {
                if (event.session_id != query.session_id)
                    return error(ErrorCode::DataLoss, "event store returned another session");
                result.through_event_sequence =
                    std::max(result.through_event_sequence, event.session_sequence);
                if (event.task_id == input.work.task_id &&
                    !event.payload.type.starts_with(kMemoryAuditPrefix))
                    input.events.push_back(std::move(event));
            }
        }
    } else
        input.history = job->request.history;

    std::size_t source_bytes = to_json_string(history_json(input.history)).size();
    for (const auto &constraint : input.user_constraints)
        source_bytes += constraint.size();
    for (const auto &record : input.records)
        source_bytes += record.statement.size();
    for (const auto &event : input.events)
        source_bytes += event.payload.data.size();
    if (source_bytes > config.max_source_bytes)
        return error(ErrorCode::ResourceExhausted, "memory source byte capacity exceeded");
    for (const auto &event : input.events)
        if (context_text::carries_marker(event.payload.data,
                                         config.consolidation.forbidden_markers))
            return error(ErrorCode::SafetyRejected, "memory source contains forbidden content");
    MemoryAgentProposal proposal;
    if (processor) {
        auto proposed = processor(input, context);
        if (!proposed)
            return proposed.error();
        proposal = std::move(proposed).value();
    }
    if (auto checked =
            memory_agent_support::validate_proposal(proposal, input, config.max_result_bytes);
        !checked)
        return checked.error();
    if (auto valid = interrupted(); !valid)
        return valid.error();

    if (input.mode == MemoryAgentMode::Recall) {
        std::vector<std::vector<MemoryRecord>> selected(groups.size());
        std::vector<std::size_t> sizes(groups.size(), kRecallHeaderMaxBytes);
        std::set<MemoryId> seen;
        const auto admit = [&](const std::vector<MemoryRecord> &records, bool filter) {
            for (const auto &record : records) {
                if (filter && !proposal.selected_memories.empty() &&
                    std::find(proposal.selected_memories.begin(), proposal.selected_memories.end(),
                              record.id) == proposal.selected_memories.end())
                    continue;
                const auto group = std::find_if(groups.begin(), groups.end(),
                                                [&](const auto &g) { return in_group(record, g); });
                const auto index = static_cast<std::size_t>(group - groups.begin());
                if (selected[index].size() >= group->max_records || seen.contains(record.id))
                    continue;
                const auto size = kRecallSeparator.size() + record.statement.size();
                if (grouped && size > group->max_bytes - sizes[index]) {
                    result.degraded = true;
                    continue;
                }
                sizes[index] += size;
                seen.insert(record.id);
                selected[index].push_back(record);
            }
        };
        admit(input.records, true);
        for (const auto &query : proposal.queries) {
            auto extra = query_records(query, [&](const auto &records) { admit(records, false); });
            if (!extra)
                return extra.error();
        }
        for (std::size_t index = 0; index < groups.size(); ++index) {
            if (!selected[index].empty())
                result.candidates.push_back(memory_block(
                    selected[index], job->id, grouped ? std::optional{index} : std::nullopt));
        }
        std::size_t bytes = 0;
        for (const auto &item : result.candidates)
            bytes += std::get<TextPart>(item.content.front()).text.size();
        if (bytes > config.max_result_bytes)
            return error(ErrorCode::ResourceExhausted, "memory context exceeds byte capacity");
    }

    const std::lock_guard serialize(commit_mutex);
    if (auto valid = interrupted(); !valid)
        return valid.error();
    if (input.mode == MemoryAgentMode::Compress) {
        auto artifact = store_projection(*job, input, proposal);
        if (!artifact)
            return artifact.error();
        result.projection_artifact = artifact.value();
    }
    if (input.mode == MemoryAgentMode::Record) {
        ProposedMemories model;
        for (const auto &memory_proposal : proposal.memories) {
            MemoryCandidate candidate;
            candidate.proposed.id = MemoryId::generate();
            candidate.proposed.kind = memory_proposal.kind;
            candidate.proposed.statement = memory_proposal.note.text;
            candidate.proposed.verification = MemoryVerification::Unverified;
            candidate.proposed.confidence = 0.5F;
            candidate.model_assisted = true;
            for (const auto citation : memory_proposal.note.citations)
                candidate.evidence.push_back(input.events[citation].event_id);
            candidate.proposed.provenance = candidate.evidence;
            model.candidates.push_back(std::move(candidate));
        }
        auto policy = config.consolidation;
        policy.allow_model_import_exemption = false;
        auto report = MemoryConsolidator{policy, processor ? &model : nullptr}.consolidate(
            *memory, input.events, *job->request.write_scope, Timestamp::now());
        if (!report)
            return report.error();
        result.consolidation = std::move(report).value();
        // A successful write changes shared knowledge even when this caller
        // is a terminal task. Invalidate all dependent request snapshots.
        const std::lock_guard lock(mutex);
        for (auto &[id, dependent] : works) {
            (void)id;
            if (std::find(dependent.binding.scopes.begin(), dependent.binding.scopes.end(),
                          *job->request.write_scope) == dependent.binding.scopes.end())
                continue;
            ++dependent.revision;
            dependent.candidates.clear();
            dependent.prefix.clear();
            dependent.compacted.clear();
            dependent.captured.clear();
            for (const auto &[other_id, other] : jobs)
                if (other_id != job->id && !other->done &&
                    other->request.task == dependent.binding.work.task_id)
                    other->cancelled.store(true);
        }
        job->revision = works.at(job->request.task).revision;
        result.revision = job->revision;
    }
    {
        const std::lock_guard lock(mutex);
        auto &work = works.at(job->request.task);
        if (input.mode == MemoryAgentMode::Recall) {
            work.candidates = result.candidates;
            work.cache_until = cache_until;
        } else if (input.mode == MemoryAgentMode::Record) {
            work.recorded_through = std::max(work.recorded_through, result.through_event_sequence);
            work.record_snapshot = pending_record_snapshot;
            job->after_sequence = work.recorded_through;
            job->record_snapshot = work.record_snapshot;
        } else {
            std::string text = "[Memory agent checkpoint: untrusted historical data]\n";
            for (const auto &note : proposal.summary) {
                text += note.text + " [source";
                for (const auto citation : note.citations)
                    text += " " + std::to_string(citation);
                text += "]\n";
            }
            if (text.size() > config.max_result_bytes)
                return error(ErrorCode::ResourceExhausted,
                             "rendered memory checkpoint exceeds capacity");
            ModelInputItem summary;
            summary.role = ModelRole::User;
            summary.authority = Sensitivity::Public;
            summary.provenance.source = "mira.memory-agent.checkpoint.v1";
            summary.content.emplace_back(TextPart{std::move(text)});
            work.prefix = input.history;
            work.compacted = {std::move(summary)};
            result.compressed_items = input.history.size();
        }
    }
    return result;
}
} // namespace mira
