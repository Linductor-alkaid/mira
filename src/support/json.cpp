#include <mira/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mira {
namespace {

Error json_error(std::string message) {
    Error error;
    error.code = ErrorCode::InvalidArgument;
    error.domain = "mira.json";
    error.safe_message = std::move(message);
    return error;
}

class Parser final {
  public:
    Parser(std::string_view text, JsonLimits limits) : text_(text), limits_(limits) {}

    Result<JsonValue> parse() {
        skip_whitespace();
        if (remaining() == 0) {
            return json_error("json document is empty");
        }
        auto value = parse_value(0);
        if (!value) {
            return value;
        }
        skip_whitespace();
        if (remaining() != 0) {
            return json_error("trailing data after json document");
        }
        return value;
    }

  private:
    [[nodiscard]] std::size_t remaining() const noexcept { return text_.size() - position_; }
    [[nodiscard]] char peek() const noexcept { return text_[position_]; }

    void skip_whitespace() noexcept {
        while (remaining() > 0) {
            const auto character = peek();
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
                ++position_;
            } else {
                break;
            }
        }
    }

    bool consume(char expected) noexcept {
        if (remaining() > 0 && peek() == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    Result<JsonValue> parse_value(std::size_t depth) {
        if (depth >= limits_.max_depth) {
            return json_error("json nesting depth exceeded");
        }
        if (remaining() == 0) {
            return json_error("unexpected end of json document");
        }
        switch (peek()) {
        case '{':
            return parse_object(depth);
        case '[':
            return parse_array(depth);
        case '"':
            return parse_string_value();
        case 't':
            return parse_literal("true", JsonValue(true));
        case 'f':
            return parse_literal("false", JsonValue(false));
        case 'n':
            return parse_literal("null", JsonValue(nullptr));
        default:
            return parse_number();
        }
    }

    Result<JsonValue> parse_literal(std::string_view literal, JsonValue value) {
        if (text_.compare(position_, literal.size(), literal) != 0) {
            return json_error("invalid json literal");
        }
        position_ += literal.size();
        return value;
    }

    Result<JsonValue> parse_object(std::size_t depth) {
        static_cast<void>(consume('{'));
        JsonValue::Object members;
        skip_whitespace();
        if (consume('}')) {
            return JsonValue(std::move(members));
        }
        while (true) {
            skip_whitespace();
            if (remaining() == 0 || peek() != '"') {
                return json_error("expected object key string");
            }
            auto key = parse_string_raw();
            if (!key) {
                return key.error();
            }
            if (std::any_of(members.begin(), members.end(),
                            [&](const auto &member) { return member.first == key.value(); })) {
                return json_error("duplicate object key");
            }
            if (members.size() >= limits_.max_object_members) {
                return json_error("json object member limit exceeded");
            }
            skip_whitespace();
            if (!consume(':')) {
                return json_error("expected ':' after object key");
            }
            skip_whitespace();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            members.emplace_back(std::move(key).value(), std::move(value).value());
            skip_whitespace();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                return JsonValue(std::move(members));
            }
            return json_error("expected ',' or '}' in object");
        }
    }

    Result<JsonValue> parse_array(std::size_t depth) {
        static_cast<void>(consume('['));
        JsonValue::Array items;
        skip_whitespace();
        if (consume(']')) {
            return JsonValue(std::move(items));
        }
        while (true) {
            if (items.size() >= limits_.max_array_items) {
                return json_error("json array item limit exceeded");
            }
            skip_whitespace();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            items.push_back(std::move(value).value());
            skip_whitespace();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                return JsonValue(std::move(items));
            }
            return json_error("expected ',' or ']' in array");
        }
    }

    Result<JsonValue> parse_string_value() {
        auto text = parse_string_raw();
        if (!text) {
            return text.error();
        }
        return JsonValue(std::move(text).value());
    }

