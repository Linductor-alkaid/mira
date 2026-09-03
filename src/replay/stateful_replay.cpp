#include <mira/stateful_replay.hpp>

#include <algorithm>
#include <utility>

namespace mira {

AnalysisReplay::AnalysisReplay(IEventStore &events, ICheckpointStore *checkpoints,
                               IMemory *memory, IArtifactStore *artifacts)
    : events_(events), checkpoints_(checkpoints), memory_(memory), artifacts_(artifacts) {}

Result<AnalysisReplayReport> AnalysisReplay::inspect(TaskId task, SessionId session,
                                                     const MemoryScope &memory_scope,
                                                     const ReplayAsOf &as_of) const {
    AnalysisReplayReport report;

    // Checkpoint at or before the requested sequence boundary.
    if (checkpoints_ != nullptr) {
        const std::uint64_t cap = as_of.through_event_sequence.value_or(UINT64_MAX);
        auto stored = checkpoints_->latest_at_or_before(task, cap);
        if (!stored) {
            return stored.error();
        }
        if (stored.value().has_value()) {
            report.checkpoint = std::move(stored).value();
            report.checkpoint_present = true;
        } else {
            report.note = "no checkpoint at or before the requested sequence";
        }
    } else {
        report.note = "checkpoint store not bound";
    }

    // Events after the checkpoint (or from the start) for the same session.
    {
        EventQuery query;
        query.session_id = session;
        query.after_sequence =
            report.checkpoint.has_value()
                ? std::optional<SessionSequence>(report.checkpoint->through_event_sequence)
                : std::nullopt;
        query.limit = 1024;
        auto page = events_.read(query);
        if (!page) {
            return page.error();
        }
        for (const auto &event : page.value().events) {
            if (event.task_id.has_value() && *event.task_id == task) {
                report.events_after_checkpoint.push_back(event);
            }
        }
    }

    // Memory as of the bitemporal coordinates.
    if (memory_ != nullptr) {
        MemoryQuery query;
        query.scopes = {memory_scope};
        query.as_of_recorded = as_of.recorded_at;
        query.as_of_valid = as_of.valid_at;
        query.max_results = 32;
        auto recalled = memory_->query(query);
        if (!recalled) {
            report.memory_degraded = true;
            report.note += (report.note.empty() ? std::string{} : "; ") +
                           std::string("memory query failed: ") + recalled.error().safe_message;
        } else {
            report.memory = std::move(recalled).value();
            if (report.memory.quality.degraded) {
                report.memory_degraded = true;
            }
        }
    } else {
        report.note += (report.note.empty() ? std::string{} : "; ") + std::string("memory store not bound");
    }

    // Evidence artifacts referenced by recalled memory: missing or erased
    // payloads degrade the replay explicitly; no content is fabricated.
    if (artifacts_ == nullptr) {
        if (!report.memory.records.empty()) {
            report.note += (report.note.empty() ? std::string{} : "; ") +
                           std::string("artifact store not bound; evidence payloads unverifiable");
        }
    } else {
        for (const auto &record : report.memory.records) {
            if (!record.evidence.has_value()) {
                continue;
            }
            ArtifactDescriptor descriptor;
            descriptor.id = record.evidence->id;
            descriptor.digest = record.evidence->digest;
            descriptor.byte_size = record.evidence->byte_size;
            descriptor.media_type = record.evidence->media_type;
            auto reader = artifacts_->open(descriptor);
            if (!reader) {
                MissingArtifactNote missing;
                missing.id = record.evidence->id;
                missing.reason = "evidence artifact unavailable: " + reader.error().safe_message;
                report.missing_artifacts.push_back(std::move(missing));
            }
        }
        if (!report.missing_artifacts.empty()) {
            report.note +=
                (report.note.empty() ? std::string{} : "; ") + "referenced artifacts unavailable";
        }
    }
    return report;
}

} // namespace mira
