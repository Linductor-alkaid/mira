// Lexical behavior of the durable conversation index after DEC-147: Chinese
// recall ranked by relevance, an oversized Chinese message, and the schema
// in-place migration of older conversation indexes.

#include "support/m36_support.hpp"
#include "support/test.hpp"

#include <mira/sqlite_conversation_index.hpp>
#include <mira/text_segmentation.hpp>

#include <cstdint>
#include <iostream>
#include <string_view>

#include <sqlite3.h>

namespace {
using namespace mira;
using namespace mira::testing;

AppendRequest speech(const MemoryAgentFixture &fixture, const MemoryWorkBinding &work,
                     std::string type, std::string text) {
    AppendRequest event;
    event.event_id = EventId::generate();
    event.runtime_id = fixture.runtime;
    event.session_id = work.work.session_id;
    event.task_id = work.work.task_id;
    event.payload = {
        std::move(type),
        to_json_string(JsonValue::Object{{"detail", JsonValue::Object{{"text", std::move(text)}}}}),
        EventClass::State};
    return event;
}

Result<SessionSequence> rebuild(MemoryAgentFixture &fixture, SqliteConversationIndex &index,
                                const MemoryWorkBinding &work, const IEventStore &events) {
    return fixture.executor
        .submit_auto([&] {
            return index.rebuild(work.principal.tenant_id, work.work.session_id, events,
                                 memory_context(work.work));
        })
        .get();
}

ContextQuery query_for(const MemoryWorkBinding &work, std::string text = {}) {
    ContextQuery query;
    query.session = work.work.session_id;
    query.scopes = work.scopes;
    query.text = std::move(text);
    return query;
}

[[nodiscard]] SqliteConversationIndexOptions options_for(const std::filesystem::path &path) {
    SqliteConversationIndexOptions options;
    options.path = path;
    options.max_segments = 16;
    return options;
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

// Raw access to a closed index file for migration and integrity checks.
[[nodiscard]] bool exec_sql(const std::filesystem::path &path, const char *sql) {
    sqlite3 *db = nullptr;
    int result = sqlite3_open(path.string().c_str(), &db);
    if (result == SQLITE_OK) {
        result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    }
    if (result != SQLITE_OK) {
        std::cerr << "SQLite index fixture failed: " << sqlite3_errmsg(db) << '\n';
    }
    sqlite3_close(db);
    return result == SQLITE_OK;
}

[[nodiscard]] std::string scalar(const std::filesystem::path &path, const std::string &sql) {
    std::string value;
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        return value;
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        const auto *text = sqlite3_column_text(statement, 0);
        if (text != nullptr) {
            value = reinterpret_cast<const char *>(text);
        }
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

int chinese_segments_come_back_most_relevant_first() {
    MemoryAgentFixture fixture;
    const auto work = fixture.binding();
    SqliteConversationIndexOptions options;
    options.path = fixture.root / "history.db";
    options.segmentation.window_entries = 1; // every message is its own segment
    options.max_segments = 16;
    auto opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto index = std::move(opened).value();
    const auto say = [&](const char *type, const char *text) {
        return fixture.events->append(speech(fixture, work, type, text));
    };
    MIRA_CHECK(say("UserMessageInjected", "北京的天气怎么样"));
    MIRA_CHECK(say("AssistantSpeech", "北京今天有雨"));
    MIRA_CHECK(say("AssistantSpeech", "打印机的纸盒空了"));
    MIRA_CHECK(say("UserMessageInjected", "打印机和扫描仪都在二楼"));
    MIRA_CHECK(rebuild(fixture, *index, work, *fixture.events).value() == 4);

    // A word inside a sentence written without spaces reaches both segments
    // that carry it. The more relevant one comes first although it is older:
    // the ordering used to be newest-first.
    auto found = index->retrieve(query_for(work, "北京天气"), {});
    MIRA_CHECK(found && found.value().quality.fts_leg_ran && !found.value().quality.degraded);
    MIRA_CHECK(found.value().candidates.size() == 2);
    MIRA_CHECK(found.value().candidates[0].through_event_sequence == 1);
    MIRA_CHECK(found.value().candidates[1].through_event_sequence == 2);
    MIRA_CHECK(found.value().candidates[0].lexical_score >
               found.value().candidates[1].lexical_score);

    // A question in which not every word appears.
    found = index->retrieve(query_for(work, "扫描仪在哪"), {});
    MIRA_CHECK(found && found.value().candidates.size() == 1);
    MIRA_CHECK(found.value().candidates.front().through_event_sequence == 4);
    found = index->retrieve(query_for(work, "打印机地址"), {});
    MIRA_CHECK(found && found.value().candidates.size() == 2);

    // Exact terms stay mandatory, and text without a lexical unit runs no
    // lexical leg.
    auto strict = query_for(work, "北京天气");
    strict.exact_terms = {"上海"};
    MIRA_CHECK(index->retrieve(strict, {}).value().candidates.empty());
    found = index->retrieve(query_for(work, "？"), {});
    MIRA_CHECK(found && !found.value().quality.fts_leg_ran);
    MIRA_CHECK(found.value().candidates.empty());

    // Without a lexical query every segment of the session still comes back,
    // newest-first as before.
    const auto unranked = index->retrieve(query_for(work), {});
    MIRA_CHECK(unranked && unranked.value().candidates.size() == 4);
    MIRA_CHECK(!unranked.value().quality.fts_leg_ran);
    for (std::size_t position = 0; position < unranked.value().candidates.size(); ++position) {
        MIRA_CHECK(unranked.value().candidates[position].through_event_sequence == 4 - position);
    }
    MIRA_CHECK(index->close());
    index.reset();
    MIRA_CHECK(exec_sql(options.path, "INSERT INTO conversation_fts(conversation_fts,rank)"
                                      " VALUES('integrity-check',1);"));
    return 0;
}

int speech_search_excludes_diagnostics_and_explains_matches() {
    MemoryAgentFixture fixture;
    const auto work = fixture.binding();
    auto options = options_for(fixture.root / "speech.db");
    options.segmentation.window_entries = 3;
    auto opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto index = std::move(opened).value();
    auto assistant = speech(fixture, work, "AssistantSpeech", "宠物是橘猫胖虎");
    assistant.payload.data = to_json_string(JsonValue::Object{
        {"detail",
         JsonValue::Object{{"text", "宠物是橘猫胖虎"},
                           {"tool_calls", JsonValue::Array{JsonValue::Object{
                                              {"wire_name", "send_fixture"},
                                              {"status", "executed"},
                                              {"arguments", R"({"text":"toolpayload"})"}}}}}}});
    auto settled = speech(fixture, work, "ConversationSettled", "unused");
    settled.payload.data = R"({"detail":{"outcome":"Answered","turns":3,"total_turns":3}})";
    MIRA_CHECK(
        fixture.events->append(speech(fixture, work, "UserMessageInjected", "我养了一只宠物")));
    MIRA_CHECK(fixture.events->append(assistant));
    MIRA_CHECK(fixture.events->append(settled));
    MIRA_CHECK(
        fixture.events->append(speech(fixture, work, "AssistantSpeech", "橘树下遇见一只白猫")));
    MIRA_CHECK(fixture.events->append(speech(fixture, work, "UserMessageInjected", "橘")));
    MIRA_CHECK(fixture.events->append(speech(fixture, work, "AssistantSpeech", "猫")));
    MIRA_CHECK(rebuild(fixture, *index, work, *fixture.events).value() == 6);

    const auto check = [&](SqliteConversationIndex &store) -> int {
        const auto found = store.retrieve(query_for(work, "那只橘猫长什么样"), {});
        MIRA_CHECK(found && found.value().candidates.size() == 1);
        const auto &candidate = found.value().candidates.front();
        MIRA_CHECK(candidate.through_event_sequence == 3 && candidate.source_events.size() == 3);
        MIRA_CHECK(candidate.lexical_matches == (std::vector<std::string>{"橘猫"}));
        MIRA_CHECK(candidate.lexical_bm25 && *candidate.lexical_bm25 < 0.0);
        MIRA_CHECK(candidate.lexical_score ==
                   -*candidate.lexical_bm25 / (1.0 - *candidate.lexical_bm25));
        const auto decoded = context_candidate_from_json(context_candidate_to_json(candidate));
        MIRA_CHECK(decoded && decoded.value().lexical_matches == candidate.lexical_matches &&
                   decoded.value().lexical_bm25 == candidate.lexical_bm25);
        for (const auto *trace : {"send_fixture", "toolpayload", "settled", "assistant"}) {
            MIRA_CHECK(store.retrieve(query_for(work, trace), {}).value().candidates.empty());
            auto exact = query_for(work);
            exact.exact_terms = {trace};
            MIRA_CHECK(store.retrieve(exact, {}).value().candidates.empty());
        }
        const auto single = store.retrieve(query_for(work, "橘 _"), {});
        MIRA_CHECK(single && single.value().candidates.size() == 2);
        return 0;
    };
    MIRA_CHECK(check(*index) == 0);
    MIRA_CHECK(index->close());
    index.reset();
    MIRA_CHECK(exec_sql(options.path,
                        "ALTER TABLE conversation_segments DROP COLUMN search_text;"
                        "UPDATE store_meta SET value='1.1' WHERE key='schema_version';"));
    opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    index = std::move(opened).value();
    MIRA_CHECK(check(*index) == 0);
    MIRA_CHECK(rebuild(fixture, *index, work, *fixture.events).value() == 6);
    MIRA_CHECK(index->close());
    return 0;
}

// Before DEC-147 the cut of an oversized message fell on a byte boundary.
// Inside a multi-byte character it left invalid UTF-8, which the publication
// check rejected, so one long Chinese message failed the rebuild of its whole
// session.
int an_oversized_chinese_message_still_indexes() {
    MemoryAgentFixture fixture;
    const auto work = fixture.binding();
    SqliteConversationIndexOptions options;
    options.path = fixture.root / "history.db";
    options.max_segments = 16;
    auto opened = SqliteConversationIndex::open(fixture.executor, options);
    MIRA_CHECK(opened);
    auto index = std::move(opened).value();
    // 2000 characters are 6000 bytes; with the six-byte "user: " prefix the
    // 4096-byte bound falls one byte into a character.
    std::string long_text;
    for (int count = 0; count < 2000; ++count) {
        long_text += "好";
    }
    MIRA_CHECK(fixture.events->append(speech(fixture, work, "UserMessageInjected", long_text)));
    MIRA_CHECK(fixture.events->append(speech(fixture, work, "AssistantSpeech", "北京今天有雨")));
    MIRA_CHECK(rebuild(fixture, *index, work, *fixture.events).value() == 2);

    auto found = index->retrieve(query_for(work, "北京天气"), {});
    MIRA_CHECK(found && found.value().candidates.size() == 1);
    found = index->retrieve(query_for(work, "好好"), {});
    MIRA_CHECK(found && found.value().candidates.size() == 1);
    const auto &truncated = found.value().candidates.front();
    MIRA_CHECK(truncated.through_event_sequence == 1);
    MIRA_CHECK(truncated.text.size() <= 4096 + 16);
    MIRA_CHECK(truncated.text.find("[truncated]") != std::string::npos);
    MIRA_CHECK(valid_utf8(truncated.text));
    MIRA_CHECK(truncated.turns.size() == 1 && valid_utf8(truncated.turns.front().text));
    MIRA_CHECK(index->close());
    return 0;
}

// Seventy segments: the migration re-indexes in pages of 64, so the case has
// to cross a page boundary.
int a_1_0_index_migrates_in_place() {
    MemoryAgentFixture fixture;
    const auto work = fixture.binding();
    const auto path = fixture.root / "history.db";
    auto options = options_for(path);
    options.segmentation.window_entries = 1; // every message is its own segment
    options.max_segments = 96;
    SessionSequence through = 0;
    {
        auto opened = SqliteConversationIndex::open(fixture.executor, options);
        MIRA_CHECK(opened);
        auto index = std::move(opened).value();
        const auto say = [&](const char *type, const char *text) {
            return fixture.events->append(speech(fixture, work, type, text));
        };
        MIRA_CHECK(say("UserMessageInjected", "北京的天气怎么样"));
        MIRA_CHECK(say("AssistantSpeech", "北京今天有雨"));
        for (int note = 3; note <= 70; ++note) {
            const auto page = "会议纪要第" + std::to_string(note) + "页";
            MIRA_CHECK(fixture.events->append(
                speech(fixture, work, "AssistantSpeech", std::string(page))));
        }
        const auto rebuilt = rebuild(fixture, *index, work, *fixture.events);
        MIRA_CHECK(rebuilt.value() == 70);
        through = rebuilt.value();
        MIRA_CHECK(index->close());
    }
    // Rewrite the file into the 1.0 layout: no index text column, and an
    // index that holds each segment's text as written.
    MIRA_CHECK(exec_sql(path,
                        "DROP TRIGGER conversation_insert;"
                        "DROP TRIGGER conversation_delete;"
                        "DROP TABLE conversation_fts;"
                        "ALTER TABLE conversation_segments DROP COLUMN search_text;"
                        "ALTER TABLE conversation_segments DROP COLUMN lexical;"
                        "CREATE VIRTUAL TABLE conversation_fts USING fts5(owner,text,"
                        " content='conversation_segments',content_rowid='rowid',"
                        " tokenize='unicode61');"
                        "INSERT INTO conversation_fts(conversation_fts,rank)"
                        " VALUES('secure-delete',1);"
                        "INSERT INTO conversation_fts(rowid,owner,text)"
                        " SELECT rowid,owner,text FROM conversation_segments;"
                        "CREATE TRIGGER conversation_insert AFTER INSERT ON conversation_segments"
                        " BEGIN INSERT INTO conversation_fts(rowid,owner,text)"
                        " VALUES(new.rowid,new.owner,new.text); END;"
                        "CREATE TRIGGER conversation_delete AFTER DELETE ON conversation_segments"
                        " BEGIN INSERT INTO conversation_fts(conversation_fts,rowid,owner,text)"
                        " VALUES('delete',old.rowid,old.owner,old.text); END;"
                        "UPDATE store_meta SET value = '1.0' WHERE key = 'schema_version';"));
    MIRA_CHECK(scalar(path, "SELECT count(*) FROM conversation_fts") == "70");
    {
        auto opened = SqliteConversationIndex::open(fixture.executor, options);
        if (!opened) {
            std::cerr << "1.0 index open failed: " << opened.error().safe_message << "\n";
            return 1;
        }
        auto index = std::move(opened).value();
        // History became searchable for a word inside a Chinese sentence
        // without a host rebuild, on both sides of the page boundary.
        const auto found = index->retrieve(query_for(work, "北京天气"), {});
        MIRA_CHECK(found && found.value().quality.fts_leg_ran);
        MIRA_CHECK(found.value().candidates.size() == 2);
        MIRA_CHECK(found.value().candidates.front().lexical_score > 0.0);
        // Only the page number tells the notes apart; it ranks its own first.
        const auto late = index->retrieve(query_for(work, "纪要第70页"), {});
        MIRA_CHECK(late && late.value().quality.fts_leg_ran);
        MIRA_CHECK(late.value().candidates.front().through_event_sequence == 70);
        MIRA_CHECK(index->close());
    }
    // Every segment now carries its index text, derived from its own text:
    // the first row, both sides of the page boundary and the last one.
    MIRA_CHECK(scalar(path, "SELECT value FROM store_meta WHERE key = 'schema_version'") == "1.2");
    MIRA_CHECK(scalar(path, "SELECT count(*) FROM pragma_table_info('conversation_segments')"
                            " WHERE name = 'lexical'") == "1");
    MIRA_CHECK(scalar(path, "SELECT count(*) FROM conversation_fts") == "70");
    for (const std::string rowid : {"1", "64", "65", "70"}) {
        const std::string where = " FROM conversation_segments WHERE rowid = " + rowid;
        MIRA_CHECK(scalar(path, "SELECT lexical" + where) ==
                   lexical_index_text(scalar(path, "SELECT search_text" + where)));
    }
    MIRA_CHECK(exec_sql(path, "INSERT INTO conversation_fts(conversation_fts,rank)"
                              " VALUES('integrity-check',1);"));
    {
        auto opened = SqliteConversationIndex::open(fixture.executor, options);
        MIRA_CHECK(opened);
        auto index = std::move(opened).value();
        // The watermark survived the migration: the same events rebuild to the
        // same sequence, and the segments answer as before.
        MIRA_CHECK(rebuild(fixture, *index, work, *fixture.events).value() == through);
        MIRA_CHECK(index->retrieve(query_for(work, "北京天气"), {}).value().candidates.size() == 2);
        MIRA_CHECK(index->close());
    }
    // The idempotent rebuild left the segments as the migration wrote them.
    MIRA_CHECK(scalar(path, "SELECT count(*) FROM conversation_segments") == "70");
    MIRA_CHECK(scalar(path, "SELECT value FROM store_meta WHERE key = 'schema_version'") == "1.2");
    {
        auto opened = SqliteConversationIndex::open(fixture.executor, options);
        MIRA_CHECK(opened);
        auto index = std::move(opened).value();
        // Erasure removes the segments and their index entries.
        const auto owner =
            memory_erasure_scope_digest(memory_session_scope(work.principal, work.work.session_id));
        MIRA_CHECK(index->erase_scope(owner));
        MIRA_CHECK(index->retrieve(query_for(work, "北京天气"), {}).value().candidates.empty());
        MIRA_CHECK(index->close());
    }
    MIRA_CHECK(scalar(path, "SELECT count(*) FROM conversation_fts") == "0");
    MIRA_CHECK(scalar(path, "SELECT count(*) FROM conversation_segments") == "0");
    MIRA_CHECK(scalar(path, "SELECT count(*) FROM conversation_erasures") == "1");
    return 0;
}

} // namespace

int main() {
    if (const int result = chinese_segments_come_back_most_relevant_first(); result != 0) {
        return result;
    }
    if (const int result = speech_search_excludes_diagnostics_and_explains_matches(); result != 0) {
        return result;
    }
    if (const int result = an_oversized_chinese_message_still_indexes(); result != 0) {
        return result;
    }
    return a_1_0_index_migrates_in_place();
}
