// M17 (DEC-032 Stage B) retrieval eval harness. Runs the frozen synthetic
// retrieval dataset (milestone §4) against InMemoryContextIndex with the
// deterministic token-hash embedding supplier, evaluates the pre-frozen gates
// R1-R4 and prints a JSON report (stdout, plus argv[1] when given).
//
// The embedding supplier is a deterministic function, not a model: metrics
// measure pipeline behaviour (recall path, degradation, ACL, determinism),
// not semantic quality (RULE-10).

#include <mira/context_retrieval.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace mira;

// ---------------------------------------------------------------------------
// Frozen configuration (milestone §4; the dataset digest is taken over this)
// ---------------------------------------------------------------------------

struct FrozenConfig final {
    std::uint64_t seed = 0x4d49'5231'3753'5442ULL; // "MIR17STB"
    std::size_t queries_per_kind = 16;             // 48 anchored queries
    std::size_t distractors_per_query = 8;         // 384 in-domain distractors
    std::size_t top_k = 10;                        // R1/R2 metric cutoff
    std::size_t embedding_dims = 64;
    double recall_gate = 1.0; // R1
    double mrr_gate = 0.90;   // R2
    std::uint64_t token_budget = 4'096;
};

[[nodiscard]] std::string digest_hex(const Sha256Digest &digest) {
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    for (const auto byte : digest.bytes) {
        std::ostringstream slot;
        slot << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
        text += slot.str();
    }
    return text;
}

// splitmix64: the deterministic driver of the whole dataset.
struct SplitMix64 final {
    std::uint64_t state = 0;
    [[nodiscard]] std::uint64_t next() {
        state += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t mixed = state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return mixed ^ (mixed >> 31U);
    }
};

[[nodiscard]] Id128 id_from(SplitMix64 &rng, std::string_view tag) {
    const std::uint64_t first = rng.next();
    const std::uint64_t second = rng.next();
    Id128::Bytes bytes{};
    std::memcpy(bytes.data(), &first, sizeof(first));
    std::memcpy(bytes.data() + sizeof(first), &second, sizeof(second));
    for (std::size_t index = 0; index < tag.size() && index < bytes.size(); ++index) {
        bytes[index] ^= static_cast<std::uint8_t>(tag[index]);
    }
    return Id128{bytes};
}

// Rare token vocabulary: short deterministic words with enough entropy that
// in-domain distractors never collide with an anchored pair.
[[nodiscard]] std::string rare_token(SplitMix64 &rng) {
    static constexpr std::string_view kAlphabet = "abcdefghjkmnpqrstuvwxyz23456789";
    std::string token(5, 'a');
    for (auto &character : token) {
        character = kAlphabet[static_cast<std::size_t>(rng.next() % kAlphabet.size())];
    }
    return token;
}

// Deterministic token-hash embedding supplier (same construction as the test
// double; FNV-1a into `dims` signed slots, L2-normalized).
class HashingEmbedder final : public IContextEmbedder {
  public:
    HashingEmbedder(ModelProfileId profile, std::size_t dims) : profile_(profile), dims_(dims) {}

    Result<ContextEmbedding> embed(const ContextEmbeddingInput &input) override {
        ContextEmbedding embedding;
        embedding.profile_id = profile_;
        embedding.values.assign(dims_, 0.0F);
        std::string token;
        const auto flush = [&]() {
            if (token.empty()) {
                return;
            }
            std::uint32_t hash = 2'166'136'261U;
            for (const char character : token) {
                hash ^= static_cast<std::uint8_t>(character);
                hash *= 16'777'619U;
            }
            embedding.values[hash % dims_] += (hash & 0x8000U) != 0U ? -1.0F : 1.0F;
            token.clear();
        };
        for (const char character : input.text) {
            const bool word = (character >= 'a' && character <= 'z') ||
                              (character >= '0' && character <= '9') || character == '_';
            if (word) {
                token += character;
            } else {
                flush();
            }
        }
        flush();
        double norm = 0.0;
        for (const float value : embedding.values) {
            norm += static_cast<double>(value) * static_cast<double>(value);
        }
        if (norm > 0.0) {
            const double scale = 1.0 / std::sqrt(norm);
            for (auto &value : embedding.values) {
                value = static_cast<float>(static_cast<double>(value) * scale);
            }
        }
        return embedding;
    }

