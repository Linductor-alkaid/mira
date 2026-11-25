// In-process lexical leg of Layer 1 after DEC-147, for text written without
// spaces between words, and the conversation segmenter's cut of an oversized
// message. The durable stores have their own suites; this one freezes the
// reference index that nothing is persisted behind.

#include "../support/test.hpp"

#include <mira/context_retrieval.hpp>
#include <mira/conversation_log.hpp>
#include <mira/memory_contracts.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

using namespace mira;

// Appended to a segment whose text was cut at the byte bound.
constexpr std::string_view kMarker = "\n[truncated]";

[[nodiscard]] SessionId session_from_seed(std::uint64_t seed) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); index += sizeof(seed)) {
        std::memcpy(bytes.data() + index, &seed, sizeof(seed));
    }
    return SessionId{Id128{bytes}};
}

[[nodiscard]] EventId event_from_seed(std::uint64_t seed) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); index += sizeof(seed)) {
        std::memcpy(bytes.data() + index, &seed, sizeof(seed));
    }
    return EventId{Id128{bytes}};
}

[[nodiscard]] MemoryScope environment_scope() {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = "device-1";
    return scope;
}

[[nodiscard]] ContextIndexAsset conversation_asset(const ContextAssetId &id,
                                                   const SessionId &session, std::string text,
                                                   std::uint64_t sequence) {
    ContextIndexAsset asset;
    asset.id = id;
    asset.kind = ContextAssetKind::ConversationSegment;
    asset.text = std::move(text);
    asset.session = session;
    asset.source_events = {event_from_seed(sequence)};
    asset.through_event_sequence = sequence;
    return asset;
}

[[nodiscard]] ConversationEntry entry(std::uint64_t sequence, std::string text) {
    ConversationEntry item;
    item.text = std::move(text);
    item.origin = event_from_seed(sequence);
    item.session_sequence = sequence;
    return item;
}

