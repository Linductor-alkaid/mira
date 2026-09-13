// M18 (DEC-032 Stage C) rerank comparison eval harness. Re-runs the frozen
// M17 retrieval dataset (same seed, same token-hash supplier; the digest is
// asserted against the frozen value so "reuse" is checked, not assumed) and
// compares the B column (retrieval order) against the C column (+ deterministic
// token-overlap reranker) under the pre-frozen gates C1-C4, printing a JSON
// report. Metrics measure pipeline behaviour, not semantic quality (RULE-10).

#include <mira/context_contracts.hpp>
#include <mira/context_rerank.hpp>
#include <mira/context_retrieval.hpp>
#include <mira/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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
// Frozen configuration (M18 §4; dataset construction mirrors M17 §4)
// ---------------------------------------------------------------------------

struct FrozenConfig final {
    std::uint64_t seed = 0x4d49'5231'3753'5442ULL; // "MIR17STB" — same dataset as M17
    std::size_t queries_per_kind = 16;
    std::size_t distractors_per_query = 8;
    std::size_t embedding_dims = 64;
    std::size_t retrieval_top_k = 30; // reranker feed (design §5.3: Top-K 30~100)
    std::size_t rerank_output = 10;   // metric cutoff, comparable to retrieval-v1 Recall@10
    std::uint64_t token_budget = 4'096;
    // Frozen digest of the reused dataset (M17 baseline anchor).
    std::string dataset_digest =
        "ffc59f6d0a0a375a6e8730906219339900b616ed1f861b274c437515a367649e";
};

[[nodiscard]] std::string digest_hex(const Sha256Digest &digest) {
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    for (const auto byte : digest.bytes) {
        std::ostringstream slot;
        slot << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<unsigned int>(byte);
        text += slot.str();
    }
    return text;
}

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

[[nodiscard]] std::string rare_token(SplitMix64 &rng) {
    static constexpr std::string_view kAlphabet = "abcdefghjkmnpqrstuvwxyz23456789";
    std::string token(5, 'a');
    for (auto &character : token) {
        character = kAlphabet[static_cast<std::size_t>(rng.next() % kAlphabet.size())];
    }
    return token;
}

class HashingEmbedder final : public IContextEmbedder {
  public:
    HashingEmbedder(ModelProfileId profile, std::size_t dims)
        : profile_(profile), dims_(dims) {}

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

// ---------------------------------------------------------------------------
// Dataset (mirrors tests/m17/m17_retrieval_eval.cpp build_dataset; identity is
// asserted through the frozen dataset_digest)
// ---------------------------------------------------------------------------

struct EvalAsset final {
    ContextIndexAsset asset;
    bool anchored = false;
    std::size_t query_index = 0;
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
                                            static_cast<std::uint8_t>(sequence >> 8U), 0, 0, 0,
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
                                            static_cast<std::uint8_t>(sequence >> 8U), 1, 0, 0,
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
            const std::string statement =
                R"({"run_id":")" + std::to_string(query_index) + R"(","workflow_id":")" +
                anchor_text + R"(","outcome":"completed","reason":"ok"})";
            dataset.assets.push_back(
                {make_learning(ContextAssetKind::WorkflowEpisode, id, statement,
                               next_sequence(), learning_scope),
                 true, query_index});
            eval_query.query.session = conversation_session;
            eval_query.query.scopes = {learning_scope};
            eval_query.query.text = "episode " + anchor_text + "workflow";
            eval_query.anchored_asset = id;
        } else {
            const ContextAssetId id{Id128{id_from(local, "les")}};
            const std::string statement =
                R"({"lesson_id":")" + std::to_string(query_index) + R"(","workflow_id":")" +
                anchor_text + R"(","reason_code":"timeout"})";
            dataset.assets.push_back(
                {make_learning(ContextAssetKind::RecoveryLesson, id, statement,
                               next_sequence(), learning_scope),
                 true, query_index});
            eval_query.query.session = conversation_session;
            eval_query.query.scopes = {learning_scope};
            eval_query.query.text = "lesson " + anchor_text + "recovery";
            eval_query.anchored_asset = id;
        }
        dataset.queries.push_back(std::move(eval_query));

        for (std::size_t distractor = 0; distractor < config.distractors_per_query;
             ++distractor) {
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

        for (std::size_t foreign = 0; foreign < 2; ++foreign) {
            const std::string noise = anchor_text + "foreign " + std::to_string(foreign);
            dataset.assets.push_back(
                {make_segment(stranger_session, ContextAssetId{Id128{id_from(local, "f")}},
                              noise, next_sequence()),
                 false, 0});
            dataset.assets.push_back(
                {make_learning(ContextAssetKind::WorkflowEpisode,
                               ContextAssetId{Id128{id_from(local, "f")}},
                               R"({"workflow_id":")" + noise + R"(","reason_code":"noise"})",
                               next_sequence(), stranger_scope),
                 false, 0});
        }
    }

    std::sort(dataset.assets.begin(), dataset.assets.end(),
              [](const EvalAsset &lhs, const EvalAsset &rhs) {
                  return lhs.asset.id.to_string() < rhs.asset.id.to_string();
              });

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
// Columns, gates, report
// ---------------------------------------------------------------------------