  private:
    ModelProfileId profile_;
    std::size_t dims_;
};

struct EvalAsset final {
    ContextIndexAsset asset;
    bool anchored = false;
    std::size_t query_index = 0; // owning query for anchored assets
};

struct EvalQuery final {
    ContextQuery query;
    ContextAssetId anchored_asset;
};

struct Dataset final {
    std::vector<EvalAsset> assets;
    std::vector<EvalQuery> queries;
    std::string digest_hex_text;
};

[[nodiscard]] ContextIndexAsset make_segment(const SessionId &session, const ContextAssetId &id,
                                             const std::string &text, std::uint64_t sequence) {
    ContextIndexAsset asset;
    asset.id = id;
    asset.kind = ContextAssetKind::ConversationSegment;
    asset.text = "user: " + text;
    asset.session = session;
    asset.through_event_sequence = sequence;
    const EventId origin{Id128{Id128::Bytes{static_cast<std::uint8_t>(sequence),
                                            static_cast<std::uint8_t>(sequence >> 8U), 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}};
    asset.source_events = {origin};
    return asset;
}

[[nodiscard]] ContextIndexAsset make_learning(ContextAssetKind kind, const ContextAssetId &id,
                                              const std::string &text, std::uint64_t sequence,
                                              const MemoryScope &scope) {
    ContextIndexAsset asset;
    asset.id = id;
    asset.kind = kind;
    asset.text = text;
    asset.scope = scope;
    asset.through_event_sequence = sequence;
    const EventId origin{Id128{Id128::Bytes{static_cast<std::uint8_t>(sequence),
                                            static_cast<std::uint8_t>(sequence >> 8U), 1, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}};
    asset.source_events = {origin};
    return asset;
}

[[nodiscard]] Dataset build_dataset(const FrozenConfig &config) {
    SplitMix64 rng{config.seed};
    Dataset dataset;

    const SessionId conversation_session{id_from(rng, "session")};
    const MemoryScope learning_scope = [] {
        MemoryScope scope;
        scope.kind = MemoryScopeKind::Environment;
        scope.subject_id = "eval-device";
        return scope;
    }();
    // Cross-domain distractor grounds prove the ACL never leaks.
    const SessionId stranger_session{id_from(rng, "stranger")};
    const MemoryScope stranger_scope = [] {
        MemoryScope scope;
        scope.kind = MemoryScopeKind::Application;
        scope.subject_id = "stranger-app";
        return scope;
    }();

    std::uint64_t sequence = 1;
    const auto next_sequence = [&sequence]() { return sequence++; };

    const std::size_t total_queries = config.queries_per_kind * 3;
    for (std::size_t query_index = 0; query_index < total_queries; ++query_index) {
        const ContextAssetKind kind = static_cast<ContextAssetKind>(query_index / 3 % 3);
        SplitMix64 local{rng.next()};

        // Anchored pair: the query and its relevant asset share rare tokens.
        std::string anchor_text;
        for (std::size_t word = 0; word < 4; ++word) {
            anchor_text += rare_token(local) + " ";
        }

        EvalQuery eval_query;
        if (kind == ContextAssetKind::ConversationSegment) {
            const ContextAssetId id{Id128{id_from(local, "seg")}};
            const std::string body = anchor_text + "conversation anchor";
            dataset.assets.push_back(
                {make_segment(conversation_session, id, body, next_sequence()), true, query_index});
            eval_query.query.session = conversation_session;
            eval_query.query.scopes = {learning_scope};
            eval_query.query.text = "recall " + anchor_text + "conversation";
            eval_query.anchored_asset = id;
        } else if (kind == ContextAssetKind::WorkflowEpisode) {
            const ContextAssetId id{Id128{id_from(local, "epi")}};
            const std::string statement = R"({"run_id":")" + std::to_string(query_index) +
                                          R"(","workflow_id":")" + anchor_text +
                                          R"(","outcome":"completed","reason":"ok"})";
            dataset.assets.push_back({make_learning(ContextAssetKind::WorkflowEpisode, id,
                                                    statement, next_sequence(), learning_scope),
                                      true, query_index});
            eval_query.query.session = conversation_session;
            eval_query.query.scopes = {learning_scope};
            eval_query.query.text = "episode " + anchor_text + "workflow";
            eval_query.anchored_asset = id;
        } else {
            const ContextAssetId id{Id128{id_from(local, "les")}};
            const std::string statement = R"({"lesson_id":")" + std::to_string(query_index) +
                                          R"(","workflow_id":")" + anchor_text +
                                          R"(","reason_code":"timeout"})";
            dataset.assets.push_back({make_learning(ContextAssetKind::RecoveryLesson, id, statement,
                                                    next_sequence(), learning_scope),
                                      true, query_index});
            eval_query.query.session = conversation_session;
            eval_query.query.scopes = {learning_scope};
            eval_query.query.text = "lesson " + anchor_text + "recovery";
            eval_query.anchored_asset = id;
        }
        dataset.queries.push_back(std::move(eval_query));

        // In-domain distractors (same ACL ground, disjoint rare tokens).
        for (std::size_t distractor = 0; distractor < config.distractors_per_query; ++distractor) {
            std::string noise;
            for (std::size_t word = 0; word < 4; ++word) {
                noise += rare_token(local) + " ";
            }
            if (kind == ContextAssetKind::ConversationSegment) {
                dataset.assets.push_back(
                    {make_segment(conversation_session, ContextAssetId{Id128{id_from(local, "d")}},
                                  noise + "distractor", next_sequence()),
                     false, 0});
            } else {
                dataset.assets.push_back(
                    {make_learning(kind, ContextAssetId{Id128{id_from(local, "d")}},
                                   "{\"run_id\":\"d\",\"workflow_id\":\"" + noise +
                                       "\",\"reason_code\":\"noise\"}",
                                   next_sequence(), learning_scope),
                     false, 0});
            }
        }

        // Cross-domain distractors: similar text, foreign ACL grounds.
        for (std::size_t foreign = 0; foreign < 2; ++foreign) {
            const std::string noise = anchor_text + "foreign " + std::to_string(foreign);
            dataset.assets.push_back(
                {make_segment(stranger_session, ContextAssetId{Id128{id_from(local, "f")}}, noise,
                              next_sequence()),
                 false, 0});
            dataset.assets.push_back(
                {make_learning(ContextAssetKind::WorkflowEpisode,
                               ContextAssetId{Id128{id_from(local, "f")}},
                               R"({"workflow_id":")" + noise + R"(","reason_code":"noise"})",
                               next_sequence(), stranger_scope),
                 false, 0});
        }
    }

    // Deterministic registration order: anchor-major is avoided by sorting on
    // asset id text so interleaving does not bias the bounded scan.
    std::sort(dataset.assets.begin(), dataset.assets.end(),
              [](const EvalAsset &lhs, const EvalAsset &rhs) {
                  return lhs.asset.id.to_string() < rhs.asset.id.to_string();
              });

    // dataset digest: sorted unique asset texts.
    std::vector<std::string> texts;
    texts.reserve(dataset.assets.size());
    for (const auto &eval_asset : dataset.assets) {
        texts.push_back(eval_asset.asset.text);
    }
    std::sort(texts.begin(), texts.end());
    texts.erase(std::unique(texts.begin(), texts.end()), texts.end());
    std::string joined;
    for (const auto &text : texts) {
        joined += text;
        joined += '\n';
    }
    dataset.digest_hex_text = digest_hex(digest_string(joined));
    return dataset;
}