    Result<std::string> parse_string_raw() {
        static_cast<void>(consume('"'));
        std::string output;
        while (true) {
            if (remaining() == 0) {
                return json_error("unterminated json string");
            }
            const auto character = static_cast<unsigned char>(text_[position_++]);
            if (output.size() >= limits_.max_string_bytes) {
                return json_error("json string length exceeded");
            }
            if (character < 0x20U) {
                return json_error("unescaped control character in json string");
            }
            if (character == '"') {
                return output;
            }
            if (character == '\\') {
                auto escaped = parse_escape();
                if (!escaped) {
                    return escaped.error();
                }
                output.append(escaped.value());
                continue;
            }
            if (character < 0x80U) {
                output.push_back(static_cast<char>(character));
                continue;
            }
            // Multi-byte UTF-8: copy the raw bytes while validating the
            // sequence so canonical digests never depend on accepted garbage.
            std::size_t continuation = 0;
            std::uint32_t code_point = 0;
            if ((character & 0xE0U) == 0xC0U) {
                continuation = 1;
                code_point = character & 0x1FU;
            } else if ((character & 0xF0U) == 0xE0U) {
                continuation = 2;
                code_point = character & 0x0FU;
            } else if ((character & 0xF8U) == 0xF0U) {
                continuation = 3;
                code_point = character & 0x07U;
            } else {
                return json_error("invalid utf-8 lead byte");
            }
            output.push_back(static_cast<char>(character));
            for (std::size_t index = 0; index < continuation; ++index) {
                if (remaining() == 0) {
                    return json_error("truncated utf-8 sequence");
                }
                const auto trailing = static_cast<unsigned char>(text_[position_++]);
                if (output.size() >= limits_.max_string_bytes) {
                    return json_error("json string length exceeded");
                }
                if ((trailing & 0xC0U) != 0x80U) {
                    return json_error("invalid utf-8 continuation byte");
                }
                code_point = (code_point << 6U) | (trailing & 0x3FU);
                output.push_back(static_cast<char>(trailing));
            }
            const std::uint32_t minimum[] = {0x0U, 0x80U, 0x800U, 0x10000U};
            if (code_point < minimum[continuation] || code_point > 0x10FFFFU ||
                (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
                return json_error("invalid utf-8 code point");
            }
        }
    }

    Result<std::uint32_t> parse_hex4() {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            if (remaining() == 0) {
                return json_error("truncated unicode escape");
            }
            const auto character = text_[position_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                return json_error("invalid unicode escape digit");
            }
        }
        return value;
    }

    Result<std::string> parse_escape() {
        if (remaining() == 0) {
            return json_error("unterminated json escape");
        }
        const auto character = text_[position_++];
        switch (character) {
        case '"':
            return std::string("\"");
        case '\\':
            return std::string("\\");
        case '/':
            return std::string("/");
        case 'b':
            return std::string("\b");
        case 'f':
            return std::string("\f");
        case 'n':
            return std::string("\n");
        case 'r':
            return std::string("\r");
        case 't':
            return std::string("\t");
        case 'u':
            return parse_unicode_escape();
        default:
            return json_error("invalid json escape");
        }
    }

    Result<std::string> parse_unicode_escape() {
        auto code_point = parse_hex4();
        if (!code_point) {
            return code_point.error();
        }
        const auto high = code_point.value();
        if (high >= 0xD800U && high <= 0xDBFFU) {
            if (remaining() < 2 || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
                return json_error("unpaired utf-16 surrogate");
            }
            position_ += 2;
            auto low = parse_hex4();
            if (!low) {
                return low.error();
            }
            if (low.value() < 0xDC00U || low.value() > 0xDFFFU) {
                return json_error("invalid utf-16 surrogate pair");
            }
            const auto combined =
                0x10000U + ((high - 0xD800U) << 10U) + (low.value() - 0xDC00U);
            return encode_utf8(combined);
        }
        if (high >= 0xDC00U && high <= 0xDFFFU) {
            return json_error("unpaired utf-16 surrogate");
        }
        return encode_utf8(high);
    }

    static Result<std::string> encode_utf8(std::uint32_t code_point) {
        std::string output;
        if (code_point < 0x80U) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800U) {
            output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point < 0x10000U) {
            output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
        return output;
    }