struct ColumnOutcome final {
    double recall = 0.0;
    double mrr = 0.0;
    std::size_t queries = 0;
    std::size_t anchored_hits = 0;
    std::size_t acl_violations = 0;
    std::uint64_t tokens_total = 0;
    std::uint64_t scoring_operations = 0; // reranker F1 evaluations (C column)
    std::uint64_t ticks_total = 0;        // rerank wall ticks (report only)
};

struct ComparisonRound final {
    ColumnOutcome base;    // B column: retrieval order Top-10
    ColumnOutcome rerank;  // C column: rerank(Top-30) -> Top-10
    double uplift = 0.0;
    std::size_t order_moves = 0;           // queries whose anchor presence moved
};

void build_index(const Dataset &dataset, HashingEmbedder &embedder, bool attach_embeddings,
                 InMemoryContextIndex &index) {
    for (const auto &eval_asset : dataset.assets) {
        const auto stored = index.upsert_asset(eval_asset.asset);
        if (!stored) {
            std::cerr << "asset registration failed: " << stored.error().safe_message << '\n';
            std::exit(2);
        }
        if (attach_embeddings) {
            const auto embedding = embedder.embed(context_embedding_input(eval_asset.asset));
            if (!embedding || !index.attach_embedding(eval_asset.asset.id, embedding.value())) {
                std::cerr << "embedding supply failed\n";
                std::exit(2);
            }
        }
    }
}

void audit_c(const std::vector<RankedContextItem> &items, const ContextQuery &query,
             std::size_t &violations, std::uint64_t &tokens, std::uint64_t retrieval_tokens);

[[nodiscard]] ComparisonRound run_comparison(const Dataset &dataset, const FrozenConfig &config,
                                             bool attach_embeddings) {
    SplitMix64 profile_rng{config.seed};
    HashingEmbedder embedder(ModelProfileId{id_from(profile_rng, "profile")},
                             config.embedding_dims);
    InMemoryContextIndex index;
    build_index(dataset, embedder, attach_embeddings, index);
    TokenOverlapContextReranker reranker(ContextRerankConfig{config.rerank_output});

    ComparisonRound round;
    round.base.queries = dataset.queries.size();
    round.rerank.queries = dataset.queries.size();
    double base_reciprocal = 0.0;
    double rerank_reciprocal = 0.0;

    for (const auto &eval_query : dataset.queries) {
        ContextQuery query = eval_query.query;
        if (attach_embeddings) {
            ContextEmbeddingInput input;
            input.asset_id = eval_query.anchored_asset;
            input.kind = ContextAssetKind::ConversationSegment;
            input.text = query.text;
            query.query_embedding = embedder.embed(input).value();
        }
        RetrievalBudget budget;
        budget.top_k = config.retrieval_top_k;
        budget.token_budget = config.token_budget;
        const auto retrieved = index.retrieve(query, budget);
        if (!retrieved) {
            std::cerr << "query failed: " << retrieved.error().safe_message << '\n';
            std::exit(2);
        }

        const auto audit = [&](const std::vector<ContextCandidate> &candidates,
                               std::size_t &violations, std::uint64_t &tokens) {
            for (const auto &candidate : candidates) {
                const bool conversation_ok =
                    !candidate.session.has_value() || *candidate.session == query.session;
                const bool scope_ok = !candidate.scope.has_value() ||
                                      std::find(query.scopes.begin(), query.scopes.end(),
                                                *candidate.scope) != query.scopes.end();
                if (!conversation_ok || !scope_ok) {
                    ++violations;
                }
            }
            tokens += retrieved.value().tokens_estimate;
        };
        audit(retrieved.value().candidates, round.base.acl_violations, round.base.tokens_total);

        // B column: anchor position in the retrieval order Top-10.
        const auto base_position = std::find_if(
            retrieved.value().candidates.begin(),
            retrieved.value().candidates.begin() +
                static_cast<std::ptrdiff_t>(std::min(config.rerank_output,
                                                     retrieved.value().candidates.size())),
            [&eval_query](const ContextCandidate &candidate) {
                return candidate.asset_id == eval_query.anchored_asset;
            });
        if (base_position != retrieved.value().candidates.begin() +
                                 static_cast<std::ptrdiff_t>(std::min(
                                     config.rerank_output, retrieved.value().candidates.size()))) {
            ++round.base.anchored_hits;
            base_reciprocal += 1.0 / static_cast<double>(
                                        base_position - retrieved.value().candidates.begin() + 1);
        }

        // C column: rerank the retrieval Top-30 down to the output bound.
        const auto started = std::chrono::steady_clock::now();
        const auto reranked = reranker.rerank(query, retrieved.value().candidates);
        const auto ended = std::chrono::steady_clock::now();
        if (!reranked) {
            std::cerr << "rerank failed: " << reranked.error().safe_message << '\n';
            std::exit(2);
        }
        round.rerank.scoring_operations += retrieved.value().candidates.size();
        round.rerank.ticks_total += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count());
        audit_c(reranked.value(), query, round.rerank.acl_violations, round.rerank.tokens_total,
                retrieved.value().tokens_estimate);

        const auto rerank_position = std::find_if(
            reranked.value().begin(), reranked.value().end(),
            [&eval_query](const RankedContextItem &item) {
                return item.candidate.asset_id == eval_query.anchored_asset;
            });
        if (rerank_position != reranked.value().end()) {
            ++round.rerank.anchored_hits;
            rerank_reciprocal +=
                1.0 / static_cast<double>(rerank_position - reranked.value().begin() + 1);
        }
        if ((base_position != retrieved.value().candidates.begin() +
                                  static_cast<std::ptrdiff_t>(std::min(
                                      config.rerank_output,
                                      retrieved.value().candidates.size()))) !=
            (rerank_position != reranked.value().end())) {
            ++round.order_moves;
        }
    }

    round.base.recall = static_cast<double>(round.base.anchored_hits) /
                        static_cast<double>(round.base.queries);
    round.base.mrr = base_reciprocal / static_cast<double>(round.base.queries);
    round.rerank.recall = static_cast<double>(round.rerank.anchored_hits) /
                          static_cast<double>(round.rerank.queries);
    round.rerank.mrr = rerank_reciprocal / static_cast<double>(round.rerank.queries);
    round.uplift = round.rerank.mrr - round.base.mrr;
    return round;
}