// ---------------------------------------------------------------------------
// Run + gates
// ---------------------------------------------------------------------------

struct RunOutcome final {
    double recall_at_k = 0.0;
    double mrr = 0.0;
    std::size_t queries = 0;
    std::size_t anchored_hits = 0;
    std::size_t acl_violations = 0;
    std::size_t lexical_hits = 0;
    std::size_t vector_hits = 0;
    std::size_t exact_hits = 0;
    std::uint64_t tokens_total = 0;
    bool embedder_attached = true;
};

[[nodiscard]] RunOutcome run_round(const Dataset &dataset, const FrozenConfig &config,
                                   bool attach_embeddings) {
    InMemoryContextIndex index;
    for (const auto &eval_asset : dataset.assets) {
        const auto stored = index.upsert_asset(eval_asset.asset);
        if (!stored) {
            std::cerr << "asset registration failed: " << stored.error().safe_message << '\n';
            std::exit(2);
        }
    }
    SplitMix64 profile_rng{config.seed};
    HashingEmbedder embedder(ModelProfileId{id_from(profile_rng, "profile")},
                             config.embedding_dims);
    if (attach_embeddings) {
        for (const auto &eval_asset : dataset.assets) {
            const auto embedding = embedder.embed(context_embedding_input(eval_asset.asset));
            if (!embedding || !index.attach_embedding(eval_asset.asset.id, embedding.value())) {
                std::cerr << "embedding supply failed\n";
                std::exit(2);
            }
        }
    }

    RunOutcome outcome;
    outcome.queries = dataset.queries.size();
    double reciprocal_sum = 0.0;
    for (const auto &eval_query : dataset.queries) {
        ContextQuery query = eval_query.query;
        if (attach_embeddings) {
            ContextEmbeddingInput input;
            input.asset_id = eval_query.anchored_asset;
            input.kind = ContextAssetKind::ConversationSegment;
            input.text = query.text;
            const auto embedded = embedder.embed(input);
            query.query_embedding = embedded.value();
        }
        RetrievalBudget budget;
        budget.top_k = config.top_k;
        budget.token_budget = config.token_budget;
        const auto result = index.retrieve(query, budget);
        if (!result) {
            std::cerr << "query failed: " << result.error().safe_message << '\n';
            std::exit(2);
        }
        outcome.lexical_hits += result.value().quality.fts_leg_ran ? 1U : 0U;
        outcome.vector_hits += result.value().quality.vector_leg_ran ? 1U : 0U;
        outcome.exact_hits += result.value().quality.exact_leg_ran ? 1U : 0U;
        outcome.tokens_total += result.value().tokens_estimate;

        // R3: ACL zero-leak audit over every returned candidate.
        for (const auto &candidate : result.value().candidates) {
            const bool conversation_ok =
                !candidate.session.has_value() || *candidate.session == query.session;
            const bool scope_ok =
                !candidate.scope.has_value() || std::find(query.scopes.begin(), query.scopes.end(),
                                                          *candidate.scope) != query.scopes.end();
            if (!conversation_ok || !scope_ok) {
                ++outcome.acl_violations;
            }
        }

        const auto position =
            std::find_if(result.value().candidates.begin(), result.value().candidates.end(),
                         [&eval_query](const ContextCandidate &candidate) {
                             return candidate.asset_id == eval_query.anchored_asset;
                         });
        if (position != result.value().candidates.end()) {
            ++outcome.anchored_hits;
            reciprocal_sum +=
                1.0 / static_cast<double>((position - result.value().candidates.begin()) + 1);
        }
    }
    outcome.recall_at_k =
        static_cast<double>(outcome.anchored_hits) / static_cast<double>(outcome.queries);
    outcome.mrr = reciprocal_sum / static_cast<double>(outcome.queries);
    return outcome;
}

