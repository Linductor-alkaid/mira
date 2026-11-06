#pragma once

#include "sqlite_support.hpp"
#include <mira/state_store.hpp>

namespace mira::storage {

struct StoreSchemaSpec final {
    const char *kind;
    SchemaVersion version;
    // DDL and migration callbacks execute inside the shared transaction;
    // neither may BEGIN/COMMIT independently.
    const char *ddl;
    Error (*error)(ErrorCode, std::string);
    Result<void> (*migrate)(sqlite3 *, SchemaVersion) = nullptr;
};

struct OpenedReferenceStore final {
    DatabaseHandle database;
    StoreDiagnostics diagnostics;
};

[[nodiscard]] Result<OpenedReferenceStore> open_reference_store(const DbOpenOptions &options,
                                                                const StoreSchemaSpec &schema);

} // namespace mira::storage