    Result<JsonValue> parse_number() {
        const std::size_t begin = position_;
        if (consume('-')) {
            // fallthrough to digits below
        }
        std::size_t integer_digits = 0;
        while (remaining() > 0 && peek() >= '0' && peek() <= '9') {
            ++position_;
            ++integer_digits;
        }
        if (integer_digits == 0) {
            return json_error("invalid json number");
        }
        bool is_float = false;
        if (remaining() > 0 && peek() == '.') {
            is_float = true;
            ++position_;
            std::size_t fraction_digits = 0;
            while (remaining() > 0 && peek() >= '0' && peek() <= '9') {
                ++position_;
                ++fraction_digits;
            }
            if (fraction_digits == 0) {
                return json_error("invalid json fraction");
            }
        }
        if (remaining() > 0 && (peek() == 'e' || peek() == 'E')) {
            is_float = true;
            ++position_;
            if (remaining() > 0 && (peek() == '+' || peek() == '-')) {
                ++position_;
            }
            std::size_t exponent_digits = 0;
            while (remaining() > 0 && peek() >= '0' && peek() <= '9') {
                ++position_;
                ++exponent_digits;
            }
            if (exponent_digits == 0) {
                return json_error("invalid json exponent");
            }
        }
        const std::string token(text_.substr(begin, position_ - begin));
        if (!is_float) {
            std::size_t index = (token[0] == '-') ? 1 : 0;
            // 18 digits always accumulate without overflowing int64; longer
            // integer tokens fall back to the double path.
            if (token.size() - index < 19) {
                std::int64_t value = 0;
                for (; index < token.size(); ++index) {
                    value = value * 10 + (token[index] - '0');
                }
                if (token[0] == '-') {
                    value = -value;
                }
                return JsonValue(value);
            }
        }
        try {
            const double value = std::stod(token);
            if (!std::isfinite(value)) {
                return json_error("json number is not finite");
            }
            return JsonValue(static_cast<JsonValue::Number>(value));
        } catch (const std::exception &) {
            return json_error("json number out of range");
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
    JsonLimits limits_;
};

void write_number(JsonValue::Number value, std::string &output) {
    if (value == static_cast<JsonValue::Number>(static_cast<std::int64_t>(value)) &&
        std::abs(value) < 9.007199254740992e15) {
        output += std::to_string(static_cast<std::int64_t>(value));
        return;
    }
    char buffer[32];
    const int length = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(buffer)) {
        output.append(buffer, static_cast<std::size_t>(length));
    }
}

void write_string(std::string_view text, std::string &output) {
    output.push_back('"');
    std::size_t continuation = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        if (continuation > 0) {
            output.push_back(text[index]);
            --continuation;
            continue;
        }
        if ((character & 0xE0U) == 0xC0U) {
            continuation = 1;
        } else if ((character & 0xF0U) == 0xE0U) {
            continuation = 2;
        } else if ((character & 0xF8U) == 0xF0U) {
            continuation = 3;
        }
        switch (character) {
        case '"':
            output += "\\\"";
            continue;
        case '\\':
            output += "\\\\";
            continue;
        case '\b':
            output += "\\b";
            continue;
        case '\f':
            output += "\\f";
            continue;
        case '\n':
            output += "\\n";
            continue;
        case '\r':
            output += "\\r";
            continue;
        case '\t':
            output += "\\t";
            continue;
        default:
            break;
        }
        if (character < 0x20U) {
            char buffer[7];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
            output += buffer;
            continue;
        }
        output.push_back(text[index]);
    }
    output.push_back('"');
}

void write_value(const JsonValue &value, std::string &output, bool canonical) {
    switch (value.kind()) {
    case JsonValue::Kind::Null:
        output += "null";
        break;
    case JsonValue::Kind::Boolean:
        output += *value.as_boolean() ? "true" : "false";
        break;
    case JsonValue::Kind::Integer:
        output += std::to_string(*value.as_integer());
        break;
    case JsonValue::Kind::Number:
        write_number(*value.as_number(), output);
        break;
    case JsonValue::Kind::String:
        write_string(*value.as_string(), output);
        break;
    case JsonValue::Kind::Array: {
        output.push_back('[');
        const auto *items = value.as_array();
        for (std::size_t index = 0; index < items->size(); ++index) {
            if (index > 0) {
                output.push_back(',');
            }
            write_value((*items)[index], output, canonical);
        }
        output.push_back(']');
        break;
    }
    case JsonValue::Kind::Object: {
        output.push_back('{');
        const auto *members = value.as_object();
        if (canonical) {
            std::vector<const std::pair<std::string, JsonValue> *> ordered;
            ordered.reserve(members->size());
            for (const auto &member : *members) {
                ordered.push_back(&member);
            }
            std::sort(ordered.begin(), ordered.end(),
                      [](const auto *lhs, const auto *rhs) { return lhs->first < rhs->first; });
            for (std::size_t index = 0; index < ordered.size(); ++index) {
                if (index > 0) {
                    output.push_back(',');
                }
                write_string(ordered[index]->first, output);
                output.push_back(':');
                write_value(ordered[index]->second, output, canonical);
            }
        } else {
            for (std::size_t index = 0; index < members->size(); ++index) {
                if (index > 0) {
                    output.push_back(',');
                }
                write_string((*members)[index].first, output);
                output.push_back(':');
                write_value((*members)[index].second, output, canonical);
            }
        }
        output.push_back('}');
        break;
    }
    }
}

} // namespace

Result<JsonValue> parse_json(std::string_view text, JsonLimits limits) {
    if (text.size() > limits.max_document_bytes) {
        return json_error("json document exceeds size limit");
    }
    Parser parser(text, limits);
    return parser.parse();
}

std::string to_json_string(const JsonValue &value) {
    std::string output;
    output.reserve(64);
    write_value(value, output, false);
    return output;
}

std::string canonical_json_string(const JsonValue &value) {
    std::string output;
    output.reserve(64);
    write_value(value, output, true);
    return output;
}

std::string json_escape(std::string_view text) {
    std::string output;
    output.reserve(text.size() + 2);
    write_string(text, output);
    return output;
}

} // namespace mira