struct Gates final {
    bool r1_recall = false;
    bool r2_mrr = false;
    bool r3_acl_zero_leakage = false;
    bool r4_determinism = false;
    std::vector<std::string> failures;
};

[[nodiscard]] JsonValue outcome_json(const RunOutcome &outcome) {
    JsonValue::Object object;
    object.emplace_back("queries", static_cast<std::int64_t>(outcome.queries));
    object.emplace_back("recall_at_k", outcome.recall_at_k);
    object.emplace_back("mrr", outcome.mrr);
    object.emplace_back("anchored_hits", static_cast<std::int64_t>(outcome.anchored_hits));
    object.emplace_back("acl_violations", static_cast<std::int64_t>(outcome.acl_violations));
    object.emplace_back("queries_with_lexical_leg",
                        static_cast<std::int64_t>(outcome.lexical_hits));
    object.emplace_back("queries_with_vector_leg", static_cast<std::int64_t>(outcome.vector_hits));
    object.emplace_back("queries_with_exact_leg", static_cast<std::int64_t>(outcome.exact_hits));
    object.emplace_back("tokens_total", static_cast<std::int64_t>(outcome.tokens_total));
    return JsonValue(std::move(object));
}

[[nodiscard]] JsonValue config_json(const FrozenConfig &config) {
    JsonValue::Object object;
    object.emplace_back("seed", static_cast<std::int64_t>(config.seed));
    object.emplace_back("queries_per_kind", static_cast<std::int64_t>(config.queries_per_kind));
    object.emplace_back("distractors_per_query",
                        static_cast<std::int64_t>(config.distractors_per_query));
    object.emplace_back("top_k", static_cast<std::int64_t>(config.top_k));
    object.emplace_back("embedding_dims", static_cast<std::int64_t>(config.embedding_dims));
    object.emplace_back("recall_gate", config.recall_gate);
    object.emplace_back("mrr_gate", config.mrr_gate);
    object.emplace_back("token_budget", static_cast<std::int64_t>(config.token_budget));
    return JsonValue(std::move(object));
}

} // namespace

