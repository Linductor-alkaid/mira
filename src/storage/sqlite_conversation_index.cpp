#include <mira/sqlite_conversation_index.hpp>

#include "sqlite_store_open.hpp"

#include <mira/content_markers.hpp>
#include <mira/memory_agent.hpp>
#include <mira/text_segmentation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace mira {
namespace {
using storage::Statement;
using storage::Transaction;

Error index_error(ErrorCode code, std::string message) {
    return storage::store_error(code, "mira.conversation_index", 0, std::move(message));
}

constexpr SchemaVersion kConversationIndexSchema{1, 2};

// The digest covers the provenance document. Search columns are derived.
constexpr const char *kTables =
    "CREATE TABLE store_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE conversation_sessions(session TEXT PRIMARY KEY, owner TEXT UNIQUE NOT NULL,"
    " through INTEGER NOT NULL, digest TEXT NOT NULL);"
    "CREATE TABLE conversation_erasures(owner TEXT PRIMARY KEY);"
    "CREATE TABLE conversation_segments("
    " id TEXT PRIMARY KEY, owner TEXT NOT NULL, through INTEGER NOT NULL,"
    " text TEXT NOT NULL, search_text TEXT NOT NULL, lexical TEXT NOT NULL, document TEXT NOT NULL,"
    " digest TEXT NOT NULL);"
    "CREATE INDEX conversation_segments_by_owner ON conversation_segments(owner,through DESC,id);";
// The full-text index and the triggers that keep it in step with the segments.
constexpr const char *kFullText =
    "CREATE VIRTUAL TABLE conversation_fts USING fts5(owner,lexical,"
    " content='conversation_segments',content_rowid='rowid',tokenize='unicode61');"
    "INSERT INTO conversation_fts(conversation_fts,rank) VALUES('secure-delete',1);"
    "CREATE TRIGGER conversation_insert AFTER INSERT ON conversation_segments BEGIN"
    " INSERT INTO conversation_fts(rowid,owner,lexical)"
    " VALUES(new.rowid,new.owner,new.lexical); END;"
    "CREATE TRIGGER conversation_delete AFTER DELETE ON conversation_segments BEGIN"
    " INSERT INTO conversation_fts(conversation_fts,rowid,owner,lexical)"
    " VALUES('delete',old.rowid,old.owner,old.lexical); END;";

bool bind_text(Statement &statement, int index, const std::string &value) {
    return sqlite3_bind_text(statement.get(), index, value.data(), static_cast<int>(value.size()),
                             storage::transient_copy()) == SQLITE_OK;
}

std::string_view column(Statement &statement, int index) {
    const auto *data = sqlite3_column_text(statement.get(), index);
    const auto size = sqlite3_column_bytes(statement.get(), index);
    return data ? std::string_view(reinterpret_cast<const char *>(data),
                                   static_cast<std::size_t>(size))
                : std::string_view{};
}

struct SearchText final {
    std::string text;
    std::string lexical;