// ACL audit for ranked items (separate helper keeps the lambda above small).
void audit_c(const std::vector<RankedContextItem> &items, const ContextQuery &query,
             std::size_t &violations, std::uint64_t &tokens, std::uint64_t retrieval_tokens) {
    for (const auto &item : items) {
        const bool conversation_ok =
            !item.candidate.session.has_value() || *item.candidate.session == query.session;
        const bool scope_ok = !item.candidate.scope.has_value() ||
                              std::find(query.scopes.begin(), query.scopes.end(),
                                        *item.candidate.scope) != query.scopes.end();
        if (!conversation_ok || !scope_ok) {
            ++violations;
        }
    }
    tokens += retrieval_tokens;
}

struct Gates final {
    bool c1_recall_kept = false;
    bool c2_uplift_non_negative = false;
    bool c3_acl_zero_leakage = false;
    bool c4_determinism = false;
    std::vector<std::string> failures;
};

[[nodiscard]] JsonValue column_json(const ColumnOutcome &column) {
    JsonValue::Object object;
    object.emplace_back("queries", static_cast<std::int64_t>(column.queries));
    object.emplace_back("recall", column.recall);
    object.emplace_back("mrr", column.mrr);
    object.emplace_back("anchored_hits", static_cast<std::int64_t>(column.anchored_hits));
    object.emplace_back("acl_violations", static_cast<std::int64_t>(column.acl_violations));
    object.emplace_back("tokens_total", static_cast<std::int64_t>(column.tokens_total));
    object.emplace_back("scoring_operations", static_cast<std::int64_t>(column.scoring_operations));
    return JsonValue(std::move(object));
}

