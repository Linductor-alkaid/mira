#pragma once

#include <mira/sqlite_memory_store.hpp>

namespace mira::storage {
// DEC-111: lifetime encloses the SQLite connection. Initial open is serial;
// after opening, all access is on the existing StoreChannel worker.
class MemoryCapacityVfs final {
  public:
    static Result<std::unique_ptr<MemoryCapacityVfs>> create(const SqliteMemoryStoreOptions &);
    ~MemoryCapacityVfs();
    const char *name() const;
    Result<SqliteMemoryCapacitySnapshot> snapshot() const;
    void reset_rejection();
    void set_operation_limit(std::uint64_t);
    bool rejected() const;

  private:
    class Impl;
    explicit MemoryCapacityVfs(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
Error memory_capacity_error();
} // namespace mira::storage