int main(int argc, char **argv) {
    std::string output_path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (output_path.empty()) {
            output_path = argument;
        }
    }

    const FrozenConfig config;
    const Dataset dataset = build_dataset(config);

    const RunOutcome hybrid = run_round(dataset, config, true);
    const RunOutcome degraded = run_round(dataset, config, false);
    const RunOutcome repeat = run_round(dataset, config, true);

    Gates gates;
    gates.r1_recall = hybrid.recall_at_k >= config.recall_gate;
    if (!gates.r1_recall) {
        gates.failures.push_back("R1: hybrid Recall@" + std::to_string(config.top_k) + " = " +
                                 std::to_string(hybrid.recall_at_k) + " below gate " +
                                 std::to_string(config.recall_gate));
    }
    gates.r2_mrr = hybrid.mrr >= config.mrr_gate;
    if (!gates.r2_mrr) {
        gates.failures.push_back("R2: hybrid MRR = " + std::to_string(hybrid.mrr) + " below gate " +
                                 std::to_string(config.mrr_gate));
    }
    gates.r3_acl_zero_leakage = hybrid.acl_violations == 0 && degraded.acl_violations == 0;
    if (!gates.r3_acl_zero_leakage) {
        gates.failures.push_back("R3: ACL leakage observed (hybrid " +
                                 std::to_string(hybrid.acl_violations) + ", degraded " +
                                 std::to_string(degraded.acl_violations) + ")");
    }
    // R4: two full in-process rounds must agree on every aggregate; over the
    // deterministic dataset identical aggregates imply identical candidate
    // sequences, which is the byte-level identity the gate asks for.
    gates.r4_determinism = repeat.recall_at_k == hybrid.recall_at_k &&
                           repeat.anchored_hits == hybrid.anchored_hits &&
                           repeat.tokens_total == hybrid.tokens_total && repeat.mrr == hybrid.mrr;
    if (!gates.r4_determinism) {
        gates.failures.push_back("R4: repeated evaluation diverged");
    }

    JsonValue::Object report;
    report.emplace_back("schema", "mira.m17.retrieval-eval.v1");
    report.emplace_back("dataset_digest", dataset.digest_hex_text);
    report.emplace_back("config", config_json(config));
    report.emplace_back("hybrid_round", outcome_json(hybrid));
    report.emplace_back("degraded_round", outcome_json(degraded));
    JsonValue::Object gates_json;
    gates_json.emplace_back("r1_recall", gates.r1_recall);
    gates_json.emplace_back("r2_mrr", gates.r2_mrr);
    gates_json.emplace_back("r3_acl_zero_leakage", gates.r3_acl_zero_leakage);
    gates_json.emplace_back("r4_determinism", gates.r4_determinism);
    report.emplace_back("gates", JsonValue(std::move(gates_json)));
    JsonValue::Array failure_list;
    for (const auto &message : gates.failures) {
        failure_list.emplace_back(message);
    }
    report.emplace_back("gate_failures", JsonValue(std::move(failure_list)));
    report.emplace_back(
        "limitations",
        "deterministic token-hash embedding supplier, not a model: metrics measure pipeline "
        "behaviour (recall path, degradation, ACL, determinism), not semantic quality; "
        "semantic recall claims require real-embedder evidence (DEC-032 §5/§9)");

    const std::string json = to_json_string(JsonValue(std::move(report)));
    std::cout << json << '\n';
    if (!output_path.empty()) {
        std::ofstream file(output_path);
        file << json << '\n';
    }

    const bool all_gates =
        gates.r1_recall && gates.r2_mrr && gates.r3_acl_zero_leakage && gates.r4_determinism;
    if (!all_gates) {
        for (const auto &message : gates.failures) {
            std::cerr << "gate failure: " << message << '\n';
        }
    }
    return all_gates ? 0 : 1;
}
