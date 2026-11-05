#include "sqlite_store_open.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace mira::storage {
namespace {
std::optional<SchemaVersion> parse_version(std::string_view text) {
    const auto dot = text.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto number = [](std::string_view part, std::uint16_t &value) {
        const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
        return result.ec == std::errc{} && result.ptr == part.data() + part.size();
    };
    SchemaVersion result;
    if (!number(text.substr(0, dot), result.major) || !number(text.substr(dot + 1), result.minor)) {
        return std::nullopt;
    }
    return result;
}
} // namespace

Result<OpenedReferenceStore> open_reference_store(const DbOpenOptions &options,
                                                  const StoreSchemaSpec &schema) {
    auto database = open_database(options);
    if (!database) {
        return database.error();
    }
    DatabaseHandle handle(database.value());
    StoreDiagnostics diagnostics;
    diagnostics.reader_schema = schema.version;
    diagnostics.read_only = options.mode == DbOpenMode::ReadOnlyDiagnostic;
    auto tables = list_tables(handle.get());
    if (!tables) {
        return tables.error();
    }
    const bool fresh = tables.value().empty();
    if (!fresh && std::find(tables.value().begin(), tables.value().end(), "store_meta") ==
                      tables.value().end()) {
        return schema.error(ErrorCode::InvalidState,
                            "store file holds an unrecognized schema; refusing to touch it");
    }
    const auto version_text =
        std::to_string(schema.version.major) + "." + std::to_string(schema.version.minor);
    if (fresh) {
        if (diagnostics.read_only) {
            diagnostics.disposition = StoreSchemaDisposition::Created;
            diagnostics.note = "empty database opened read-only; no schema installed";
        } else {
            Transaction transaction(handle.get());
            if (!transaction.valid()) {
                return schema.error(ErrorCode::Internal,
                                    "store initialization transaction rejected");
            }
            if (auto installed = exec_sql(handle.get(), schema.ddl); !installed) {
                return installed.error();
            }
            if (auto kind = meta_write(handle.get(), "store_kind", schema.kind); !kind) {
                return kind.error();
            }
            if (auto version = meta_write(handle.get(), "schema_version", version_text); !version) {
                return version.error();
            }
            if (auto committed = transaction.commit(); !committed) {
                return committed.error();
            }
            diagnostics.disposition = StoreSchemaDisposition::Created;
            diagnostics.file_schema = schema.version;
        }
    } else {
        auto kind = meta_read(handle.get(), "store_kind");
        if (!kind) {
            return kind.error();
        }
        if (!kind.value() || *kind.value() != schema.kind) {
            return schema.error(ErrorCode::InvalidArgument,
                                "store file belongs to a different store kind");
        }
        auto stored = meta_read(handle.get(), "schema_version");
        if (!stored) {
            return stored.error();
        }
        if (!stored.value()) {
            return schema.error(ErrorCode::InvalidState, "store schema version is missing");
        }
        const auto version = parse_version(*stored.value());
        if (!version) {
            return schema.error(ErrorCode::DataLoss, "store schema version is malformed");
        }
        diagnostics.file_schema = *version;
        const bool newer =
            version->major > schema.version.major ||
            (version->major == schema.version.major && version->minor > schema.version.minor);
        if (newer) {
            if (!diagnostics.read_only) {
                return schema.error(
                    ErrorCode::UnsupportedVersion,
                    "store schema is newer than this reader; reopen read_only_diagnostic");
            }
            diagnostics.disposition = StoreSchemaDisposition::ReadOnlyDiagnostic;
            diagnostics.note = "file schema is newer than this reader; writes are rejected";
        } else if (*version == schema.version) {
            diagnostics.disposition = StoreSchemaDisposition::UpToDate;
        } else {
            if (!validate_schema_version(*version, schema.version)) {
                return schema.error(ErrorCode::UnsupportedVersion,
                                    "store schema is too old for this reader");
            }
            if (diagnostics.read_only) {
                return schema.error(ErrorCode::UnsupportedVersion,
                                    "older store schema requires writable migration");
            }
            Transaction transaction(handle.get());
            if (!transaction.valid()) {
                return schema.error(ErrorCode::Internal, "store migration transaction rejected");
            }
            if (schema.migrate) {
                if (auto migrated = schema.migrate(handle.get(), *version); !migrated) {
                    return migrated.error();
                }
            }
            if (auto stamp = meta_write(handle.get(), "schema_version", version_text); !stamp) {
                return stamp.error();
            }
            if (auto committed = transaction.commit(); !committed) {
                return committed.error();
            }
            diagnostics.disposition = StoreSchemaDisposition::Migrated;
        }
    }
    Statement journal(handle.get(), "PRAGMA journal_mode");
    if (journal.valid() && sqlite3_step(journal.get()) == SQLITE_ROW) {
        const auto *mode = reinterpret_cast<const char *>(sqlite3_column_text(journal.get(), 0));
        diagnostics.journal_mode = mode ? mode : "unknown";
    }
    Statement pages(handle.get(), "PRAGMA page_count");
    if (pages.valid() && sqlite3_step(pages.get()) == SQLITE_ROW) {
        diagnostics.page_count = sqlite3_column_int64(pages.get(), 0);
    }
    return OpenedReferenceStore{std::move(handle), std::move(diagnostics)};
}
} // namespace mira::storage