[[nodiscard]] bool valid_utf8(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto lead = static_cast<std::uint8_t>(text[offset]);
        const std::size_t length = lead < 0x80U   ? 1
                                   : lead < 0xC0U ? 0
                                   : lead < 0xE0U ? 2
                                   : lead < 0xF0U ? 3
                                   : lead < 0xF8U ? 4
                                                  : 0;
        if (length == 0 || offset + length > text.size()) {
            return false;
        }
        for (std::size_t index = 1; index < length; ++index) {
            if ((static_cast<std::uint8_t>(text[offset + index]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        offset += length;
    }
    return true;
}

int chinese_text_is_recalled_by_the_lexical_leg() {
    const SessionId session = session_from_seed(2);
    InMemoryContextIndex index;
    const auto weather = context_asset_id_from_seed("cjk|weather");
    const auto printer = context_asset_id_from_seed("cjk|printer");
    const auto whiteboard = context_asset_id_from_seed("cjk|whiteboard");
    const auto docker = context_asset_id_from_seed("cjk|docker");
    MIRA_CHECK(
        index
            .upsert_asset(conversation_asset(weather, session, "user: 我想知道北京的天气怎么样", 1))
            .has_value());
    MIRA_CHECK(index
                   .upsert_asset(conversation_asset(printer, session,
                                                    "user: 打印机卡纸时先开前盖再取出纸盒", 2))
                   .has_value());
    MIRA_CHECK(
        index.upsert_asset(conversation_asset(whiteboard, session, "user: 白板马克笔需要更换了", 3))
            .has_value());
    MIRA_CHECK(index
                   .upsert_asset(conversation_asset(
                       docker, session, "user: restart docker compose before redeploy", 4))
                   .has_value());

    ContextQuery query;
    query.session = session;
    query.scopes = {environment_scope()};
    const RetrievalBudget budget;

    // A word inside a sentence written without spaces.
    query.text = "北京天气";
    auto found = index.retrieve(query, budget);
    MIRA_CHECK(found && found.value().quality.fts_leg_ran && !found.value().quality.degraded);
    MIRA_CHECK(found.value().candidates.size() == 1);
    MIRA_CHECK(found.value().candidates.front().asset_id == weather);

    // A question in which not every word appears: both records sharing a word
    // come back, the one sharing more of the question first.
    query.text = "打印机怎么卡纸了";
    found = index.retrieve(query, budget);
    MIRA_CHECK(found && found.value().candidates.size() == 2);
    MIRA_CHECK(found.value().candidates.front().asset_id == printer);
    MIRA_CHECK(found.value().candidates.front().lexical_score >
               found.value().candidates.back().lexical_score);
    // The same inputs give the same order.
    const auto again = index.retrieve(query, budget);
    MIRA_CHECK(again && again.value().candidates.size() == 2);
    MIRA_CHECK(again.value().candidates.front().asset_id == printer);

    // Single characters are not units of this leg: a record sharing only one
    // common character is not a match.
    query.text = "马克笔";
    found = index.retrieve(query, budget);
    MIRA_CHECK(found && found.value().candidates.size() == 1);
    MIRA_CHECK(found.value().candidates.front().asset_id == whiteboard);

    // Words of a script that separates them with spaces match as before.
    query.text = "docker compose";
    found = index.retrieve(query, budget);
    MIRA_CHECK(found && found.value().candidates.size() == 1);
    MIRA_CHECK(found.value().candidates.front().asset_id == docker);

    // Text without a single unit matches nothing.
    query.text = "？!";
    found = index.retrieve(query, budget);
    MIRA_CHECK(found && found.value().candidates.empty());
    return 0;
}

// Before DEC-147 the cut of an oversized message fell on a byte boundary and
// split a multi-byte character, leaving invalid UTF-8.
int oversized_multibyte_entry_cuts_on_a_character_boundary() {
    const SessionId session = session_from_seed(3);
    const ConversationSegmentationOptions options;

    // "user: " plus 2000 three-byte characters: the 4096-byte bound falls one
    // byte into the 1364th character, which is left out whole.
    std::string long_text;
    for (int count = 0; count < 2000; ++count) {
        long_text += "好";
    }
    const auto truncated =
        segment_conversation(session, std::vector<ConversationEntry>{entry(1, long_text)}, options);
    MIRA_CHECK(truncated.has_value() && truncated.value().size() == 1);
    const auto &segment = truncated.value().front();
    MIRA_CHECK(segment.text.size() == 6 + 1363 * 3 + kMarker.size());
    MIRA_CHECK(segment.text.size() <= options.max_segment_bytes + 16);
    MIRA_CHECK(segment.text.find("[truncated]") != std::string::npos);
    MIRA_CHECK(valid_utf8(segment.text));
    MIRA_CHECK(segment.entries.size() == 1 && segment.entries.front().text == segment.text);

    // Four-byte characters back off the same way.
    std::string emoji;
    for (int count = 0; count < 1100; ++count) {
        emoji += "\xf0\x9f\x98\x80";
    }
    const auto pictographs =
        segment_conversation(session, std::vector<ConversationEntry>{entry(2, emoji)}, options);
    MIRA_CHECK(pictographs.has_value() && pictographs.value().size() == 1);
    MIRA_CHECK(pictographs.value().front().text.size() == (6 + 1022 * 4) + kMarker.size());
    MIRA_CHECK(valid_utf8(pictographs.value().front().text));

    // A line exactly at the bound is kept whole; one byte past it is cut.
    const auto exact = segment_conversation(
        session, std::vector<ConversationEntry>{entry(3, std::string(4090, 'x'))}, options);
    MIRA_CHECK(exact.has_value() && exact.value().front().text.size() == options.max_segment_bytes);
    MIRA_CHECK(exact.value().front().text.find("[truncated]") == std::string::npos);
    const auto past = segment_conversation(
        session, std::vector<ConversationEntry>{entry(4, std::string(4091, 'x'))}, options);
    MIRA_CHECK(past.has_value());
    MIRA_CHECK(past.value().front().text.size() == options.max_segment_bytes + kMarker.size());
    return 0;
}

} // namespace

int main() {
    if (const int result = chinese_text_is_recalled_by_the_lexical_leg(); result != 0) {
        return result;
    }
    return oversized_multibyte_entry_cuts_on_a_character_boundary();
}
