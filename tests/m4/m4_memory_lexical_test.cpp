// Lexical leg of the durable memory store after DEC-147: Chinese recall,
// relevance order, the schema 1.9 migration and the query unit bound.

#include "../support/test.hpp"

#include <mira/sqlite_memory_store.hpp>
#include <mira/text_segmentation.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <kairo/executor.hpp>
#include <sqlite3.h>

namespace {

using namespace mira;

std::filesystem::path temp_dir() {
    static const auto dir = [] {
        auto base = std::filesystem::temp_directory_path();
        std::random_device device;
        auto path = base / ("mira-m4-memory-lexical-" + std::to_string(device()));
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

[[nodiscard]] MemoryScope scope_of(std::string subject) {
    MemoryScope scope;
    scope.kind = MemoryScopeKind::Environment;
    scope.subject_id = std::move(subject);
    return scope;
}

[[nodiscard]] MemoryRecord make_record(const MemoryScope &scope, std::string statement) {
    MemoryRecord value;
    value.id = MemoryId::generate();
    value.scope = scope;
    value.kind = MemoryKind::EnvironmentFact;
    value.statement = std::move(statement);
    value.recorded_at = std::chrono::system_clock::now();
    value.validity.valid_from = value.recorded_at;
    value.provenance = {EventId::generate()};
    value.verification = MemoryVerification::Verified;
    value.confidence = 0.8F;
    return value;
}

[[nodiscard]] MemoryMutation add_for(const MemoryRecord &record) {
    MemoryMutation mutation;
    mutation.id = MutationId::generate();
    mutation.scope = record.scope;
    mutation.proposed = record;
    mutation.evidence = record.provenance;
    return mutation;
}

// Opens and closes one store file; the executor outlives every opening.
struct Fixture final {
    explicit Fixture(std::filesystem::path path) : path_(std::move(path)) {
        executor_.initialize(kairo::ExecutorConfig{});
    }
    ~Fixture() {
        close();
        (void)executor_.shutdown(true);
    }
    [[nodiscard]] bool open() {
        SqliteMemoryStoreOptions options;
        options.path = path_;
        auto opened = SqliteMemoryStore::open(executor_, options);
        if (!opened) {
            return false;
        }
        store = std::move(opened).value();
        return true;
    }
    void close() {
        if (store != nullptr) {
            (void)store->close();
            store.reset();
        }
    }
    [[nodiscard]] bool add(const MemoryScope &scope, std::string statement) {
        return store->apply(add_for(make_record(scope, std::move(statement)))).has_value();
    }
    [[nodiscard]] Result<MemoryQueryResult> query(const MemoryScope &scope,
                                                  const std::string &text) const {
        MemoryQuery query;
        query.scopes = {scope};
        query.text = text;
        return store->query(query);
    }

    kairo::Executor executor_;
    std::filesystem::path path_;
    std::unique_ptr<SqliteMemoryStore> store;
};

// Raw access to a closed store file, to rewrite it into an older layout.
[[nodiscard]] bool exec(const std::filesystem::path &path, const char *sql) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        return false;
    }
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return result == SQLITE_OK;
}

[[nodiscard]] std::string scalar(const std::filesystem::path &path, const char *sql) {
    std::string value;
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        return value;
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK &&
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

int chinese_statements_are_recalled() {
    Fixture fixture(temp_dir() / "lexical-chinese.db");
    MIRA_CHECK(fixture.open());
    const auto scope = scope_of("lab");
    const std::string weather = "我想知道北京的天气怎么样";
    const std::string docker = "重启 docker 服务时记得先 down 旧容器";
    MIRA_CHECK(fixture.add(scope, weather));
    MIRA_CHECK(fixture.add(scope, "白板马克笔需要更换了"));
    MIRA_CHECK(fixture.add(scope, docker));

    // A word inside a sentence written without spaces.
    auto found = fixture.query(scope, "北京天气");
    MIRA_CHECK(found && found.value().quality.fts_leg_ran && !found.value().quality.degraded);
    MIRA_CHECK(found.value().records.size() == 1);
    MIRA_CHECK(found.value().records.front().statement == weather);

    // A natural-language question: not every word of it has to appear, and
    // the record sharing more of it comes first.
    found = fixture.query(scope, "怎么重启 docker 服务");
    MIRA_CHECK(found && found.value().records.size() == 2);
    MIRA_CHECK(found.value().records.front().statement == docker);
    found = fixture.query(scope, "马克笔在哪");
    MIRA_CHECK(found && found.value().records.size() == 1);
    MIRA_CHECK(found.value().records.front().statement == "白板马克笔需要更换了");

    // A full-width spelling finds the half-width record.
    MIRA_CHECK(fixture.add(scope, "部署脚本 deploy.sh 需要 staging 环境变量"));
    found = fixture.query(scope, "ＤＥＰＬＯＹ ＳＴＡＧＩＮＧ");
    MIRA_CHECK(found && found.value().records.size() == 1);
    MIRA_CHECK(found.value().records.front().statement ==
               "部署脚本 deploy.sh 需要 staging 环境变量");

    // The exact leg stays the mandatory channel: a term no record carries
    // empties the answer even though the lexical leg matched.
    MemoryQuery strict;
    strict.scopes = {scope};
    strict.text = "北京天气";
    strict.exact_terms = {"上海"};
    MIRA_CHECK(fixture.store->query(strict) &&
               fixture.store->query(strict).value().records.empty());

    // Text without a single lexical unit runs no lexical leg.
    found = fixture.query(scope, "？！");
    MIRA_CHECK(found && found.value().records.empty());
    MIRA_CHECK(!found.value().quality.fts_leg_ran);
    return 0;
}

// Regression for the inverted bm25 mapping: the old formula gave a stronger
// match a smaller contribution, so the lexical leg returned the weakest hit
// first as soon as more than one record matched with different strength.
int stronger_matches_come_first() {
    Fixture fixture(temp_dir() / "lexical-order.db");
    MIRA_CHECK(fixture.open());
    const auto scope = scope_of("lab");
    const std::string strongest = "calibration calibration calibration runs on Friday";
    const std::string shorter = "calibration runs on Friday";
    const std::string longer = "the lab runs calibration and staging checks every Friday";
    // The term must stay rare enough for the index to give it weight.
    for (int index = 0; index < 12; ++index) {
        MIRA_CHECK(fixture.add(scope, "unrelated filler note number " + std::to_string(index) +
                                          " about meetings"));
    }
    MIRA_CHECK(fixture.add(scope, strongest));
    MIRA_CHECK(fixture.add(scope, shorter));
    MIRA_CHECK(fixture.add(scope, longer));

    const auto found = fixture.query(scope, "calibration");
    MIRA_CHECK(found && found.value().quality.fts_leg_ran);
    MIRA_CHECK(found.value().records.size() == 3);
    MIRA_CHECK(found.value().records[0].statement == strongest);
    MIRA_CHECK(found.value().records[1].statement == shorter);
    MIRA_CHECK(found.value().records[2].statement == longer);
    return 0;
}

int bigrams_rank_adjacent_characters_above_other_cooccurrences() {
    Fixture fixture(temp_dir() / "lexical-phrases.db");
    MIRA_CHECK(fixture.open());
    const auto scope = scope_of("lab");
    const std::string adjacent = "我查蓝牙設定";
    const auto add = [&](std::string statement, const char *id) {
        auto record = make_record(scope, std::move(statement));
        record.id = MemoryId::parse(id).value();
        return fixture.store->apply(add_for(record));
    };
    // Use the largest ID for the adjacent match so an equal lexical score
    // cannot pass by winning the RRF leg's ID tie-break.
    MIRA_CHECK(add(adjacent, "00000000000000000000000000000003"));
    MIRA_CHECK(add("我查蓝設牙定", "00000000000000000000000000000001"));
    MIRA_CHECK(add("我查牙蓝設定", "00000000000000000000000000000002"));

    const auto found = fixture.query(scope, "蓝牙");
    MIRA_CHECK(found && found.value().records.size() == 3);
    // The character terms allow all three hits. Only the first statement
    // contains the phrase in the query's order and at adjacent positions.
    MIRA_CHECK(found.value().records.front().statement == adjacent);
    return 0;
}

int older_schema_is_reindexed_in_place() {
    const auto path = temp_dir() / "lexical-migration.db";
    const auto scope = scope_of("lab");
    const std::string weather = "我想知道北京的天气怎么样";
    {
        Fixture fixture(path);
        MIRA_CHECK(fixture.open());
        MIRA_CHECK(fixture.add(scope, weather));
        MIRA_CHECK(fixture.add(scope, "白板马克笔已干"));
        fixture.close();
    }
    // Rewrite the file into the 1.8 layout, where the index held each
    // statement as written and a word inside a Chinese sentence could not
    // be matched.
    MIRA_CHECK(exec(path, "DELETE FROM memory_fts;"
                          "INSERT INTO memory_fts(record_id, statement)"
                          " SELECT id, statement FROM memory_records;"
                          "UPDATE store_meta SET value = '1.8' WHERE key = 'schema_version';"));
    {
        Fixture fixture(path);
        MIRA_CHECK(fixture.open());
        MIRA_CHECK(fixture.store->diagnostics().disposition == StoreSchemaDisposition::Migrated);
        auto found = fixture.query(scope, "北京天气");
        MIRA_CHECK(found && found.value().records.size() == 1);
        MIRA_CHECK(found.value().records.front().statement == weather);
        found = fixture.query(scope, "马克笔");
        MIRA_CHECK(found && found.value().records.size() == 1);
        MIRA_CHECK(found.value().records.front().statement == "白板马克笔已干");
        fixture.close();
    }
    // The index now holds the segmentation and the record is intact.
    MIRA_CHECK(scalar(path, "SELECT value FROM store_meta WHERE key = 'schema_version'") == "1.10");
    MIRA_CHECK(scalar(path, "SELECT count(*) FROM memory_fts") == "2");
    MIRA_CHECK(scalar(path, "SELECT statement FROM memory_fts WHERE statement LIKE '%北%'") ==
               lexical_index_text(weather));
    {
        // Nothing is left to migrate, and the records are untouched.
        Fixture fixture(path);
        MIRA_CHECK(fixture.open());
        MIRA_CHECK(fixture.store->diagnostics().disposition == StoreSchemaDisposition::UpToDate);
        const auto found = fixture.query(scope, "北京天气");
        MIRA_CHECK(found && found.value().records.size() == 1);
        MIRA_CHECK(found.value().records.front().statement == weather);
    }
    return 0;
}

int overlong_queries_match_a_subset_and_say_so() {
    Fixture fixture(temp_dir() / "lexical-units.db");
    MIRA_CHECK(fixture.open());
    const auto scope = scope_of("lab");
    const std::string printer = "打印机 IP 是 192.168.1.42";
    MIRA_CHECK(fixture.add(scope, printer));
    MIRA_CHECK(fixture.add(scope, "茶水间在二楼东侧"));

    // Two hundred further characters make the query far larger than one
    // query may match.
    std::string text = "打印机地址";
    for (std::uint32_t codepoint = 0x4E00U; codepoint < 0x4E00U + 200U; ++codepoint) {
        text += static_cast<char>(0xE0U | (codepoint >> 12U));
        text += static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
        text += static_cast<char>(0x80U | (codepoint & 0x3FU));
    }
    const auto partial = fixture.query(scope, text);
    MIRA_CHECK(partial && partial.value().quality.fts_leg_ran);
    MIRA_CHECK(partial.value().quality.degraded);
    MIRA_CHECK(partial.value().quality.note.find("unit bound") != std::string::npos);
    MIRA_CHECK(partial.value().records.size() == 1);
    MIRA_CHECK(partial.value().records.front().statement == printer);

    // A query within the bound is matched completely.
    const auto complete = fixture.query(scope, "打印机地址");
    MIRA_CHECK(complete && complete.value().records.size() == 1);
    MIRA_CHECK(complete.value().records.front().statement == printer);
    MIRA_CHECK(!complete.value().quality.degraded);
    MIRA_CHECK(complete.value().quality.note.empty());
    return 0;
}

} // namespace

int main() {
    if (const int result = chinese_statements_are_recalled(); result != 0) {
        return result;
    }
    if (const int result = stronger_matches_come_first(); result != 0) {
        return result;
    }
    if (const int result = bigrams_rank_adjacent_characters_above_other_cooccurrences();
        result != 0) {
        return result;
    }
    if (const int result = older_schema_is_reindexed_in_place(); result != 0) {
        return result;
    }
    return overlong_queries_match_a_subset_and_say_so();
}
