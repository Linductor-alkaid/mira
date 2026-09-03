#pragma once

#include <mira/context_contracts.hpp>
#include <mira/event_store.hpp>
#include <mira/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mira::testing {

// ---------------------------------------------------------------------------
// Event log seeding for checkpoint/recovery tests
// ---------------------------------------------------------------------------

class TaskEventLog final {
  public:
    TaskEventLog(RuntimeId runtime, SessionId session, TaskId task)
        : runtime_id_(runtime), session_id_(session), task_id_(task) {}

    // JSON-vocabulary event; payload is serialized compactly.
    TaskEventLog &add(std::string type, JsonValue payload,
                      EventClass classification = EventClass::State) {
        envelopes_.push_back(envelope(std::move(type), to_json_string(payload), classification));
        return *this;
    }

    // Pipe-vocabulary event (ActionJournal format).
    TaskEventLog &add_pipe(std::string type, std::string payload,
                           EventClass classification = EventClass::Critical) {
        envelopes_.push_back(envelope(std::move(type), std::move(payload), classification));
        return *this;
    }

    TaskEventLog &add_raw(std::string type, std::string payload) {
        envelopes_.push_back(envelope(std::move(type), std::move(payload), EventClass::State));
        return *this;
    }

    void append_to(IEventStore &store) const {
        for (const auto &event : envelopes_) {
            AppendRequest request;
            request.event_id = event.event_id;
            request.runtime_id = event.runtime_id;
            request.session_id = event.session_id;
            request.task_id = event.task_id;
            request.payload = event.payload;
            request.required = Durability::ProcessCrash;
            const auto receipt = store.append(request);
            if (!receipt) {
                std::abort();
            }
        }
    }

    [[nodiscard]] const std::vector<EventEnvelope> &envelopes() const noexcept {
        return envelopes_;
    }

    [[nodiscard]] std::vector<EventEnvelope> up_to(std::uint64_t sequence) const {
        std::vector<EventEnvelope> prefix;
        for (const auto &event : envelopes_) {
            if (event.session_sequence <= sequence) {
                prefix.push_back(event);
            }
        }
        return prefix;
    }

    [[nodiscard]] EventEnvelope envelope(std::string type, std::string data,
                                         EventClass classification) {
        EventEnvelope event;
        event.event_id = EventId::generate();
        event.runtime_id = runtime_id_;
        event.session_id = session_id_;
        event.task_id = task_id_;
        event.session_sequence = next_sequence_++;
        event.task_sequence = event.session_sequence;
        event.timestamp = Timestamp::now();
        event.schema_version = SchemaVersion{};
        event.payload = EventPayload{std::move(type), std::move(data), classification};
        return event;
    }

  private:
    RuntimeId runtime_id_;
    SessionId session_id_;
    TaskId task_id_;
    std::uint64_t next_sequence_ = 1;
    std::vector<EventEnvelope> envelopes_;
};

// ---------------------------------------------------------------------------
// Context item helpers
// ---------------------------------------------------------------------------

[[nodiscard]] inline ContextItem text_item(ContextItemKind kind, ContextAuthority authority,
                                           std::string text, std::uint64_t sequence = 0) {
    ContextItem item;
    item.id = ContextItemId::generate();
    item.kind = kind;
    item.authority = authority;
    item.sequence = sequence;
    TextPart part;
    part.text = std::move(text);
    item.content.emplace_back(std::move(part));
    return item;
}

[[nodiscard]] inline ContextItem image_item(ContextItemKind kind, ContextAuthority authority,
                                            std::uint64_t image_bytes, std::uint64_t sequence = 0,
                                            std::string text = {}) {
    ContextItem item;
    item.id = ContextItemId::generate();
    item.kind = kind;
    item.authority = authority;
    item.sequence = sequence;
    ArtifactRef reference;
    reference.id = ArtifactId::generate();
    reference.byte_size = image_bytes;
    reference.media_type = "image/png";
    ImagePart image;
    image.source = reference;
    image.media_type = reference.media_type;
    item.content.emplace_back(std::move(image));
    if (!text.empty()) {
        TextPart part;
        part.text = std::move(text);
        item.content.emplace_back(std::move(part));
    }
    return item;
}

} // namespace mira::testing
