// Quality gate for the lexical leg of the shipped SqliteMemoryStore (DEC-147).
//
// Drives the hybrid-retrieval benchmark dataset through the store with no
// query vector, so each answer is the lexical leg's alone, and floors its
// Recall@10. Before DEC-147 this run reached 0.100: 18 of the 20 queries came
// back empty, and the benchmark recorded that number as the lexical baseline.

#include "../support/test.hpp"

#include <mira/json.hpp>
#include <mira/sqlite_memory_store.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <kairo/executor.hpp>

namespace {

using namespace mira;

[[nodiscard]] Id128 ordinal_id(std::uint64_t ordinal) {
    Id128::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(ordinal >> ((index % 8) * 8U));
    }
    return Id128{bytes};
}

// Per-kind scope allowlists (design Context/Memory §6.3), as the benchmark
// harness assigns them.
[[nodiscard]] MemoryScope scope_for_kind(MemoryKind kind, std::uint64_t ordinal) {
    MemoryScope scope;
    if (kind == MemoryKind::ApplicationFact || kind == MemoryKind::Procedure) {
        scope.kind = MemoryScopeKind::Application;
        scope.subject_id = "eval-app";
    } else if (kind == MemoryKind::RecoveryLesson) {
        scope.kind = MemoryScopeKind::Agent;
        scope.subject_id = "eval-agent";
    } else {
        scope.kind = MemoryScopeKind::Environment;
        scope.subject_id = "eval-lab";
    }
    scope.tenant_id = "eval-tenant-" + std::to_string(ordinal % 1);
    return scope;
}

[[nodiscard]] MemoryVerification floor_for_kind(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::Preference:
        return MemoryVerification::Unverified;
    case MemoryKind::RecoveryLesson:
        return MemoryVerification::Verified;
    default:
        return MemoryVerification::Observed;
    }
}

struct GoldQuery final {
    std::string id;
    std::string text;
    std::set<std::string> gold;
};

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int lexical_recall_at_10_is_floored() {
    const std::filesystem::path dataset_path = MIRA_LEXICAL_EVAL_DATASET;
    const auto dataset = parse_json(read_file(dataset_path));
    if (!dataset || !dataset.value().is_object()) {
        std::cerr << "lexical eval: dataset is missing or not JSON: " << dataset_path << "\n";
        return 1;
    }
    const auto *records = dataset.value().find("records");
    const auto *queries = dataset.value().find("queries");
    if (records == nullptr || queries == nullptr || !records->is_array() || !queries->is_array()) {
        std::cerr << "lexical eval: dataset has no records or queries\n";
        return 1;
    }

    kairo::Executor executor;
    MIRA_CHECK(executor.initialize(kairo::ExecutorConfig{}));
    std::filesystem::path path;
    {
        std::random_device device;
        path = std::filesystem::temp_directory_path() /
               ("mira-m4-lexical-eval-" + std::to_string(device()) + ".db");
    }
    {
        SqliteMemoryStoreOptions options;
        options.path = path;
        // The gate measures the lexical ranking, not the diversity cap.
        options.max_per_scope_kind = 64;
        auto opened = SqliteMemoryStore::open(executor, options);
        MIRA_CHECK(opened.has_value());
        auto store = std::move(opened).value();

        // dataset id -> record id
        std::map<std::string, MemoryId> seeded;
        std::uint64_t ordinal = 100;
        for (const auto &entry : *records->as_array()) {
            const auto *id = entry.find("id");
            const auto *kind = entry.find("kind");
            const auto *statement = entry.find("statement");
            MIRA_CHECK(id != nullptr && kind != nullptr && statement != nullptr);
            const auto parsed_kind = memory_kind_from(*kind->as_string());
            MIRA_CHECK(parsed_kind.has_value());
            MemoryRecord record;
            record.id = MemoryId{ordinal_id(ordinal)};
            record.scope = scope_for_kind(*parsed_kind, ordinal);
            record.kind = *parsed_kind;
            record.statement = *statement->as_string();
            // Anchored at now: a kind policy adds an expiry at write time.
            record.recorded_at = std::chrono::system_clock::now() +
                                 std::chrono::seconds(static_cast<std::int64_t>(ordinal));
            record.validity.valid_from = record.recorded_at;
            record.provenance = {EventId{ordinal_id(ordinal + 5000)}};
            record.verification = floor_for_kind(*parsed_kind);
            record.confidence = 0.8F;
            MemoryMutation mutation;
            mutation.id = MutationId{ordinal_id(ordinal + 9000)};
            mutation.type = MemoryMutationType::Add;
            mutation.scope = record.scope;
            mutation.proposed = record;
            mutation.evidence = record.provenance;
            const auto applied = store->apply(mutation);
            if (!applied) {
                std::cerr << "lexical eval: seeding " << id->as_string()->c_str()
                          << " failed: " << applied.error().safe_message << "\n";
                return 1;
            }
            seeded.emplace(*id->as_string(), record.id);
            ++ordinal;
        }

        const MemoryScope scopes[] = {scope_for_kind(MemoryKind::EnvironmentFact, 0),
                                      scope_for_kind(MemoryKind::ApplicationFact, 0),
                                      scope_for_kind(MemoryKind::RecoveryLesson, 0)};
        double recall_sum = 0.0;
        std::size_t count = 0;
        for (const auto &entry : *queries->as_array()) {
            const auto *id = entry.find("id");
            const auto *text = entry.find("text");
            const auto *gold = entry.find("gold");
            MIRA_CHECK(id != nullptr && text != nullptr && gold != nullptr && gold->is_array());
            MemoryQuery query;
            query.scopes = {std::begin(scopes), std::end(scopes)};
            query.text = *text->as_string();
            const auto found = store->query(query);
            MIRA_CHECK(found.has_value());
            MIRA_CHECK(found.value().quality.fts_leg_ran);
            std::set<std::string> top;
            for (std::size_t index = 0; index < found.value().records.size() && index < 10;
                 ++index) {
                for (const auto &[dataset_id, record_id] : seeded) {
                    if (record_id == found.value().records[index].id) {
                        top.insert(dataset_id);
                        break;
                    }
                }
            }
            std::size_t hit = 0;
            for (const auto &member : *gold->as_array()) {
                hit += static_cast<std::size_t>(top.count(*member.as_string()) != 0 ? 1 : 0);
            }
            recall_sum += static_cast<double>(hit) / static_cast<double>(gold->as_array()->size());
            ++count;
        }
        const double recall = recall_sum / static_cast<double>(count);
        std::cout << "lexical R@10 = " << recall << " over " << count << " queries\n";
        MIRA_CHECK(count >= 20);
        MIRA_CHECK(recall >= 0.90);
        MIRA_CHECK(store->close().has_value());
    }
    MIRA_CHECK(executor.shutdown(true) == kairo::ShutdownResult::Completed);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return 0;
}

} // namespace

int main() { return lexical_recall_at_10_is_floored(); }
