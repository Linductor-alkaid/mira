#include <kairo/executor.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>

using namespace mira;
namespace {
unsigned checks = 0;
void check(bool good, const std::source_location at = std::source_location::current()) {
    ++checks;
    if (!good)
        throw std::runtime_error("capacity check failed at " + std::to_string(at.line()));
}
#define CAP_CHECK(value) check(static_cast<bool>(value))
template <class T> T take(Result<T> r) {
    if (!r)
        throw std::runtime_error("capacity fixture failed: " + r.error().safe_message);
    CAP_CHECK(r);
    return std::move(r.value());
}
struct ExecutorOwner {
    kairo::Executor value;
    ExecutorOwner() { CAP_CHECK(value.initialize(kairo::ExecutorConfig{})); }
    ~ExecutorOwner() { CAP_CHECK(value.shutdown(true) == kairo::ShutdownResult::Completed); }
};
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("mira-memory-cap-" + Id128::generate().to_string());
    Directory() { std::filesystem::create_directory(path); }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
MemoryMutation mutation(const MemoryScope &scope, std::size_t n) {
    MemoryRecord record;
    record.id = MemoryId::generate();
    record.scope = scope;
    record.kind = MemoryKind::Episode;
    record.verification = MemoryVerification::Verified;
    record.confidence = 1.0F;
    record.statement = "capacity record " + std::to_string(n) + " " + std::string(8192, 'x');
    record.recorded_at = std::chrono::system_clock::now();
    record.validity.valid_from = record.recorded_at;
    record.provenance = {EventId::generate()};
    MemoryMutation m;
    m.id = MutationId::generate();
    m.type = MemoryMutationType::Add;
    m.scope = scope;
    m.proposed = record;
    m.evidence = record.provenance;
    return m;
}
void capacity_and_recovery() {
    ExecutorOwner executor;
    Directory dir;
    SqliteMemoryStoreOptions options;
    options.path = dir.path / "." / "memory.db"; // Native VFS normalizes this spelling.
    options.max_storage_bytes = 1024 * 1024;
    options.capacity_reserve_bytes = 256 * 1024;
    auto store = take(SqliteMemoryStore::open(executor.value, options));
    const MemoryScope scope{MemoryScopeKind::Session, "capacity-test", std::nullopt};
    const auto first = mutation(scope, 0);
    take(store->apply(first));
    bool rejected = false;
    for (std::size_t n = 1; n < 1000; ++n) {
        const auto result = store->apply(mutation(scope, n));
        const auto snap = take(store->capacity_snapshot());
        std::uint64_t measured = 0;
        for (const auto &entry : std::filesystem::directory_iterator(dir.path))
            if (entry.is_regular_file())
                measured += entry.file_size();
        CAP_CHECK(measured <= options.max_storage_bytes);
        CAP_CHECK(measured == snap.file_bytes);
        CAP_CHECK(snap.file_bytes <= options.max_storage_bytes);
        CAP_CHECK(snap.peak_admitted_file_bytes <= options.max_storage_bytes);
        if (!result) {
            CAP_CHECK(result.error().code == ErrorCode::ResourceExhausted);
            CAP_CHECK(result.error().domain == "mira.state_store.capacity");
            rejected = true;
            break;
        }
    }
    CAP_CHECK(rejected);
    CAP_CHECK(take(store->get(first.proposed.id, scope)));
    CAP_CHECK(take(store->apply(first)).idempotent_replay);
    auto second = SqliteMemoryStore::open(executor.value, options);
    CAP_CHECK(!second); // Exclusive owner never silently shares admission.
    CAP_CHECK(store->close());
    CAP_CHECK(!store->capacity_snapshot());
    store.reset();
    store = take(SqliteMemoryStore::open(executor.value, options));
    CAP_CHECK(take(store->get(first.proposed.id, scope)));
    CAP_CHECK(take(store->apply(first)).idempotent_replay);
    ErasureRequest erase;
    erase.scope = scope;
    erase.reason = "capacity fixture erasure";
    erase.include_artifacts = false;
    const auto erased = store->erase(erase);
    CAP_CHECK(erased || erased.error().code == ErrorCode::ResourceExhausted);
    CAP_CHECK(take(store->capacity_snapshot()).file_bytes <= options.max_storage_bytes);
    CAP_CHECK(store->close());
    store.reset();
    options.max_storage_bytes = 512 * 1024; // Rejected as an invalid budget, never truncates.
    CAP_CHECK(!SqliteMemoryStore::open(executor.value, options));
    options.max_storage_bytes = 1024 * 1024;
    store = take(SqliteMemoryStore::open(executor.value, options));
    CAP_CHECK(store->close());
    store.reset();
    std::filesystem::resize_file(options.path, 2 * 1024 * 1024);
    CAP_CHECK(!SqliteMemoryStore::open(executor.value, options));
    CAP_CHECK(std::filesystem::file_size(options.path) == 2 * 1024 * 1024);
}
} // namespace
int main() {
    try {
        capacity_and_recovery();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "[mira_m31_memory_capacity_test] " << checks << " checks, 0 failures\n";
    return 0;
}