    void append(std::string_view utterance) {
        if (utterance.empty())
            return;
        if (!text.empty())
            text += '\n';
        text += utterance;
        const auto units = lexical_index_text(utterance);
        if (units.empty())
            return;
        if (!lexical.empty()) {
            lexical += ' ';
            lexical += storage::kConversationFtsBoundary;
            lexical += ' ';
        }
        lexical += units;
    }
};

std::string_view utterance(const ConversationSegmentEntry &turn,
                           const ConversationEntry *source = nullptr) {
    if (turn.kind == ConversationEntry::Kind::LoopOutcome)
        return {};
    std::string_view line = turn.text;
    const std::string_view prefix =
        turn.kind == ConversationEntry::Kind::AssistantSpeech ? "assistant: " : "user: ";
    if (!line.starts_with(prefix))
        return {};
    line.remove_prefix(prefix.size());
    constexpr std::string_view truncation = "\n[truncated]";
    if (line.ends_with(truncation))
        line.remove_suffix(truncation.size());
    if (turn.kind == ConversationEntry::Kind::UserMessage)
        return line;
    if (source)
        return truncate_utf8(source->spoken_text, line.size());
    if (line.starts_with("tool call "))
        return {};
    return line.substr(0, line.find("\ntool call "));
}

[[nodiscard]] Result<void> migrate_conversation_search(sqlite3 *db, SchemaVersion previous) {
    std::string rebuild = "DROP TRIGGER conversation_insert;"
                          "DROP TRIGGER conversation_delete;"
                          "DROP TABLE conversation_fts;"
                          "ALTER TABLE conversation_segments ADD COLUMN search_text TEXT NOT NULL"
                          " DEFAULT '';";
    if (previous.minor == 0)
        rebuild += "ALTER TABLE conversation_segments ADD COLUMN lexical TEXT NOT NULL DEFAULT '';";
    rebuild += kFullText;
    if (auto rebuilt = storage::exec_sql(db, rebuild.c_str()); !rebuilt) {
        return rebuilt.error();
    }
    Statement page(db,
                   "SELECT rowid,owner,document,digest FROM conversation_segments WHERE rowid > ?1"
                   " ORDER BY rowid LIMIT 64");
    Statement store(db,
                    "UPDATE conversation_segments SET search_text=?1, lexical=?2 WHERE rowid=?3");
    Statement index(db, "INSERT INTO conversation_fts(rowid,owner,lexical) VALUES(?1,?2,?3)");
    if (!page.valid() || !store.valid() || !index.valid()) {
        return index_error(ErrorCode::Internal,
                           "conversation index migration could not be prepared");
    }
    std::int64_t last = 0;
    for (;;) {
        // A page is read to its end before any row of it is written.
        std::vector<std::tuple<std::int64_t, std::string, SearchText>> rows;
        if (sqlite3_bind_int64(page.get(), 1, last) != SQLITE_OK) {
            return index_error(ErrorCode::Internal,
                               "conversation index migration could not read segments");
        }
        int step = SQLITE_OK;
        while ((step = sqlite3_step(page.get())) == SQLITE_ROW) {
            const auto document = column(page, 2);
            if (document.size() > 128 * 1024 ||
                digest_string(document).to_string() != column(page, 3))
                return index_error(ErrorCode::DataLoss,
                                   "conversation migration found a bad digest");
            const auto parsed = parse_json(document);
            if (!parsed)
                return parsed.error();
            const auto candidate = context_candidate_from_json(parsed.value());
            if (!candidate)
                return candidate.error();
            SearchText search;
            for (const auto &turn : candidate.value().turns)
                search.append(utterance(turn));
            rows.emplace_back(sqlite3_column_int64(page.get(), 0), std::string(column(page, 1)),
                              std::move(search));
        }
        page.reset();
        if (step != SQLITE_DONE) {
            return index_error(ErrorCode::Internal,
                               "conversation index migration could not read segments");
        }
        if (rows.empty()) {
            break;
        }
        for (const auto &[rowid, owner, search] : rows) {
            // The update fires no trigger; only inserts and deletes have one.
            if (!bind_text(store, 1, search.text) || !bind_text(store, 2, search.lexical) ||
                sqlite3_bind_int64(store.get(), 3, rowid) != SQLITE_OK ||
                sqlite3_step(store.get()) != SQLITE_DONE) {
                return index_error(ErrorCode::Internal,
                                   "conversation index migration could not store a segment");
            }
            store.reset();
            if (sqlite3_bind_int64(index.get(), 1, rowid) != SQLITE_OK ||
                !bind_text(index, 2, owner) || !bind_text(index, 3, search.lexical) ||
                sqlite3_step(index.get()) != SQLITE_DONE) {
                return index_error(ErrorCode::Internal,
                                   "conversation index migration could not index a segment");
            }
            index.reset();
            last = rowid;
        }
    }
    return {};
}

struct SessionHead final {
    std::string owner;
    SessionSequence through = 0;
    std::string digest;
};

Result<std::optional<SessionHead>> read_head(sqlite3 *db, const std::string &session) {
    Statement select(db, "SELECT owner,through,digest FROM conversation_sessions WHERE session=?1");
    if (!select.valid() || !bind_text(select, 1, session))
        return index_error(ErrorCode::Internal, "conversation head query could not be prepared");
    const auto step = sqlite3_step(select.get());
    if (step == SQLITE_DONE)
        return std::optional<SessionHead>{};
    if (step != SQLITE_ROW)
        return index_error(ErrorCode::Internal, "conversation head could not be read");
    const auto through = sqlite3_column_int64(select.get(), 1);
    if (through < 0 || !digest_from_hex(column(select, 0)) || !digest_from_hex(column(select, 2)))
        return index_error(ErrorCode::DataLoss, "conversation head is malformed");
    return std::optional{SessionHead{std::string(column(select, 0)),
                                     static_cast<SessionSequence>(through),
                                     std::string(column(select, 2))}};
}

Result<bool> erased(sqlite3 *db, const std::string &owner) {
    Statement select(db, "SELECT 1 FROM conversation_erasures WHERE owner=?1");
    if (!select.valid() || !bind_text(select, 1, owner))
        return index_error(ErrorCode::Internal, "conversation erasure query could not be prepared");
    const auto step = sqlite3_step(select.get());
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        return index_error(ErrorCode::Internal, "conversation erasure state could not be read");
    return step == SQLITE_ROW;
}

Result<std::optional<SessionHead>> read_rebuild_head(sqlite3 *db, const std::string &session,
                                                     const std::string &owner) {
    const auto blocked = erased(db, owner);
    if (!blocked)
        return blocked.error();
    if (blocked.value())
        return index_error(ErrorCode::PermissionDenied, "conversation erasure barrier is active");
    auto head = read_head(db, session);
    if (!head)
        return head.error();
    if (head.value() && head.value()->owner != owner)
        return index_error(ErrorCode::PermissionDenied, "conversation index owner does not match");
    return head;
}

Result<std::size_t> count(sqlite3 *db, const char *sql, const std::string *owner = nullptr) {
    Statement select(db, sql);
    if (!select.valid() || (owner && !bind_text(select, 1, *owner)) ||
        sqlite3_step(select.get()) != SQLITE_ROW)
        return index_error(ErrorCode::Internal, "conversation index count failed");
    return static_cast<std::size_t>(sqlite3_column_int64(select.get(), 0));
}

Result<void> remove_segments(sqlite3 *db, const std::string &owner) {
    Statement remove(db, "DELETE FROM conversation_segments WHERE owner=?1");
    if (!remove.valid() || !bind_text(remove, 1, owner) ||
        sqlite3_step(remove.get()) != SQLITE_DONE)
        return index_error(ErrorCode::Internal, "conversation segment deletion failed");
    return {};
}

struct SegmentRow final {
    ContextCandidate candidate;
    SearchText search;
    std::string document;
    std::string digest;
};

struct SessionProjection final {
    SessionSequence through = 0;
    std::vector<SegmentRow> segments;
    std::string digest;
};

Result<void> check_context(const OperationContext &context) {
    if (context.cancelled())
        return index_error(ErrorCode::Cancelled, "conversation rebuild cancelled");
    if (context.expired(Timestamp::now()))
        return index_error(ErrorCode::DeadlineExceeded, "conversation rebuild deadline exceeded");
    return {};
}

Result<SessionProjection> project(SessionId session, const IEventStore &source,
                                  const SqliteConversationIndexOptions &options,
                                  const OperationContext &context) {
    EventSliceQuery query;
    query.session_id = session;
    query.limit = 128;
    query.byte_limit = options.max_source_bytes;
    std::size_t bytes_left = options.max_source_bytes;
    std::vector<ConversationEntry> entries;
    const ContextIndexPolicy policy;
    for (;;) {
        if (auto active = check_context(context); !active)
            return active.error();
        auto read = source.read_slice(query);
        if (!read)
            return read.error();
        auto &page = read.value();
        if (page.snapshot_through > options.max_source_events)
            return index_error(ErrorCode::ResourceExhausted,
                               "conversation source event limit exceeded");
        if ((query.through_sequence && *query.through_sequence != page.snapshot_through) ||
            page.scanned_through > page.snapshot_through ||
            page.has_more != (page.scanned_through < page.snapshot_through) ||
            page.scanned_through < query.after_sequence ||
            (page.has_more && page.scanned_through == query.after_sequence) ||
            page.events.size() > query.limit ||
            page.events.size() != page.scanned_through - query.after_sequence)
            return index_error(ErrorCode::DataLoss, "conversation source page is not contiguous");
        auto previous = query.after_sequence;
        for (const auto &event : page.events) {
            if (event.session_id != session || event.event_id.is_nil() ||
                event.session_sequence != ++previous)
                return index_error(ErrorCode::DataLoss, "conversation source event is invalid");
            if (event.payload.type.size() > bytes_left ||
                event.payload.data.size() > bytes_left - event.payload.type.size())
                return index_error(ErrorCode::ResourceExhausted,
                                   "conversation source byte limit exceeded");
            bytes_left -= event.payload.type.size() + event.payload.data.size();
            auto projected = project_conversation_entry(event);
            if (!projected)
                return projected.error();
            if (projected.value()) {
                const auto &text = projected.value()->text;
                if (text.find('\0') != std::string::npos ||
                    matches_content_marker(text, policy.forbidden_markers))
                    return index_error(ErrorCode::SafetyRejected,
                                       "conversation source is not indexable");
                entries.push_back(std::move(*projected.value()));
            }
        }
        query.through_sequence = page.snapshot_through;
        query.after_sequence = page.scanned_through;
        if (!page.has_more)
            break;
        if (bytes_left == 0)
            return index_error(ErrorCode::ResourceExhausted,
                               "conversation source byte limit exceeded");
        query.byte_limit = bytes_left;
    }
    auto segments = segment_conversation(session, entries, options.segmentation);
    if (!segments)
        return segments.error();
    SessionProjection result;
    result.through = *query.through_sequence;
    if (segments.value().size() > options.max_segments)
        return index_error(ErrorCode::ResourceExhausted, "conversation segment limit exceeded");
    std::string digests;
    for (auto &segment : segments.value()) {
        ContextIndexAsset asset{segment.asset_id,
                                ContextAssetKind::ConversationSegment,
                                std::move(segment.text),
                                session,
                                std::nullopt,
                                std::move(segment.source_events),
                                std::move(segment.entries),
                                segment.through_sequence};
        if (auto valid = asset.validate(policy); !valid)
            return valid.error();
        SegmentRow row;
        row.candidate.asset_id = asset.id;
        row.candidate.session = session;
        row.candidate.text = std::move(asset.text);
        row.candidate.turns = std::move(asset.entries);
        row.candidate.source_events = std::move(asset.source_events);
        row.candidate.through_event_sequence = asset.through_event_sequence;
        for (std::size_t i = 0; i < row.candidate.turns.size(); ++i)
            row.search.append(utterance(row.candidate.turns[i], &entries[segment.first_entry + i]));
        row.document = canonical_json_string(context_candidate_to_json(row.candidate));
        // A source entry with invalid UTF-8 cannot be encoded; reject it
        // before publication rather than store unreadable text.
        if (!parse_json(row.document))
            return index_error(ErrorCode::DataLoss, "conversation segment cannot be encoded");
        row.digest = digest_string(row.document).to_string();
        digests += row.digest;
        result.segments.push_back(std::move(row));
    }
    result.digest = digest_string(digests).to_string();
    return result;
}

Result<SessionSequence> publish(sqlite3 *db, const SqliteConversationIndexOptions &options,
                                const std::string &session, const std::string &owner,
                                const SessionProjection &projection) {
    Transaction tx(db);
    if (!tx.valid())
        return index_error(ErrorCode::Unavailable,
                           "conversation index write transaction unavailable");
    const auto head = read_rebuild_head(db, session, owner);
    if (!head)
        return head.error();
    if (head.value()) {
        if (projection.through < head.value()->through)
            return index_error(ErrorCode::InvalidState, "conversation snapshot is stale");
        if (projection.through == head.value()->through) {
            if (projection.digest != head.value()->digest)
                return index_error(ErrorCode::DataLoss,
                                   "conversation changed under the same watermark");
            return projection.through;
        }
    } else {
        const auto sessions = count(db, "SELECT count(*) FROM conversation_sessions");
        if (!sessions)
            return sessions.error();
        if (sessions.value() >= options.max_sessions)
            return index_error(ErrorCode::ResourceExhausted, "conversation session limit reached");
    }

    if (auto removed = remove_segments(db, owner); !removed)
        return removed.error();
    const auto rows = count(db, "SELECT count(*) FROM conversation_segments");
    if (!rows)
        return rows.error();
    if (rows.value() > options.max_segments - projection.segments.size())
        return index_error(ErrorCode::ResourceExhausted, "conversation index capacity reached");

    Statement insert(db, "INSERT INTO conversation_segments(id,owner,through,text,search_text,"
                         "lexical,document,digest) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
    for (const auto &row : projection.segments) {
        if (!insert.valid() || !bind_text(insert, 1, row.candidate.asset_id.to_string()) ||
            !bind_text(insert, 2, owner) ||
            sqlite3_bind_int64(insert.get(), 3,
                               static_cast<std::int64_t>(row.candidate.through_event_sequence)) !=
                SQLITE_OK ||
            !bind_text(insert, 4, row.candidate.text) || !bind_text(insert, 5, row.search.text) ||
            !bind_text(insert, 6, row.search.lexical) || !bind_text(insert, 7, row.document) ||
            !bind_text(insert, 8, row.digest) || sqlite3_step(insert.get()) != SQLITE_DONE)
            return index_error(ErrorCode::Internal, "conversation segment insert failed");
        insert.reset();
    }
    Statement water(db, "INSERT INTO conversation_sessions(session,owner,through,digest)"
                        " VALUES(?1,?2,?3,?4) ON CONFLICT(session) DO UPDATE SET"
                        " through=excluded.through,digest=excluded.digest");
    if (!water.valid() || !bind_text(water, 1, session) || !bind_text(water, 2, owner) ||
        sqlite3_bind_int64(water.get(), 3, static_cast<std::int64_t>(projection.through)) !=
            SQLITE_OK ||
        !bind_text(water, 4, projection.digest) || sqlite3_step(water.get()) != SQLITE_DONE)
        return index_error(ErrorCode::Internal, "conversation watermark write failed");
    if (auto committed = tx.commit(); !committed)
        return committed.error();
    return projection.through;
}

class QueryDeadline final {
  public:
    QueryDeadline(sqlite3 *db, std::chrono::steady_clock::time_point deadline)
        : db_(db), deadline_(deadline) {
        sqlite3_progress_handler(
            db_, 128,
            [](void *self) {
                return std::chrono::steady_clock::now() >=
                               static_cast<QueryDeadline *>(self)->deadline_
                           ? 1
                           : 0;
            },
            this);
    }
    ~QueryDeadline() { sqlite3_progress_handler(db_, 0, nullptr, nullptr); }

  private:
    sqlite3 *db_;
    std::chrono::steady_clock::time_point deadline_;
};

void note_deadline(ContextRetrievalResult &result) {
    result.quality.deadline_exceeded = true;
    result.quality.degraded = true;
    result.quality.note = "conversation retrieval deadline reached";
}

// The MATCH expression starts with the owner phrase, followed by the
// searchable terms in bind order. Report the latter using FTS5's own hits.
void conversation_fts_matches(const Fts5ExtensionApi *api, Fts5Context *fts,
                              sqlite3_context *result, int, sqlite3_value **) noexcept {
    std::array<char, storage::kMaxFtsMatchUnits> flags;
    const int terms = api->xPhraseCount(fts) - 1;
    if (terms < 1 || terms > static_cast<int>(flags.size())) {
        sqlite3_result_error_code(result, SQLITE_CORRUPT);
        return;
    }
    for (int phrase = 1; phrase <= terms; ++phrase) {
        Fts5PhraseIter iter{};
        int column_index = -1;
        const int status = api->xPhraseFirstColumn(fts, phrase, &iter, &column_index);
        if (status != SQLITE_OK) {
            sqlite3_result_error_code(result, status);
            return;
        }
        flags[static_cast<std::size_t>(phrase - 1)] = column_index == 1 ? '1' : '0';
    }
    sqlite3_result_text(result, flags.data(), terms, storage::transient_copy());
}

Result<void> register_conversation_fts_matches(sqlite3 *db) {
    fts5_api *api = nullptr;
    Statement lookup(db, "SELECT fts5(?1)");
    if (!lookup.valid() ||
        // SQLite's typed pointer argument writes the API pointer through this slot.
        // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
        sqlite3_bind_pointer(lookup.get(), 1, static_cast<void *>(&api), "fts5_api_ptr", nullptr) !=
            SQLITE_OK ||
        sqlite3_step(lookup.get()) != SQLITE_ROW || !api ||
        api->xCreateFunction(api, "mira_conversation_matches", nullptr, conversation_fts_matches,
                             nullptr) != SQLITE_OK)
        return index_error(ErrorCode::Internal,
                           "conversation match evidence could not be registered");
    return {};
}

Result<ContextRetrievalResult> search(sqlite3 *db, const ContextQuery &query,
                                      const RetrievalBudget &budget,
                                      std::chrono::steady_clock::time_point deadline) {
    ContextRetrievalResult result;
    if (query.query_embedding) {
        result.quality.vector_degraded = true;
        result.quality.degraded = true;
        result.quality.note = "conversation index supplies lexical and exact results";
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        note_deadline(result);
        return result;
    }
    auto head = read_head(db, query.session->to_string());
    if (!head)
        return head.error();
    if (!head.value())
        return result;
    const auto &owner = head.value()->owner;
    const bool authorized =
        std::any_of(query.scopes.begin(), query.scopes.end(), [&](const MemoryScope &scope) {
            return scope.kind == MemoryScopeKind::Session &&
                   memory_erasure_scope_digest(scope).to_string() == owner;
        });
    if (!authorized)
        return result;
    const auto blocked = erased(db, owner);
    if (!blocked)
        return blocked.error();
    if (blocked.value())
        return result;

    const auto match = storage::fts_match_expression(query.text, true);
    if ((query.query_embedding && match.expression.empty() && query.exact_terms.empty()) ||
        (!query.text.empty() && match.expression.empty()))
        return result;
    // A lexical query puts the most relevant segment first (DEC-147). FTS5
    // bm25 ranks are negative-better, so ascending rank is descending
    // relevance. Without one the order stays newest-first.
    const bool lexical = !match.expression.empty();
    std::string sql = "SELECT s.document,s.digest,s.id,s.through,s.text,";
    sql += lexical ? "rank,mira_conversation_matches(conversation_fts)" : "0.0,NULL";
    sql += " FROM conversation_segments s";
    if (lexical)
        sql += " JOIN conversation_fts ON conversation_fts.rowid=s.rowid";
    sql += " WHERE s.owner=?";
    if (lexical)
        sql += " AND conversation_fts MATCH ? AND rank MATCH 'bm25(0.0,1.0)'";
    for (std::size_t i = 0; i < query.exact_terms.size(); ++i)
        sql += " AND instr(lower(s.search_text),lower(?))>0";
    sql += lexical ? " ORDER BY rank,s.through DESC,s.id LIMIT ?"
                   : " ORDER BY s.through DESC,s.id LIMIT ?";
    Statement rows(db, sql.c_str());
    if (!rows.valid() || !bind_text(rows, 1, owner))
        return index_error(ErrorCode::Internal, "conversation search could not be prepared");
    int parameter = 2;
    if (lexical && !bind_text(rows, parameter++,
                              "owner:\"" + owner + "\" AND lexical:(" + match.expression + ")"))
        return index_error(ErrorCode::Internal, "conversation search could not bind keywords");
    for (const auto &term : query.exact_terms)
        if (!bind_text(rows, parameter++, term))
            return index_error(ErrorCode::Internal,
                               "conversation search could not bind exact term");
    if (sqlite3_bind_int64(rows.get(), parameter, static_cast<std::int64_t>(budget.top_k)) !=
        SQLITE_OK)
        return index_error(ErrorCode::Internal, "conversation search could not bind result limit");

    QueryDeadline guard(db, deadline);
    result.quality.fts_leg_ran = lexical;
    result.quality.exact_leg_ran = !query.exact_terms.empty();
    if (match.truncated) {
        result.quality.degraded = true;
        result.quality.note =
            "lexical query exceeded the unit bound; a subset of its units was matched";
    }
    int step = SQLITE_ROW;
    while (std::chrono::steady_clock::now() < deadline &&
           (step = sqlite3_step(rows.get())) == SQLITE_ROW) {
        const auto text = column(rows, 4);
        const auto tokens = std::max<std::uint64_t>(16, text.size() / 4 + 8);
        if (tokens > budget.token_budget - result.tokens_estimate)
            continue;
        const auto document = column(rows, 0);
        if (document.size() > 128 * 1024 || digest_string(document).to_string() != column(rows, 1))
            return index_error(ErrorCode::DataLoss, "conversation segment digest is invalid");
        const auto parsed = parse_json(document);
        if (!parsed)
            return index_error(ErrorCode::DataLoss, "conversation segment document is invalid");
        auto decoded = context_candidate_from_json(parsed.value());
        const auto through = sqlite3_column_int64(rows.get(), 3);
        if (!decoded || decoded.value().session != query.session || decoded.value().scope ||
            decoded.value().asset_id.to_string() != column(rows, 2) ||
            decoded.value().kind != ContextAssetKind::ConversationSegment || through <= 0 ||
            decoded.value().through_event_sequence != static_cast<SessionSequence>(through) ||
            decoded.value().through_event_sequence > head.value()->through ||
            decoded.value().text != text)
            return index_error(ErrorCode::DataLoss,
                               "conversation segment does not match its index");
        auto &candidate = decoded.value();
        candidate.exact_hit = result.quality.exact_leg_ran;
        candidate.lexical_score = 0.0;
        if (lexical) {
            // The same mapping as the memory store's lexical leg: 0 for a match
            // the index gives no weight, towards 1 as the match strengthens.
            const double rank = sqlite3_column_double(rows.get(), 5);
            const auto hits = column(rows, 6);
            if (hits.size() != match.terms.size())
                return index_error(ErrorCode::DataLoss,
                                   "conversation match evidence is incomplete");
            for (std::size_t i = 0; i < hits.size(); ++i)
                if (hits[i] == '1')
                    candidate.lexical_matches.push_back(match.terms[i]);
            candidate.lexical_bm25 = rank;
            const double magnitude = std::max(0.0, -rank);
            candidate.lexical_score = magnitude / (1.0 + magnitude);
        }
        candidate.score = candidate.lexical_score + (candidate.exact_hit ? 1.0 : 0.0);
        result.tokens_estimate += tokens;
        result.candidates.push_back(std::move(candidate));
    }
    if (step == SQLITE_INTERRUPT ||
        (step == SQLITE_ROW && std::chrono::steady_clock::now() >= deadline))
        note_deadline(result);
    else if (step != SQLITE_DONE)
        return index_error(ErrorCode::Internal, "conversation search failed");
    return result;
}

} // namespace

class SqliteConversationIndex::Impl final {
  public:
    Impl(kairo::Executor &executor, storage::DatabaseHandle handle,
         SqliteConversationIndexOptions opts)
        : options(std::move(opts)), database(std::move(handle)),
          channel(executor, database.get(),
                  {"mira-conversation-index", options.max_pending_requests,
                   options.operation_timeout}) {}

