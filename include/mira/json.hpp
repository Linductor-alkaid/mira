#pragma once

#include <mira/core_contracts.hpp>
#include <mira/event_store.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// JSON value model
// ---------------------------------------------------------------------------

// Minimal, dependency-free JSON value used for model wire dialects, event
// payloads and schema validation. Object key order is preserved as inserted;
// canonical_json_string() provides the order-independent digest form.
class JsonValue final {
  public:
    using Array = std::vector<JsonValue>;
    using Object = std::vector<std::pair<std::string, JsonValue>>;
    using Number = double;

    JsonValue() noexcept = default;
    JsonValue(std::nullptr_t) noexcept {}
    JsonValue(bool value) noexcept : variant_(value) {}
    JsonValue(std::int64_t value) noexcept : variant_(value) {}
    JsonValue(int value) noexcept : variant_(static_cast<std::int64_t>(value)) {}
    JsonValue(std::size_t value) noexcept : variant_(static_cast<std::int64_t>(value)) {}
    JsonValue(Number value) noexcept : variant_(value) {}
    JsonValue(std::string value) noexcept : variant_(std::move(value)) {}
    JsonValue(const char *value) : variant_(std::string(value)) {}
    JsonValue(Array value) noexcept : variant_(std::move(value)) {}
    JsonValue(Object value) noexcept : variant_(std::move(value)) {}

    enum class Kind : std::uint8_t { Null, Boolean, Integer, Number, String, Array, Object };

    [[nodiscard]] Kind kind() const noexcept {
        switch (variant_.index()) {
        case 1:
            return Kind::Boolean;
        case 2:
            return Kind::Integer;
        case 3:
            return Kind::Number;
        case 4:
            return Kind::String;
        case 5:
            return Kind::Array;
        case 6:
            return Kind::Object;
        case 0:
        default:
            return Kind::Null;
        }
    }

    [[nodiscard]] bool is_null() const noexcept { return kind() == Kind::Null; }
    [[nodiscard]] bool is_boolean() const noexcept { return kind() == Kind::Boolean; }
    [[nodiscard]] bool is_integer() const noexcept { return kind() == Kind::Integer; }
    [[nodiscard]] bool is_number() const noexcept {
        return kind() == Kind::Integer || kind() == Kind::Number;
    }
    [[nodiscard]] bool is_string() const noexcept { return kind() == Kind::String; }
    [[nodiscard]] bool is_array() const noexcept { return kind() == Kind::Array; }
    [[nodiscard]] bool is_object() const noexcept { return kind() == Kind::Object; }

    [[nodiscard]] std::optional<bool> as_boolean() const noexcept {
        if (auto *value = std::get_if<bool>(&variant_)) {
            return *value;
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::int64_t> as_integer() const noexcept {
        if (auto *value = std::get_if<std::int64_t>(&variant_)) {
            return *value;
        }
        if (auto *value = std::get_if<Number>(&variant_)) {
            if (*value == static_cast<Number>(static_cast<std::int64_t>(*value))) {
                return static_cast<std::int64_t>(*value);
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<double> as_number() const noexcept {
        if (auto *value = std::get_if<std::int64_t>(&variant_)) {
            return static_cast<double>(*value);
        }
        if (auto *value = std::get_if<Number>(&variant_)) {
            return *value;
        }
        return std::nullopt;
    }
    [[nodiscard]] const std::string *as_string() const noexcept {
        return std::get_if<std::string>(&variant_);
    }
    [[nodiscard]] const Array *as_array() const noexcept { return std::get_if<Array>(&variant_); }
    [[nodiscard]] const Object *as_object() const noexcept { return std::get_if<Object>(&variant_); }

    // Object member lookup; returns nullptr when this is not an object or the
    // key is absent. First match wins; duplicate keys are rejected by the
    // parser so production payloads never contain them.
    [[nodiscard]] const JsonValue *find(std::string_view key) const noexcept {
        const auto *object = as_object();
        if (object == nullptr) {
            return nullptr;
        }
        for (const auto &member : *object) {
            if (member.first == key) {
                return &member.second;
            }
        }
        return nullptr;
    }

    [[nodiscard]] JsonValue *find(std::string_view key) noexcept {
        auto *object = std::get_if<Object>(&variant_);
        if (object == nullptr) {
            return nullptr;
        }
        for (auto &member : *object) {
            if (member.first == key) {
                return &member.second;
            }
        }
        return nullptr;
    }

    void set(std::string key, JsonValue value) {
        if (auto *object = std::get_if<Object>(&variant_); object != nullptr) {
            for (auto &member : *object) {
                if (member.first == key) {
                    member.second = std::move(value);
                    return;
                }
            }
            object->emplace_back(std::move(key), std::move(value));
        }
    }

    friend bool operator==(const JsonValue &lhs, const JsonValue &rhs) = default;

  private:
    std::variant<std::nullptr_t, bool, std::int64_t, Number, std::string, Array, Object> variant_;
};

// ---------------------------------------------------------------------------
// Parsing and serialization
// ---------------------------------------------------------------------------

struct JsonLimits final {
    std::size_t max_depth = 32;
    std::size_t max_document_bytes = 4 * 1024 * 1024;
    std::size_t max_string_bytes = 1024 * 1024;
    std::size_t max_array_items = 100'000;
    std::size_t max_object_members = 100'000;
};

// Strict RFC 8259 subset parser: rejects duplicate keys, trailing data,
// NaN/Infinity, invalid UTF-8 and anything beyond the configured limits.
[[nodiscard]] Result<JsonValue> parse_json(std::string_view text,
                                           JsonLimits limits = JsonLimits{});

// Compact serialization used for wire payloads and durable events.
[[nodiscard]] std::string to_json_string(const JsonValue &value);

// Canonical serialization used for digests: object keys sorted bytewise,
// no insignificant whitespace, numbers normalized through the integer path
// when exactly representable. Secrets must be removed before digesting.
[[nodiscard]] std::string canonical_json_string(const JsonValue &value);

[[nodiscard]] inline Sha256Digest canonical_json_digest(const JsonValue &value) {
    return digest_string(canonical_json_string(value));
}

// Escapes control characters and quotes; used for embedding untrusted text.
[[nodiscard]] std::string json_escape(std::string_view text);

} // namespace mira
