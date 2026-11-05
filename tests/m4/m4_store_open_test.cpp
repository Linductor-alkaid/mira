#include "support/test.hpp"

#include <kairo/executor.hpp>
#include <mira/sqlite_memory_store.hpp>
#include <mira/state_store.hpp>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {
using namespace mira;

class Database final {
  public:
    explicit Database(const std::filesystem::path &path) {
        (void)sqlite3_open(path.string().c_str(), &db);
    }
    ~Database() { sqlite3_close_v2(db); }
    bool exec(const char *sql) {
        return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    bool version(const std::string &text) {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE store_meta SET value=?1 WHERE key='schema_version'", -1,
                               &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        const bool ok = sqlite3_bind_text(statement, 1, text.c_str(), -1, nullptr) == SQLITE_OK &&
                        sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return ok;
    }
    std::string text(const char *sql) {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
            return "<query-error>";
        }
        std::string result;
        if (sqlite3_step(statement) == SQLITE_ROW) {
            const auto *value = sqlite3_column_text(statement, 0);
            result = value ? reinterpret_cast<const char *>(value) : "<null>";
        }
        sqlite3_finalize(statement);
        return result;
    }
    sqlite3 *db = nullptr;
};

struct Fixture final {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("mira-store-open-" + SessionId::generate().to_string());
    kairo::Executor executor;
    Fixture() {
        std::filesystem::create_directories(root);
        (void)executor.initialize(kairo::ExecutorConfig{});
    }
    ~Fixture() {
        (void)executor.shutdown(true);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Result<StoreDiagnostics> open(bool memory, const std::filesystem::path &path,
                                  bool read_only = false) {
        if (memory) {
            SqliteMemoryStoreOptions options;
            options.path = path;
            options.read_only_diagnostic = read_only;
            options.busy_timeout = std::chrono::milliseconds(1);
            auto store = SqliteMemoryStore::open(executor, options);
            if (!store) {
                return store.error();
            }
            auto diagnostics = store.value()->diagnostics();
            if (auto closed = store.value()->close(); !closed) {
                return closed.error();
            }
            return diagnostics;
        }
        SqliteStoreOptions options;
        options.path = path;
        options.read_only_diagnostic = read_only;
        options.busy_timeout = std::chrono::milliseconds(1);
        auto store = SqliteCheckpointStore::open(executor, options);
        if (!store) {
            return store.error();
        }
        auto diagnostics = store.value()->diagnostics();
        if (auto closed = store.value()->close(); !closed) {
            return closed.error();
        }
        return diagnostics;
    }
};

constexpr const char *kVersion = "SELECT value FROM store_meta WHERE key='schema_version'";

int version_matrix_preserves_metadata() {
    Fixture fixture;
    for (const bool memory : {false, true}) {
        const auto path = fixture.root / (memory ? "memory.db" : "checkpoint.db");
        const std::string current = memory ? "1.1" : "1.0";
        auto fresh = fixture.open(memory, path);
        MIRA_CHECK(fresh && fresh.value().disposition == StoreSchemaDisposition::Created);
        auto reopened = fixture.open(memory, path);
        MIRA_CHECK(reopened && reopened.value().disposition == StoreSchemaDisposition::UpToDate);
        for (const std::string invalid : {"", "1", "1.", " .1", "1.1tail", "65537.1", "1.65536",
                                          "18446744073709551616.0", "-1.0", "+1.0", "1.0.0"}) {
            {
                Database db(path);
                MIRA_CHECK(db.version(invalid));
            }
            auto bad = fixture.open(memory, path);
            MIRA_CHECK(!bad && bad.error().code == ErrorCode::DataLoss);
            MIRA_CHECK(bad.error().domain ==
                       (memory ? "mira.state_store.memory" : "mira.state_store.checkpoint"));
            {
                Database db(path);
                MIRA_CHECK(db.text(kVersion) == invalid);
                MIRA_CHECK(db.version(current));
            }
            MIRA_CHECK(fixture.open(memory, path));
        }
        {
            Database db(path);
            MIRA_CHECK(db.version("1.99"));
        }
        auto future = fixture.open(memory, path);
        MIRA_CHECK(!future && future.error().code == ErrorCode::UnsupportedVersion);
        auto diagnostic = fixture.open(memory, path, true);
        MIRA_CHECK(diagnostic && diagnostic.value().read_only);
        MIRA_CHECK(diagnostic.value().disposition == StoreSchemaDisposition::ReadOnlyDiagnostic);
        {
            Database db(path);
            MIRA_CHECK(db.text(kVersion) == "1.99");
            MIRA_CHECK(db.version(current));
            MIRA_CHECK(db.exec("DELETE FROM store_meta WHERE key='schema_version'"));
        }
        auto missing = fixture.open(memory, path);
        MIRA_CHECK(!missing && missing.error().code == ErrorCode::InvalidState);
        {
            Database db(path);
            MIRA_CHECK(db.exec("INSERT INTO store_meta VALUES('schema_version', '1.0')"));
            MIRA_CHECK(db.version(current));
            MIRA_CHECK(db.exec("UPDATE store_meta SET value='wrong-kind' WHERE key='store_kind'"));
        }
        auto kind = fixture.open(memory, path);
        MIRA_CHECK(!kind && kind.error().code == ErrorCode::InvalidArgument);
        Database unchanged(path);
        MIRA_CHECK(unchanged.text("SELECT value FROM store_meta WHERE key='store_kind'") ==
                   "wrong-kind");
    }
    return 0;
}

int migration_and_version_stamp_roll_back_together() {
    Fixture fixture;
    const auto path = fixture.root / "migration.db";
    MIRA_CHECK(fixture.open(true, path));
    {
        Database db(path);
        MIRA_CHECK(db.exec("ALTER TABLE memory_mutations DROP COLUMN scope_key"));
        MIRA_CHECK(db.version("1.0"));
    }
    auto readonly = fixture.open(true, path, true);
    MIRA_CHECK(!readonly && readonly.error().code == ErrorCode::UnsupportedVersion);
    {
        Database db(path);
        MIRA_CHECK(db.exec(
            "CREATE TRIGGER reject_stamp BEFORE UPDATE ON store_meta "
            "WHEN NEW.key='schema_version' BEGIN SELECT RAISE(ABORT, 'test stamp failure'); END"));
    }
    auto failed = fixture.open(true, path);
    MIRA_CHECK(!failed);
    {
        Database db(path);
        MIRA_CHECK(db.text(kVersion) == "1.0");
        MIRA_CHECK(db.text("SELECT count(*) FROM pragma_table_info('memory_mutations') WHERE "
                           "name='scope_key'") == "0");
        MIRA_CHECK(db.exec("DROP TRIGGER reject_stamp"));
    }
    auto migrated = fixture.open(true, path);
    MIRA_CHECK(migrated && migrated.value().disposition == StoreSchemaDisposition::Migrated);
    Database db(path);
    MIRA_CHECK(db.text(kVersion) == "1.1");
    MIRA_CHECK(
        db.text(
            "SELECT count(*) FROM pragma_table_info('memory_mutations') WHERE name='scope_key'") ==
        "1");
    return 0;
}

int read_only_unknown_and_busy_databases() {
    Fixture fixture;
    for (const bool memory : {false, true}) {
        const auto prefix = memory ? "memory-" : "checkpoint-";
        const auto empty = fixture.root / (std::string(prefix) + "empty.db");
        { Database create(empty); }
        auto readonly = fixture.open(memory, empty, true);
        MIRA_CHECK(readonly && readonly.value().read_only);
        {
            Database db(empty);
            MIRA_CHECK(db.text("SELECT count(*) FROM sqlite_master WHERE type='table'") == "0");
        }
        const auto unknown = fixture.root / (std::string(prefix) + "unknown.db");
        {
            Database db(unknown);
            MIRA_CHECK(db.exec("CREATE TABLE foreign_table(value TEXT)"));
        }
        auto rejected = fixture.open(memory, unknown);
        MIRA_CHECK(!rejected && rejected.error().code == ErrorCode::InvalidState);
        {
            Database db(unknown);
            MIRA_CHECK(db.text("SELECT count(*) FROM sqlite_master WHERE name='foreign_table'") ==
                       "1");
        }
        const auto corrupt = fixture.root / (std::string(prefix) + "corrupt.db");
        {
            std::ofstream file(corrupt);
            file << "broken-database";
        }
        MIRA_CHECK(!fixture.open(memory, corrupt));
        {
            std::ifstream file(corrupt);
            std::string content;
            file >> content;
            MIRA_CHECK(content == "broken-database");
        }
        const auto busy = fixture.root / (std::string(prefix) + "busy.db");
        {
            Database db(busy);
            MIRA_CHECK(db.exec("BEGIN EXCLUSIVE"));
            MIRA_CHECK(!fixture.open(memory, busy));
            MIRA_CHECK(db.exec("ROLLBACK"));
        }
        MIRA_CHECK(fixture.open(memory, busy));
    }
    return 0;
}
} // namespace

int main() {
    for (auto test :
         {version_matrix_preserves_metadata, migration_and_version_stamp_roll_back_together,
          read_only_unknown_and_busy_databases}) {
        if (const int status = test(); status != 0) {
            return status;
        }
    }
    return 0;
}
