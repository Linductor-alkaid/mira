#include <mira/context_retrieval.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace mira {
namespace {

[[nodiscard]] Error retrieval_error(ContextDomainCode code, std::string message) {
    return make_context_error(code, std::move(message));
}

// Deterministic asset kind names; the closed string set is contract surface.
constexpr std::string_view kConversationSegmentName = "ConversationSegment";
constexpr std::string_view kWorkflowEpisodeName = "WorkflowEpisode";
constexpr std::string_view kRecoveryLessonName = "RecoveryLesson";

[[nodiscard]] std::string to_lower_ascii(std::string_view text) {
    std::string lowered(text);
    for (char &character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

// Lexical tokenizer for the in-process leg: lowercase [a-z0-9_] runs.
[[nodiscard]] std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char character : to_lower_ascii(text)) {
        const bool word = (character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') || character == '_';
        if (word) {
            current += character;
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

[[nodiscard]] double cosine_similarity(const std::vector<float> &lhs, const std::vector<float> &rhs) {
    double dot = 0.0;
    double lhs_norm = 0.0;
    double rhs_norm = 0.0;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        const double left = static_cast<double>(lhs[index]);
        const double right = static_cast<double>(rhs[index]);
        dot += left * right;
        lhs_norm += left * left;
        rhs_norm += right * right;
    }
    if (lhs_norm <= 0.0 || rhs_norm <= 0.0) {
        return 0.0;
    }
    return dot / (std::sqrt(lhs_norm) * std::sqrt(rhs_norm));
}

[[nodiscard]] bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const std::string lowered = to_lower_ascii(haystack);
    const std::string lowered_needle = to_lower_ascii(needle);
    return lowered.find(lowered_needle) != std::string::npos;
}

[[nodiscard]] bool text_carries_marker(std::string_view text, const std::vector<std::string> &markers) {
    for (const auto &marker : markers) {
        if (!marker.empty() && contains_case_insensitive(text, marker)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool embedding_usable(const ContextEmbedding &embedding,
                                    const ContextIndexPolicy &policy) {
    return !embedding.values.empty() && embedding.values.size() <= policy.max_embedding_dims &&
           std::all_of(embedding.values.begin(), embedding.values.end(),
                       [](float value) { return std::isfinite(value); }) &&
           !embedding.profile_id.value.is_nil();
}

struct IndexedAsset final {
    ContextIndexAsset asset;
    std::optional<ContextEmbedding> embedding;
};

} // namespace

std::string context_asset_kind_name(ContextAssetKind kind) {
    switch (kind) {
    case ContextAssetKind::ConversationSegment:
        return std::string(kConversationSegmentName);
    case ContextAssetKind::WorkflowEpisode:
        return std::string(kWorkflowEpisodeName);
    case ContextAssetKind::RecoveryLesson:
        return std::string(kRecoveryLessonName);
    }
    return "Unknown";
}

std::optional<ContextAssetKind> context_asset_kind_from(std::string_view name) {
    if (name == kConversationSegmentName) {
        return ContextAssetKind::ConversationSegment;
    }
    if (name == kWorkflowEpisodeName) {
        return ContextAssetKind::WorkflowEpisode;
    }
    if (name == kRecoveryLessonName) {
        return ContextAssetKind::RecoveryLesson;
    }
    return std::nullopt;
}

ContextAssetId context_asset_id_from_seed(std::string_view seed) {
    const auto digest = digest_string(seed);
    Id128::Bytes bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return ContextAssetId{Id128{bytes}};
}

Result<void> ContextIndexPolicy::validate() const {
    if (max_asset_text_bytes < 64 || max_asset_text_bytes > 256 * 1024) {
        return retrieval_error(ContextDomainCode::InvalidItem, "asset text bound out of range");
    }
    if (max_source_events == 0 || max_source_events > 1'024) {
        return retrieval_error(ContextDomainCode::InvalidItem, "source event bound out of range");
    }
    if (max_assets == 0 || max_assets > 10'000'000) {
        return retrieval_error(ContextDomainCode::InvalidItem, "asset count bound out of range");
    }
    if (max_embedding_dims == 0 || max_embedding_dims > 4'096) {
        return retrieval_error(ContextDomainCode::InvalidItem, "embedding dimension bound out of range");
    }
    for (const auto &marker : forbidden_markers) {
        if (marker.empty()) {
            return retrieval_error(ContextDomainCode::InvalidItem,
                                    "forbidden markers must not be empty");
        }
    }
    return Result<void>{};
}

Result<void> ContextIndexAsset::validate(const ContextIndexPolicy &policy) const {
    if (id.is_nil()) {
        return retrieval_error(ContextDomainCode::InvalidItem, "asset id must not be nil");
    }
    if (text.empty()) {
        return retrieval_error(ContextDomainCode::InvalidItem, "asset text must not be empty");
    }
    if (text.size() > policy.max_asset_text_bytes) {
        return retrieval_error(ContextDomainCode::InvalidItem, "asset text exceeds the policy bound");
    }
    if (source_events.size() > policy.max_source_events) {
        return retrieval_error(ContextDomainCode::InvalidItem, "provenance exceeds the policy bound");
    }
    const bool session_owned = session.has_value() && !session->is_nil();
    const bool scope_owned = scope.has_value();
    if (kind == ContextAssetKind::ConversationSegment) {
        if (!session_owned || scope_owned) {
            return retrieval_error(ContextDomainCode::InvalidItem,
                                    "conversation segments own a session and no scope");
        }
    } else if (!scope_owned || session_owned) {
        return retrieval_error(ContextDomainCode::InvalidItem,
                                "learning assets own a scope and no session");
    }
    if (text_carries_marker(text, policy.forbidden_markers)) {
        return retrieval_error(ContextDomainCode::ForbiddenContent,
                                "asset text carries a forbidden marker");
    }
    return Result<void>{};
}

ContextEmbeddingInput context_embedding_input(const ContextIndexAsset &asset) {
    ContextEmbeddingInput input;
    input.asset_id = asset.id;
    input.kind = asset.kind;
    input.text = asset.text;
    input.source_events = asset.source_events;
    return input;
}

Result<void> ContextQuery::validate() const {
    if (!session.has_value() && scopes.empty()) {
        return retrieval_error(ContextDomainCode::ScopeDenied,
                                "context queries must state a session or a scope allowlist");
    }
    if (session.has_value() && session->is_nil()) {
        return retrieval_error(ContextDomainCode::ScopeDenied, "session grant must not be nil");
    }
    for (const auto &scope : scopes) {
        if (scope.subject_id.empty() && scope.kind != MemoryScopeKind::Environment &&
            scope.kind != MemoryScopeKind::Agent) {
            return retrieval_error(ContextDomainCode::ScopeDenied,
                                    "scoped queries require a subject id");
        }
        if (scope.subject_id.size() > 256) {
            return retrieval_error(ContextDomainCode::ScopeDenied, "scope subject id is too long");
        }
    }
    if (kinds.has_value() && kinds->empty()) {
        return retrieval_error(ContextDomainCode::InvalidItem, "kind filter must not be empty");
    }
    if (query_embedding.has_value()) {
        if (query_embedding->values.size() > 4'096) {
            return retrieval_error(ContextDomainCode::InvalidItem,
                                    "query embedding exceeds dimension bounds");
        }
        if (!std::all_of(query_embedding->values.begin(), query_embedding->values.end(),
                         [](float value) { return std::isfinite(value); })) {
            return retrieval_error(ContextDomainCode::InvalidItem,
                                    "query embedding carries non-finite values");
        }
        if (query_embedding->profile_id.value.is_nil()) {
            return retrieval_error(ContextDomainCode::InvalidItem,
                                    "query embedding requires a profile id");
        }
    }
    return Result<void>{};
}

Result<void> RetrievalBudget::validate() const {
    if (top_k == 0 || top_k > 512) {
        return retrieval_error(ContextDomainCode::InvalidItem, "top_k must be within (0, 512]");
    }
    if (token_budget == 0) {
        return retrieval_error(ContextDomainCode::InvalidItem, "token budget must be positive");
    }
    if (deadline.count() < 0) {
        return retrieval_error(ContextDomainCode::InvalidItem, "deadline must not be negative");
    }
    if (max_vector_scan == 0 || max_vector_scan > 1'000'000) {
        return retrieval_error(ContextDomainCode::InvalidItem,
                               "vector scan bound out of range");
    }
    return Result<void>{};
}

// ---------------------------------------------------------------------------
// InMemoryContextIndex
// ---------------------------------------------------------------------------

class InMemoryContextIndex::Impl final {
  public:
    Impl(ContextIndexPolicy policy, ContextRetrievalWeights weights)
        : policy_(std::move(policy)), weights_(weights) {}

    [[nodiscard]] Result<void> upsert_asset(const ContextIndexAsset &asset) {
        auto valid = asset.validate(policy_);
        if (!valid) {
            return valid;
        }
        const std::lock_guard<std::mutex> guard(mutex_);
        if (entries_.size() >= policy_.max_assets) {
            const auto existing = std::find_if(entries_.begin(), entries_.end(),
                                               [&asset](const IndexedAsset &entry) {
                                                   return entry.asset.id == asset.id;
                                               });
            if (existing == entries_.end()) {
                return retrieval_error(ContextDomainCode::InvalidLimits,
                                       "asset count exceeds the policy bound");
            }
        }
        auto existing = std::find_if(entries_.begin(), entries_.end(),
                                     [&asset](const IndexedAsset &entry) {
                                         return entry.asset.id == asset.id;
                                     });
        if (existing != entries_.end()) {
            if (existing->asset.through_event_sequence > asset.through_event_sequence) {
                return retrieval_error(ContextDomainCode::StaleBuild,
                                       "asset watermark must not move backwards");
            }
            if (existing->asset.through_event_sequence == asset.through_event_sequence &&
                (existing->asset.text != asset.text ||
                 existing->asset.kind != asset.kind)) {
                return retrieval_error(ContextDomainCode::InvalidItem,
                                       "asset content changed without advancing the watermark");
            }
            const bool advanced =
                existing->asset.through_event_sequence < asset.through_event_sequence;
            existing->asset = asset;
            if (advanced) {
                // The old embedding projects content that no longer matches
                // the source; it stays stale until re-supplied.
                existing->embedding.reset();
            }
            return Result<void>{};
        }
        entries_.push_back(IndexedAsset{asset, std::nullopt});
        return Result<void>{};
    }

    [[nodiscard]] Result<void> attach_embedding(const ContextAssetId &id, ContextEmbedding embedding) {
        if (!embedding_usable(embedding, policy_)) {
            return retrieval_error(ContextDomainCode::InvalidItem,
                                   "embedding is empty, oversized, non-finite or profile-less");
        }
        const std::lock_guard<std::mutex> guard(mutex_);
        auto entry = std::find_if(entries_.begin(), entries_.end(),
                                  [&id](const IndexedAsset &candidate) {
                                      return candidate.asset.id == id;
                                  });
        if (entry == entries_.end()) {
            return retrieval_error(ContextDomainCode::InvalidItem, "embedding for an unknown asset");
        }
        entry->embedding = std::move(embedding);
        return Result<void>{};
    }

    [[nodiscard]] Result<void> drop_asset(const ContextAssetId &id) {
        const std::lock_guard<std::mutex> guard(mutex_);
        const auto entry = std::find_if(entries_.begin(), entries_.end(),
                                        [&id](const IndexedAsset &candidate) {
                                            return candidate.asset.id == id;
                                        });
        if (entry == entries_.end()) {
            return retrieval_error(ContextDomainCode::InvalidItem, "unknown asset");
        }
        entries_.erase(entry);
        return Result<void>{};
    }

    [[nodiscard]] std::size_t index_lag() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return static_cast<std::size_t>(std::count_if(
            entries_.begin(), entries_.end(),
            [](const IndexedAsset &entry) { return !entry.embedding.has_value(); }));
    }

    [[nodiscard]] std::size_t clear_embeddings() {
        const std::lock_guard<std::mutex> guard(mutex_);
        std::size_t dropped = 0;
        for (auto &entry : entries_) {
            if (entry.embedding.has_value()) {
                entry.embedding.reset();
                ++dropped;
            }
        }
        return dropped;
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return entries_.size();
    }

    [[nodiscard]] bool contains(const ContextAssetId &id) const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return std::any_of(entries_.begin(), entries_.end(),
                           [&id](const IndexedAsset &entry) { return entry.asset.id == id; });
    }

    [[nodiscard]] bool has_embedding(const ContextAssetId &id) const {
        const std::lock_guard<std::mutex> guard(mutex_);
        const auto entry = std::find_if(entries_.begin(), entries_.end(),
                                        [&id](const IndexedAsset &candidate) {
                                            return candidate.asset.id == id;
                                        });
        return entry != entries_.end() && entry->embedding.has_value();
    }

    [[nodiscard]] std::optional<ContextIndexAsset> asset(const ContextAssetId &id) const {
        const std::lock_guard<std::mutex> guard(mutex_);
        const auto entry = std::find_if(entries_.begin(), entries_.end(),
                                        [&id](const IndexedAsset &candidate) {
                                            return candidate.asset.id == id;
                                        });
        if (entry == entries_.end()) {
            return std::nullopt;
        }
        return entry->asset;
    }

    [[nodiscard]] Result<ContextRetrievalResult> retrieve(const ContextQuery &query,
                                                          const RetrievalBudget &budget) {
        const auto valid_query = query.validate();
        if (!valid_query) {
            return valid_query.error();
        }
        const auto valid_budget = budget.validate();
        if (!valid_budget) {
            return valid_budget.error();
        }

        // The snapshot copies the projection once; retrieval itself is pure
        // over the snapshot so a slow host call cannot hold the write lock.
        std::vector<IndexedAsset> snapshot;
        std::size_t lag = 0;
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            snapshot = entries_;
            lag = static_cast<std::size_t>(std::count_if(
                snapshot.begin(), snapshot.end(),
                [](const IndexedAsset &entry) { return !entry.embedding.has_value(); }));
        }

        ContextRetrievalResult result;
        result.quality.index_lag = lag;

        // ACL + kind universe (deny by default; never crossed by similarity).
        std::vector<const IndexedAsset *> universe;
        for (const auto &entry : snapshot) {
            if (query.kinds.has_value() &&
                std::find(query.kinds->begin(), query.kinds->end(), entry.asset.kind) ==
                    query.kinds->end()) {
                continue;
            }
            if (entry.asset.session.has_value()) {
                if (!query.session.has_value() || *query.session != *entry.asset.session) {
                    continue;
                }
            } else if (entry.asset.scope.has_value()) {
                if (std::find(query.scopes.begin(), query.scopes.end(), *entry.asset.scope) ==
                    query.scopes.end()) {
                    continue;
                }
            } else {
                continue; // ownerless assets are never retrievable
            }
            universe.push_back(&entry);
        }

        const auto deadline_point = std::chrono::steady_clock::now() + budget.deadline;

        // Exact leg: verbatim substrings, mandatory when stated.
        std::set<ContextAssetId> exact_hits;
        if (!query.exact_terms.empty()) {
            result.quality.exact_leg_ran = true;
            for (const auto *entry : universe) {
                bool all = true;
                for (const auto &term : query.exact_terms) {
                    if (!contains_case_insensitive(entry->asset.text, term)) {
                        all = false;
                        break;
                    }
                }
                if (all) {
                    exact_hits.insert(entry->asset.id);
                }
            }
        }

        // Lexical leg: query-token coverage over the universe.
        std::map<ContextAssetId, double> lexical_rank;
        if (!query.text.empty() && std::chrono::steady_clock::now() < deadline_point) {
            result.quality.fts_leg_ran = true;
            const std::vector<std::string> query_tokens = tokenize(query.text);
            std::set<std::string> query_set(query_tokens.begin(), query_tokens.end());
            if (!query_set.empty()) {
                for (const auto *entry : universe) {
                    const std::vector<std::string> asset_tokens = tokenize(entry->asset.text);
                    std::set<std::string> asset_set(asset_tokens.begin(), asset_tokens.end());
                    std::size_t overlap = 0;
                    for (const auto &token : query_set) {
                        if (asset_set.count(token) != 0) {
                            ++overlap;
                        }
                    }
                    if (overlap > 0) {
                        lexical_rank[entry->asset.id] =
                            static_cast<double>(overlap) / static_cast<double>(query_set.size());
                    }
                }
            }
        }

        // Vector leg: bounded linear cosine over registration order.
        std::map<ContextAssetId, double> vector_scores;
        if (query.query_embedding.has_value() && !query.query_embedding->values.empty() &&
            std::chrono::steady_clock::now() < deadline_point) {
            const auto &query_vector = query.query_embedding->values;
            const std::size_t query_dim = query_vector.size();
            std::size_t vector_errors = 0;
            std::size_t scanned = 0;
            for (const auto &entry : snapshot) {
                if (scanned >= budget.max_vector_scan) {
                    break;
                }
                if (!entry.embedding.has_value()) {
                    continue; // supply lag, not corruption
                }
                ++scanned;
                if (entry.embedding->values.size() != query_dim ||
                    !(entry.embedding->profile_id == query.query_embedding->profile_id)) {
                    ++vector_errors;
                    continue;
                }
                const double similarity =
                    cosine_similarity(entry.embedding->values, query_vector);
                if (similarity >= 0.0) {
                    vector_scores[entry.asset.id] = similarity;
                }
            }
            if (!vector_scores.empty() || vector_errors == 0) {
                result.quality.vector_leg_ran = true;
            } else {
                result.quality.vector_degraded = true;
                result.quality.degraded = true;
                result.quality.note = "vector index unusable; exact/lexical legs answered the query";
            }
        }

        if (std::chrono::steady_clock::now() >= deadline_point) {
            result.quality.deadline_exceeded = true;
            result.quality.degraded = true;
            result.quality.note = "retrieval deadline exceeded; partial results returned";
        }

        const bool exact_hit_any = !exact_hits.empty();
        const bool any_leg = exact_hit_any || !lexical_rank.empty() || !vector_scores.empty();
        const bool ranking_attempted = !query.text.empty() || !query.exact_terms.empty() ||
                                       query.query_embedding.has_value();
        if (ranking_attempted && !any_leg && !result.quality.deadline_exceeded) {
            return result; // nothing matched; an empty answer beats a scope dump
        }

        struct Scored final {
            const IndexedAsset *entry = nullptr;
            double score = 0.0;
        };
        std::vector<Scored> scored;
        scored.reserve(universe.size());
        std::set<std::string> seen_text;
        for (const auto *entry : universe) {
            const bool exact_hit = exact_hits.count(entry->asset.id) != 0;
            const auto lexical = lexical_rank.find(entry->asset.id);
            const auto vector = vector_scores.find(entry->asset.id);
            const bool touched =
                exact_hit || lexical != lexical_rank.end() || vector != vector_scores.end();
            if (any_leg && !touched) {
                continue; // ranking legs ran; only matches participate
            }
            if (exact_hit_any && !exact_hit) {
                continue; // explicit exact terms are mandatory filters
            }
            if (!seen_text.insert(entry->asset.text).second) {
                continue; // duplicate-statement suppression
            }
            double score = 0.0;
            if (exact_hit) {
                score += weights_.exact;
            }
            if (lexical != lexical_rank.end()) {
                score += weights_.lexical * lexical->second;
            }
            if (vector != vector_scores.end()) {
                score += weights_.vector * vector->second;
            }
            scored.push_back(Scored{entry, score});
        }
        std::sort(scored.begin(), scored.end(), [](const Scored &lhs, const Scored &rhs) {
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            if (lhs.entry->asset.kind != rhs.entry->asset.kind) {
                return lhs.entry->asset.kind < rhs.entry->asset.kind;
            }
            return lhs.entry->asset.id.to_string() < rhs.entry->asset.id.to_string();
        });

        for (const auto &candidate : scored) {
            if (result.candidates.size() >= budget.top_k) {
                break;
            }
            const std::string &text = candidate.entry->asset.text;
            const std::uint64_t tokens =
                std::max<std::uint64_t>(16, text.size() / 4 + 8);
            if (result.tokens_estimate + tokens > budget.token_budget) {
                continue;
            }
            result.tokens_estimate += tokens;
            ContextCandidate item;
            item.asset_id = candidate.entry->asset.id;
            item.kind = candidate.entry->asset.kind;
            item.text = text;
            item.session = candidate.entry->asset.session;
            item.scope = candidate.entry->asset.scope;
            item.source_events = candidate.entry->asset.source_events;
            item.through_event_sequence = candidate.entry->asset.through_event_sequence;
            item.score = candidate.score;
            item.exact_hit = exact_hits.count(item.asset_id) != 0;
            if (const auto lexical = lexical_rank.find(item.asset_id);
                lexical != lexical_rank.end()) {
                item.lexical_score = lexical->second;
            }
            if (const auto vector = vector_scores.find(item.asset_id);
                vector != vector_scores.end()) {
                item.vector_hit = true;
                item.vector_similarity = vector->second;
            }
            result.candidates.push_back(std::move(item));
        }
        return result;
    }

  private:
    ContextIndexPolicy policy_;
    ContextRetrievalWeights weights_;
    mutable std::mutex mutex_;
    std::vector<IndexedAsset> entries_;
};

InMemoryContextIndex::InMemoryContextIndex(ContextIndexPolicy policy,
                                           ContextRetrievalWeights weights)
    : impl_(std::make_unique<Impl>(std::move(policy), weights)) {}

InMemoryContextIndex::~InMemoryContextIndex() = default;

Result<void> InMemoryContextIndex::upsert_asset(const ContextIndexAsset &asset) {
    return impl_->upsert_asset(asset);
}

Result<void> InMemoryContextIndex::attach_embedding(const ContextAssetId &id,
                                                     ContextEmbedding embedding) {
    return impl_->attach_embedding(id, std::move(embedding));
}

Result<void> InMemoryContextIndex::drop_asset(const ContextAssetId &id) {
    return impl_->drop_asset(id);
}

std::size_t InMemoryContextIndex::index_lag() const { return impl_->index_lag(); }

std::size_t InMemoryContextIndex::clear_embeddings() { return impl_->clear_embeddings(); }

std::size_t InMemoryContextIndex::size() const { return impl_->size(); }

bool InMemoryContextIndex::contains(const ContextAssetId &id) const { return impl_->contains(id); }

bool InMemoryContextIndex::has_embedding(const ContextAssetId &id) const {
    return impl_->has_embedding(id);
}

std::optional<ContextIndexAsset> InMemoryContextIndex::asset(const ContextAssetId &id) const {
    return impl_->asset(id);
}

Result<ContextRetrievalResult> InMemoryContextIndex::retrieve(const ContextQuery &query,
                                                              const RetrievalBudget &budget) {
    return impl_->retrieve(query, budget);
}

// ---------------------------------------------------------------------------
// Conversation segmentation
// ---------------------------------------------------------------------------

Result<void> ConversationSegmentationOptions::validate() const {
    if (window_entries == 0 || window_entries > 1'024) {
        return retrieval_error(ContextDomainCode::InvalidItem, "window size out of range");
    }
    if (max_segment_bytes < 64 || max_segment_bytes > 256 * 1024) {
        return retrieval_error(ContextDomainCode::InvalidItem, "segment byte bound out of range");
    }
    return Result<void>{};
}

namespace {

// One deterministic line of segment text: "user: ..." / "loop: ...".
[[nodiscard]] std::string entry_line(const ConversationEntry &entry) {
    std::ostringstream line;
    line << (entry.kind == ConversationEntry::Kind::UserMessage ? "user: " : "loop: ")
         << entry.text;
    return line.str();
}

} // namespace

Result<std::vector<ConversationSegment>>
segment_conversation(const SessionId &session, std::span<const ConversationEntry> entries,
                     const ConversationSegmentationOptions &options) {
    const auto valid = options.validate();
    if (!valid) {
        return valid.error();
    }
    if (session.is_nil()) {
        return retrieval_error(ContextDomainCode::InvalidItem, "segmentation requires a session");
    }

    std::vector<ConversationSegment> segments;
    std::size_t first_entry = 0;
    std::size_t consumed = 0;
    std::string text;
    std::vector<EventId> source_events;
    std::vector<ConversationSegmentEntry> window_entries;
    SessionSequence through = 0;

    const auto close_window = [&]() {
        if (consumed == 0) {
            return;
        }
        ConversationSegment segment;
        segment.session = session;
        segment.first_entry = first_entry;
        segment.entry_count = consumed;
        segment.text = text;
        segment.source_events = std::move(source_events);
        segment.entries = std::move(window_entries);
        segment.through_sequence = through;
        segment.asset_id = context_asset_id_from_seed(
            "mira.conversation.segment|" + session.to_string() + "|" +
            std::to_string(first_entry) + "|" + std::to_string(consumed));
        segments.push_back(std::move(segment));
        first_entry += consumed;
        consumed = 0;
        text.clear();
        source_events.clear();
        window_entries.clear();
        through = 0;
    };

    for (const auto &entry : entries) {
        std::string line = entry_line(entry);
        if (line.size() > options.max_segment_bytes) {
            // A single oversized entry becomes a truncated singleton window;
            // boundedness wins over completeness (RULE-08).
            if (consumed > 0) {
                close_window();
            }
            const std::string truncated_line =
                line.substr(0, options.max_segment_bytes) + "\n[truncated]";
            ConversationSegment segment;
            segment.session = session;
            segment.first_entry = first_entry;
            segment.entry_count = 1;
            segment.text = truncated_line;
            segment.source_events = {entry.origin};
            segment.entries = {ConversationSegmentEntry{truncated_line, entry.origin,
                                                        entry.session_sequence}};
            segment.through_sequence = entry.session_sequence;
            segment.asset_id = context_asset_id_from_seed(
                "mira.conversation.segment|" + session.to_string() + "|" +
                std::to_string(first_entry) + "|1");
            segments.push_back(std::move(segment));
            first_entry += 1;
            continue;
        }
        if (consumed > 0 && text.size() + 1 + line.size() > options.max_segment_bytes) {
            close_window();
        }
        if (!text.empty()) {
            text += '\n';
        }
        text += line;
        source_events.push_back(entry.origin);
        window_entries.push_back(
            ConversationSegmentEntry{line, entry.origin, entry.session_sequence});
        through = std::max(through, entry.session_sequence);
        ++consumed;
        if (consumed >= options.window_entries) {
            close_window();
        }
    }
    close_window();
    return segments;
}

// ---------------------------------------------------------------------------
// Layer 0 admission path
// ---------------------------------------------------------------------------

ContextItem context_item_from_candidate(const ContextCandidate &candidate) {
    ContextItem item;
    if (const auto parsed = ContextItemId::parse(candidate.asset_id.to_string()); parsed) {
        item.id = *parsed;
    }
    item.kind = ContextItemKind::RetrievedMemory;
    item.authority = ContextAuthority::RetrievedMemory;
    item.priority = ContextPriority::Normal;
    TextPart part;
    part.text = candidate.text;
    part.sensitivity = Sensitivity::Internal;
    item.content.push_back(std::move(part));
    item.provenance = candidate.source_events;
    item.sequence = candidate.through_event_sequence;
    item.consumed = true;
    return item;
}

// ---------------------------------------------------------------------------
// JSON serialization (schema "mira.context.candidate.v1")
// ---------------------------------------------------------------------------

JsonValue context_candidate_to_json(const ContextCandidate &candidate) {
    JsonValue::Object object;
    object.emplace_back(
        "schema_version",
        JsonValue::Object{{"major", static_cast<std::int64_t>(context_contract_version().major)},
                          {"minor", static_cast<std::int64_t>(context_contract_version().minor)}});
    object.emplace_back("asset_id", candidate.asset_id.to_string());
    object.emplace_back("kind", context_asset_kind_name(candidate.kind));
    object.emplace_back("text", candidate.text);
    if (candidate.session.has_value()) {
        object.emplace_back("session", candidate.session->to_string());
    }
    if (candidate.scope.has_value()) {
        object.emplace_back("scope", memory_scope_to_json(*candidate.scope));
    }
    JsonValue::Array provenance;
    for (const auto &event : candidate.source_events) {
        provenance.emplace_back(event.to_string());
    }
    object.emplace_back("provenance", JsonValue(std::move(provenance)));
    object.emplace_back("through_event_sequence",
                        static_cast<std::int64_t>(candidate.through_event_sequence));
    object.emplace_back("score", candidate.score);
    object.emplace_back("exact_hit", candidate.exact_hit);
    object.emplace_back("lexical_score", candidate.lexical_score);
    object.emplace_back("vector_hit", candidate.vector_hit);
    object.emplace_back("vector_similarity", candidate.vector_similarity);
    return JsonValue(std::move(object));
}

Result<ContextCandidate> context_candidate_from_json(const JsonValue &json) {
    if (!json.is_object()) {
        return retrieval_error(ContextDomainCode::SchemaUnsupported,
                               "context candidate must be an object");
    }
    const auto *version = json.find("schema_version");
    if (version == nullptr || !version->is_object()) {
        return retrieval_error(ContextDomainCode::SchemaUnsupported,
                               "context candidate requires a schema version");
    }
    SchemaVersion incoming{0, 0};
    if (const auto *major = version->find("major"); major != nullptr && major->is_number()) {
        incoming.major = static_cast<std::uint16_t>(major->as_number().value_or(0.0));
    }
    if (const auto *minor = version->find("minor"); minor != nullptr && minor->is_number()) {
        incoming.minor = static_cast<std::uint16_t>(minor->as_number().value_or(0.0));
    }
    const auto supported = validate_schema_version(incoming, context_contract_version());
    if (!supported) {
        return supported.error();
    }

    ContextCandidate candidate;
    const auto *asset = json.find("asset_id");
    if (asset == nullptr || !asset->is_string()) {
        return retrieval_error(ContextDomainCode::InvalidItem,
                               "context candidate requires an asset id");
    }
    const auto parsed_asset = ContextAssetId::parse(*asset->as_string());
    if (!parsed_asset) {
        return retrieval_error(ContextDomainCode::InvalidItem, "asset id is malformed");
    }
    candidate.asset_id = *parsed_asset;
    const auto *kind = json.find("kind");
    if (kind == nullptr || !kind->is_string()) {
        return retrieval_error(ContextDomainCode::InvalidItem, "context candidate requires a kind");
    }
    const auto parsed_kind = context_asset_kind_from(*kind->as_string());
    if (!parsed_kind) {
        return retrieval_error(ContextDomainCode::InvalidItem, "asset kind is unknown");
    }
    candidate.kind = *parsed_kind;
    if (const auto *text = json.find("text"); text != nullptr && text->is_string()) {
        candidate.text = *text->as_string();
    }
    if (const auto *session = json.find("session");
        session != nullptr && session->is_string()) {
        const auto parsed_session = SessionId::parse(*session->as_string());
        if (!parsed_session) {
            return retrieval_error(ContextDomainCode::InvalidItem, "candidate session is malformed");
        }
        candidate.session = parsed_session;
    }
    if (const auto *scope = json.find("scope"); scope != nullptr && scope->is_object()) {
        auto parsed_scope = memory_scope_from_json(*scope);
        if (!parsed_scope) {
            return retrieval_error(ContextDomainCode::InvalidItem, "candidate scope is malformed");
        }
        candidate.scope = std::move(parsed_scope).value();
    }
    if (const auto *provenance = json.find("provenance");
        provenance != nullptr && provenance->is_array()) {
        for (const auto &event : *provenance->as_array()) {
            if (event.is_string()) {
                const auto parsed_event = EventId::parse(*event.as_string());
                if (!parsed_event) {
                    return retrieval_error(ContextDomainCode::InvalidItem,
                                           "provenance event id is malformed");
                }
                candidate.source_events.push_back(*parsed_event);
            }
        }
    }
    if (const auto *sequence = json.find("through_event_sequence");
        sequence != nullptr && sequence->is_number()) {
        const auto value = sequence->as_number().value_or(0.0);
        if (value < 0.0) {
            return retrieval_error(ContextDomainCode::InvalidItem,
                                   "through_event_sequence must not be negative");
        }
        candidate.through_event_sequence = static_cast<std::uint64_t>(value);
    }
    if (const auto *score = json.find("score"); score != nullptr && score->is_number()) {
        candidate.score = score->as_number().value_or(0.0);
    }
    if (const auto *exact = json.find("exact_hit"); exact != nullptr && exact->is_boolean()) {
        candidate.exact_hit = exact->as_boolean().value_or(false);
    }
    if (const auto *lexical = json.find("lexical_score");
        lexical != nullptr && lexical->is_number()) {
        candidate.lexical_score = lexical->as_number().value_or(0.0);
    }
    if (const auto *vector_hit = json.find("vector_hit");
        vector_hit != nullptr && vector_hit->is_boolean()) {
        candidate.vector_hit = vector_hit->as_boolean().value_or(false);
    }
    if (const auto *similarity = json.find("vector_similarity");
        similarity != nullptr && similarity->is_number()) {
        candidate.vector_similarity = similarity->as_number().value_or(0.0);
    }
    return candidate;
}

} // namespace mira
