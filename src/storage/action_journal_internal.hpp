#pragma once

#include <mira/action_journal.hpp>

#include <array>
#include <charconv>
#include <string_view>

namespace mira::journal_internal {
inline bool safe_text(std::string_view text, std::size_t limit, bool field) {
    if (text.size() > limit || (field && text.empty())) {
        return false;
    }
    for (const char character : text) {
        const auto c = static_cast<unsigned char>(character);
        if (c < 0x20 || c == 0x7f || (field && c == '|')) {
            return false;
        }
    }
    return true;
}

inline bool valid_intent(const ActionIntent &intent, RuntimeId runtime) {
    return !runtime.is_nil() && intent.runtime_id == runtime && !intent.action_id.is_nil() &&
           !intent.session_id.is_nil() && !intent.task_id.is_nil() &&
           safe_text(intent.target.type, 1024, true) && safe_text(intent.target.id, 1024, true) &&
           safe_text(intent.target.scope, 1024, true);
}

inline std::string encode(const ActionIntent &intent, std::string_view suffix = {}) {
    auto data = intent.action_id.to_string() + "|" + intent.task_id.to_string() + "|" +
                std::to_string(intent.task_epoch) + "|" + std::to_string(intent.environment_epoch) +
                "|" + intent.action_digest.to_string() + "|" + intent.target.type + "|" +
                intent.target.id + "|" + intent.target.scope;
    if (!suffix.empty()) {
        data += "|";
        data += suffix;
    }
    return data;
}

inline std::string binding(const ActionIntent &intent) {
    return intent.runtime_id.to_string() + "|" + intent.session_id.to_string() + "|" +
           encode(intent);
}

struct Payload final {
    ActionId action_id;
    TaskId task_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    Sha256Digest digest;
    ResourceDescriptor target;
    std::string suffix;
};

inline std::optional<Payload> parse(std::string_view data) {
    if (data.size() > 16'384) {
        return std::nullopt;
    }
    std::array<std::string_view, 8> fields;
    std::string_view suffix;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto separator = data.find('|');
        if (separator == std::string_view::npos && i != fields.size() - 1) {
            return std::nullopt;
        }
        fields[i] = data.substr(0, separator);
        if (separator != std::string_view::npos) {
            data.remove_prefix(separator + 1);
        }
        if (i == fields.size() - 1 && separator != std::string_view::npos) {
            suffix = data; // The entire legacy tail is text, including '|'.
        }
    }
    const auto action = ActionId::parse(fields[0]);
    const auto task = TaskId::parse(fields[1]);
    const auto digest = digest_from_hex(fields[4]);
    if (!action || action->is_nil() || !task || task->is_nil() || !digest ||
        !safe_text(fields[5], 1024, true) || !safe_text(fields[6], 1024, true) ||
        !safe_text(fields[7], 1024, true) || !safe_text(suffix, 4096, false)) {
        return std::nullopt;
    }
    Payload result;
    result.action_id = *action;
    result.task_id = *task;
    result.digest = *digest;
    const auto number = [](std::string_view value, std::uint64_t &out) {
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
    };
    if (!number(fields[2], result.task_epoch) || !number(fields[3], result.environment_epoch)) {
        return std::nullopt;
    }
    result.target = {std::string(fields[5]), std::string(fields[6]), std::string(fields[7])};
    result.suffix = suffix;
    return result;
}

inline bool matches(const Payload &payload, const ActionIntent &intent) {
    return payload.action_id == intent.action_id && payload.task_id == intent.task_id &&
           payload.task_epoch == intent.task_epoch &&
           payload.environment_epoch == intent.environment_epoch &&
           payload.digest == intent.action_digest && payload.target.type == intent.target.type &&
           payload.target.id == intent.target.id && payload.target.scope == intent.target.scope;
}
} // namespace mira::journal_internal