    const SqliteConversationIndexOptions options;
    storage::DatabaseHandle database;
    storage::StoreChannel channel;
    // Odd while erasing or quarantined. A reader rechecks after completing
    // its StoreChannel call so concurrent erasure invalidates older results.
    std::atomic<std::uint64_t> privacy_epoch{0};
    std::mutex erasure_mutex;
};

SqliteConversationIndex::SqliteConversationIndex(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SqliteConversationIndex::~SqliteConversationIndex() { (void)close(); }

Result<std::unique_ptr<SqliteConversationIndex>>
SqliteConversationIndex::open(kairo::Executor &executor, SqliteConversationIndexOptions options) {
    if (options.path.empty() || options.max_sessions == 0 || options.max_sessions > 4096 ||
        options.max_segments == 0 || options.max_segments > 100000 ||
        options.max_erasure_barriers == 0 || options.max_erasure_barriers > 65536 ||
        options.max_source_events == 0 || options.max_source_events > 65536 ||
        options.max_source_bytes == 0 || options.max_source_bytes > 64 * 1024 * 1024 ||
        options.max_pending_requests == 0 || options.max_pending_requests > 256 ||
        options.operation_timeout.count() <= 0 ||
        options.operation_timeout > std::chrono::minutes(10) ||
        options.segmentation.window_entries > ContextIndexPolicy{}.max_source_events ||
        options.segmentation.max_segment_bytes > 4096 || !options.segmentation.validate())
        return index_error(ErrorCode::InvalidArgument, "invalid conversation index options");
    storage::DbOpenOptions db_options;
    db_options.path = options.path;
    db_options.exclusive_owner = true;
    db_options.busy_timeout = std::min(options.operation_timeout, std::chrono::milliseconds{5000});
    const std::string ddl = std::string(kTables) + kFullText;
    auto opened = storage::open_reference_store(
        db_options, {"conversation-index", kConversationIndexSchema, ddl.c_str(), index_error,
                     migrate_conversation_search});
    if (!opened)
        return opened.error();
    if (opened.value().diagnostics.read_only)
        return index_error(ErrorCode::UnsupportedVersion, "unsupported conversation index schema");
    if (auto secure = storage::exec_sql(opened.value().database.get(), "PRAGMA secure_delete=ON;");
        !secure)
        return secure.error();
    if (auto matches = register_conversation_fts_matches(opened.value().database.get()); !matches)
        return matches.error();
    auto impl =
        std::make_unique<Impl>(executor, std::move(opened.value().database), std::move(options));
    return std::unique_ptr<SqliteConversationIndex>(new SqliteConversationIndex(std::move(impl)));
}

Result<SessionSequence> SqliteConversationIndex::rebuild(const TenantId &tenant, SessionId session,
                                                         const IEventStore &source,
                                                         const OperationContext &context) {
    if (context.session.is_nil())
        return index_error(ErrorCode::InvalidArgument,
                           "conversation rebuild requires context.session");
    if (tenant.is_nil() || session.is_nil())
        return index_error(ErrorCode::InvalidArgument,
                           "conversation rebuild requires tenant and session");
    if (context.session != session)
        return index_error(ErrorCode::PermissionDenied,
                           "conversation rebuild session binding disagrees");
    const auto epoch = impl_->privacy_epoch.load();
    if ((epoch & 1U) != 0U)
        return index_error(ErrorCode::Unavailable, "conversation index erasure fence is active");
    PrincipalContext principal;
    principal.tenant_id = tenant;
    const auto owner =
        memory_erasure_scope_digest(memory_session_scope(principal, session)).to_string();
    if (auto active = check_context(context); !active)
        return active.error();
    const auto session_key = session.to_string();
    auto permitted = impl_->channel.run<std::optional<SessionHead>>(
        [session_key, owner](sqlite3 *db) { return read_rebuild_head(db, session_key, owner); });
    if (!permitted)
        return permitted.error();
    if (impl_->privacy_epoch.load() != epoch)
        return index_error(ErrorCode::InvalidState,
                           "conversation projection invalidated by erasure");
    auto projection = project(session, source, impl_->options, context);
    if (!projection)
        return projection.error();
    if (auto active = check_context(context); !active)
        return active.error();
    return impl_->channel.run<SessionSequence>(
        [state = impl_.get(), epoch, owner, session = session_key,
         projection = std::move(projection).value()](sqlite3 *db) -> Result<SessionSequence> {
            if (state->privacy_epoch.load() != epoch)
                return index_error(ErrorCode::InvalidState,
                                   "conversation projection invalidated by erasure");
            return publish(db, state->options, session, owner, projection);
        });
}

Result<ContextRetrievalResult> SqliteConversationIndex::retrieve(const ContextQuery &query,
                                                                 const RetrievalBudget &budget) {
    if (auto valid = query.validate(); !valid)
        return valid.error();
    if (auto valid = budget.validate(); !valid)
        return valid.error();
    const auto valid_text = [](const std::string &text) {
        return text.size() <= 4096 && text.find('\0') == std::string::npos;
    };
    if (query.scopes.size() > 16 || query.exact_terms.size() > 16 || !valid_text(query.text) ||
        !std::all_of(query.exact_terms.begin(), query.exact_terms.end(),
                     [&](const auto &term) { return !term.empty() && valid_text(term); }))
        return index_error(ErrorCode::InvalidArgument, "conversation query exceeds its bounds");
    if (!query.session ||
        (query.kinds && std::find(query.kinds->begin(), query.kinds->end(),
                                  ContextAssetKind::ConversationSegment) == query.kinds->end()))
        return ContextRetrievalResult{};
    const auto epoch = impl_->privacy_epoch.load();
    if ((epoch & 1U) != 0U)
        return index_error(ErrorCode::Unavailable, "conversation index erasure fence is active");
    const auto deadline = std::chrono::steady_clock::now() +
                          std::min(budget.deadline, impl_->options.operation_timeout);
    auto result = impl_->channel.run<ContextRetrievalResult>(
        [query, budget, deadline](sqlite3 *db) { return search(db, query, budget, deadline); });
    if (impl_->privacy_epoch.load() != epoch)
        return index_error(ErrorCode::InvalidState, "conversation query invalidated by erasure");
    return result;
}

Result<void> SqliteConversationIndex::erase_scope(const Sha256Digest &digest) {
    if (digest == Sha256Digest{})
        return index_error(ErrorCode::InvalidArgument,
                           "conversation erasure scope must be nonzero");
    const std::lock_guard lock(impl_->erasure_mutex);
    if ((impl_->privacy_epoch.load() & 1U) != 0U)
        return index_error(ErrorCode::Unavailable, "conversation index requires erasure recovery");
    impl_->privacy_epoch.fetch_add(1);
    auto result = impl_->channel.run<void>(
        [owner = digest.to_string(),
         capacity = impl_->options.max_erasure_barriers](sqlite3 *db) -> Result<void> {
            Transaction tx(db);
            if (!tx.valid())
                return index_error(ErrorCode::Unavailable,
                                   "conversation erasure transaction unavailable");
            const auto blocked = erased(db, owner);
            if (!blocked)
                return blocked.error();
            if (!blocked.value()) {
                const auto barriers = count(db, "SELECT count(*) FROM conversation_erasures");
                if (!barriers)
                    return barriers.error();
                if (barriers.value() >= capacity)
                    return index_error(ErrorCode::ResourceExhausted,
                                       "conversation erasure barrier capacity reached");
                Statement insert(db, "INSERT INTO conversation_erasures(owner) VALUES(?1)");
                if (!insert.valid() || !bind_text(insert, 1, owner) ||
                    sqlite3_step(insert.get()) != SQLITE_DONE)
                    return index_error(ErrorCode::Internal,
                                       "conversation erasure barrier write failed");
            }
            if (auto removed = remove_segments(db, owner); !removed)
                return removed;
            return tx.commit();
        });
    if (result)
        impl_->privacy_epoch.fetch_add(1);
    return result;
}

Result<void> SqliteConversationIndex::close() noexcept { return impl_->channel.close(); }

} // namespace mira