[[nodiscard]] JsonValue round_json(const ComparisonRound &round) {
    JsonValue::Object object;
    object.emplace_back("base_retrieval_order_top10", column_json(round.base));
    object.emplace_back("rerank_top30_to_top10", column_json(round.rerank));
    object.emplace_back("mrr_uplift", round.uplift);
    object.emplace_back("anchor_position_changes", static_cast<std::int64_t>(round.order_moves));
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
    if (dataset.digest_hex_text != config.dataset_digest) {
        std::cerr << "dataset digest mismatch: got " << dataset.digest_hex_text << ", want "
                  << config.dataset_digest << "\n";
        return 2;
    }

    const ComparisonRound hybrid = run_comparison(dataset, config, true);
    const ComparisonRound degraded = run_comparison(dataset, config, false);
    const ComparisonRound repeat = run_comparison(dataset, config, true);

    Gates gates;
    // C1: recall preserved in both columns (hybrid path).
    gates.c1_recall_kept =
        hybrid.base.recall == 1.0 && hybrid.rerank.recall == 1.0;
    if (!gates.c1_recall_kept) {
        gates.failures.push_back("C1: recall not preserved (base " +
                                 std::to_string(hybrid.base.recall) + ", rerank " +
                                 std::to_string(hybrid.rerank.recall) + ")");
    }
    // C2: reranker uplift over the same run's B column is non-negative.
    gates.c2_uplift_non_negative = hybrid.uplift >= 0.0;
    if (!gates.c2_uplift_non_negative) {
        gates.failures.push_back("C2: negative MRR uplift " + std::to_string(hybrid.uplift));
    }
    // C3: ACL zero-leak passthrough in every audited column.
    gates.c3_acl_zero_leakage = hybrid.base.acl_violations == 0 &&
                                hybrid.rerank.acl_violations == 0 &&
                                degraded.rerank.acl_violations == 0;
    if (!gates.c3_acl_zero_leakage) {
        gates.failures.push_back("C3: ACL leakage observed");
    }
    // C4: repeated in-process rounds agree on every aggregate (deterministic
    // pipeline; identical aggregates imply identical candidate sequences).
    gates.c4_determinism = repeat.rerank.recall == hybrid.rerank.recall &&
                           repeat.rerank.mrr == hybrid.rerank.mrr &&
                           repeat.rerank.anchored_hits == hybrid.rerank.anchored_hits &&
                           repeat.base.mrr == hybrid.base.mrr;
    if (!gates.c4_determinism) {
        gates.failures.push_back("C4: repeated evaluation diverged");
    }

    JsonValue::Object report;
    report.emplace_back("schema", "mira.m18.rerank-eval.v1");
    report.emplace_back("dataset_digest", dataset.digest_hex_text);
    report.emplace_back("reused_dataset", dataset.digest_hex_text == config.dataset_digest);
    JsonValue::Object config_json;
    config_json.emplace_back("seed", static_cast<std::int64_t>(config.seed));
    config_json.emplace_back("queries", static_cast<std::int64_t>(dataset.queries.size()));
    config_json.emplace_back("retrieval_top_k", static_cast<std::int64_t>(config.retrieval_top_k));
    config_json.emplace_back("rerank_output", static_cast<std::int64_t>(config.rerank_output));
    config_json.emplace_back("embedding_dims", static_cast<std::int64_t>(config.embedding_dims));
    report.emplace_back("config", JsonValue(std::move(config_json)));
    report.emplace_back("hybrid_round", round_json(hybrid));
    report.emplace_back("degraded_round", round_json(degraded));
    JsonValue::Object gates_json;
    gates_json.emplace_back("c1_recall_kept", gates.c1_recall_kept);
    gates_json.emplace_back("c2_uplift_non_negative", gates.c2_uplift_non_negative);
    gates_json.emplace_back("c3_acl_zero_leakage", gates.c3_acl_zero_leakage);
    gates_json.emplace_back("c4_determinism", gates.c4_determinism);
    report.emplace_back("gates", JsonValue(std::move(gates_json)));
    JsonValue::Array failure_list;
    for (const auto &message : gates.failures) {
        failure_list.emplace_back(message);
    }
    report.emplace_back("gate_failures", JsonValue(std::move(failure_list)));
    report.emplace_back(
        "limitations",
        "deterministic token-hash embedding supplier and token-overlap reranker, not models: "
        "metrics measure pipeline behaviour (recall preservation, uplift direction, ACL "
        "passthrough, determinism), not semantic reranking quality; uplift claims require "
        "real embedder/reranker evidence (DEC-032 §5/§9)");

    // Wall-clock rerank cost is report-only, not part of the JSON: the report
    // must stay byte-identical across runs (C4), which timing cannot be.
    std::cerr << "rerank wall cost (report-only): hybrid " << hybrid.rerank.ticks_total
              << " ns, degraded " << degraded.rerank.ticks_total << " ns\n";
    const std::string json = to_json_string(JsonValue(std::move(report)));
    std::cout << json << '\n';
    if (!output_path.empty()) {
        std::ofstream file(output_path);
        file << json << '\n';
    }

    const bool all_gates = gates.c1_recall_kept && gates.c2_uplift_non_negative &&
                           gates.c3_acl_zero_leakage && gates.c4_determinism;
    if (!all_gates) {
        for (const auto &message : gates.failures) {
            std::cerr << "gate failure: " << message << '\n';
        }
    }
    return all_gates ? 0 : 1;
}
