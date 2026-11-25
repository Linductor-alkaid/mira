// Contract tests for the deterministic text primitives (DEC-147): lexical
// segmentation and character-boundary truncation. Freezes the public semantics
// every lexical leg depends on: ASCII words unchanged, Chinese and kana as
// characters and adjacent pairs, normalization before splitting, invalid UTF-8
// as separators, and units that are always safe inside a quoted FTS5 term.

#include "../support/test.hpp"

#include <mira/text_segmentation.hpp>
#include <mira/tool_executor.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace mira;

[[nodiscard]] std::vector<std::string> texts(std::string_view text, LexicalUnits units) {
    std::vector<std::string> result;
    for (const auto &unit : segment_lexically(text, units)) {
        result.push_back(unit.text);
    }
    return result;
}

[[nodiscard]] bool valid_utf8(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto lead = static_cast<std::uint8_t>(text[offset]);
        const std::size_t length = lead < 0x80U   ? 1
                                   : lead < 0xC0U ? 0
                                   : lead < 0xE0U ? 2
                                   : lead < 0xF0U ? 3
                                   : lead < 0xF8U ? 4
                                                  : 0;
        if (length == 0 || offset + length > text.size()) {
            return false;
        }
        for (std::size_t index = 1; index < length; ++index) {
            if ((static_cast<std::uint8_t>(text[offset + index]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        offset += length;
    }
    return true;
}

int ascii_words_are_unchanged() {
    // A script that separates words with spaces gives the same units for every
    // selection, lower-cased exactly as the ASCII tokenizer did.
    const std::vector<std::string> words = {"the", "weather", "in", "beijing", "2024"};
    MIRA_CHECK(texts("The Weather in BeiJing, 2024!", LexicalUnits::IndexUnits) == words);
    MIRA_CHECK(texts("The Weather in BeiJing, 2024!", LexicalUnits::MatchUnits) == words);
    MIRA_CHECK(texts("The Weather in BeiJing, 2024!", LexicalUnits::CoarseUnits) == words);
    // Identifiers split where SQLite's unicode61 splits them.
    const std::vector<std::string> identifiers = {"api_key", "deploy", "sh",  "v2", "4",
                                                  "1",       "192",    "168", "1",  "42"};
    MIRA_CHECK(texts("API_KEY deploy.sh v2.4.1 192.168.1.42", LexicalUnits::IndexUnits) ==
               identifiers);
    return 0;
}

int chinese_yields_characters_and_pairs() {
    // Every character, and after it the pair that starts there.
    MIRA_CHECK(texts("北京天", LexicalUnits::MatchUnits) ==
               (std::vector<std::string>{"北", "北京", "京", "京天", "天"}));
    MIRA_CHECK(texts("北京天", LexicalUnits::IndexUnits) ==
               (std::vector<std::string>{"北", "京", "天"}));
    MIRA_CHECK(texts("北京天", LexicalUnits::CoarseUnits) ==
               (std::vector<std::string>{"北京", "京天"}));
    // A run of one character is the only coarse unit its run has.
    MIRA_CHECK(texts("打，印", LexicalUnits::CoarseUnits) ==
               (std::vector<std::string>{"打", "印"}));
    // Punctuation and the ideographic space end a run.
    MIRA_CHECK(texts("白板马克笔已干，蓝牙偶发断连。", LexicalUnits::CoarseUnits) ==
               (std::vector<std::string>{"白板", "板马", "马克", "克笔", "笔已", "已干", "蓝牙",
                                         "牙偶", "偶发", "发断", "断连"}));
    return 0;
}

int kana_joins_the_same_run_as_han() {
    const std::vector<std::string> sounds = {"东", "京", "タ", "ワ", "ー", "ひ", "ら", "が", "な"};
    MIRA_CHECK(texts("东京タワー ひらがな", LexicalUnits::IndexUnits) == sounds);
    MIRA_CHECK(texts("东京タワー", LexicalUnits::CoarseUnits) ==
               (std::vector<std::string>{"东京", "京タ", "タワ", "ワー"}));
    return 0;
}

int mixed_text_keeps_words_apart_from_characters() {
    MIRA_CHECK(texts("重启 docker 服务", LexicalUnits::IndexUnits) ==
               (std::vector<std::string>{"重", "启", "docker", "服", "务"}));
    MIRA_CHECK(texts("重启 docker 服务", LexicalUnits::CoarseUnits) ==
               (std::vector<std::string>{"重启", "docker", "服务"}));
    // Full-width ASCII folds to half-width before splitting.
    MIRA_CHECK(texts("ＡＰＩ_ＫＥＹ　ｘ", LexicalUnits::IndexUnits) ==
               (std::vector<std::string>{"api_key", "x"}));
    MIRA_CHECK(texts("全角ＡＢＣ", LexicalUnits::CoarseUnits) ==
               (std::vector<std::string>{"全角", "abc"}));
    return 0;
}

int invisible_characters_are_dropped() {
    // A zero-width space inside a word no longer splits it.
    MIRA_CHECK(texts("pa\u200Bssword", LexicalUnits::IndexUnits) ==
               (std::vector<std::string>{"password"}));
    // Inside a Han run the pair is the two characters, with nothing between.
    MIRA_CHECK(texts("北\u200B京", LexicalUnits::MatchUnits) ==
               (std::vector<std::string>{"北", "北京", "京"}));
    MIRA_CHECK(texts("北\u200B京", LexicalUnits::CoarseUnits) ==
               (std::vector<std::string>{"北京"}));
    // The soft hyphen and a variation selector behave the same way.
    MIRA_CHECK(texts("a\u00ADb", LexicalUnits::IndexUnits) == (std::vector<std::string>{"ab"}));
    MIRA_CHECK(texts("\uFEFFstart", LexicalUnits::IndexUnits) ==
               (std::vector<std::string>{"start"}));
    return 0;
}

int invalid_bytes_separate() {
    // A truncated character is dropped; the text before and after it survives.
    const std::string cut = std::string("好") + "\xe5" + "的";
    MIRA_CHECK(texts(cut, LexicalUnits::IndexUnits) == (std::vector<std::string>{"好", "的"}));
    // No pair spans the invalid byte.
    MIRA_CHECK(texts(cut, LexicalUnits::CoarseUnits) == (std::vector<std::string>{"好", "的"}));
    // Overlong forms, surrogates and stray continuation bytes are invalid.
    MIRA_CHECK(texts("\xc0\x80\xe0\x80\x80\xed\xa0\x80\x80\xff", LexicalUnits::MatchUnits).empty());
    // A word is cut at an invalid byte.
    const std::string split = std::string("ab") + "\xff" + "cd";
    MIRA_CHECK(texts(split, LexicalUnits::IndexUnits) == (std::vector<std::string>{"ab", "cd"}));
    return 0;
}

int units_are_deterministic_and_safe_to_quote() {
    const std::string text = "用户偏好简洁回复 API_KEY 打印机 IP 是 192.168.1.42 ＡＢＣ 东京タワー";
    const auto first = texts(text, LexicalUnits::MatchUnits);
    MIRA_CHECK(first == texts(text, LexicalUnits::MatchUnits));
    MIRA_CHECK(!first.empty());
    for (const auto &unit : first) {
        MIRA_CHECK(!unit.empty());
        MIRA_CHECK(unit.find(' ') == std::string::npos);
        MIRA_CHECK(unit.find('"') == std::string::npos);
        MIRA_CHECK(valid_utf8(unit));
    }
    // The index text is the index units joined by single spaces, and text
    // without a single unit has an empty one.
    std::string joined;
    for (const auto &unit : texts(text, LexicalUnits::IndexUnits)) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += unit;
    }
    MIRA_CHECK(lexical_index_text(text) == joined);
    MIRA_CHECK(lexical_index_text("。 ？!").empty());
    MIRA_CHECK(lexical_index_text("").empty());
    // A bigram is exactly two characters.
    for (const auto &unit : segment_lexically("北京天", LexicalUnits::CoarseUnits)) {
        MIRA_CHECK(unit.kind == LexicalUnitKind::Bigram);
        MIRA_CHECK(unit.text == "北京" || unit.text == "京天");
    }
    return 0;
}

int truncation_ends_on_a_character_boundary() {
    const std::string text = "北京x"; // 3 + 3 + 1 bytes
    MIRA_CHECK(truncate_utf8(text, 100) == text);
    MIRA_CHECK(truncate_utf8(text, 7) == text);
    MIRA_CHECK(truncate_utf8(text, 6) == "北京");
    // The cut falls inside the second character and backs off to its start.
    MIRA_CHECK(truncate_utf8(text, 5) == "北");
    MIRA_CHECK(truncate_utf8(text, 3) == "北");
    // Inside the first character: nothing of it is kept.
    MIRA_CHECK(truncate_utf8(text, 1).empty());
    MIRA_CHECK(truncate_utf8(text, 0).empty());
    MIRA_CHECK(truncate_utf8("plain", 2) == "pl");
    // Four-byte characters back off the same way.
    const std::string emoji = "\xf0\x9f\x98\x80\xf0\x9f\x98\x81";
    MIRA_CHECK(truncate_utf8(emoji, 5) == emoji.substr(0, 4));
    MIRA_CHECK(truncate_utf8(emoji, 4) == emoji.substr(0, 4));
    // The bounded tool summary marks its cut and stays valid UTF-8.
    const std::string summary = truncate_text("打印机卡纸", 7);
    MIRA_CHECK(summary == "打印...");
    MIRA_CHECK(truncate_text("北京", 8) == "北京");
    return 0;
}

} // namespace

int main() {
    if (const int result = ascii_words_are_unchanged(); result != 0) {
        return result;
    }
    if (const int result = chinese_yields_characters_and_pairs(); result != 0) {
        return result;
    }
    if (const int result = kana_joins_the_same_run_as_han(); result != 0) {
        return result;
    }
    if (const int result = mixed_text_keeps_words_apart_from_characters(); result != 0) {
        return result;
    }
    if (const int result = invisible_characters_are_dropped(); result != 0) {
        return result;
    }
    if (const int result = invalid_bytes_separate(); result != 0) {
        return result;
    }
    if (const int result = units_are_deterministic_and_safe_to_quote(); result != 0) {
        return result;
    }
    return truncation_ends_on_a_character_boundary();
}
