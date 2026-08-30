#pragma once

#include <mira/model_contracts.hpp>
#include <mira/model_dialect.hpp>
#include <mira/model_profile.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// SSE framing layer
// ---------------------------------------------------------------------------

// One dispatched Server-Sent Event. Field parsing follows the WHATWG spec:
// CRLF/LF/CRA line ends, single optional leading space after the colon,
// multi-line data joined with '\n', dispatch on the first empty line.
struct SseMessage final {
    std::string event; // Empty means the default "message" event.
    std::string data;
    std::optional<std::string> id;
    std::optional<std::int64_t> retry_ms;
};

struct SseFramingLimits final {
    std::size_t max_line_bytes = 16 * 1024;
    std::size_t max_event_data_bytes = 1024 * 1024;
    std::size_t max_events = 20'000;
    std::size_t max_total_bytes = 32ULL * 1024ULL * 1024ULL;
};

// Incremental framer over arbitrary network chunks. Any fragmentation of
// lines, UTF-8 code points and JSON tokens is handled because only complete
// events are ever dispatched. Oversized input fails closed instead of being
// truncated.
class SseFramingParser final {
  public:
    explicit SseFramingParser(SseFramingLimits limits = SseFramingLimits{});

    // Returns the messages completed by this chunk (possibly none).
    [[nodiscard]] Result<std::vector<SseMessage>> feed(std::string_view chunk);
    // Flushes a final non-empty partial line; EOF otherwise discards nothing.
    [[nodiscard]] Result<std::vector<SseMessage>> finish();

    [[nodiscard]] std::size_t bytes_fed() const noexcept { return bytes_fed_; }
    [[nodiscard]] std::size_t events_dispatched() const noexcept { return events_dispatched_; }

  private:
    [[nodiscard]] Result<std::vector<SseMessage>> process_line(const std::string &line);
    [[nodiscard]] Result<std::vector<SseMessage>> process_line_buffer();
    [[nodiscard]] Result<std::vector<SseMessage>> dispatch();

    SseFramingLimits limits_;
    std::string buffer_;
    std::string pending_event_;
    std::string pending_data_;
    std::optional<std::string> pending_id_;
    std::optional<std::int64_t> pending_retry_ms_;
    std::size_t bytes_fed_ = 0;
    std::size_t events_dispatched_ = 0;
};

// ---------------------------------------------------------------------------
// Responses SSE reducer
// ---------------------------------------------------------------------------

struct SseStreamLimits final {
    SseFramingLimits framing;
    std::size_t max_open_output_items = 64;
    std::size_t max_accumulated_text_bytes = 4ULL * 1024ULL * 1024ULL;
    std::size_t max_arguments_buffer_bytes = 1024 * 1024;
    std::size_t max_preview_bytes = 16 * 1024;
};

// UI-only, explicitly unvalidated text accumulation. A preview is never a
// decision source, never enters Memory and never triggers tools or actions.
struct UnvalidatedModelPreview final {
    std::string text;
    std::size_t dropped_updates = 0;
    bool truncated = false;
};

// Counts and terminal classification for EventStore summaries.
struct SseStreamStats final {
    std::uint64_t stream_sequence = 0;     // Local monotonic sequence.
    std::optional<std::int64_t> last_remote_sequence;
    std::size_t events = 0;
    std::size_t bytes = 0;
    std::size_t text_deltas = 0;
    std::size_t preview_drops = 0;
    std::size_t duplicate_events_collapsed = 0;
    bool terminal_seen = false;
    bool cancel_seen = false;
};

// Typed reducer over `openai.responses.v1` SSE streams. Enforces event
// pairing (item/content part add-done), unique terminals, remote sequence
// discipline and bounded buffers; produces the same canonical ModelResponse
// the synchronous mapper would.
class ResponsesSseParser final {
  public:
    ResponsesSseParser(const ModelRequest &request, const ModelProfile &profile,
                       SseStreamLimits limits = SseStreamLimits{});

    [[nodiscard]] Result<void> feed(std::string_view chunk);
    // EOF handling: a stream that ends without a terminal is an ambiguous
    // completion, not a partial success.
    [[nodiscard]] Result<ModelResponse> finish();

    // Cooperative cancellation: after this call the parser still records a
    // terminal as a late diagnostic but flags it; admission is the caller's.
    void note_cancel_requested() noexcept { cancel_seen_ = true; }

    [[nodiscard]] const SseStreamStats &stats() const noexcept { return stats_; }
    [[nodiscard]] UnvalidatedModelPreview take_preview();
    [[nodiscard]] bool terminal_seen() const noexcept { return stats_.terminal_seen; }

  private:
    struct OpenItem final {
        std::string item_id;
        std::string type;
        bool closed = false;
        std::vector<std::string> open_parts; // content part keys
        std::string text;                    // accumulated output_text deltas
        std::string args;                    // buffered function arguments
        std::string refusal;
        bool text_done = false;
        bool args_done = false;
        bool refusal_done = false;
    };

    [[nodiscard]] Result<void> reduce(const SseMessage &message);
    [[nodiscard]] Result<void> check_remote_sequence(const JsonValue &data);
    [[nodiscard]] Result<void> note_terminal(const char *kind);
    [[nodiscard]] OpenItem *find_open_item(const std::string &item_id);
    [[nodiscard]] Result<void> append_preview(std::string_view delta);

    ModelRequest request_;
    ModelProfile profile_;
    SseStreamLimits limits_;
    SseFramingParser framer_;
    std::vector<OpenItem> items_;
    std::string accumulated_text_;
    std::string preview_text_;
    std::size_t preview_drops_ = 0;
    bool terminal_seen_ = false;
    bool cancel_seen_ = false;
    std::optional<ModelResponse> terminal_response_;
    SseStreamStats stats_;
};

} // namespace mira
