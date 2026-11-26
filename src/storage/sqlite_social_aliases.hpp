#pragma once

#include "sqlite_support.hpp"

#include <mira/content_markers.hpp>
#include <mira/json.hpp>
#include <mira/sqlite_social_directory.hpp>

#include <algorithm>
#include <optional>
#include <set>

namespace mira::storage::social_aliases {

inline Error directory_error(ErrorCode code, std::string message) {
    return store_error(code, "mira.social_directory", 0, std::move(message));
}

inline bool bind_text(Statement &statement, int index, const std::string &value) {
    return sqlite3_bind_text(statement.get(), index, value.data(), static_cast<int>(value.size()),
                             transient_copy()) == SQLITE_OK;
}

inline std::string column(Statement &statement, int index) {
    const auto *text = sqlite3_column_text(statement.get(), index);
    const auto size = sqlite3_column_bytes(statement.get(), index);
    return text ? std::string(reinterpret_cast<const char *>(text), static_cast<std::size_t>(size))
                : std::string{};
}

inline bool alias_space(char byte) {
    return std::string_view(" \t\r\n\f\v").find(byte) != std::string_view::npos;
}

inline std::string normalize_alias(std::string_view text,
                                   std::vector<std::size_t> *positions = nullptr) {
    std::string key;
    key.reserve(text.size());
    std::optional<std::size_t> pending_space;
    const auto append = [&](char byte, std::size_t position) {
        key.push_back(byte);
        if (positions)
            positions->push_back(position);
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto byte = text[i];
        if (alias_space(byte)) {
            if (!key.empty() && !pending_space)
                pending_space = i;
            continue;
        }
        if (pending_space) {
            append(' ', *pending_space);
            pending_space.reset();
        }
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
        append(byte, i);
    }
    return key;
}

inline Result<std::string> alias_key(const std::string &alias,
                                     const std::vector<std::string> &forbidden) {
    if (alias.size() > 256 || !parse_json(canonical_json_string(JsonValue{alias})))
        return directory_error(ErrorCode::InvalidArgument, "social alias is invalid");
    if (matches_content_marker(alias, forbidden))
        return directory_error(ErrorCode::SafetyRejected, "social alias is not indexable");
    if (std::ranges::any_of(alias, [](char byte) {
            const auto value = static_cast<unsigned char>(byte);
            return !alias_space(byte) && (value < 32 || value == 127);
        }))
        return directory_error(ErrorCode::InvalidArgument, "social alias has control bytes");
    auto key = normalize_alias(alias);
    if (key.empty() || matches_content_marker(key, forbidden))
        return directory_error(ErrorCode::InvalidArgument,
                               "social alias is invalid after normalization");
    return key;
}

inline bool ascii_name_byte(char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_';
}

class AliasScanDeadline final {
  public:
    AliasScanDeadline(sqlite3 *db, std::chrono::steady_clock::time_point cutoff)
        : db_(db), cutoff_(cutoff) {
        sqlite3_progress_handler(
            db_, 128,
            [](void *self) {
                const auto &scan = *static_cast<const AliasScanDeadline *>(self);
                return std::chrono::steady_clock::now() >= scan.cutoff_ ? 1 : 0;
            },
            this);
    }
    ~AliasScanDeadline() { sqlite3_progress_handler(db_, 0, nullptr, nullptr); }

  private:
    sqlite3 *db_;
    std::chrono::steady_clock::time_point cutoff_;
};

inline Result<SocialLiteralAliases> scan_aliases(sqlite3 *db, const std::string &scope,
                                                 std::string_view message, std::size_t limit,
                                                 std::chrono::steady_clock::time_point deadline) {
    const auto timeout = [] {
        return directory_error(ErrorCode::DeadlineExceeded, "social alias scan expired");
    };
    if (std::chrono::steady_clock::now() >= deadline)
        return timeout();
    AliasScanDeadline guard(db, deadline);
    std::vector<std::size_t> positions;
    const auto normalized = normalize_alias(message, &positions);
    Statement rows(db, "SELECT DISTINCT a.name_key FROM social_aliases a JOIN social_objects o"
                       " ON o.scope=a.scope AND o.object=a.object"
                       " WHERE a.scope=?1 AND instr(?2,a.name_key)>0 AND o.revision>0");
    if (!rows.valid() || !bind_text(rows, 1, scope) || !bind_text(rows, 2, normalized))
        return directory_error(ErrorCode::Internal, "social alias scan preparation failed");
    std::vector<std::size_t> longest(normalized.size());
    int step = SQLITE_OK;
    while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
        if (std::chrono::steady_clock::now() >= deadline)
            return timeout();
        const auto key = column(rows, 0);
        if (key.empty() || key.size() > 256)
            return directory_error(ErrorCode::DataLoss, "social alias scan found an invalid key");
        for (auto offset = normalized.find(key); offset != std::string::npos;
             offset = normalized.find(key, offset + 1)) {
            const auto end = offset + key.size();
            const bool embedded = (offset > 0 && ascii_name_byte(key.front()) &&
                                   ascii_name_byte(normalized[offset - 1])) ||
                                  (end < normalized.size() && ascii_name_byte(key.back()) &&
                                   ascii_name_byte(normalized[end]));
            if (!embedded)
                longest[offset] = std::max(longest[offset], key.size());
        }
    }
    if (step == SQLITE_INTERRUPT)
        return timeout();
    if (step != SQLITE_DONE)
        return directory_error(ErrorCode::Internal, "social alias scan failed");
    SocialLiteralAliases result;
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < longest.size();) {
        const auto length = longest[i];
        if (length == 0) {
            ++i;
            continue;
        }
        const auto key = std::string_view(normalized).substr(i, length);
        if (seen.insert(key).second) {
            if (result.matches.size() == limit) {
                result.truncated = true;
                break;
            }
            const auto begin = positions[i];
            const auto end = positions[i + length - 1] + 1;
            result.matches.push_back(
                {std::string(message.substr(begin, end - begin)), std::string(key), begin});
        }
        i += length;
    }
    return result;
}

} // namespace mira::storage::social_aliases
