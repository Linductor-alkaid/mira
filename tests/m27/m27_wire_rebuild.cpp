// M27 (DEC-041 first stage) world-state IVA gate matrix - WS-G3 (rebuild and
// cross-instance determinism over the frozen dataset) and WS-G6 (the
// mira.worldstate.v1 wire schema and the mira.world_state error domain).
// Expectations come from milestone plan §4 (docs/plans/
// m27-world-state-projection-core.md); the case table lives in
// m27_world_state_test.cpp.

#include "m27_cases.hpp"

#include <mira/json.hpp>

#include <executor/executor.hpp>

#include <algorithm>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mira::m27 {
namespace {

[[nodiscard]] std::size_t count_kind(const WorldState &state, WorldChangeKind kind) {
    std::size_t seen = 0;
    for (const auto &change : state.recent_changes) {
        if (change.kind == kind) {
            ++seen;
        }
    }
    return seen;
}

[[nodiscard]] JsonValue::Object cloned_object(const JsonValue &value) { return *value.as_object(); }

// Rewrites the member `key` of `object` through `transform`. A missing member
// is a structural bug of the test itself, not a contract outcome - abort.
template <typename Transform>
void rewrite_member(JsonValue::Object &object, const char *key, Transform transform) {
    for (auto &member : object) {
        if (member.first == key) {
            transform(member.second);
            return;
        }
    }
    std::cerr << "m27: wire rewrite: member '" << key << "' not found\n";
    std::abort();
}

// Parses `text`, hands the root object copy to `mutate`, and returns the
// canonical re-serialization (the wire reader's fail-closed face then runs on
// the mutated payload).
template <typename Mutate> std::string mutated_wire(const std::string &text, Mutate mutate) {
    const auto doc = parse_json(text);
    if (!doc.has_value()) {
        std::cerr << "m27: wire rewrite: the source payload failed to parse\n";
        std::abort();
    }
    JsonValue::Object root = cloned_object(doc.value());
    mutate(root);
    return canonical_json_string(JsonValue(std::move(root)));
}

[[nodiscard]] std::vector<std::string> sorted_members(const JsonValue &value) {
    std::vector<std::string> names;
    for (const auto &member : *value.as_object()) {
        names.push_back(member.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

int expect_members(const JsonValue &value, const std::vector<std::string_view> &expected,
                   const char *what) {
    const auto names = sorted_members(value);
    if (names.size() != expected.size()) {
        std::cerr << "m27: " << what << ": member count " << names.size()
                  << " != " << expected.size() << '\n';
        return 1;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (names[index] != expected[index]) {
            std::cerr << "m27: " << what << ": member[" << index << "] '" << names[index]
                      << "' != '" << expected[index] << "'\n";
            return 1;
        }
    }
    return 0;
}

[[nodiscard]] std::string populated_wire_text() {
    const auto state = rebuild_world_state(frozen_dataset(), dataset_options());
    return state.value().to_json();
}

} // namespace

// ---------------------------------------------------------------------------
// WS-G3: the frozen dataset digest anchor (pinned regression constant; covers
// all six input kinds over the conflict, expiry and eviction paths).
// ---------------------------------------------------------------------------

int frozen_dataset_digest_anchor() {
    const auto dataset = frozen_dataset();
    const auto options = dataset_options();
    const auto rebuilt = rebuild_world_state(dataset, options);
    MIRA_CHECK(expect_ok(rebuilt, "dataset rebuild") == 0);
    const auto &state = rebuilt.value();

    const std::string actual = state.digest().to_string();
    if (actual != kFrozenDatasetDigestHex) {
        std::cerr << "m27: dataset digest drifted; pin kFrozenDatasetDigestHex to actual=" << actual
                  << '\n';
        return 1;
    }

    // Structural face of the dataset projection (freeze-level expectations).
    MIRA_CHECK(state.validate().has_value());
    MIRA_CHECK(state.entities.size() == 2);
    for (const auto &entity : state.entities) {
        MIRA_CHECK(entity.stale);
        MIRA_CHECK(entity.confidence == 0.5);
    }
    MIRA_CHECK(state.foreground_app.status == WorldBeliefStatus::Believed);
    MIRA_CHECK(state.foreground_app.package_name == "com.example.launcher");
    MIRA_CHECK(state.current_page.status == WorldBeliefStatus::Stale);
    MIRA_CHECK(state.current_page.confidence == 0.95 / 2);
    MIRA_CHECK(state.recent_changes.size() == 12);
    MIRA_CHECK(count_kind(state, WorldChangeKind::ConflictMarked) == 1);
    MIRA_CHECK(count_kind(state, WorldChangeKind::EntityEvicted) == 1);
    MIRA_CHECK(count_kind(state, WorldChangeKind::EntityStaled) == 3);
    MIRA_CHECK(count_kind(state, WorldChangeKind::NavigationObserved) == 1);
    MIRA_CHECK(count_kind(state, WorldChangeKind::ToolSettled) == 1);
    MIRA_CHECK(count_kind(state, WorldChangeKind::VerificationSettled) == 1);
    MIRA_CHECK(count_kind(state, WorldChangeKind::PageAssumed) == 2);
    MIRA_CHECK(count_kind(state, WorldChangeKind::EntityObserved) == 2);
    for (std::size_t index = 1; index < state.recent_changes.size(); ++index) {
        MIRA_CHECK(state.recent_changes[index].change_sequence >
                   state.recent_changes[index - 1].change_sequence);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G3: rebuild equals the incremental advance under the same options.
// ---------------------------------------------------------------------------

int rebuild_equals_incremental_advance() {
    const auto dataset = frozen_dataset();
    const auto options = dataset_options();

    const auto rebuilt = rebuild_world_state(dataset, options);
    MIRA_CHECK(expect_ok(rebuilt, "rebuild") == 0);
    const auto incremental = fold_all(dataset, options);
    MIRA_CHECK(expect_ok(incremental, "incremental") == 0);
    MIRA_CHECK(rebuilt.value().digest() == incremental.value().digest());
    MIRA_CHECK(rebuilt.value().to_json() == incremental.value().to_json());
    MIRA_CHECK(rebuilt.value().validate().has_value());
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G3: input prefixes rebuild to the incremental projection at that prefix
// (prefix truncation replay equivalence).
// ---------------------------------------------------------------------------

int prefix_replay_equivalence() {
    const auto dataset = frozen_dataset();
    const auto options = dataset_options();

    WorldState advanced;
    std::size_t index = 0;
    for (const auto &record : dataset) {
        auto applied = advance(std::move(advanced), record, options);
        MIRA_CHECK(expect_ok(applied, "incremental step") == 0);
        advanced = std::move(applied).value();
        ++index;
        if (index != 1 && index != 2 && index != 3 && index != 5 && index != dataset.size()) {
            continue;
        }
        const auto rebuilt =
            rebuild_world_state(std::span<const WorldStateInput>(dataset.data(), index), options);
        MIRA_CHECK(expect_ok(rebuilt, "prefix rebuild") == 0);
        MIRA_CHECK(rebuilt.value().digest() == advanced.digest());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G3: prune is the named eviction-order implementation - a no-op within
// capacity, and a caller-side convenience outside the rebuild recipe (its
// output participates in no WS-G3 equivalence assertion).
// ---------------------------------------------------------------------------

int prune_within_capacity_noop_and_outside_recipe() {
    const auto options = dataset_options();
    const auto dataset = frozen_dataset();
    auto rebuilt = rebuild_world_state(dataset, options);
    MIRA_CHECK(expect_ok(rebuilt, "rebuild") == 0);

    // Within capacity: total no-op.
    const auto pruned = prune_entities(rebuilt.value(), options);
    MIRA_CHECK(pruned.digest() == rebuilt.value().digest());
    MIRA_CHECK(pruned.recent_changes.size() == rebuilt.value().recent_changes.size());

    // Over capacity: evicts down to the frozen order with EntityEvicted
    // entries; the result is an audit/convenience projection and is
    // intentionally NOT rebuild-equal to anything (non-recipe semantics).
    WorldState bloated = rebuilt.value();
    bloated.entities.push_back(make_entity(8, false, 1.0, 50));
    bloated.entities.push_back(make_entity(9, false, 1.0, 51));
    bloated.entities.push_back(make_entity(10, true, 1.0, 52));
    const auto over = prune_entities(std::move(bloated), options);
    MIRA_CHECK(over.entities.size() == options.max_entities);
    // 3 prune evictions on top of the 1 insert-overflow eviction the dataset
    // rebuild already carries in its ring.
    MIRA_CHECK(count_kind(over, WorldChangeKind::EntityEvicted) == 4);
    MIRA_CHECK(over.digest() != rebuilt.value().digest());
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G3: the full pipeline replays byte-identically on executor workers and
// matches the in-process anchor (M16-M20 digest-anchoring convention; §4.6
// harness discipline: submit_auto, futures consumed, shutdown from the
// non-worker thread after the futures settle).
// ---------------------------------------------------------------------------

int cross_thread_pipeline_replay_byte_identical() {
    const auto dataset = frozen_dataset();
    const auto options = dataset_options();

    executor::Executor executor;
    MIRA_CHECK(executor.initialize(executor::ExecutorConfig{}));
    auto rebuild_future = executor.submit_auto(
        [&dataset, &options] { return rebuild_world_state(dataset, options); });
    auto fold_future =
        executor.submit_auto([&dataset, &options] { return fold_all(dataset, options); });

    const auto rebuilt = rebuild_future.get();
    MIRA_CHECK(expect_ok(rebuilt, "worker rebuild") == 0);
    const auto folded = fold_future.get();
    MIRA_CHECK(expect_ok(folded, "worker fold") == 0);

    MIRA_CHECK(rebuilt.value().digest() == folded.value().digest());
    MIRA_CHECK(rebuilt.value().digest().to_string() == kFrozenDatasetDigestHex);
    MIRA_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G6: canonical JSON round trip over the populated projection.
// ---------------------------------------------------------------------------

int wire_round_trip_byte_identical() {
    const auto state = rebuild_world_state(frozen_dataset(), dataset_options());
    MIRA_CHECK(expect_ok(state, "populated projection") == 0);
    MIRA_CHECK(!state.value().entities.empty());
    MIRA_CHECK(!state.value().recent_changes.empty());

    const std::string text = state.value().to_json();
    const auto round = WorldState::from_json(text);
    MIRA_CHECK(expect_ok(round, "wire read") == 0);
    MIRA_CHECK(round.value().to_json() == text);
    MIRA_CHECK(round.value().digest() == state.value().digest());

    // The serialized form is canonical: re-serializing the parsed document
    // reproduces the exact bytes.
    const auto doc = parse_json(text);
    MIRA_CHECK(expect_ok(doc, "wire parse") == 0);
    MIRA_CHECK(canonical_json_string(doc.value()) == text);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G6: DEC-002 version policy ({1,x} reads; {0,x}/{2,x}+ rejected).
// ---------------------------------------------------------------------------

int wire_version_policy_dec002() {
    const auto set_version = [](JsonValue::Object &root, std::int64_t major, std::int64_t minor) {
        rewrite_member(root, "schema_version", [major, minor](JsonValue &member) {
            JsonValue::Object version;
            version.emplace_back("major", JsonValue(major));
            version.emplace_back("minor", JsonValue(minor));
            member = JsonValue(std::move(version));
        });
    };

    const std::string text = populated_wire_text();

    // {1,1} and {1,0xffff} read.
    for (const std::int64_t minor : {std::int64_t{1}, std::int64_t{0xFFFF}}) {
        const auto read = WorldState::from_json(
            mutated_wire(text, [&](JsonValue::Object &root) { set_version(root, 1, minor); }));
        MIRA_CHECK(expect_ok(read, "supported version") == 0);
        MIRA_CHECK(
            (read.value().schema_version == SchemaVersion{1, static_cast<std::uint16_t>(minor)}));
    }

    // {0,x} and {2,x}+ -> SchemaUnsupported.
    for (const std::int64_t major : {std::int64_t{0}, std::int64_t{2}, std::int64_t{3}}) {
        const auto rejected = WorldState::from_json(
            mutated_wire(text, [&](JsonValue::Object &root) { set_version(root, major, 0); }));
        MIRA_CHECK(expect_error(rejected, WorldStateDomainCode::SchemaUnsupported,
                                "unsupported major") == 0);
    }

    // A structurally legal but out-of-range minor is a value failure.
    const auto huge_minor = WorldState::from_json(
        mutated_wire(text, [&](JsonValue::Object &root) { set_version(root, 1, 0x10000); }));
    MIRA_CHECK(expect_error(huge_minor, WorldStateDomainCode::StateInvalid,
                            "minor beyond SchemaVersion") == 0);

    // Broken version members are malformed payloads.
    const auto missing_minor =
        WorldState::from_json(mutated_wire(text, [](JsonValue::Object &root) {
            rewrite_member(root, "schema_version", [](JsonValue &member) {
                JsonValue::Object version = cloned_object(member);
                const auto removed =
                    std::remove_if(version.begin(), version.end(),
                                   [](const auto &entry) { return entry.first == "minor"; });
                version.erase(removed, version.end());
                member = JsonValue(std::move(version));
            });
        }));
    MIRA_CHECK(
        expect_error(missing_minor, WorldStateDomainCode::PayloadMalformed, "missing minor") == 0);

    const auto string_major = WorldState::from_json(mutated_wire(text, [](JsonValue::Object &root) {
        rewrite_member(root, "schema_version", [](JsonValue &member) {
            JsonValue::Object version = cloned_object(member);
            rewrite_member(version, "major",
                           [](JsonValue &value) { value = JsonValue(std::string("1")); });
            member = JsonValue(std::move(version));
        });
    }));
    MIRA_CHECK(expect_error(string_major, WorldStateDomainCode::PayloadMalformed, "string major") ==
               0);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G6: the wire reader fails closed - unknown/missing/wrong-typed members
// are PayloadMalformed; illegal values (including vocabulary-class values and
// change_sequence regression) fold to StateInvalid; VocabularyViolation is
// never produced on the reader path.
// ---------------------------------------------------------------------------

int wire_fail_closed_reader() {
    const std::string text = populated_wire_text();

    // Not JSON at all.
    MIRA_CHECK(expect_error(WorldState::from_json("{definitely not json"),
                            WorldStateDomainCode::PayloadMalformed, "parse failure") == 0);

    // Unknown root member.
    MIRA_CHECK(expect_error(WorldState::from_json(mutated_wire(text,
                                                               [](JsonValue::Object &root) {
                                                                   root.emplace_back(
                                                                       "extra",
                                                                       JsonValue(std::int64_t{1}));
                                                               })),
                            WorldStateDomainCode::PayloadMalformed, "unknown member") == 0);

    // Missing root member.
    MIRA_CHECK(expect_error(WorldState::from_json(mutated_wire(
                                text,
                                [](JsonValue::Object &root) {
                                    const auto removed = std::remove_if(
                                        root.begin(), root.end(), [](const auto &entry) {
                                            return entry.first == "entities";
                                        });
                                    root.erase(removed, root.end());
                                })),
                            WorldStateDomainCode::PayloadMalformed, "missing member") == 0);

    // Unknown nested member (foreground_app).
    MIRA_CHECK(expect_error(WorldState::from_json(mutated_wire(
                                text,
                                [](JsonValue::Object &root) {
                                    rewrite_member(root, "foreground_app", [](JsonValue &member) {
                                        JsonValue::Object foreground = cloned_object(member);
                                        foreground.emplace_back("capabilities",
                                                                JsonValue(std::int64_t{1}));
                                        member = JsonValue(std::move(foreground));
                                    });
                                })),
                            WorldStateDomainCode::PayloadMalformed, "unknown nested member") == 0);

    // Wrong JSON type (status as number).
    MIRA_CHECK(expect_error(WorldState::from_json(mutated_wire(
                                text,
                                [](JsonValue::Object &root) {
                                    rewrite_member(root, "foreground_app", [](JsonValue &member) {
                                        JsonValue::Object foreground = cloned_object(member);
                                        rewrite_member(foreground, "status", [](JsonValue &value) {
                                            value = JsonValue(std::int64_t{5});
                                        });
                                        member = JsonValue(std::move(foreground));
                                    });
                                })),
                            WorldStateDomainCode::PayloadMalformed, "wrong member type") == 0);

    // Illegal belief name -> StateInvalid (value layer).
    MIRA_CHECK(expect_error(WorldState::from_json(mutated_wire(
                                text,
                                [](JsonValue::Object &root) {
                                    rewrite_member(root, "foreground_app", [](JsonValue &member) {
                                        JsonValue::Object foreground = cloned_object(member);
                                        rewrite_member(foreground, "status", [](JsonValue &value) {
                                            value = JsonValue(std::string("believe"));
                                        });
                                        member = JsonValue(std::move(foreground));
                                    });
                                })),
                            WorldStateDomainCode::StateInvalid, "belief name outside set") == 0);

    // change_sequence regression -> StateInvalid.
    MIRA_CHECK(expect_error(WorldState::from_json(mutated_wire(
                                text,
                                [](JsonValue::Object &root) {
                                    rewrite_member(root, "recent_changes", [](JsonValue &member) {
                                        JsonValue::Array changes = *member.as_array();
                                        const auto set_sequence = [&changes](std::size_t index,
                                                                             std::int64_t value) {
                                            JsonValue::Object change =
                                                cloned_object(changes[index]);
                                            rewrite_member(change, "change_sequence",
                                                           [value](JsonValue &field) {
                                                               field = JsonValue(value);
                                                           });
                                            changes[index] = JsonValue(std::move(change));
                                        };
                                        set_sequence(0, 90);
                                        set_sequence(1, 40);
                                        member = JsonValue(std::move(changes));
                                    });
                                })),
                            WorldStateDomainCode::StateInvalid, "change_sequence regression") == 0);

    // Negative change_sequence -> StateInvalid (value layer).
    MIRA_CHECK(expect_error(WorldState::from_json(mutated_wire(
                                text,
                                [](JsonValue::Object &root) {
                                    rewrite_member(root, "recent_changes", [](JsonValue &member) {
                                        JsonValue::Array changes = *member.as_array();
                                        JsonValue::Object change = cloned_object(changes.front());
                                        rewrite_member(change, "change_sequence",
                                                       [](JsonValue &field) {
                                                           field = JsonValue(std::int64_t{-1});
                                                       });
                                        changes.front() = JsonValue(std::move(change));
                                        member = JsonValue(std::move(changes));
                                    });
                                })),
                            WorldStateDomainCode::StateInvalid, "negative change_sequence") == 0);

    // Entity with a nil observation_id: vocabulary-class value illegality
    // folds to StateInvalid on the reader path (never VocabularyViolation).
    const auto nil_oid = WorldState::from_json(mutated_wire(text, [](JsonValue::Object &root) {
        rewrite_member(root, "entities", [](JsonValue &member) {
            JsonValue::Array entities = *member.as_array();
            JsonValue::Object entity = cloned_object(entities.front());
            rewrite_member(entity, "element", [](JsonValue &element) {
                JsonValue::Object fields = cloned_object(element);
                rewrite_member(fields, "observation_id",
                               [](JsonValue &value) { value = JsonValue(std::string(32, '0')); });
                element = JsonValue(std::move(fields));
            });
            entities.front() = JsonValue(std::move(entity));
            member = JsonValue(std::move(entities));
        });
    }));
    MIRA_CHECK(expect_error(nil_oid, WorldStateDomainCode::StateInvalid,
                            "nil observation_id folds to StateInvalid") == 0);
    MIRA_CHECK(nil_oid.error().domain_code !=
               static_cast<std::int32_t>(WorldStateDomainCode::VocabularyViolation));
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G6: the wire key set is exactly the §4.4 frozen list (and only that).
// ---------------------------------------------------------------------------

int wire_key_sets_exactly_frozen() {
    const std::string text = populated_wire_text();
    const auto doc = parse_json(text);
    MIRA_CHECK(expect_ok(doc, "wire parse") == 0);
    const JsonValue &root = doc.value();

    MIRA_CHECK(expect_members(root,
                              {"current_page", "entities", "foreground_app", "recent_changes",
                               "schema_version"},
                              "root key set") == 0);
    MIRA_CHECK(expect_members(*root.find("schema_version"), {"major", "minor"},
                              "schema_version key set") == 0);
    MIRA_CHECK(
        expect_members(*root.find("foreground_app"),
                       {"activity_name", "observed_at", "package_name", "sensitive", "status"},
                       "foreground_app key set") == 0);
    MIRA_CHECK(expect_members(*root.find("current_page"),
                              {"app_id", "confidence", "recognized_at", "state_id", "status"},
                              "current_page key set") == 0);

    const JsonValue::Array *entities = root.find("entities")->as_array();
    MIRA_CHECK(entities != nullptr && !entities->empty());
    MIRA_CHECK(
        expect_members(entities->front(),
                       {"confidence", "element", "last_seen_at", "source_event_digest", "stale"},
                       "entity key set") == 0);
    const JsonValue *element = entities->front().find("element");
    MIRA_CHECK(element != nullptr);
    MIRA_CHECK(expect_members(*element,
                              {"bounds", "environment_epoch", "evidence_digest", "observation_id",
                               "source", "space", "stable_hint"},
                              "element key set") == 0);
    MIRA_CHECK(expect_members(*element->find("bounds"), {"bottom", "left", "right", "top"},
                              "bounds key set") == 0);

    const JsonValue::Array *changes = root.find("recent_changes")->as_array();
    MIRA_CHECK(changes != nullptr && !changes->empty());
    MIRA_CHECK(
        expect_members(changes->front(),
                       {"change_sequence", "changed_at", "kind", "source_event_digest", "summary"},
                       "change entry key set") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// WS-G6: all eight error codes - stable name, int32 value, deterministic
// ErrorCode assignment, and a reachable trigger per code.
// ---------------------------------------------------------------------------

int error_domain_eight_codes_triple_reachable() {
    // Triple mapping over the frozen table.
    for (const auto &entry : kDomainCodes) {
        MIRA_CHECK(world_state_domain_code_name(entry.code) == entry.name);
        const Error error = make_world_state_error(entry.code, "m27 probe");
        MIRA_CHECK(error.domain == kDomainName);
        MIRA_CHECK(error.domain_code == entry.value);
        MIRA_CHECK(error.code == entry.mapped);
        MIRA_CHECK(!error.retryable);
        MIRA_CHECK(error.safe_message == "m27 probe");
    }

    const WorldStateOptions options;
    const WorldState empty;

    // OptionsInvalid
    WorldStateOptions zeroed;
    zeroed.max_entities = 0;
    MIRA_CHECK(expect_error(zeroed.validate(), WorldStateDomainCode::OptionsInvalid,
                            "OptionsInvalid") == 0);
    // StateInvalid (change_sequence regression)
    WorldState regression;
    regression.recent_changes.push_back(WorldChange{});
    regression.recent_changes.back().change_sequence = 5;
    regression.recent_changes.push_back(WorldChange{});
    regression.recent_changes.back().change_sequence = 5;
    MIRA_CHECK(expect_error(regression.validate(), WorldStateDomainCode::StateInvalid,
                            "StateInvalid") == 0);
    // RecordInvalid (input record structurally illegal)
    MIRA_CHECK(expect_error(apply_tool_settled(empty, make_tool("toolref:a.b", "nope", 1), options),
                            WorldStateDomainCode::RecordInvalid, "RecordInvalid") == 0);
    // VocabularyViolation (operator input path only)
    MIRA_CHECK(expect_error(apply_screen_state(empty, make_screen("app.A", "", 0.5, 1), options),
                            WorldStateDomainCode::VocabularyViolation, "VocabularyViolation") == 0);
    // StaleTargetMissing
    MIRA_CHECK(
        expect_error(apply_verification(empty, make_verification("confirmed", 0.9, 1), options),
                     WorldStateDomainCode::StaleTargetMissing, "StaleTargetMissing") == 0);
    // PayloadTooLarge (rejected, never truncated)
    MIRA_CHECK(expect_error(
                   apply_world_observation(
                       empty, make_observation(0x71, make_app(std::string(2100, 'x'), "a"), {}, 1),
                       options),
                   WorldStateDomainCode::PayloadTooLarge, "PayloadTooLarge") == 0);
    // SchemaUnsupported / PayloadMalformed
    MIRA_CHECK(expect_error(WorldState::from_json("{\"schema_version\":{\"major\":2,\"minor\":0},"
                                                  "\"foreground_app\":{},\"current_page\":{},"
                                                  "\"entities\":[],\"recent_changes\":[]}"),
                            WorldStateDomainCode::SchemaUnsupported, "SchemaUnsupported") == 0);
    MIRA_CHECK(expect_error(WorldState::from_json("{"), WorldStateDomainCode::PayloadMalformed,
                            "PayloadMalformed") == 0);
    return 0;
}

} // namespace mira::m27
